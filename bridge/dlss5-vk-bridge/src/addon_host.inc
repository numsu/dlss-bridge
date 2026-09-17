// ReShade add-on host (the v0.2 line).
//
// The same bridge core as the Vulkan layer, hosted by ReShade instead of the
// Vulkan loader. The layer needs a registry entry, an environment variable and
// a Steam restart, and a machine-wide implicit layer is loaded into every
// Vulkan process there is. The RenoDX DLSS 5 add-on the bridge exists for is a
// ReShade add-on already, so the bridge can be one too: drop the file next to
// it and ReShade loads both. What ReShade gives us is enough for the in-frame
// handoff, which is the only mode that ever produced a visible result:
//
//   * VkDevice, every VkQueue with its capabilities, and every VkCommandBuffer
//     as it begins, executes secondaries and is submitted -- all the handles
//     the game itself holds, from ReShade's device / queue / command-list
//     events.
//   * The adapter LUID (device_properties::adapter_luid), so the D3D12 device
//     lands on the same GPU.
//   * A device created with VK_KHR_external_memory_win32 (ReShade enables it
//     when the driver has it) -- the shared textures import.
//
// What it does not give us, and how each gap is closed:
//
//   * No VkInstance / VkPhysicalDevice. ReShade's are layer-internal handles
//     that vulkan-1.dll's trampolines would not accept, and an add-on has no
//     public way to the game's own. Memory properties -- the one thing the
//     core reads off a physical device -- come from a private, one-call
//     VkInstance created with every implicit layer disabled (AddonProbeMemory),
//     and the imports additionally ask the driver which memory types the
//     handle itself allows (vk_interop.inc).
//   * No VK_KHR_external_semaphore_win32: the D3D12 fences cannot import as
//     timeline semaphores. The in-frame handoff uses CPU fence waits, so the
//     missing import does not alter this host's synchronization contract.
//   * ReShade's execute_command_list fires BEFORE the submit goes down, so
//     nothing can be queued behind the game's work: a slot retires through the
//     ev_done event at the tail of its sandwich instead of a retire fence.
//   * ReShade loads add-ons at the first vkCreateInstance and unloads them
//     when the last instance is destroyed. A game that creates and destroys
//     instances back to back could unload a module whose NGX patches are live,
//     so explicit initialization pins the module and AddonInit / AddonUninit
//     are idempotent: each load re-registers with ReShade and each unload only
//     unregisters.
//
// Same rule as the layer, harder here because ReShade's own hooks sit
// between us and the driver: never be the reason a game call fails. The
// add-on submits nothing of its own, creates nothing before the first bridged
// evaluate, and stands down entirely when the layer build is also present.

#include <reshade.hpp>

// ---- host traits the core consults ----------------------------------------
static const bool kHostSubmitsBehindGame = false;  // execute_command_list precedes the submit

// The core's own submits set this around vkQueueSubmit so a host hook can
// tell them from the game's. This host never submits, but the core references
// the flag.
static __declspec(thread) int g_internal_submit;

// Forward declarations into vk_interop.inc and the main file.
static bool     BridgeWantsSubmitMatch();
static uint32_t BridgeMatchOne(VkCommandBuffer cb);
static void     BridgeOnGameSubmitDone(VkQueue queue, uint32_t family, bool tracked, uint32_t slots, bool ok);
static void     BridgeNoteUnmatchedSubmit(uint32_t family, bool tracked);
static void     BridgeOnCommandBufferBegin(VkCommandBuffer cmd);
static void     BridgeOnExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer *secondaries);
static void     BridgeDeviceLost(VkDevice dev);
static DWORD WINAPI NgxWatch(void *);

#include "vk_dispatch.inc"

