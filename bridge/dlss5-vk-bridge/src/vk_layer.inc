// Vulkan layer plumbing.
//
// The bridge has to do two things the game never asked for: import D3D12 shared
// memory and fences into the game's VkDevice, and submit its own work on the
// game's queues. Both require device extensions (external_memory_win32,
// external_semaphore_win32) and the timeline-semaphore feature to be enabled at
// vkCreateDevice -- and the game will not enable them. A layer is the clean way
// to add them: it interposes vkCreateInstance / vkCreateDevice, appends the
// extensions, and injects the feature, transparently to the game.
//
// It also intercepts vkQueueSubmit, which is the only place a Vulkan semaphore
// can be signalled, and so the only place the cross-API handoff can be driven.
//
// The layer is registered implicitly, so it is loaded into every Vulkan process
// on the machine, most of which will never touch DLSS. The rule everywhere
// below is therefore: NEVER be the reason a create call fails or a handle is
// unusable. Anything we add is added only if the driver advertises it, a create
// that fails with our additions is retried with the application's own arguments
// untouched, and a handle the layer does not track is forwarded straight down
// the chain rather than refused. The worst outcome is a game the bridge cannot
// attach to, never a game that will not start.

#include <vulkan/vk_layer.h>

// Guard so the bridge's own submits are not re-intercepted by our own hook.
static __declspec(thread) int g_internal_submit;

// Forward declarations into vk_interop.inc.
static bool     BridgeWantsSubmitMatch();
static uint32_t BridgeMatchSubmit(uint32_t count, const VkSubmitInfo *submits);
static uint32_t BridgeMatchSubmit2(uint32_t count, const VkSubmitInfo2 *submits);
static void     BridgeOnGameSubmitDone(VkQueue queue, uint32_t family, bool tracked, uint32_t slots, bool ok);
static void     BridgeNoteUnmatchedSubmit(uint32_t family, bool tracked);
static void     BridgeOnCommandBufferBegin(VkCommandBuffer cmd);
static void     BridgeOnExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer *secondaries);
static void     BridgeDeviceLost(VkDevice dev);

// ---- host traits the core consults ----------------------------------------
// The layer sees a submit after it went down the chain, so it can queue a
// retire fence behind it; it enables external_semaphore_win32 itself, so the
// D3D12 fences import as timeline semaphores; and with its own command pool it
static const bool kHostSubmitsBehindGame = true;

#include "vk_dispatch.inc"

// Slots are recycled: a destroyed instance/device clears its slot and the next
// create reuses it. (Appending only, as this once did, meant a process that
// creates and destroys devices -- engines routinely create a throwaway device
// to probe the hardware -- eventually filled the table, after which its real
// device went untracked.) `g_*_count` is the high-water mark, not a population.
static InstanceDispatch g_inst[8];  static volatile LONG g_inst_count;
static DeviceDispatch   g_dev[8];   static volatile LONG g_dev_count;
static CRITICAL_SECTION g_disp_cs;

struct QueueRec { VkQueue queue; VkDevice device; uint32_t family; };
static QueueRec g_queues[64]; static volatile LONG g_queue_count;

// The next layer's resolvers, kept so a handle the tables do not know can still
// be forwarded instead of refused.
static PFN_vkGetInstanceProcAddr g_next_gipa;
static PFN_vkGetDeviceProcAddr   g_next_gdpa;

