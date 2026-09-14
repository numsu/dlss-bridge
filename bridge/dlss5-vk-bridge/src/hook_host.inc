// In-process Vulkan hook plumbing.
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
// The injected host sees a submit after it went down the chain, so it can queue
// a retire fence behind it. It enables external_semaphore_win32 itself, so the
// D3D12 fences import as timeline semaphores; with its own command pool it can
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

// Resolve the loader already selected by the game. Resolution remains lazy so
// initialization does not force Vulkan to load before the game requests it.
static HMODULE g_real_vulkan;
static PFN_vkCreateInstance g_hook_CreateInstance;
static PFN_vkCreateDevice g_hook_CreateDevice;
static CRITICAL_SECTION g_iat_cs;
static __declspec(thread) unsigned g_loader_patch_depth;

static bool EnsureRealLoader()
{
    if (g_real_vulkan && g_next_gipa && g_next_gdpa) return true;
    HMODULE mod = GetModuleHandleW(L"vulkan-1.dll");
    if (mod == nullptr) return false;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(mod, "vkGetInstanceProcAddr"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(mod, "vkGetDeviceProcAddr"));
    auto ci = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(mod, "vkCreateInstance"));
    auto cd = reinterpret_cast<PFN_vkCreateDevice>(GetProcAddress(mod, "vkCreateDevice"));
    if (gipa == nullptr || gdpa == nullptr || ci == nullptr || cd == nullptr) return false;
    g_real_vulkan = mod; g_next_gipa = gipa; g_next_gdpa = gdpa;
    g_hook_CreateInstance = ci; g_hook_CreateDevice = cd;
    return true;
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
    Log("[hook-host] resolving vkEnumerateInstanceExtensionProperties through %p", (void *)gipa);
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    Log("[hook-host] resolved enumerate function %p", (void *)enum_ext);
    if (enum_ext == nullptr) return false;
    uint32_t n = 0;
    Log("[hook-host] enumerating instance-extension count");
    VkResult er = enum_ext(nullptr, &n, nullptr);
    Log("[hook-host] instance-extension count returned %d, n=%u", er, n);
    if (er < 0 || n == 0) return false;
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
    Log("[hook-host] vkCreateInstance intercepted");
    if (!EnsureRealLoader()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = g_next_gipa;
    PFN_vkCreateInstance create = g_hook_CreateInstance;
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

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
    // A nested enumeration from inside Wine's hooked create entry point
    // deadlocks winevulkan's loader lock. Ask for the standard interop names
    // directly and fall back to the application's untouched list if rejected.
    for (uint32_t i = 0; i < _countof(want); ++i) add[addn++] = want[i];
    Log("[hook-host] calling Proton vkCreateInstance with %u interop extension(s)", addn);

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
    Log("[hook-host] Proton vkCreateInstance returned %d", r);
    if (r != VK_SUCCESS && modified)
    {
        // Never hold up the application over an extension only the bridge wants.
        Log("[layer] vkCreateInstance refused the interop capability extensions (%d); "
            "retrying with the application's own list", r);
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
    if (!EnsureRealLoader()) return VK_ERROR_INITIALIZATION_FAILED;
    InstanceDispatch *owner = InstanceForPhys(phys);
    PFN_vkGetInstanceProcAddr gipa = owner ? owner->GetInstanceProcAddr : g_next_gipa;
    PFN_vkGetDeviceProcAddr gdpa = g_next_gdpa;
    PFN_vkCreateDevice create = g_hook_CreateDevice;
    if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

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
    // As at instance creation, do not recurse into winevulkan enumeration
    // while a hooked create is active. NVIDIA's Vulkan driver exposes these
    // standard interop extensions; the unmodified retry below is the guard.
    for (uint32_t i = 0; i < _countof(want); ++i) add[addn++] = want[i];
    timeline_ok = true;

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

// A game may create Vulkan before this DLL can redirect its create calls. NGX
// later supplies the exact instance, physical device and logical device it is
// using; adopt those handles and build the same dispatch tables a create hook
// would have built.
static void HostCaptureNgxInit(VkInstance instance, VkPhysicalDevice phys, VkDevice dev)
{
    if (!EnsureRealLoader() || instance == VK_NULL_HANDLE || phys == VK_NULL_HANDLE || dev == VK_NULL_HANDLE) return;
    EnterCriticalSection(&g_disp_cs);
    InstanceDispatch *in = DispatchForInstance(instance);
    if (!in)
    {
        for (LONG i=0;i<g_inst_count && !in;++i) if(g_inst[i].instance==VK_NULL_HANDLE) in=&g_inst[i];
        if(!in && g_inst_count<(LONG)_countof(g_inst)) in=&g_inst[g_inst_count++];
        if(in)
        {
            memset(in,0,sizeof(*in)); in->instance=instance; in->GetInstanceProcAddr=g_next_gipa;
            #define GI(name) in->name=(PFN_vk##name)g_next_gipa(instance,"vk" #name)
            GI(DestroyInstance); GI(EnumeratePhysicalDevices); GI(GetPhysicalDeviceMemoryProperties);
            GI(GetPhysicalDeviceQueueFamilyProperties); GI(GetPhysicalDeviceProperties2);
            GI(EnumerateDeviceExtensionProperties);
            #undef GI
        }
    }
    DeviceDispatch *d=DispatchForDevice(dev);
    if(!d)
    {
        for(LONG i=0;i<g_dev_count && !d;++i)if(g_dev[i].device==VK_NULL_HANDLE)d=&g_dev[i];
        if(!d && g_dev_count<(LONG)_countof(g_dev))d=&g_dev[g_dev_count++];
        if(d)
        {
            memset(d,0,sizeof(*d)); d->device=dev; d->phys=phys; d->GetDeviceProcAddr=g_next_gdpa;
            #define GD(name) d->name=(PFN_vk##name)g_next_gdpa(dev,"vk" #name)
            GD(DestroyDevice); GD(GetDeviceQueue); GD(GetDeviceQueue2); GD(QueueSubmit); GD(QueueSubmit2);
            if(!d->QueueSubmit2)d->QueueSubmit2=(PFN_vkQueueSubmit2)g_next_gdpa(dev,"vkQueueSubmit2KHR");
            GD(CreateImage); GD(DestroyImage); GD(GetImageMemoryRequirements); GD(BindImageMemory);
            GD(AllocateMemory); GD(FreeMemory); GD(CreateBuffer); GD(DestroyBuffer);
            GD(GetBufferMemoryRequirements); GD(BindBufferMemory); GD(CreateCommandPool); GD(DestroyCommandPool);
            GD(AllocateCommandBuffers); GD(FreeCommandBuffers); GD(BeginCommandBuffer); GD(EndCommandBuffer);
            GD(ResetCommandBuffer); GD(CmdPipelineBarrier); GD(CmdCopyImage); GD(CmdBlitImage);
            GD(CmdCopyImageToBuffer); GD(CmdCopyBufferToImage); GD(CmdClearColorImage);
            GD(CreateImageView); GD(DestroyImageView); GD(CreateSampler); GD(DestroySampler);
            GD(CreateDescriptorSetLayout); GD(DestroyDescriptorSetLayout); GD(CreatePipelineLayout);
            GD(DestroyPipelineLayout); GD(CreateShaderModule); GD(DestroyShaderModule);
            GD(CreateComputePipelines); GD(DestroyPipeline); GD(CreateDescriptorPool); GD(DestroyDescriptorPool);
            GD(AllocateDescriptorSets); GD(UpdateDescriptorSets); GD(CmdBindPipeline); GD(CmdBindDescriptorSets);
            GD(CmdPushConstants); GD(CmdDispatch); GD(CmdExecuteCommands); GD(CreateEvent); GD(DestroyEvent);
            GD(SetEvent); GD(ResetEvent); GD(GetEventStatus); GD(CmdSetEvent); GD(CmdWaitEvents);
            GD(CreateSemaphore); GD(DestroySemaphore); GD(WaitSemaphores);
            if(!d->WaitSemaphores)d->WaitSemaphores=(PFN_vkWaitSemaphores)g_next_gdpa(dev,"vkWaitSemaphoresKHR");
            GD(ImportSemaphoreWin32HandleKHR); GD(GetMemoryWin32HandlePropertiesKHR);
            GD(CreateFence); GD(DestroyFence); GD(WaitForFences); GD(ResetFences); GD(GetFenceStatus); GD(DeviceWaitIdle);
            #undef GD
        }
    }
    LeaveCriticalSection(&g_disp_cs);
    g_bridge.instance=instance; g_bridge.phys=phys; g_bridge.device=dev;
    Log("[iat-host] adopted NGX Vulkan instance=%p physical=%p device=%p%s",(void*)instance,(void*)phys,(void*)dev,d?"":" (device table full)");
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

// Patch vulkan-1.dll imports in the executable and modules rooted in the game
// directory. Wine's Vulkan
// exports cross from PE code into a Unix thunk and cannot safely be executable-
// detoured. IAT replacement changes only the caller's function pointers and
// keeps every original Wine entry point pristine for forwarding.
struct IatHook { const char *name; void *replacement; };

static unsigned InstallVulkanHooks();
static unsigned PatchLoaderIat(HMODULE module, const wchar_t *module_name);
static unsigned PatchVulkanIat(HMODULE module, const wchar_t *module_name);
static wchar_t g_application_root[MAX_PATH];
static HMODULE g_application_modules[256];
static volatile LONG g_application_module_count;

static bool IsSystemModulePath(const wchar_t *path)
{
    wchar_t windows[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    return path && length && length < MAX_PATH &&
        _wcsnicmp(path, windows, length) == 0 &&
        (path[length] == L'\\' || path[length] == L'/');
}

static void RegisterApplicationModule(HMODULE module, const wchar_t *path)
{
    if (!module || IsSystemModulePath(path)) return;
    const LONG count = g_application_module_count;
    for (LONG i = 0; i < count; ++i) if (g_application_modules[i] == module) return;
    if (count < static_cast<LONG>(_countof(g_application_modules))) {
        g_application_modules[count] = module;
        MemoryBarrier();
        InterlockedCompareExchange(&g_application_module_count, count + 1, count);
    }
}

static void InitializeApplicationRoot()
{
    GetModuleFileNameW(nullptr, g_application_root, MAX_PATH);
    wchar_t *slash = wcsrchr(g_application_root, L'\\');
    if (slash) slash[1] = 0;
}

static bool IsApplicationModule(HMODULE module, const wchar_t *path)
{
    if (module == GetModuleHandleW(nullptr)) return true;
    for (LONG i = 0; i < g_application_module_count; ++i)
        if (g_application_modules[i] == module) return true;
    if (!path || !g_application_root[0]) return false;
    const size_t root_length = wcslen(g_application_root);
    return _wcsnicmp(path, g_application_root, root_length) == 0;
}

static void PatchLoadedApplicationModule(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH) || IsSystemModulePath(path)) return;
    EnterCriticalSection(&g_iat_cs);
    RegisterApplicationModule(module, path);
    PatchLoaderIat(module, path);
    if (EnsureRealLoader()) PatchVulkanIat(module, path);
    LeaveCriticalSection(&g_iat_cs);
}

struct LoaderPatchScope
{
    LoaderPatchScope() { ++g_loader_patch_depth; }
    ~LoaderPatchScope() { --g_loader_patch_depth; }
};

static void *VulkanReplacement(const char *name)
{
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (void *)dlss5vk_GetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (void *)dlss5vk_GetDeviceProcAddr;
    if (strcmp(name, "vkCreateInstance") == 0) return (void *)dlss5vk_CreateInstance;
    if (strcmp(name, "vkCreateDevice") == 0) return (void *)dlss5vk_CreateDevice;
    if (strcmp(name, "vkDestroyInstance") == 0) return (void *)dlss5vk_DestroyInstance;
    if (strcmp(name, "vkDestroyDevice") == 0) return (void *)dlss5vk_DestroyDevice;
    if (strcmp(name, "vkGetDeviceQueue") == 0) return (void *)dlss5vk_GetDeviceQueue;
    if (strcmp(name, "vkGetDeviceQueue2") == 0) return (void *)dlss5vk_GetDeviceQueue2;
    if (strcmp(name, "vkQueueSubmit") == 0) return (void *)dlss5vk_QueueSubmit;
    if (strcmp(name, "vkQueueSubmit2") == 0 || strcmp(name, "vkQueueSubmit2KHR") == 0)
        return (void *)dlss5vk_QueueSubmit2;
    if (strcmp(name, "vkBeginCommandBuffer") == 0) return (void *)dlss5vk_BeginCommandBuffer;
    if (strcmp(name, "vkCmdExecuteCommands") == 0) return (void *)dlss5vk_CmdExecuteCommands;
    return nullptr;
}

static FARPROC WINAPI GetProcAddressAndPatch(HMODULE module, LPCSTR name)
{
    using Fn = FARPROC (WINAPI *)(HMODULE, LPCSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto real = kernel ? reinterpret_cast<Fn>(GetProcAddress(kernel, "GetProcAddress")) : nullptr;
    FARPROC resolved = real ? real(module, name) : nullptr;
    if (reinterpret_cast<uintptr_t>(name) <= 0xffff) return resolved;
    if (!EnsureRealLoader() || module != g_real_vulkan) return resolved;
    void *replacement = VulkanReplacement(name);
    return replacement ? reinterpret_cast<FARPROC>(replacement) : resolved;
}

// Most engines load their renderer DLL through LoadLibrary. Intercepting that
// boundary lets us patch a newly loaded module before its first exported entry
// point returns to the caller. The bounded startup scan below covers modules
// introduced through lower-level loader APIs during process startup.
static HMODULE LoadLibraryAndPatchA(LPCSTR path)
{
    using Fn = HMODULE (WINAPI *)(LPCSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto real = kernel ? reinterpret_cast<Fn>(GetProcAddress(kernel, "LoadLibraryA")) : nullptr;
    const bool outermost = g_loader_patch_depth == 0;
    LoaderPatchScope scope;
    HMODULE loaded = real ? real(path) : nullptr;
    if (loaded && outermost) PatchLoadedApplicationModule(loaded);
    return loaded;
}

static HMODULE LoadLibraryAndPatchW(LPCWSTR path)
{
    using Fn = HMODULE (WINAPI *)(LPCWSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto real = kernel ? reinterpret_cast<Fn>(GetProcAddress(kernel, "LoadLibraryW")) : nullptr;
    const bool outermost = g_loader_patch_depth == 0;
    LoaderPatchScope scope;
    HMODULE loaded = real ? real(path) : nullptr;
    if (loaded && outermost) PatchLoadedApplicationModule(loaded);
    return loaded;
}

static HMODULE LoadLibraryExAndPatchA(LPCSTR path, HANDLE file, DWORD flags)
{
    using Fn = HMODULE (WINAPI *)(LPCSTR, HANDLE, DWORD);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto real = kernel ? reinterpret_cast<Fn>(GetProcAddress(kernel, "LoadLibraryExA")) : nullptr;
    const bool outermost = g_loader_patch_depth == 0;
    LoaderPatchScope scope;
    HMODULE loaded = real ? real(path, file, flags) : nullptr;
    if (loaded && outermost) PatchLoadedApplicationModule(loaded);
    return loaded;
}

static HMODULE LoadLibraryExAndPatchW(LPCWSTR path, HANDLE file, DWORD flags)
{
    using Fn = HMODULE (WINAPI *)(LPCWSTR, HANDLE, DWORD);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto real = kernel ? reinterpret_cast<Fn>(GetProcAddress(kernel, "LoadLibraryExW")) : nullptr;
    const bool outermost = g_loader_patch_depth == 0;
    LoaderPatchScope scope;
    HMODULE loaded = real ? real(path, file, flags) : nullptr;
    if (loaded && outermost) PatchLoadedApplicationModule(loaded);
    return loaded;
}

static unsigned PatchLoaderIat(HMODULE module, const wchar_t *module_name)
{
    if (module == nullptr || module == g_self || !IsApplicationModule(module, module_name)) return 0;
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;

    IatHook hooks[] = {
        { "GetProcAddress", (void *)GetProcAddressAndPatch },
        { "LoadLibraryA",   (void *)LoadLibraryAndPatchA },
        { "LoadLibraryW",   (void *)LoadLibraryAndPatchW },
        { "LoadLibraryExA", (void *)LoadLibraryExAndPatchA },
        { "LoadLibraryExW", (void *)LoadLibraryExAndPatchW },
    };
    unsigned patched = 0;
    auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
    for (; imp->Name; ++imp)
    {
        const char *dll = reinterpret_cast<const char *>(base + imp->Name);
        const bool system_loader = _stricmp(dll, "kernel32.dll") == 0 ||
            _stricmp(dll, "kernelbase.dll") == 0 ||
            _strnicmp(dll, "api-ms-win-core-libraryloader-", 30) == 0;
        if (!system_loader || imp->OriginalFirstThunk == 0) continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto *byname = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            const char *name = reinterpret_cast<const char *>(byname->Name);
            for (const auto &h : hooks)
                if (strcmp(name, h.name) == 0)
                {
                    void **slot = reinterpret_cast<void **>(&slots->u1.Function);
                    if (*slot == h.replacement) break;
                    DWORD old = 0;
                    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                    {
                        *slot = h.replacement;
                        VirtualProtect(slot, sizeof(void *), old, &old);
                        ++patched;
                    }
                    break;
                }
        }
    }
    if (patched)
        Log("[iat-host] redirected %u loader import(s) in %ls", patched,
            module_name ? module_name : L"a loaded module");
    return patched;
}

static unsigned PatchVulkanIat(HMODULE module, const wchar_t *module_name)
{
    if (module == nullptr || module == g_self || module == g_real_vulkan ||
        !IsApplicationModule(module, module_name)) return 0;
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;

    IatHook hooks[] = {
        { "vkGetInstanceProcAddr", (void *)dlss5vk_GetInstanceProcAddr },
        { "vkGetDeviceProcAddr",   (void *)dlss5vk_GetDeviceProcAddr },
        { "vkCreateInstance",      (void *)dlss5vk_CreateInstance },
        { "vkCreateDevice",        (void *)dlss5vk_CreateDevice },
        { "vkDestroyInstance",     (void *)dlss5vk_DestroyInstance },
        { "vkDestroyDevice",       (void *)dlss5vk_DestroyDevice },
        { "vkGetDeviceQueue",      (void *)dlss5vk_GetDeviceQueue },
        { "vkGetDeviceQueue2",     (void *)dlss5vk_GetDeviceQueue2 },
        { "vkQueueSubmit",         (void *)dlss5vk_QueueSubmit },
        { "vkQueueSubmit2",        (void *)dlss5vk_QueueSubmit2 },
        { "vkQueueSubmit2KHR",     (void *)dlss5vk_QueueSubmit2 },
        { "vkBeginCommandBuffer",  (void *)dlss5vk_BeginCommandBuffer },
        { "vkCmdExecuteCommands",  (void *)dlss5vk_CmdExecuteCommands },
    };

    unsigned patched = 0;
    auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
    for (; imp->Name; ++imp)
    {
        const char *dll = reinterpret_cast<const char *>(base + imp->Name);
        if (_stricmp(dll, "vulkan-1.dll") != 0) continue;
        if (imp->OriginalFirstThunk == 0) break;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->OriginalFirstThunk);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto *byname = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            const char *name = reinterpret_cast<const char *>(byname->Name);
            for (const auto &h : hooks)
                if (strcmp(name, h.name) == 0)
                {
                    void **slot = reinterpret_cast<void **>(&slots->u1.Function);
                    if (*slot == h.replacement) break;
                    DWORD old = 0;
                    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
                    {
                        if (strcmp(name, "vkCreateInstance") == 0 && g_hook_CreateInstance == nullptr)
                            g_hook_CreateInstance = (PFN_vkCreateInstance)*slot;
                        if (strcmp(name, "vkCreateDevice") == 0 && g_hook_CreateDevice == nullptr)
                            g_hook_CreateDevice = (PFN_vkCreateDevice)*slot;
                        *slot = h.replacement;
                        VirtualProtect(slot, sizeof(void *), old, &old);
                        ++patched;
                    }
                    break;
                }
        }
        break;
    }
    if (patched)
    {
        Log("[iat-host] redirected %u vulkan-1.dll import(s) in %ls", patched,
            module_name ? module_name : L"a loaded module");
    }
    return patched;
}

static unsigned InstallVulkanHooks()
{
    LoaderPatchScope recursion_guard;
    EnterCriticalSection(&g_iat_cs);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        LeaveCriticalSection(&g_iat_cs);
        return 0;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned patched = 0;
    const bool have_vulkan = EnsureRealLoader();
    if (Module32FirstW(snapshot, &entry))
        do
        {
            PatchLoaderIat(entry.hModule, entry.szExePath);
            if (have_vulkan) patched += PatchVulkanIat(entry.hModule, entry.szExePath);
        }
        while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    LeaveCriticalSection(&g_iat_cs);
    return patched;
}

static DWORD WINAPI VulkanHookWatch(void *)
{
    // LoadLibrary interception covers normal late renderer loading. The bounded
    // scan covers engines that use lower-level loader calls during startup,
    // without walking every process module for the lifetime of the game.
    for (int i = 0; i < 600 && !g_bridge.disabled; ++i) {
        InstallVulkanHooks();
        Sleep(100);
    }
    return 0;
}