// ---- what ReShade told us --------------------------------------------------
struct AddonDevice
{
    VkDevice                          device;       // VK_NULL_HANDLE = free slot
    DeviceDispatch                    disp;
    bool                              disp_ready;
    uint8_t                           luid[VK_LUID_SIZE];
    bool                              luid_valid;
    uint32_t                          vendor_id;
    uint32_t                          device_id;
    char                              name[256];
    VkPhysicalDeviceMemoryProperties  mem;
    bool                              mem_ready;
    bool                              mem_tried;
};
struct AddonQueue { VkQueue queue; VkDevice device; uint32_t flags; };

// Slots are recycled as in the layer: engines create and destroy throwaway
// devices, and the counts are high-water marks.
static AddonDevice      g_adev[8];    static volatile LONG g_adev_count;
static AddonQueue       g_aqueue[64]; static volatile LONG g_aqueue_count;
static CRITICAL_SECTION g_disp_cs;    // initialized explicitly before AddonInit continues

static bool   g_addon_registered;     // this load registered with ReShade
static bool   g_addon_active;         // events wired; the bridge may run
static bool   g_addon_stood_down;     // another bridge is in this process
static HANDLE g_addon_singleton;

static uint32_t AddonQueueFlags(VkQueue q)
{
    uint32_t flags = 0;
    EnterCriticalSection(&g_disp_cs);
    for (LONG i = 0; i < g_aqueue_count; ++i)
        if (g_aqueue[i].queue == q) { flags = g_aqueue[i].flags; break; }
    LeaveCriticalSection(&g_disp_cs);
    return flags;
}

// ---- device entry points ---------------------------------------------------
// Resolved through vulkan-1.dll's exported vkGetDeviceProcAddr, which returns
// the top of the game's own layer chain for the device -- the same functions
// the game calls. Never from inside a ReShade event: init_device fires while
// the loader is still inside vkCreateDevice and the device's dispatch table is
// not filled in yet, so this runs lazily from the first bridged evaluate.
static PFN_vkGetDeviceProcAddr AddonLoaderGdpa()
{
    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    return vk ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vk, "vkGetDeviceProcAddr")) : nullptr;
}

static bool AddonResolveDispatch(AddonDevice &ad)
{
    if (ad.disp_ready) return true;
    PFN_vkGetDeviceProcAddr gdpa = AddonLoaderGdpa();
    if (gdpa == nullptr)
    { Log("[addon] vulkan-1.dll is not loaded in this process; cannot resolve the device entry points"); return false; }

    DeviceDispatch *d = &ad.disp;
    memset(d, 0, sizeof *d);
    d->device = ad.device;
    d->phys   = VK_NULL_HANDLE;
    d->GetDeviceProcAddr = gdpa;
    VkDevice dev = ad.device;
    #define GD(name) d->name = (PFN_vk##name)gdpa(dev, "vk" #name)
    GD(DestroyDevice); GD(GetDeviceQueue); GD(GetDeviceQueue2);
    GD(QueueSubmit); GD(QueueSubmit2); GD(QueuePresentKHR);
    if (d->QueueSubmit2 == nullptr)
        d->QueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(dev, "vkQueueSubmit2KHR");
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
    if (d->WaitSemaphores == nullptr)
        d->WaitSemaphores = (PFN_vkWaitSemaphores)gdpa(dev, "vkWaitSemaphoresKHR");
    GD(ImportSemaphoreWin32HandleKHR);
    GD(GetMemoryWin32HandlePropertiesKHR);
    GD(GetMemoryHostPointerPropertiesEXT);
    GD(CreateFence); GD(DestroyFence); GD(WaitForFences); GD(ResetFences); GD(GetFenceStatus);
    GD(DeviceWaitIdle);
    #undef GD

    // What the in-frame handoff cannot do without.
    struct { const void *fn; const char *name; } must[] = {
        { (const void *)d->CreateImage,            "vkCreateImage" },
        { (const void *)d->GetImageMemoryRequirements, "vkGetImageMemoryRequirements" },
        { (const void *)d->AllocateMemory,         "vkAllocateMemory" },
        { (const void *)d->BindImageMemory,        "vkBindImageMemory" },
        { (const void *)d->CreateBuffer,           "vkCreateBuffer" },
        { (const void *)d->CreateImageView,        "vkCreateImageView" },
        { (const void *)d->CreateEvent,            "vkCreateEvent" },
        { (const void *)d->SetEvent,               "vkSetEvent" },
        { (const void *)d->ResetEvent,             "vkResetEvent" },
        { (const void *)d->GetEventStatus,         "vkGetEventStatus" },
        { (const void *)d->CmdSetEvent,            "vkCmdSetEvent" },
        { (const void *)d->CmdWaitEvents,          "vkCmdWaitEvents" },
        { (const void *)d->CmdPipelineBarrier,     "vkCmdPipelineBarrier" },
        { (const void *)d->CmdCopyImage,           "vkCmdCopyImage" },
        { (const void *)d->CmdBlitImage,           "vkCmdBlitImage" },
        { (const void *)d->CmdCopyImageToBuffer,   "vkCmdCopyImageToBuffer" },
        { (const void *)d->CmdCopyBufferToImage,   "vkCmdCopyBufferToImage" },
        { (const void *)d->CreateComputePipelines, "vkCreateComputePipelines" },
        { (const void *)d->CmdDispatch,            "vkCmdDispatch" },
        { (const void *)d->DeviceWaitIdle,         "vkDeviceWaitIdle" },
    };
    for (const auto &m : must)
        if (m.fn == nullptr)
        { Log("[addon] the loader has no %s for the device; the bridge cannot run here", m.name); return false; }
    if (d->GetMemoryWin32HandlePropertiesKHR == nullptr)
        Log("[addon] vkGetMemoryWin32HandlePropertiesKHR is not available: the device was created without "
            "VK_KHR_external_memory_win32 (ReShade enables it when the driver has it); the shared-texture "
            "import will most likely fail");
    ad.disp_ready = true;
    return true;
}