static DeviceDispatch *DispatchForDevice(VkDevice d)
{
    if (d == VK_NULL_HANDLE) return nullptr;
    for (LONG i = 0; i < g_dev_count; ++i) if (g_dev[i].device == d) return &g_dev[i];
    return nullptr;
}
static DeviceDispatch *AnyDeviceDispatch()
{
    for (LONG i = 0; i < g_dev_count; ++i)
        if (g_dev[i].device != VK_NULL_HANDLE && g_dev[i].QueueSubmit) return &g_dev[i];
    return nullptr;
}
// Device-level objects carry the loader's dispatch pointer as their first
// word, the same one for every object of one device: the standard way to
// find a command buffer's device without having tracked the buffer.
static DeviceDispatch *DispatchForKey(const void *object)
{
    if (object == nullptr) return AnyDeviceDispatch();
    const void *key = *reinterpret_cast<void *const *>(object);
    for (LONG i = 0; i < g_dev_count; ++i)
        if (g_dev[i].device != VK_NULL_HANDLE &&
            *reinterpret_cast<void *const *>(g_dev[i].device) == key) return &g_dev[i];
    return AnyDeviceDispatch();
}
// A queue the layer never saw handed out (a path we do not intercept) must
// still be submittable. The dispatch a queue actually reaches is selected by
// the loader from the queue's own dispatch table, so forwarding through any
// tracked device's entry point is correct; only our per-frame bookkeeping,
// which needs the family, is skipped.
static DeviceDispatch *DispatchForQueue(VkQueue q, uint32_t *family, bool *tracked)
{
    for (LONG i = 0; i < g_queue_count; ++i)
        if (g_queues[i].queue == q)
        {
            if (family) *family = g_queues[i].family;
            if (DeviceDispatch *d = DispatchForDevice(g_queues[i].device))
            {
                if (tracked) *tracked = true;
                return d;
            }
            break;
        }
    if (tracked) *tracked = false;
    return AnyDeviceDispatch();
}
static InstanceDispatch *DispatchForInstance(VkInstance in)
{
    if (in == VK_NULL_HANDLE) return nullptr;
    for (LONG i = 0; i < g_inst_count; ++i) if (g_inst[i].instance == in) return &g_inst[i];
    return nullptr;
}

// The physical device handed to vkCreateDevice belongs to exactly one of the
// instances alive in this process. Querying it through a DIFFERENT instance's
// entry point (which "just take the newest instance" did) is undefined and can
// take the loader down inside the game's own device creation -- a process that
// creates a second instance, as a launcher or an overlay does, could not start.
static InstanceDispatch *InstanceForPhys(VkPhysicalDevice phys)
{
    for (LONG i = 0; i < g_inst_count; ++i)
    {
        InstanceDispatch &d = g_inst[i];
        if (d.instance == VK_NULL_HANDLE || d.EnumeratePhysicalDevices == nullptr) continue;
        uint32_t n = 0;
        if (d.EnumeratePhysicalDevices(d.instance, &n, nullptr) < 0 || n == 0) continue;
        VkPhysicalDevice list[32];
        if (n > (uint32_t)_countof(list)) n = (uint32_t)_countof(list);
        if (d.EnumeratePhysicalDevices(d.instance, &n, list) < 0) continue;
        for (uint32_t k = 0; k < n; ++k) if (list[k] == phys) return &d;
    }
    // Enumeration can be unavailable; with a single instance in the process
    // there is nothing to confuse it with, so that one is still the answer.
    InstanceDispatch *only = nullptr;
    for (LONG i = 0; i < g_inst_count; ++i)
        if (g_inst[i].instance != VK_NULL_HANDLE)
        {
            if (only) return nullptr;   // more than one: refuse to guess
            only = &g_inst[i];
        }
    return only;
}

// The loader threads its per-object next-link through the pNext chain of the
// create info as a VkLayerInstanceCreateInfo / VkLayerDeviceCreateInfo with
// function == ..._LINK_INFO. This walks to it.
static VkLayerInstanceCreateInfo *FindInstanceLink(const VkInstanceCreateInfo *ci)
{
    auto *p = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<void *>(ci->pNext));
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = reinterpret_cast<VkLayerInstanceCreateInfo *>(const_cast<void *>(p->pNext));
    return p;
}
static VkLayerDeviceCreateInfo *FindDeviceLink(const VkDeviceCreateInfo *ci)
{
    auto *p = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<void *>(ci->pNext));
    while (p && !(p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                  p->function == VK_LAYER_LINK_INFO))
        p = reinterpret_cast<VkLayerDeviceCreateInfo *>(const_cast<void *>(p->pNext));
    return p;
}