// ---- memory properties through a private instance ---------------------------
// The only physical-device query the core needs. The instance is ours alone,
// lives for this one function and, on loaders that honour
// VK_LOADER_LAYERS_DISABLE (1.3.234+), is created with every implicit layer
// -- ReShade included -- left out, so nothing else in the process sees it.
// Runs on the game's thread from the evaluate hook, never inside a loader call
// (the loader holds a non-recursive global lock through vkCreateInstance and
// vkCreateDevice, so a nested vkCreateInstance from a ReShade event would
// deadlock).
static bool AddonProbeMemory(AddonDevice &ad)
{
    if (ad.mem_ready || ad.mem_tried) return ad.mem_ready;
    ad.mem_tried = true;

    HMODULE vk = GetModuleHandleW(L"vulkan-1.dll");
    auto gipa = vk ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr")) : nullptr;
    auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    if (create == nullptr) { Log("[addon] probe instance: no vkCreateInstance in vulkan-1.dll"); return false; }

    static const wchar_t *kVar = L"VK_LOADER_LAYERS_DISABLE";
    wchar_t *saved = nullptr;
    if (DWORD need = GetEnvironmentVariableW(kVar, nullptr, 0))
    {
        saved = new wchar_t[need];
        GetEnvironmentVariableW(kVar, saved, need);
    }
    SetEnvironmentVariableW(kVar, L"~implicit~");

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "dlss5-vk-bridge memory-properties probe";
    app.apiVersion       = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    VkInstance in = VK_NULL_HANDLE;
    VkResult r = create(&ci, nullptr, &in);
    if (r != VK_SUCCESS) { app.apiVersion = VK_API_VERSION_1_0; r = create(&ci, nullptr, &in); }

    SetEnvironmentVariableW(kVar, saved);   // nullptr deletes it again
    delete[] saved;
    if (r != VK_SUCCESS || in == VK_NULL_HANDLE)
    { Log("[addon] probe instance: vkCreateInstance failed (%d); memory types will follow the requirement bits alone", (int)r); return false; }

    auto destroy   = reinterpret_cast<PFN_vkDestroyInstance>(gipa(in, "vkDestroyInstance"));
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(in, "vkEnumeratePhysicalDevices"));
    auto props     = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(in, "vkGetPhysicalDeviceProperties"));
    auto props2    = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(in, "vkGetPhysicalDeviceProperties2"));
    auto memprops  = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(in, "vkGetPhysicalDeviceMemoryProperties"));

    VkPhysicalDevice pds[16];
    uint32_t n = 16;
    if (enumerate == nullptr || props == nullptr || memprops == nullptr ||
        (enumerate(in, &n, pds) < VK_SUCCESS)) n = 0;

    int pick = -1;
    const char *how = "";
    if (ad.luid_valid && props2 != nullptr)
        for (uint32_t i = 0; i < n && pick < 0; ++i)
        {
            VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
            VkPhysicalDeviceProperties2 pp  = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            pp.pNext = &idp;
            props2(pds[i], &pp);
            if (idp.deviceLUIDValid && memcmp(idp.deviceLUID, ad.luid, VK_LUID_SIZE) == 0) { pick = (int)i; how = "LUID"; }
        }
    for (uint32_t i = 0; i < n && pick < 0; ++i)
    {
        VkPhysicalDeviceProperties pp = {};
        props(pds[i], &pp);
        if (pp.vendorID == ad.vendor_id && pp.deviceID == ad.device_id) { pick = (int)i; how = "vendor/device id"; }
    }
    if (pick < 0 && n == 1) { pick = 0; how = "being the only physical device"; }

    if (pick >= 0)
    {
        memprops(pds[pick], &ad.mem);
        ad.mem_ready = ad.mem.memoryTypeCount > 0;
        Log("[addon] memory properties read through a private instance: physical device %d of %u, matched by %s "
            "(%u memory types, %u heaps)", pick, n, how, ad.mem.memoryTypeCount, ad.mem.memoryHeapCount);
    }
    else
        Log("[addon] probe instance: none of %u physical device(s) matched the game's adapter; memory types "
            "will follow the requirement bits alone", n);
    if (destroy) destroy(in, nullptr);
    return ad.mem_ready;
}

// ---- the device the core runs on --------------------------------------------
// Picked by the dispatch key of the command buffer NGX handed us (the loader
// writes the same dispatch pointer into a device and everything created from
// it), falling back to the most recent live device.
static AddonDevice *AddonAcquireDevice(VkCommandBuffer cmd)
{
    if (!g_addon_active)
    {
        static bool said = false;
        if (!said) { said = true; Log("[addon] an NGX evaluate arrived while the add-on is not wired to ReShade"); }
        return nullptr;
    }
    AddonDevice *ad = nullptr;
    EnterCriticalSection(&g_disp_cs);
    const void *key = cmd != VK_NULL_HANDLE ? *reinterpret_cast<void *const *>(cmd) : nullptr;
    for (LONG i = 0; i < g_adev_count; ++i)
    {
        if (g_adev[i].device == VK_NULL_HANDLE) continue;
        if (key != nullptr && *reinterpret_cast<void *const *>(g_adev[i].device) == key) { ad = &g_adev[i]; break; }
        ad = &g_adev[i];
    }
    LeaveCriticalSection(&g_disp_cs);
    if (ad == nullptr)
    {
        static bool said = false;
        if (!said) { said = true; Log("[addon] ReShade has not reported a Vulkan device yet"); }
        return nullptr;
    }
    if (!AddonResolveDispatch(*ad)) return nullptr;
    AddonProbeMemory(*ad);   // best effort; logged either way
    return ad;
}

// ---- ReShade events --------------------------------------------------------
static void AddonForgetQueues(VkDevice dev)
{
    for (LONG i = 0; i < g_aqueue_count; ++i)
        if (g_aqueue[i].device == dev) g_aqueue[i] = AddonQueue{};
}