// Adds `add` to a copied extension list if not already present.
static void AppendExtensions(const char *const *existing, uint32_t n,
                             const char *const *add, uint32_t addn,
                             const char **out, uint32_t *outn)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ++i) out[k++] = existing[i];
    for (uint32_t j = 0; j < addn; ++j)
    {
        bool have = false;
        for (uint32_t i = 0; i < n; ++i) if (strcmp(existing[i], add[j]) == 0) { have = true; break; }
        if (!have) out[k++] = add[j];
    }
    *outn = k;
}

// ---- vkCreateInstance ------------------------------------------------------
static bool InstanceExtSupported(PFN_vkGetInstanceProcAddr gipa, const char *name)
{
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (enum_ext == nullptr) return false;
    uint32_t n = 0;
    if (enum_ext(nullptr, &n, nullptr) < 0 || n == 0) return false;
    VkExtensionProperties *props = (VkExtensionProperties *)malloc(n * sizeof(VkExtensionProperties));
    if (props == nullptr) return false;
    bool have = false;
    if (enum_ext(nullptr, &n, props) >= 0)
        for (uint32_t i = 0; i < n; ++i) if (strcmp(props[i].extensionName, name) == 0) { have = true; break; }
    free(props);
    return have;
}

static VKAPI_ATTR VkResult VKAPI_CALL dlss5vk_CreateInstance(
    const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out)
{
    VkLayerInstanceCreateInfo *link = FindInstanceLink(ci);
    if (link == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerInstanceLink *self = link->u.pLayerInfo;
    PFN_vkGetInstanceProcAddr gipa = self->pfnNextGetInstanceProcAddr;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    g_next_gipa = gipa;

    // Advance the chain so the next layer gets its own link on the way down.
    link->u.pLayerInfo = self->pNext;

    // Add only what this loader/ICD actually advertises. These three are core
    // in Vulkan 1.1 and present everywhere in practice, but asking for an
    // extension that is not there fails the whole call, and a game that cannot
    // create an instance cannot start.
    static const char *want[] = {
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_KHR_get_physical_device_properties2",
    };
    const char *add[_countof(want)];
    uint32_t addn = 0;
    for (uint32_t i = 0; i < _countof(want); ++i)
        if (InstanceExtSupported(gipa, want[i])) add[addn++] = want[i];

    const char *merged[128];
    uint32_t mergedn = 0;
    bool modified = false;
    if ((uint64_t)ci->enabledExtensionCount + addn <= _countof(merged))
    {
        AppendExtensions(ci->ppEnabledExtensionNames, ci->enabledExtensionCount,
                         add, addn, merged, &mergedn);
        modified = mergedn != ci->enabledExtensionCount;
    }

    VkInstanceCreateInfo mci = *ci;
    if (modified)
    {
        mci.ppEnabledExtensionNames = merged;
        mci.enabledExtensionCount   = mergedn;
    }

    VkResult r = create(&mci, alloc, out);
    if (r != VK_SUCCESS && modified)
    {
        // Never hold up the application over an extension only the bridge wants.
        Log("[layer] vkCreateInstance refused the interop capability extensions (%d); "
            "retrying with the application's own list", r);
        link->u.pLayerInfo = self->pNext;   // the next layer consumed the link
        r = create(ci, alloc, out);
        modified = false;
    }
    if (r != VK_SUCCESS) return r;

    EnterCriticalSection(&g_disp_cs);
    InstanceDispatch *d = nullptr;
    for (LONG i = 0; i < g_inst_count && d == nullptr; ++i)
        if (g_inst[i].instance == VK_NULL_HANDLE) d = &g_inst[i];
    if (d == nullptr && g_inst_count < (LONG)_countof(g_inst)) d = &g_inst[g_inst_count++];
    if (d)
    {
        memset(d, 0, sizeof(*d));
        d->instance            = *out;
        d->GetInstanceProcAddr = gipa;
        d->DestroyInstance     = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
        d->EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(*out, "vkEnumeratePhysicalDevices");
        d->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(*out, "vkGetPhysicalDeviceMemoryProperties");
        d->GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties");
        d->GetPhysicalDeviceProperties2      = (PFN_vkGetPhysicalDeviceProperties2)gipa(*out, "vkGetPhysicalDeviceProperties2");
        d->EnumerateDeviceExtensionProperties= (PFN_vkEnumerateDeviceExtensionProperties)gipa(*out, "vkEnumerateDeviceExtensionProperties");
    }
    LeaveCriticalSection(&g_disp_cs);
    Log("[layer] instance created; %u capability extension(s) added%s.",
        modified ? mergedn - ci->enabledExtensionCount : 0, d ? "" : " (not tracked: table full)");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL dlss5vk_DestroyInstance(VkInstance in, const VkAllocationCallbacks *a)
{
    InstanceDispatch *d = DispatchForInstance(in);
    PFN_vkDestroyInstance fn = d ? d->DestroyInstance : nullptr;
    if (d)
    {
        EnterCriticalSection(&g_disp_cs);
        memset(d, 0, sizeof(*d));   // free the slot for the next instance
        LeaveCriticalSection(&g_disp_cs);
    }
    if (fn) fn(in, a);
    else if (g_next_gipa)
    {
        auto next = reinterpret_cast<PFN_vkDestroyInstance>(g_next_gipa(in, "vkDestroyInstance"));
        if (next) next(in, a);
    }
}

// ---- vkCreateDevice --------------------------------------------------------
static bool DeviceSupportsExt(InstanceDispatch *inst, VkPhysicalDevice phys, const char *name)
{
    if (inst == nullptr || inst->EnumerateDeviceExtensionProperties == nullptr) return false;
    uint32_t n = 0;
    if (inst->EnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr) < 0 || n == 0) return false;
    VkExtensionProperties *props = (VkExtensionProperties *)malloc(n * sizeof(VkExtensionProperties));
    if (props == nullptr) return false;
    bool have = false;
    if (inst->EnumerateDeviceExtensionProperties(phys, nullptr, &n, props) >= 0)
        for (uint32_t i = 0; i < n; ++i) if (strcmp(props[i].extensionName, name) == 0) { have = true; break; }
    free(props);
    return have;
}

static VKAPI_ATTR VkResult VKAPI_CALL dlss5vk_CreateDevice(
    VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
    const VkAllocationCallbacks *alloc, VkDevice *out)
{
    VkLayerDeviceCreateInfo *link = FindDeviceLink(ci);
    if (link == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceLink *self = link->u.pLayerInfo;
    PFN_vkGetInstanceProcAddr gipa = self->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   gdpa = self->pfnNextGetDeviceProcAddr;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(nullptr, "vkCreateDevice"));
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    g_next_gdpa = gdpa;
    link->u.pLayerInfo = self->pNext;

    // Only add what this physical device actually supports, asked through the
    // instance that owns it; if the bridge cannot have its extensions the game
    // still gets its device and simply renders without the bridge.
    InstanceDispatch *inst = InstanceForPhys(phys);
    if (inst == nullptr)
        Log("[layer] this physical device could not be matched to a tracked instance; "
            "creating the device untouched -- the bridge will stay inactive here");
    static const char *want[] = {
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_win32",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_win32",
        "VK_KHR_timeline_semaphore",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_EXT_external_memory_host",
    };
    const char *add[_countof(want)];
    uint32_t addn = 0;
    bool timeline_ok = false;
    for (uint32_t i = 0; i < _countof(want); ++i)
        if (DeviceSupportsExt(inst, phys, want[i]))
        {
            add[addn++] = want[i];
            if (strcmp(want[i], "VK_KHR_timeline_semaphore") == 0) timeline_ok = true;
        }

    const char *merged[128];
    uint32_t mergedn = 0;
    bool modified = false;
    if ((uint64_t)ci->enabledExtensionCount + addn <= _countof(merged))
    {
        AppendExtensions(ci->ppEnabledExtensionNames, ci->enabledExtensionCount,
                         add, addn, merged, &mergedn);
        modified = mergedn != ci->enabledExtensionCount;
    }

    VkDeviceCreateInfo mci = *ci;
    if (modified)
    {
        mci.ppEnabledExtensionNames = merged;
        mci.enabledExtensionCount   = mergedn;
    }

    // Inject the timeline-semaphore feature, but only when the extension is
    // there to back it: a feature struct for an extension that is not enabled
    // is what VK_ERROR_FEATURE_NOT_PRESENT is for, and that would be our fault.
    // If the game already put a VkPhysicalDeviceTimelineSemaphoreFeatures (or
    // Vulkan12Features) in the chain, leave its choice alone.
    VkPhysicalDeviceTimelineSemaphoreFeatures ts = {};
    ts.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    ts.timelineSemaphore = VK_TRUE;
    bool have_ts = false;
    for (auto *p = (const VkBaseInStructure *)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES ||
            p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            have_ts = true;
    if (!have_ts && timeline_ok)
    { ts.pNext = (void *)mci.pNext; mci.pNext = &ts; modified = true; }

    VkResult r = create(phys, &mci, alloc, out);
    if (r != VK_SUCCESS && modified)
    {
        Log("[layer] vkCreateDevice refused the interop extensions (%d); retrying with the "
            "game's own device parameters -- the bridge will stay inactive here", r);
        link->u.pLayerInfo = self->pNext;   // the next layer consumed the link
        r = create(phys, ci, alloc, out);
        modified = false;
        addn = 0;
    }
    if (r != VK_SUCCESS) { Log("[layer] next vkCreateDevice failed (%d)", r); return r; }

    EnterCriticalSection(&g_disp_cs);
    DeviceDispatch *d = nullptr;
    for (LONG i = 0; i < g_dev_count && d == nullptr; ++i)
        if (g_dev[i].device == VK_NULL_HANDLE) d = &g_dev[i];
    if (d == nullptr && g_dev_count < (LONG)_countof(g_dev)) d = &g_dev[g_dev_count++];
    if (d)
    {
        memset(d, 0, sizeof(*d));
        d->device = *out; d->phys = phys; d->GetDeviceProcAddr = gdpa;
        #define GD(name) d->name = (PFN_vk##name)gdpa(*out, "vk" #name)
        GD(DestroyDevice); GD(GetDeviceQueue); GD(GetDeviceQueue2);
        GD(QueueSubmit); GD(QueueSubmit2);
        // On a device where synchronization2 comes from VK_KHR_synchronization2
        // rather than core 1.3, only the KHR name resolves -- and this layer
        // intercepts vkQueueSubmit2KHR, so leaving the pointer null would turn
        // the app's perfectly working submit into an error of our making.
        if (d->QueueSubmit2 == nullptr)
            d->QueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(*out, "vkQueueSubmit2KHR");
        GD(CreateImage); GD(DestroyImage); GD(GetImageMemoryRequirements); GD(BindImageMemory);
        GD(AllocateMemory); GD(FreeMemory);
        GD(CreateBuffer); GD(DestroyBuffer); GD(GetBufferMemoryRequirements); GD(BindBufferMemory);
        GD(CreateCommandPool); GD(DestroyCommandPool); GD(AllocateCommandBuffers); GD(FreeCommandBuffers);
        GD(BeginCommandBuffer); GD(EndCommandBuffer); GD(ResetCommandBuffer);
        GD(CmdPipelineBarrier); GD(CmdCopyImage); GD(CmdBlitImage);
        GD(CmdCopyImageToBuffer); GD(CmdCopyBufferToImage); GD(CmdClearColorImage);
        GD(CreateImageView); GD(DestroyImageView);
        GD(CreateSampler); GD(DestroySampler);
        GD(CreateDescriptorSetLayout); GD(DestroyDescriptorSetLayout);
        GD(CreatePipelineLayout); GD(DestroyPipelineLayout);
        GD(CreateShaderModule); GD(DestroyShaderModule);
        GD(CreateComputePipelines); GD(DestroyPipeline);
        GD(CreateDescriptorPool); GD(DestroyDescriptorPool);
        GD(AllocateDescriptorSets); GD(UpdateDescriptorSets);
        GD(CmdBindPipeline); GD(CmdBindDescriptorSets); GD(CmdPushConstants); GD(CmdDispatch);
        GD(CmdExecuteCommands);
        GD(CreateEvent); GD(DestroyEvent); GD(SetEvent); GD(ResetEvent); GD(GetEventStatus);
        GD(CmdSetEvent); GD(CmdWaitEvents);
        GD(CreateSemaphore); GD(DestroySemaphore); GD(WaitSemaphores);
        // Same core-vs-KHR split for timeline semaphores (core 1.2).
        if (d->WaitSemaphores == nullptr)
            d->WaitSemaphores = (PFN_vkWaitSemaphores)gdpa(*out, "vkWaitSemaphoresKHR");
        GD(ImportSemaphoreWin32HandleKHR);
        GD(GetMemoryWin32HandlePropertiesKHR);
        GD(GetMemoryHostPointerPropertiesEXT);
        GD(CreateFence); GD(DestroyFence); GD(WaitForFences); GD(ResetFences); GD(GetFenceStatus);
        GD(DeviceWaitIdle);
        #undef GD
        // vkQueueSubmit2 is core in 1.3 and an extension entry point before it.
        if (d->QueueSubmit2 == nullptr)
            d->QueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(*out, "vkQueueSubmit2KHR");
    }
    LeaveCriticalSection(&g_disp_cs);
    Log("[layer] device created; %u interop extension(s) enabled%s.",
        modified ? mergedn - ci->enabledExtensionCount : 0, d ? "" : " (not tracked: table full)");
    return VK_SUCCESS;
}

// ---- queue tracking --------------------------------------------------------
static void RememberQueue(VkDevice dev, uint32_t family, VkQueue q)
{
    if (q == VK_NULL_HANDLE) return;
    EnterCriticalSection(&g_disp_cs);
    bool known = false;
    for (LONG i = 0; i < g_queue_count; ++i) if (g_queues[i].queue == q) { known = true; break; }
    if (!known && g_queue_count < (LONG)_countof(g_queues))
        g_queues[g_queue_count++] = { q, dev, family };
    LeaveCriticalSection(&g_disp_cs);
}

// A queue handle is only valid while its device is; a recycled handle from a
// later device must not be answered out of a dead entry.
static void ForgetQueues(VkDevice dev)
{
    EnterCriticalSection(&g_disp_cs);
    for (LONG i = 0; i < g_queue_count; )
        if (g_queues[i].device == dev) g_queues[i] = g_queues[--g_queue_count];
        else ++i;
    LeaveCriticalSection(&g_disp_cs);
}

static VKAPI_ATTR void VKAPI_CALL dlss5vk_GetDeviceQueue(
    VkDevice dev, uint32_t family, uint32_t index, VkQueue *out)
{
    DeviceDispatch *d = DispatchForDevice(dev);
    PFN_vkGetDeviceQueue fn = d ? d->GetDeviceQueue : nullptr;
    if (fn == nullptr && g_next_gdpa) fn = (PFN_vkGetDeviceQueue)g_next_gdpa(dev, "vkGetDeviceQueue");
    if (fn == nullptr) { if (out) *out = VK_NULL_HANDLE; return; }   // never hand back a stale handle
    fn(dev, family, index, out);
    if (d) RememberQueue(dev, family, out ? *out : VK_NULL_HANDLE);
}
static VKAPI_ATTR void VKAPI_CALL dlss5vk_GetDeviceQueue2(
    VkDevice dev, const VkDeviceQueueInfo2 *info, VkQueue *out)
{
    DeviceDispatch *d = DispatchForDevice(dev);
    PFN_vkGetDeviceQueue2 fn = d ? d->GetDeviceQueue2 : nullptr;
    if (fn == nullptr && g_next_gdpa) fn = (PFN_vkGetDeviceQueue2)g_next_gdpa(dev, "vkGetDeviceQueue2");
    if (fn == nullptr) { if (out) *out = VK_NULL_HANDLE; return; }
    fn(dev, info, out);
    if (d) RememberQueue(dev, info ? info->queueFamilyIndex : 0, out ? *out : VK_NULL_HANDLE);
}

// ---- vkQueueSubmit : the handoff point ------------------------------------
// The bridge's work rides in whichever of the game's command buffers the
// evaluate hook was called with. Which submit carries that buffer is the
// game's business, not something to guess: v0.1.12 reacted to the first
// submit after the hook, which holds for a single-threaded renderer and is
// wrong for idTech 8, whose DLSS pass is recorded and submitted from another
// thread on its async-compute family with unrelated submits in between
// (issue #3). So each submit is matched against the buffers the bridge has
// work in flight for: a match queues the bridge's own follow-up right behind
// it on the same queue, anything else is only counted. The claim happens
// BEFORE the submit goes down the chain, so the slot cannot be retired and
// reused under it. Our own submits set g_internal_submit and are ignored.
static VKAPI_ATTR VkResult VKAPI_CALL dlss5vk_QueueSubmit(
    VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence)
{
    uint32_t family = 0;
    bool tracked = false;
    DeviceDispatch *d = DispatchForQueue(queue, &family, &tracked);
    if (d == nullptr || d->QueueSubmit == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

    if (g_internal_submit) return d->QueueSubmit(queue, count, submits, fence);

    const uint32_t slots = BridgeWantsSubmitMatch() ? BridgeMatchSubmit(count, submits) : 0;
    const ULONGLONG submit_started = GetTickCount64();
    VkResult r = d->QueueSubmit(queue, count, submits, fence);
    const ULONGLONG submit_elapsed = GetTickCount64() - submit_started;
    if (submit_elapsed >= 8)
        Log("[vk-submit] game GPU vkQueueSubmit blocked %llums family=%u submits=%u matched-slots=0x%X",
            (unsigned long long)submit_elapsed, family, count, slots);
    if (slots)
        BridgeOnGameSubmitDone(queue, family, tracked, slots, r == VK_SUCCESS);
    else if (r == VK_SUCCESS && BridgeWantsSubmitMatch())
        BridgeNoteUnmatchedSubmit(family, tracked);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL dlss5vk_QueueSubmit2(
    VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence)
{
    uint32_t family = 0;
    bool tracked = false;
    DeviceDispatch *d = DispatchForQueue(queue, &family, &tracked);
    if (d == nullptr || d->QueueSubmit2 == nullptr) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (g_internal_submit) return d->QueueSubmit2(queue, count, submits, fence);

    const uint32_t slots = BridgeWantsSubmitMatch() ? BridgeMatchSubmit2(count, submits) : 0;
    const ULONGLONG submit_started = GetTickCount64();
    VkResult r = d->QueueSubmit2(queue, count, submits, fence);
    const ULONGLONG submit_elapsed = GetTickCount64() - submit_started;
    if (submit_elapsed >= 8)
        Log("[vk-submit] game GPU vkQueueSubmit2 blocked %llums family=%u submits=%u matched-slots=0x%X",
            (unsigned long long)submit_elapsed, family, count, slots);
    if (slots)
        BridgeOnGameSubmitDone(queue, family, tracked, slots, r == VK_SUCCESS);
    else if (r == VK_SUCCESS && BridgeWantsSubmitMatch())
        BridgeNoteUnmatchedSubmit(family, tracked);
    return r;
}

// ---- command-buffer lifetime ----------------------------------------------
// Two more facts about the game's command buffers, both cheap to learn: a
// buffer the bridge recorded into being begun again means that recording was
// discarded (its slot is released without ever seeing a submit), and a
// secondary being executed from a primary tells the submit matcher which
// primary to look for.
static VKAPI_ATTR VkResult VKAPI_CALL dlss5vk_BeginCommandBuffer(
    VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{
    DeviceDispatch *d = DispatchForKey(cmd);
    if (d == nullptr || d->BeginCommandBuffer == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    BridgeOnCommandBufferBegin(cmd);
    return d->BeginCommandBuffer(cmd, info);
}
static VKAPI_ATTR void VKAPI_CALL dlss5vk_CmdExecuteCommands(
    VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer *secondaries)
{
    DeviceDispatch *d = DispatchForKey(cmd);
    if (d == nullptr || d->CmdExecuteCommands == nullptr) return;
    BridgeOnExecuteCommands(cmd, count, secondaries);
    d->CmdExecuteCommands(cmd, count, secondaries);
}

// ---- proc-address resolution ----------------------------------------------
static VKAPI_ATTR void VKAPI_CALL dlss5vk_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *a)
{
    // The bridge holds Vulkan objects on this device and may have a worker
    // mid-frame; both have to be gone before the device is. (Tearing down after
    // the device dies is how these bridges hang on quit.)
    BridgeDeviceLost(dev);

    DeviceDispatch *d = DispatchForDevice(dev);
    PFN_vkDestroyDevice fn = d ? d->DestroyDevice : nullptr;
    if (fn == nullptr && g_next_gdpa) fn = (PFN_vkDestroyDevice)g_next_gdpa(dev, "vkDestroyDevice");
    ForgetQueues(dev);
    if (d)
    {
        EnterCriticalSection(&g_disp_cs);
        memset(d, 0, sizeof(*d));   // free the slot for the next device
        LeaveCriticalSection(&g_disp_cs);
    }
    if (fn) fn(dev, a);
}

#define INTERCEPT(name) if (strcmp(pName, "vk" #name) == 0) return (PFN_vkVoidFunction)dlss5vk_##name

// Exported (see dlss5-vk-bridge.def); no __declspec here so the loader-facing
// vkNegotiate... below does not conflict with vk_layer.h's own declaration.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
dlss5vk_GetDeviceProcAddr(VkDevice dev, const char *pName)
{
    // A device the layer does not track gets the chain's own pointers, so none
    // of our wrappers can ever be reached with no dispatch behind them.
    DeviceDispatch *d = DispatchForDevice(dev);
    if (d == nullptr) return g_next_gdpa ? g_next_gdpa(dev, pName) : nullptr;

    INTERCEPT(GetDeviceProcAddr);
    INTERCEPT(DestroyDevice);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    INTERCEPT(QueueSubmit);
    INTERCEPT(BeginCommandBuffer);
    INTERCEPT(CmdExecuteCommands);
    // Claim vkQueueSubmit2/KHR only when there is a real pointer to forward
    // to; on a device without synchronization2 the app must keep getting the
    // chain's own answer (null), not a wrapper that can only fail.
    if (strcmp(pName, "vkQueueSubmit2") == 0 || strcmp(pName, "vkQueueSubmit2KHR") == 0)
    {
        if (d->QueueSubmit2) return (PFN_vkVoidFunction)dlss5vk_QueueSubmit2;
        return d->GetDeviceProcAddr ? d->GetDeviceProcAddr(dev, pName) : nullptr;
    }
    return d->GetDeviceProcAddr ? d->GetDeviceProcAddr(dev, pName) : nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
dlss5vk_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
    INTERCEPT(GetInstanceProcAddr);
    INTERCEPT(CreateInstance);
    INTERCEPT(CreateDevice);
    if (instance != VK_NULL_HANDLE && DispatchForInstance(instance) == nullptr)
        return g_next_gipa ? g_next_gipa(instance, pName) : nullptr;
    INTERCEPT(DestroyInstance);
    INTERCEPT(GetDeviceProcAddr);
    INTERCEPT(DestroyDevice);
    INTERCEPT(GetDeviceQueue);
    INTERCEPT(GetDeviceQueue2);
    INTERCEPT(QueueSubmit);
    INTERCEPT(QueueSubmit2);
    INTERCEPT(BeginCommandBuffer);
    INTERCEPT(CmdExecuteCommands);
    if (strcmp(pName, "vkQueueSubmit2KHR") == 0) return (PFN_vkVoidFunction)dlss5vk_QueueSubmit2;
    InstanceDispatch *in = DispatchForInstance(instance);
    if (in && in->GetInstanceProcAddr) return in->GetInstanceProcAddr(instance, pName);
    return g_next_gipa ? g_next_gipa(instance, pName) : nullptr;
}

// Loader/layer interface negotiation. Declared by vk_layer.h; exported via the
// .def file so we do not re-add dllexport to that existing declaration.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct)
{
    if (!EnsureRuntimeInitialized()) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion > 2)
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr       = dlss5vk_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr         = dlss5vk_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