static void OnInitDevice(reshade::api::device *device)
{
    if (device == nullptr || device->get_api() != reshade::api::device_api::vulkan) return;
    const VkDevice dev = reinterpret_cast<VkDevice>(device->get_native());

    AddonDevice snapshot = {};
    snapshot.device = dev;
    uint64_t luid = 0;
    if (device->get_property(reshade::api::device_properties::adapter_luid, &luid) && luid != 0)
    {
        memcpy(snapshot.luid, &luid, VK_LUID_SIZE);
        snapshot.luid_valid = true;
    }
    device->get_property(reshade::api::device_properties::vendor_id, &snapshot.vendor_id);
    device->get_property(reshade::api::device_properties::device_id, &snapshot.device_id);
    if (!device->get_property(reshade::api::device_properties::description, snapshot.name))
        snapshot.name[0] = 0;
    snapshot.name[sizeof snapshot.name - 1] = 0;

    EnterCriticalSection(&g_disp_cs);
    AddonDevice *slot = nullptr;
    for (LONG i = 0; i < g_adev_count && slot == nullptr; ++i)
        if (g_adev[i].device == dev) slot = &g_adev[i];
    for (LONG i = 0; i < g_adev_count && slot == nullptr; ++i)
        if (g_adev[i].device == VK_NULL_HANDLE) slot = &g_adev[i];
    if (slot == nullptr && g_adev_count < (LONG)(sizeof g_adev / sizeof g_adev[0]))
        slot = &g_adev[g_adev_count++];
    if (slot) *slot = snapshot;
    LeaveCriticalSection(&g_disp_cs);

    if (slot == nullptr) { Log("[addon] device table full; Vulkan device %p goes untracked", (void *)dev); return; }
    Log("[addon] ReShade reports Vulkan device %p: %s (vendor 0x%04x, device 0x%04x, LUID %s)",
        (void *)dev, snapshot.name[0] ? snapshot.name : "unnamed adapter",
        snapshot.vendor_id, snapshot.device_id, snapshot.luid_valid ? "known" : "unknown");
}

static void OnDestroyDevice(reshade::api::device *device)
{
    if (device == nullptr || device->get_api() != reshade::api::device_api::vulkan) return;
    const VkDevice dev = reinterpret_cast<VkDevice>(device->get_native());
    BridgeDeviceLost(dev);     // releases everything the core built on it (fires before the device is destroyed)
    EnterCriticalSection(&g_disp_cs);
    for (LONG i = 0; i < g_adev_count; ++i)
        if (g_adev[i].device == dev) { g_adev[i] = AddonDevice{}; }
    AddonForgetQueues(dev);
    LeaveCriticalSection(&g_disp_cs);
    Log("[addon] Vulkan device %p destroyed", (void *)dev);
}

static void OnInitCommandQueue(reshade::api::command_queue *queue)
{
    if (queue == nullptr) return;
    reshade::api::device *device = queue->get_device();
    if (device == nullptr || device->get_api() != reshade::api::device_api::vulkan) return;
    AddonQueue rec = {};
    rec.queue  = reinterpret_cast<VkQueue>(queue->get_native());
    rec.device = reinterpret_cast<VkDevice>(device->get_native());
    // command_queue_type's graphics / compute / copy bits are VK_QUEUE_GRAPHICS_BIT /
    // COMPUTE_BIT / TRANSFER_BIT -- ReShade reports the family's queueFlags.
    rec.flags  = static_cast<uint32_t>(queue->get_type());

    EnterCriticalSection(&g_disp_cs);
    AddonQueue *slot = nullptr;
    for (LONG i = 0; i < g_aqueue_count && slot == nullptr; ++i)
        if (g_aqueue[i].queue == rec.queue) slot = &g_aqueue[i];
    for (LONG i = 0; i < g_aqueue_count && slot == nullptr; ++i)
        if (g_aqueue[i].queue == VK_NULL_HANDLE) slot = &g_aqueue[i];
    if (slot == nullptr && g_aqueue_count < (LONG)(sizeof g_aqueue / sizeof g_aqueue[0]))
        slot = &g_aqueue[g_aqueue_count++];
    if (slot) *slot = rec;
    LeaveCriticalSection(&g_disp_cs);
    if (g_cfg.verbose)
        Log("[addon] queue %p on device %p: %s%s%s", (void *)rec.queue, (void *)rec.device,
            rec.flags & VK_QUEUE_GRAPHICS_BIT ? "graphics " : "",
            rec.flags & VK_QUEUE_COMPUTE_BIT  ? "compute "  : "",
            rec.flags & VK_QUEUE_TRANSFER_BIT ? "transfer"  : "");
}

static void OnDestroyCommandQueue(reshade::api::command_queue *queue)
{
    if (queue == nullptr) return;
    const VkQueue q = reinterpret_cast<VkQueue>(queue->get_native());
    EnterCriticalSection(&g_disp_cs);
    for (LONG i = 0; i < g_aqueue_count; ++i)
        if (g_aqueue[i].queue == q) g_aqueue[i] = AddonQueue{};
    LeaveCriticalSection(&g_disp_cs);
}

// vkBeginCommandBuffer: a buffer being recorded afresh no longer carries a
// sandwich that was never submitted.
static void OnResetCommandList(reshade::api::command_list *cmd_list)
{
    if (cmd_list == nullptr) return;
    BridgeOnCommandBufferBegin(reinterpret_cast<VkCommandBuffer>(cmd_list->get_native()));
}

// vkCmdExecuteCommands, one secondary at a time.
static void OnExecuteSecondaryCommandList(reshade::api::command_list *cmd_list, reshade::api::command_list *secondary)
{
    if (cmd_list == nullptr || secondary == nullptr) return;
    const VkCommandBuffer s = reinterpret_cast<VkCommandBuffer>(secondary->get_native());
    BridgeOnExecuteCommands(reinterpret_cast<VkCommandBuffer>(cmd_list->get_native()), 1, &s);
}

// vkQueueSubmit / vkQueueSubmit2, one command buffer at a time and before the
// submit goes down. The claim marks the slot so it cannot be recycled under
// its sandwich; the queue's capabilities settle the family probe (blit or
// not); and with kHostSubmitsBehindGame false the core queues nothing here.
static void OnExecuteCommandList(reshade::api::command_queue *queue, reshade::api::command_list *cmd_list)
{
    if (queue == nullptr || cmd_list == nullptr || !BridgeWantsSubmitMatch()) return;
    const VkQueue q = reinterpret_cast<VkQueue>(queue->get_native());
    const uint32_t slots = BridgeMatchOne(reinterpret_cast<VkCommandBuffer>(cmd_list->get_native()));
    if (slots) BridgeOnGameSubmitDone(q, kUnknownFamily, true, slots, true);
    else       BridgeNoteUnmatchedSubmit(kUnknownFamily, true);
}

// ---- configuration from ReShade.ini ----------------------------------------
// [DLSS5_VK_BRIDGE] in ReShade.ini overrides dlss5-vk-bridge.cfg key by key,
// so a ReShade user never has to find the cfg file.
static void AddonApplyIniOverrides()
{
    struct { const char *key; int *dst; } keys[] = {
        { "verbose", &g_cfg.verbose },
    };
    for (const auto &k : keys)
    {
        int v = 0;
        if (!reshade::get_config_value(nullptr, "DLSS5_VK_BRIDGE", k.key, v)) continue;
        if (*k.dst != v) Log("[addon] ReShade.ini [DLSS5_VK_BRIDGE] %s=%d overrides the cfg file", k.key, v);
        *k.dst = v;
    }
}

// ---- ReShade entry points --------------------------------------------------
extern "C" __declspec(dllexport) const char *NAME        = "dlss5-vk-bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Lets a D3D12-only DLSS 5 add-on run the Super Resolution pass of a Vulkan game: "
    "a private D3D12 NGX session fed from the game's Vulkan frame through shared textures.";
extern "C" __declspec(dllexport) const char *WEBSITE     = "https://github.com/numsu/dlss-bridge";
extern "C" __declspec(dllexport) const char *ISSUES      = "https://github.com/numsu/dlss-bridge/issues";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    if (!EnsureRuntimeInitialized()) return false;
    if (!reshade::register_addon(addon_module, reshade_module))
    {
        Log("[addon] ReShade refused the registration: this build targets add-on API %u, which needs "
            "ReShade 6.6 or newer with add-on support", (unsigned)RESHADE_API_VERSION);
        return false;
    }
    g_addon_registered = true;

    // One bridge per process. The layer build patches the same NGX exports;
    // two bridges would fight over them and over the shared textures.
    if (!g_addon_stood_down && GetModuleHandleW(L"dlss5-vk-bridge.dll") != nullptr)
    {
        g_addon_stood_down = true;
        Log("[addon] the dlss5-vk-bridge Vulkan layer is also loaded in this process; the add-on stands "
            "down so only one bridge runs. Unregister the layer (see README) to use the add-on.");
    }
    if (!g_addon_stood_down && g_addon_singleton == nullptr)
    {
        wchar_t name[96];
        swprintf_s(name, L"Local\\dlss5-vk-bridge.addon.%lu", GetCurrentProcessId());
        g_addon_singleton = CreateEventW(nullptr, TRUE, FALSE, name);
        if (g_addon_singleton != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            CloseHandle(g_addon_singleton);
            g_addon_singleton = nullptr;
            g_addon_stood_down = true;
            Log("[addon] another copy of the dlss5-vk-bridge add-on is already loaded in this process; "
                "this one stands down");
        }
    }
    if (g_addon_stood_down)
    {
        reshade::log::message(reshade::log::level::warning,
            "dlss5-vk-bridge: another bridge build is loaded in this process; this add-on stands down "
            "(details in dlss5-vk-bridge.log next to the add-on)");
        return true;
    }

    AddonApplyIniOverrides();
    reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
    reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
    reshade::register_event<reshade::addon_event::init_command_queue>(OnInitCommandQueue);
    reshade::register_event<reshade::addon_event::destroy_command_queue>(OnDestroyCommandQueue);
    reshade::register_event<reshade::addon_event::reset_command_list>(OnResetCommandList);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(OnExecuteSecondaryCommandList);
    reshade::register_event<reshade::addon_event::execute_command_list>(OnExecuteCommandList);
    g_addon_active = true;

    static bool watch_started = false;
    if (!watch_started)
    {
        watch_started = true;
        Log("[addon] registered with ReShade (add-on API %u); starting the NGX Vulkan evaluate/create hooks",
            (unsigned)RESHADE_API_VERSION);
        CreateThread(nullptr, 0, NgxWatch, nullptr, 0, nullptr);
    }
    else
        Log("[addon] re-registered with ReShade for a new Vulkan instance");
    reshade::log::message(reshade::log::level::info,
        "dlss5-vk-bridge " DLSS5VK_VERSION_STRING " attached (ReShade add-on host); "
        "detail in dlss5-vk-bridge.log next to the add-on");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    // The last VkInstance is gone. ReShade will FreeLibrary us next, which the
    // pin turns into a no-op; the NGX hooks and the watcher stay, and the next
    // vkCreateInstance calls AddonInit again.
    if (g_addon_registered) reshade::unregister_addon(addon_module, reshade_module);   // drops our events too
    g_addon_registered = false;
    g_addon_active     = false;
    EnterCriticalSection(&g_disp_cs);
    for (LONG i = 0; i < g_aqueue_count; ++i) g_aqueue[i] = AddonQueue{};
    LeaveCriticalSection(&g_disp_cs);
    Log("[addon] ReShade unloaded the add-on (last Vulkan instance destroyed); the module stays resident "
        "and re-attaches at the next instance");
}
