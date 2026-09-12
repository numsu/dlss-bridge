// The Vulkan half of the bridge: importing the D3D12 shared textures and fences
// onto the game's device, moving pixels across the API boundary, and driving
// the cross-API handoff at vkQueueSubmit time.
//
// Design in one paragraph. The game's NGX Vulkan evaluate is handed an open
// VkCommandBuffer. We forward the game's evaluate untouched, then record the
// input copies (game Color/Depth/MV -> the shared textures) straight into that
// same command buffer, so they run as part of the game's own submit. When the
// game submits, the vkQueueSubmit hook does the rest on the same queue: a
// signal-only submit raises fence_in, the D3D12 evaluate waits on it and
// signals fence_out (this is where a DLSS 5 add-on inserts its pass), and a
// final copy-back submit waits fence_out and writes the shared Output into the
// game's own Output image. Two D3D12 fences imported as timeline semaphores
// (sem_in / sem_out) carry the ordering across the API boundary.
//
// Layouts: DLSS for Vulkan requires the resources it is handed to be in
// VK_IMAGE_LAYOUT_GENERAL, so the input copies transition the game images
// GENERAL -> TRANSFER_SRC and back, and (because the copy-back submit is issued
// before the game submits anything else) the game Output is still GENERAL when
// the copy-back runs. The shared textures use ALLOW_SIMULTANEOUS_ACCESS on the
// D3D12 side, so no cross-API image state transitions are needed -- the two
// timeline semaphores are the whole synchronisation contract.

// ---- cached dispatch / device state ---------------------------------------
static DeviceDispatch               *g_disp;        // game device dispatch
static VkPhysicalDeviceMemoryProperties g_mem_props;

static UINT umin(UINT a, UINT b) { return a < b ? a : b; }

#ifndef DLSS5VK_ADDON_HOST
static InstanceDispatch *BridgeInstance()
{
    if (InstanceDispatch *in = DispatchForInstance(g_bridge.instance)) return in;
    for (LONG i = 0; i < g_inst_count; ++i)     // slots are recycled: skip the free ones
        if (g_inst[i].instance != VK_NULL_HANDLE) return &g_inst[i];
    return nullptr;
}
#endif

// The game may hand NGX a different physical image each frame (double buffering),
// so the game-side handles are read fresh every frame; only the shared textures
// on the D3D12 side persist.
struct GameRes
{
    VkImage            image;
    VkImageView        view;    // the game's own view -- sampleable whatever the format
    VkFormat           fmt;
    VkImageAspectFlags aspect;
    uint32_t           mip, layer, w, h;
};

static NVSDK_NGX_Resource_VK *GetVkRes(const NVSDK_NGX_Parameter *p, const char *key)
{
    void *v = nullptr;
    if (p == nullptr || p->Get(key, &v) != NGX_SUCCESS) return nullptr;
    return reinterpret_cast<NVSDK_NGX_Resource_VK *>(v);
}

static bool ReadGameRes(const NVSDK_NGX_Parameter *p, const char *key, GameRes *r)
{
    NVSDK_NGX_Resource_VK *res = GetVkRes(p, key);
    if (res == nullptr || res->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW) return false;
    NVSDK_NGX_ImageViewInfo_VK &iv = res->Resource.ImageViewInfo;
    if (iv.Image == VK_NULL_HANDLE) return false;
    r->image  = iv.Image;
    r->view   = iv.ImageView;
    r->fmt    = iv.Format;
    // The aspect the game reports is whatever its own wrapper stored, not
    // necessarily one Vulkan would accept for the image: idTech 8 hands its
    // D24S8 depth over as a COLOR-aspect view. Where the format settles it,
    // take the aspect from the format, so depth still reaches DLSS.
    VkImageAspectFlags aspect = iv.SubresourceRange.aspectMask;
    switch (iv.Format)
    {
    case VK_FORMAT_D16_UNORM:         case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT:   case VK_FORMAT_D32_SFLOAT_S8_UINT:
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
    default:
        if (aspect == 0) aspect = VK_IMAGE_ASPECT_COLOR_BIT; break;
    }
    r->aspect = aspect;
    // A view whose reported aspect had to be corrected is not trusted to be
    // depth-only; the bridge makes its own (see GetSampleableDepthView).
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT && iv.SubresourceRange.aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT)
        r->view = VK_NULL_HANDLE;
    r->mip    = iv.SubresourceRange.baseMipLevel;
    r->layer  = iv.SubresourceRange.baseArrayLayer;
    r->w      = iv.Width;
    r->h      = iv.Height;
    return true;
}

// ---- format mapping (Vulkan alias <-> DXGI on the D3D12 side) --------------
struct FmtPair { VkFormat vk; DXGI_FORMAT dxgi; };

// Colour / output candidates. The Vulkan alias keeps the same texel block size
// as the game's format, so the input copy is a size-compatible vkCmdCopyImage;
// only an unlisted format falls back to a blit into RGBA16F.
static const FmtPair kColorFmts[] = {
    { VK_FORMAT_R16G16B16A16_SFLOAT,      DXGI_FORMAT_R16G16B16A16_FLOAT },
    { VK_FORMAT_R16G16B16A16_UNORM,       DXGI_FORMAT_R16G16B16A16_UNORM },
    { VK_FORMAT_R32G32B32A32_SFLOAT,      DXGI_FORMAT_R32G32B32A32_FLOAT },
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, DXGI_FORMAT_R10G10B10A2_UNORM  },
    { VK_FORMAT_B10G11R11_UFLOAT_PACK32,  DXGI_FORMAT_R11G11B10_FLOAT     },
    { VK_FORMAT_R8G8B8A8_UNORM,           DXGI_FORMAT_R8G8B8A8_UNORM      },
    { VK_FORMAT_A8B8G8R8_UNORM_PACK32,    DXGI_FORMAT_R8G8B8A8_UNORM      },
    { VK_FORMAT_B8G8R8A8_UNORM,           DXGI_FORMAT_B8G8R8A8_UNORM      },
};

static void MapColor(VkFormat game, VkFormat *aliasVk, DXGI_FORMAT *dxgi, bool *needBlit)
{
    for (const FmtPair &f : kColorFmts)
        if (f.vk == game) { *aliasVk = f.vk; *dxgi = f.dxgi; *needBlit = false; return; }
    *aliasVk = VK_FORMAT_R16G16B16A16_SFLOAT;
    *dxgi    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    *needBlit = true;   // convert an unknown colour format into RGBA16F
}

// Motion-vector candidates. DLSS reads .xy of any two- or four-channel float
// format, so the alias mirrors the game's texel size and the input stays a
// vkCmdCopyImage. idTech 8 hands over RGBA16F (issue #3): the fixed RG16F
// alias of earlier versions forced a blit there, which its compute-only DLSS
// queue cannot run.
static const FmtPair kMvFmts[] = {
    { VK_FORMAT_R16G16_SFLOAT,       DXGI_FORMAT_R16G16_FLOAT       },
    { VK_FORMAT_R32G32_SFLOAT,       DXGI_FORMAT_R32G32_FLOAT       },
    { VK_FORMAT_R16G16B16A16_SFLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT },
    { VK_FORMAT_R32G32B32A32_SFLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT },
};

static void MapMotionVectors(VkFormat game, VkFormat *aliasVk, DXGI_FORMAT *dxgi, bool *needBlit)
{
    for (const FmtPair &f : kMvFmts)
        if (f.vk == game) { *aliasVk = f.vk; *dxgi = f.dxgi; *needBlit = false; return; }
    *aliasVk = VK_FORMAT_R16G16_SFLOAT;
    *dxgi    = DXGI_FORMAT_R16G16_FLOAT;
    *needBlit = true;   // convert an unknown motion-vector format into RG16F
}

static bool IsFloatDepth(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// ---- import helpers --------------------------------------------------------
static uint32_t PickMemType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g_mem_props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_mem_props.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < g_mem_props.memoryTypeCount; ++i)
        if (bits & (1u << i)) return i;
    // No memory properties at all (a host that could not read them): the
    // lowest permitted type is the driver's preferred one for the resource.
    for (uint32_t i = 0; i < 32; ++i)
        if (bits & (1u << i)) return i;
    return 0;
}

static bool VkImportSharedTex(int slot)
{
    SharedTex &t = g_bridge.tex[slot];
    VkDevice dev = g_bridge.device;

    VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.pNext         = &ext;
    ic.imageType     = VK_IMAGE_TYPE_2D;
    ic.format        = t.fmtvk;
    ic.extent        = { t.w, t.h, 1 };
    ic.mipLevels     = 1;
    ic.arrayLayers   = 1;
    ic.samples       = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ic.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (slot == SLOT_COLOR || slot == SLOT_OUTPUT || slot == SLOT_DEPTH)
        ic.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (g_disp->CreateImage(dev, &ic, nullptr, &t.image) != VK_SUCCESS)
    { Log("[vk] %s: vkCreateImage(alias) failed", kSlotName[slot]); return false; }

    VkMemoryRequirements req = {};
    g_disp->GetImageMemoryRequirements(dev, t.image, &req);
    uint32_t type_bits = req.memoryTypeBits;
#ifdef DLSS5VK_ADDON_HOST
    // The driver also says which memory types this particular handle may be
    // imported as; the intersection with the image's own requirements is what
    // the spec asks an import to allocate from.
    if (g_disp->GetMemoryWin32HandlePropertiesKHR)
    {
        VkMemoryWin32HandlePropertiesKHR hp = { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        if (g_disp->GetMemoryWin32HandlePropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                                      t.nt_handle, &hp) == VK_SUCCESS &&
            (type_bits & hp.memoryTypeBits))
            type_bits &= hp.memoryTypeBits;
    }
#endif

    VkImportMemoryWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    imp.handle     = t.nt_handle;

    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.image = t.image;
    ded.pNext = &imp;

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext           = &ded;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = PickMemType(type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (g_disp->AllocateMemory(dev, &mai, nullptr, &t.memory) != VK_SUCCESS)
    { Log("[vk] %s: import vkAllocateMemory failed", kSlotName[slot]); return false; }
    if (g_disp->BindImageMemory(dev, t.image, t.memory, 0) != VK_SUCCESS)
    { Log("[vk] %s: vkBindImageMemory failed", kSlotName[slot]); return false; }

    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    Log("[vk] %-6s imported as VkImage %p (%ux%u)", kSlotName[slot], (void *)t.image, t.w, t.h);
    return true;
}

static bool VkImportCrossBuffer(int direction)
{
    const bool upload = direction == CROSS_UPLOAD;
    bool &active = upload ? g_mgpu.direct_vulkan_upload : g_mgpu.direct_vulkan_download;
    active = false;
    if (!g_mgpu.active || !g_mgpu.host_memory[direction] ||
        !g_disp->GetMemoryHostPointerPropertiesEXT)
    {
        Log("[vk] direct cross-buffer %s unavailable; retaining the D3D12 game-adapter copy",
            upload ? "input" : "output");
        return false;
    }

    VkExternalMemoryBufferCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.pNext = &ext;
    bci.size = g_mgpu.host_bytes;
    bci.usage = upload ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_disp->CreateBuffer(g_bridge.device, &bci, nullptr, &g_mgpu.cross_vk[direction]) != VK_SUCCESS)
    {
        Log("[vk] direct %s cross-buffer vkCreateBuffer failed; retaining the D3D12 copy",
            upload ? "input" : "output");
        return false;
    }

    VkMemoryRequirements req = {};
    g_disp->GetBufferMemoryRequirements(g_bridge.device, g_mgpu.cross_vk[direction], &req);
    VkMemoryHostPointerPropertiesEXT hp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    if (g_disp->GetMemoryHostPointerPropertiesEXT(
            g_bridge.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            g_mgpu.host_memory[direction], &hp) != VK_SUCCESS ||
        !(hp.memoryTypeBits & req.memoryTypeBits))
    {
        Log("[vk] %s host pointer is not importable; retaining the D3D12 copy",
            upload ? "input" : "output");
        return false;
    }

    VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = g_mgpu.host_memory[direction];
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &imp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = PickMemType(hp.memoryTypeBits & req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (g_disp->AllocateMemory(g_bridge.device, &mai, nullptr,
                               &g_mgpu.cross_vk_memory[direction]) != VK_SUCCESS ||
        g_disp->BindBufferMemory(g_bridge.device, g_mgpu.cross_vk[direction],
                                 g_mgpu.cross_vk_memory[direction], 0) != VK_SUCCESS)
    {
        Log("[vk] %s cross-buffer import/bind failed; retaining the D3D12 copy",
            upload ? "input" : "output");
        return false;
    }
    active = true;
    Log("[vk] direct neural-%s buffer imported; GPU 0 D3D12 %s copy removed",
        upload ? "input" : "output", upload ? "input" : "output");
    return true;
}

static bool VkImportFence(VkSemaphore *out, HANDLE h, const char *name)
{
    VkDevice dev = g_bridge.device;
    VkSemaphoreTypeCreateInfo st = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sc = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sc.pNext = &st;
    if (g_disp->CreateSemaphore(dev, &sc, nullptr, out) != VK_SUCCESS)
    { Log("[vk] %s: vkCreateSemaphore(timeline) failed", name); return false; }

    VkImportSemaphoreWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    imp.semaphore  = *out;
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    imp.handle     = h;
    if (g_disp->ImportSemaphoreWin32HandleKHR(dev, &imp) != VK_SUCCESS)
    { Log("[vk] %s: vkImportSemaphoreWin32HandleKHR failed", name); return false; }
    Log("[vk] %s imported as timeline semaphore %p", name, (void *)*out);
    return true;
}

// ---- device / session bring-up --------------------------------------------
#ifndef DLSS5VK_ADDON_HOST
static bool EnsureVkDevice(VkCommandBuffer)
{
    if (g_bridge.device != VK_NULL_HANDLE && g_disp) return true;

    // Prefer what the NGX Vulkan Init hook captured; fall back to a device the
    // layer saw created. Slot 0 is not necessarily it -- slots are recycled, so
    // after a throwaway probe device is destroyed the live one can sit anywhere
    // in the table and slot 0 holds a null handle.
    if (g_bridge.device == VK_NULL_HANDLE)
    {
        for (LONG i = 0; i < g_dev_count; ++i)
        {
            if (g_dev[i].device == VK_NULL_HANDLE) continue;
            g_bridge.device = g_dev[i].device;
            g_bridge.phys   = g_dev[i].phys;
            if (InstanceDispatch *in = InstanceForPhys(g_bridge.phys))
                g_bridge.instance = in->instance;
            break;
        }
    }
    g_disp = DispatchForDevice(g_bridge.device);
    if (g_disp == nullptr) { Log("[vk] no device dispatch; is the layer active?"); return false; }

    InstanceDispatch *in = BridgeInstance();
    if (in == nullptr || in->GetPhysicalDeviceMemoryProperties == nullptr)
    { Log("[vk] no instance dispatch for memory properties"); return false; }
    in->GetPhysicalDeviceMemoryProperties(g_bridge.phys, &g_mem_props);

    // Capture the device LUID so the D3D12 device lands on the same GPU.
    g_bridge.luid_valid = false;
    if (in->GetPhysicalDeviceProperties2)
    {
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 pp  = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        pp.pNext = &idp;
        in->GetPhysicalDeviceProperties2(g_bridge.phys, &pp);
        if (idp.deviceLUIDValid)
        {
            memcpy(g_bridge.game_luid, idp.deviceLUID, VK_LUID_SIZE);
            g_bridge.luid_valid = true;
        }
    }
    return true;
}
#else
// The add-on host sees no instance and no physical device: ReShade reports the
// VkDevice and the adapter LUID, the loader's exported vkGetDeviceProcAddr
// resolves the device entry points, and the memory properties come from a
// short-lived private instance (see AddonAcquireDevice).
static bool EnsureVkDevice(VkCommandBuffer cmd)
{
    if (g_bridge.device != VK_NULL_HANDLE && g_disp) return true;
    AddonDevice *ad = AddonAcquireDevice(cmd);
    if (ad == nullptr) return false;
    g_bridge.device   = ad->device;
    g_bridge.phys     = VK_NULL_HANDLE;
    g_bridge.instance = VK_NULL_HANDLE;
    g_disp            = &ad->disp;
    g_mem_props       = ad->mem;
    memcpy(g_bridge.game_luid, ad->luid, VK_LUID_SIZE);
    g_bridge.luid_valid = ad->luid_valid;
    return true;
}
#endif

static void VkReleaseSharedTex(int slot);

// ---- per-frame command resources (bridge-owned) ---------------------------
// The retire fences are device-level and serve both paths. The command pool
// (holding the async copy-back buffers) needs the queue family the DLSS pass
// is submitted on, which is only known once its submit has been matched.
static bool EnsureRetireFences()
{
    VkDevice dev = g_bridge.device;
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        if (g_bridge.vkframe[i].retire) continue;
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence f = VK_NULL_HANDLE;
        if (g_disp->CreateFence(dev, &fci, nullptr, &f) != VK_SUCCESS)
        { Log("[vk] vkCreateFence failed"); return false; }
        g_bridge.vkframe[i].retire    = f;
        g_bridge.vkframe[i].submitted = false;
    }
    return true;
}
static bool EnsureVkFrames(uint32_t family)
{
    if (g_bridge.vk_ready) return true;
    if (!EnsureRetireFences()) return false;
    VkDevice dev = g_bridge.device;
    g_bridge.vk_queue_family = family;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (g_disp->CreateCommandPool(dev, &pci, nullptr, &g_bridge.vk_pool) != VK_SUCCESS)
    { Log("[vk] vkCreateCommandPool failed"); return false; }

    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool        = g_bridge.vk_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (g_disp->AllocateCommandBuffers(dev, &ai, &g_bridge.vkframe[i].copy_out) != VK_SUCCESS)
        { Log("[vk] vkAllocateCommandBuffers failed"); return false; }
    }
    g_bridge.vk_ready = true;
    Log("[vk] per-frame command resources ready (family %u)", family);
    return true;
}

// ---- the sync-handoff worker -----------------------------------------------
// Why a sandwich at all: the async path submitted the copy-back as its own
// fence-chained submit AFTER the game's. That is provably too late -- the
// game's post-processing reads its DLSS output in the same command buffer,
// right after the evaluate, so every async copy-back landed on pixels nobody
// would ever read again ("2000 frames bridged", zero visible change). The
// evaluate hook sits exactly between the game's evaluate and those consumers,
// so with sync=1 it records the wait and the copy-back INLINE at that spot:
//
//   game cmd buffer: ... game evaluate | input copies | SetEvent(ev_inputs)
//                    | WaitEvents(ev_output) | copy-back | SetEvent(ev_done)
//                    | game post-fx ...
//
// The GPU parks at WaitEvents until this worker thread has (1) seen ev_inputs
// go off, (2) CPU-signalled fence_in to release the D3D12 evaluate, (3) waited
// for fence_out on the CPU, and (4) host-set ev_output. The wait chain is
// GPU -> worker -> D3D12 queue -> worker -> GPU: acyclic, and every worker
// wait is bounded, so a wedge degrades into one skipped frame, never a hang.
// ev_output is set on EVERY exit path -- a parked WaitEvents with nobody left
// to set its event is a device loss.
//
// The worker is armed when the sandwich is RECORDED, not when some later
// submit is seen: which submit carries the DLSS pass is not the bridge's
// business (v0.1.12 assumed "the next one", which idTech 8's multi-threaded,
// multi-queue submission proved wrong -- issue #3). Slots are queued in
// recording order and driven strictly in that order, since the D3D12
// evaluate's temporal history must see the frames in sequence. The game's
// thread never waits for this thread: a slot whose sandwich has not retired
// is simply not reused, and that frame is forwarded untouched instead.
static HANDLE        g_worker_thread;
static DWORD         g_worker_tid;
static HANDLE        g_worker_wake;         // auto-reset: slots were queued
static HANDLE        g_worker_fence_event;  // the worker's own fence_out wait
static volatile LONG g_worker_stop;
static bool          g_worker_healthy;      // last JUDGED frame was a clean 0;
                                            // gates game_eval=0 skipping. Only
                                            // touched on the game's thread.
static volatile LONG g_release_deferred;    // teardown was requested ON the worker
static bool          g_sync_active;
static bool          g_sync_init_done;

static void ApplyFrameScalars(const FrameScalars *s);

// Recording-order queue of slots: single producer (the game's thread, under
// g_frame_cs), single consumer (the worker). A slot is queued at most once
// while in flight, so kFrames + 1 entries can never overflow.
static int           g_wq[Bridge::kFrames + 1];
static volatile LONG g_wq_head;             // consumer
static volatile LONG g_wq_tail;             // producer
static void WorkQueuePush(int slot)
{
    g_wq[g_wq_tail] = slot;
    MemoryBarrier();
    g_wq_tail = (g_wq_tail + 1) % (Bridge::kFrames + 1);
    SetEvent(g_worker_wake);
}
static int WorkQueuePop()
{
    if (g_wq_head == g_wq_tail) return -1;
    const int slot = g_wq[g_wq_head];
    MemoryBarrier();
    g_wq_head = (g_wq_head + 1) % (Bridge::kFrames + 1);
    return slot;
}

// The frame slots are shared by the game's threads and the worker.
// g_frame_cs serialises the game-side users (the evaluate hook, the submit
// hooks, the command-buffer hooks, teardown). The worker never takes it -- it
// reads only what was written before its slot was queued, plus the volatile
// flags -- so a fault on the game's thread can never strand it.
static CRITICAL_SECTION g_frame_cs;
static __declspec(thread) int g_frame_cs_depth;
static void FrameLock()   { EnterCriticalSection(&g_frame_cs); ++g_frame_cs_depth; }
static void FrameUnlock() { --g_frame_cs_depth; LeaveCriticalSection(&g_frame_cs); }
static void BridgeFrameUnlockAll()   // from the frame path's exception handler
{ while (g_frame_cs_depth > 0) FrameUnlock(); }

// How long the worker gives the GPU to reach a recorded sandwich, counted
// from the moment the worker gets to that slot -- so it also covers a game
// that records its DLSS pass well ahead of submitting it.
static const ULONGLONG kInputsTimeoutMs = 3000;

// While waiting on one slot's inputs, a LATER slot's inputs going off means
// the GPU has moved past this one: its sandwich either never went out, or
// went to a queue the later one did not have to wait for. Waiting on would
// only keep that later frame parked behind a verdict that cannot come.
static bool WorkerLaterSlotReached(int slot)
{
    const UINT64 mine = g_bridge.vkframe[slot].value;
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        if (i == slot) continue;
        const Bridge::VkFrame &o = g_bridge.vkframe[i];
        if (!o.in_flight || o.worker_done || o.value <= mine || !o.ev_inputs) continue;
        if (g_disp->GetEventStatus(g_bridge.device, o.ev_inputs) == VK_EVENT_SET) return true;
    }
    return false;
}

// Results 0/1/2/4 come from D3D12RunEvaluateCore; 8 = ev_inputs never fired
// (the GPU did not reach our copies in time, the recording was discarded, or
// a later frame overtook it), 16 = the D3D12 queue did not finish the
// evaluate in time. The verdict is applied on the game's thread (BridgeJudge).
static void WorkerRunSlot(int slot)
{
    Bridge::VkFrame &fr = g_bridge.vkframe[slot];
    const UINT64 v = fr.value;
    LONG result = 0;

    // There is no host-blocking wait on a VkEvent, so this poll is forced.
    // Keep it polite: pause-spin through the expected sub-frame wait, yield
    // if it drags, and once it is clearly stalled (several frames late)
    // sleep -- latency has stopped mattering, stop burning the core.
    const ULONGLONG t0 = GetTickCount64();
    for (unsigned spins = 0;; ++spins)
    {
        VkResult st = g_disp->GetEventStatus(g_bridge.device, fr.ev_inputs);
        if (st == VK_EVENT_SET) break;
        const ULONGLONG waited = GetTickCount64() - t0;
        if (st != VK_EVENT_RESET || waited > kInputsTimeoutMs || fr.abandoned || g_worker_stop)
        { result = 8; break; }
        if ((spins & 255) == 255 && WorkerLaterSlotReached(slot))
        {
            // Re-check first: on one queue this slot's copies precede the
            // later one's, and both may have gone off since the last poll.
            if (g_disp->GetEventStatus(g_bridge.device, fr.ev_inputs) == VK_EVENT_SET) break;
            result = 8; break;
        }
        if      (waited > 100)  Sleep(1);
        else if (spins > 4096)  SwitchToThread();
        else for (int i = 0; i < 64; ++i) YieldProcessor();
    }

    if (result == 0)
    {
        // This frame's own scalars (its jitter above all): the next frame
        // may already be recorded and must not lend it its values.
        ApplyFrameScalars(&fr.scalars);
        // The D3D12 fence is the same object Vulkan imported; a CPU-side
        // signal satisfies the queue Wait inside the evaluate core.
        g_bridge.fence_in->Signal(v);
        D3D12RunEvaluateCore(v, &result, true);
        if (result == 0 && g_bridge.fence_out->GetCompletedValue() < v)
        {
            g_bridge.fence_out->SetEventOnCompletion(v, g_worker_fence_event);
            if (WaitForSingleObject(g_worker_fence_event, 1000) != WAIT_OBJECT_0)
                result = 16;
        }
        static bool said_first = false;
        if (result == 0 && !said_first)
        { said_first = true; Log("[bridge] first D3D12 evaluate completed inside the game's frame"); }
    }

    g_disp->SetEvent(g_bridge.device, fr.ev_output);   // ALWAYS: unpark the GPU

    fr.result = result;
    MemoryBarrier();
    InterlockedExchange(&fr.worker_done, 1);
}

static DWORD WINAPI BridgeWorkerProc(void *)
{
    for (;;)
    {
        WaitForSingleObject(g_worker_wake, INFINITE);
        for (;;)
        {
            if (g_worker_stop) return 0;
            const int slot = WorkQueuePop();
            if (slot < 0) break;
            WorkerRunSlot(slot);
        }
    }
}

// Events and the worker are device-level, so unlike the command pool (which
// needs the queue family, known only at submit time) they can be created at
// hook time -- they must be, the sandwich is recorded before the first submit.
static void EnsureSyncObjects()
{
    if (g_sync_init_done || !g_cfg.sync) return;
    g_sync_init_done = true;

    if (!g_disp->CreateEvent || !g_disp->DestroyEvent || !g_disp->SetEvent || !g_disp->ResetEvent ||
        !g_disp->GetEventStatus || !g_disp->CmdSetEvent || !g_disp->CmdWaitEvents)
    { Log("[bridge] device dispatch lacks VkEvent entry points; falling back to the async copy-back"); return; }

    VkDevice dev = g_bridge.device;
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        VkEventCreateInfo eci = { VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
        Bridge::VkFrame &fr = g_bridge.vkframe[i];
        if (g_disp->CreateEvent(dev, &eci, nullptr, &fr.ev_inputs) != VK_SUCCESS ||
            g_disp->CreateEvent(dev, &eci, nullptr, &fr.ev_output) != VK_SUCCESS ||
            g_disp->CreateEvent(dev, &eci, nullptr, &fr.ev_done)   != VK_SUCCESS)
        { Log("[bridge] vkCreateEvent failed; falling back to the async copy-back"); return; }
    }
    g_wq_head = g_wq_tail = 0;
    g_worker_wake        = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_worker_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_worker_wake || !g_worker_fence_event)
    { Log("[bridge] worker event creation failed; falling back to the async copy-back"); return; }
    InterlockedExchange(&g_worker_stop, 0);   // a previous device's teardown left it set
    g_worker_thread = CreateThread(nullptr, 0, BridgeWorkerProc, nullptr, 0, &g_worker_tid);
    if (g_worker_thread == nullptr)
    { Log("[bridge] worker thread creation failed; falling back to the async copy-back"); return; }
    if (SetThreadPriority(g_worker_thread, THREAD_PRIORITY_HIGHEST))
        Log("[bridge] sync worker raised to high priority");
    else
        Log("[bridge] sync worker priority raise was unavailable (error %lu)", GetLastError());
    g_sync_active = true;
    Log("[bridge] synchronous in-frame handoff active (sync=1): the game's frame now waits "
        "for the D3D12 result before its own post-processing reads the output");
}

// depth-aspect round-trip staging, sized to the shared depth image.
static bool EnsureDepthScratch(int slot_i, VkDeviceSize bytes)
{
    Bridge::VkFrame &fr = g_bridge.vkframe[slot_i];
    if (fr.depth_scratch != VK_NULL_HANDLE && fr.depth_scratch_size >= bytes) return true;

    VkDevice dev = g_bridge.device;
    if (fr.depth_scratch)     { g_disp->DestroyBuffer(dev, fr.depth_scratch, nullptr); fr.depth_scratch = VK_NULL_HANDLE; }
    if (fr.depth_scratch_mem) { g_disp->FreeMemory(dev, fr.depth_scratch_mem, nullptr); fr.depth_scratch_mem = VK_NULL_HANDLE; }

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_disp->CreateBuffer(dev, &bci, nullptr, &fr.depth_scratch) != VK_SUCCESS) return false;

    VkMemoryRequirements req = {};
    g_disp->GetBufferMemoryRequirements(dev, fr.depth_scratch, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = PickMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (g_disp->AllocateMemory(dev, &mai, nullptr, &fr.depth_scratch_mem) != VK_SUCCESS) return false;
    if (g_disp->BindBufferMemory(dev, fr.depth_scratch, fr.depth_scratch_mem, 0) != VK_SUCCESS) return false;
    fr.depth_scratch_size = bytes;
    return true;
}

// ---- depth conversion (compute) -------------------------------------------
// D16/D24 depth cannot be transfer-copied into the shared R32_SFLOAT image,
// but the depth view the game hands NGX is sampleable whatever the format --
// the driver's own DLSS reads it exactly that way. One 8x8 compute pass reads
// that view and writes normalized depth into the shared image. 32-bit float
// depth keeps the bit-exact buffer round-trip in CopyDepth.
#include "depth_convert_spv.h"

static VkSampler             g_depth_sampler;
static VkDescriptorSetLayout g_depth_dsl;
static VkPipelineLayout      g_depth_pl;
static VkPipeline            g_depth_pipe;
static VkDescriptorPool      g_depth_pool;
static VkDescriptorSet       g_depth_desc[Bridge::kFrames];
static VkImageView           g_depth_store_view;   // storage view of the shared image
static bool                  g_depth_pipe_failed;

static void DestroyDepthPipeline()
{
    VkDevice dev = g_bridge.device;
    if (dev == VK_NULL_HANDLE || g_disp == nullptr) return;
    if (g_depth_pool && g_disp->DestroyDescriptorPool)
    { g_disp->DestroyDescriptorPool(dev, g_depth_pool, nullptr); g_depth_pool = VK_NULL_HANDLE; }
    if (g_depth_pipe && g_disp->DestroyPipeline)
    { g_disp->DestroyPipeline(dev, g_depth_pipe, nullptr); g_depth_pipe = VK_NULL_HANDLE; }
    if (g_depth_pl && g_disp->DestroyPipelineLayout)
    { g_disp->DestroyPipelineLayout(dev, g_depth_pl, nullptr); g_depth_pl = VK_NULL_HANDLE; }
    if (g_depth_dsl && g_disp->DestroyDescriptorSetLayout)
    { g_disp->DestroyDescriptorSetLayout(dev, g_depth_dsl, nullptr); g_depth_dsl = VK_NULL_HANDLE; }
    if (g_depth_sampler && g_disp->DestroySampler)
    { g_disp->DestroySampler(dev, g_depth_sampler, nullptr); g_depth_sampler = VK_NULL_HANDLE; }
    for (int i = 0; i < Bridge::kFrames; ++i) g_depth_desc[i] = VK_NULL_HANDLE;
}

static bool EnsureDepthPipeline()
{
    if (g_depth_pipe) return true;
    if (g_depth_pipe_failed) return false;
    VkDevice dev = g_bridge.device;

    if (!g_disp->CreateSampler || !g_disp->CreateDescriptorSetLayout || !g_disp->CreatePipelineLayout ||
        !g_disp->CreateShaderModule || !g_disp->CreateComputePipelines || !g_disp->CreateDescriptorPool ||
        !g_disp->AllocateDescriptorSets || !g_disp->UpdateDescriptorSets || !g_disp->CreateImageView ||
        !g_disp->CmdBindPipeline || !g_disp->CmdBindDescriptorSets || !g_disp->CmdPushConstants ||
        !g_disp->CmdDispatch)
    { g_depth_pipe_failed = true; Log("[vk] depth convert: device dispatch lacks compute entry points"); return false; }

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (g_disp->CreateSampler(dev, &sci, nullptr, &g_depth_sampler) != VK_SUCCESS) goto bad;

    {
        VkDescriptorSetLayoutBinding b[2] = {};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_depth_sampler };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 2;
        li.pBindings    = b;
        if (g_disp->CreateDescriptorSetLayout(dev, &li, nullptr, &g_depth_dsl) != VK_SUCCESS) goto bad;

        VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 2 * sizeof(uint32_t) };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &g_depth_dsl;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pcr;
        if (g_disp->CreatePipelineLayout(dev, &pli, nullptr, &g_depth_pl) != VK_SUCCESS) goto bad;

        VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smi.codeSize = sizeof(kDepthConvertSpv);
        smi.pCode    = kDepthConvertSpv;
        VkShaderModule sm = VK_NULL_HANDLE;
        if (g_disp->CreateShaderModule(dev, &smi, nullptr, &sm) != VK_SUCCESS) goto bad;

        VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = sm;
        cpi.stage.pName  = "main";
        cpi.layout       = g_depth_pl;
        VkResult pr = g_disp->CreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &g_depth_pipe);
        g_disp->DestroyShaderModule(dev, sm, nullptr);
        if (pr != VK_SUCCESS || g_depth_pipe == VK_NULL_HANDLE) goto bad;

        VkDescriptorPoolSize ps[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Bridge::kFrames },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          Bridge::kFrames },
        };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets       = Bridge::kFrames;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = ps;
        if (g_disp->CreateDescriptorPool(dev, &dpi, nullptr, &g_depth_pool) != VK_SUCCESS) goto bad;

        VkDescriptorSetLayout layouts[Bridge::kFrames];
        for (int i = 0; i < Bridge::kFrames; ++i) layouts[i] = g_depth_dsl;
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool     = g_depth_pool;
        dai.descriptorSetCount = Bridge::kFrames;
        dai.pSetLayouts        = layouts;
        if (g_disp->AllocateDescriptorSets(dev, &dai, g_depth_desc) != VK_SUCCESS) goto bad;
    }
    Log("[vk] depth convert compute pipeline ready");
    return true;

bad:
    Log("[vk] depth convert pipeline creation failed; non-float depth will not be fed");
    DestroyDepthPipeline();
    g_depth_pipe_failed = true;
    return false;
}

// ---- resource (re)build ----------------------------------------------------
// Called from the evaluate hook when the geometry / formats change. Creates the
// D3D12 shared textures, imports them into Vulkan, binds them into the private
// NGX parameter block, builds the DLSS feature and the depth staging buffers.
static bool RebuildResources(const NVSDK_NGX_Parameter *p, const GameRes &col,
                             const GameRes &out, const GameRes &dep, const GameRes &mv)
{
    Log("[bridge] (re)building resources for %ux%u -> %ux%u", col.w, col.h, out.w, out.h);
    // The caller guarantees no bridged frame is in flight (BridgeSlotsBusy()
    // == 0): nothing recorded into a game command buffer still references
    // what is released below, every retire fence is signalled, and every
    // D3D12 wait the bridge queued has been satisfied -- so the drain below
    // cannot deadlock. Drop the cached depth views too: the old depth images
    // may die with the old contract.
    D3D12DrainGpu();
    if (g_disp && g_bridge.device)
        for (int i = 0; i < Bridge::kFrames; ++i)
        {
            Bridge::VkFrame &fr = g_bridge.vkframe[i];
            if (fr.submitted && fr.retire)
            {
                g_disp->ResetFences(g_bridge.device, 1, &fr.retire);
                fr.submitted = false;
            }
            if (fr.game_depth_view && g_disp->DestroyImageView)
            { g_disp->DestroyImageView(g_bridge.device, fr.game_depth_view, nullptr); fr.game_depth_view = VK_NULL_HANDLE; }
        }
    g_bridge.frame_ready = false;
    D3D12ReleaseMultiGpuFrameResources();
    for (int i = 0; i < SLOT_COUNT; ++i) VkReleaseSharedTex(i);

    g_bridge.width  = col.w; g_bridge.height = col.h;
    g_bridge.out_width = out.w; g_bridge.out_height = out.h;

    unsigned rw = 0, rh = 0;
    if (!(p && p->Get("DLSS.Render.Subrect.Dimensions.Width",  &rw) == NGX_SUCCESS && rw)) rw = col.w;
    if (!(p && p->Get("DLSS.Render.Subrect.Dimensions.Height", &rh) == NGX_SUCCESS && rh)) rh = col.h;
    g_bridge.render_w = rw; g_bridge.render_h = rh;
    g_bridge.ngx_out_w = out.w; g_bridge.ngx_out_h = out.h;

    struct Plan { int slot; UINT w, h; DXGI_FORMAT dxgi; VkFormat vk; bool uav; } plan[SLOT_COUNT];

    VkFormat cvk; DXGI_FORMAT cdx; bool cblit;
    MapColor(col.fmt, &cvk, &cdx, &cblit);
    plan[SLOT_COLOR] = { SLOT_COLOR, col.w, col.h, cdx, cvk, false };

    VkFormat ovk; DXGI_FORMAT odx; bool oblit;
    MapColor(out.fmt, &ovk, &odx, &oblit);
    if (g_cfg.output_transport == dlss_bridge::OutputTransport::R11G11B10Float)
    {
        ovk = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        odx = DXGI_FORMAT_R11G11B10_FLOAT;
        oblit = out.fmt != ovk;
        Log("[mgpu] packed output transport enabled: neural output and return plane use R11G11B10_FLOAT");
    }
    plan[SLOT_OUTPUT] = { SLOT_OUTPUT, out.w, out.h, odx, ovk, true };

    plan[SLOT_DEPTH] = { SLOT_DEPTH, dep.w, dep.h, DXGI_FORMAT_R32_FLOAT,   VK_FORMAT_R32_SFLOAT,    true };
    VkFormat mvk; DXGI_FORMAT mdx; bool mblit;
    MapMotionVectors(mv.fmt, &mvk, &mdx, &mblit);
    plan[SLOT_MV]    = { SLOT_MV,    mv.w,  mv.h,  mdx, mvk, false };
    if (cblit || oblit || mblit)
        Log("[bridge] format(s) without a direct alias -- colour %d%s, motion vectors %d%s, output %d%s -- "
            "are converted by a blit, which only a graphics queue can run",
            (int)col.fmt, cblit ? " (blit)" : "", (int)mv.fmt, mblit ? " (blit)" : "",
            (int)out.fmt, oblit ? " (blit)" : "");

    for (const Plan &pl : plan)
    {
        if (!D3D12MakeSharedTexture(pl.slot, pl.w, pl.h, pl.dxgi, pl.uav)) { BridgeFail("shared texture"); return false; }
        if (g_mgpu.active && !D3D12MakeNeuralTexture(pl.slot, pl.w, pl.h, pl.dxgi, pl.uav))
        { BridgeFail("neural texture"); return false; }
        g_bridge.tex[pl.slot].fmtvk = pl.vk;
        if (!VkImportSharedTex(pl.slot)) { BridgeFail("import texture"); return false; }
        g_bridge.slot_w[pl.slot] = pl.w; g_bridge.slot_h[pl.slot] = pl.h;
    }

    if (!D3D12BuildCrossBuffer()) { BridgeFail("cross-adapter buffer"); return false; }
    if (g_mgpu.active)
    {
        (void)VkImportCrossBuffer(CROSS_UPLOAD);
        (void)VkImportCrossBuffer(CROSS_DOWNLOAD);
    }

    // storage view of the shared depth image, for the compute conversion pass
    if (g_disp->CreateImageView)
    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image            = g_bridge.tex[SLOT_DEPTH].image;
        vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vi.format           = VK_FORMAT_R32_SFLOAT;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (g_disp->CreateImageView(g_bridge.device, &vi, nullptr, &g_depth_store_view) != VK_SUCCESS)
            g_depth_store_view = VK_NULL_HANDLE;
    }

    for (int i = 0; i < SLOT_COUNT; ++i)
        g_bridge.params->Set(kSlotKey[i], NeuralTex(i));

    for (int i = 0; i < Bridge::kFrames; ++i)
        if (!EnsureDepthScratch(i, (VkDeviceSize)plan[SLOT_DEPTH].w * plan[SLOT_DEPTH].h * 4))
        { Log("[vk] depth staging alloc failed"); BridgeFail("depth staging"); return false; }

    if (!D3D12CreateDlssFeature(p)) { BridgeFail("create feature"); return false; }

    g_bridge.game_fmt[SLOT_COLOR] = col.fmt;
    g_bridge.game_fmt[SLOT_OUTPUT] = out.fmt;
    g_bridge.game_fmt[SLOT_DEPTH] = dep.fmt;
    g_bridge.game_fmt[SLOT_MV] = mv.fmt;

    g_bridge.frame_ready = true;
    g_bridge.consecutive_fails = 0;
    return true;
}

static bool ContractChanged(const GameRes &col, const GameRes &out,
                            const GameRes &dep, const GameRes &mv)
{
    return !g_bridge.frame_ready
        || g_bridge.slot_w[SLOT_COLOR]  != col.w || g_bridge.slot_h[SLOT_COLOR]  != col.h
        || g_bridge.slot_w[SLOT_OUTPUT] != out.w || g_bridge.slot_h[SLOT_OUTPUT] != out.h
        || g_bridge.slot_w[SLOT_DEPTH]  != dep.w || g_bridge.slot_h[SLOT_DEPTH]  != dep.h
        || g_bridge.slot_w[SLOT_MV]     != mv.w  || g_bridge.slot_h[SLOT_MV]     != mv.h
        || g_bridge.game_fmt[SLOT_COLOR]  != col.fmt || g_bridge.game_fmt[SLOT_OUTPUT] != out.fmt
        || g_bridge.game_fmt[SLOT_DEPTH]  != dep.fmt || g_bridge.game_fmt[SLOT_MV]     != mv.fmt;
}

// ---- per-frame scalar forwarding ------------------------------------------
// Same rules as the DX11 bridge: mirror the values that must not be invented
// (jitter, sharpness), substitute the documented neutral where the game's value
// is optional (MV scale, exposure), and OR the reset flag with our own.
// Captured on the game's thread at record time and applied to the private
// parameter block right before the D3D12 evaluate that consumes them -- which
// in the sync sandwich runs on the worker, possibly after the next frame has
// already been recorded.
static const char *const kSubrectBaseKeys[8] = {
    "DLSS.Input.Color.Subrect.Base.X",  "DLSS.Input.Color.Subrect.Base.Y",
    "DLSS.Input.Depth.Subrect.Base.X",  "DLSS.Input.Depth.Subrect.Base.Y",
    "DLSS.Input.MV.Subrect.Base.X",     "DLSS.Input.MV.Subrect.Base.Y",
    "DLSS.Output.Subrect.Base.X",       "DLSS.Output.Subrect.Base.Y",
};
static bool GetFloatOr(const NVSDK_NGX_Parameter *g, const char *key, float neutral, float *out)
{
    float v = neutral;
    const bool has = g && g->Get(key, &v) == NGX_SUCCESS;
    *out = has ? v : neutral;
    return has;
}
static void CaptureFrameScalars(const NVSDK_NGX_Parameter *g, FrameScalars *s)
{
    memset(s, 0, sizeof(*s));
    s->has_jitter_x  = GetFloatOr(g, "Jitter.Offset.X", 0.0f, &s->jitter_x);
    s->has_jitter_y  = GetFloatOr(g, "Jitter.Offset.Y", 0.0f, &s->jitter_y);
    s->has_sharpness = GetFloatOr(g, "Sharpness",       0.0f, &s->sharpness);
    GetFloatOr(g, "MV.Scale.X",     1.0f, &s->mv_scale_x);
    GetFloatOr(g, "MV.Scale.Y",     1.0f, &s->mv_scale_y);
    GetFloatOr(g, "Pre.Exposure",   1.0f, &s->pre_exposure);
    GetFloatOr(g, "Exposure.Scale", 1.0f, &s->exposure_scale);
    for (int i = 0; i < 8; ++i)
    { unsigned v = 0; if (g) g->Get(kSubrectBaseKeys[i], &v); s->bases[i] = v; }

    unsigned reset = 0;
    if (g) g->Get("Reset", &reset);
    if (g_bridge.need_reset) reset = 1;
    s->reset = reset;
    g_bridge.need_reset = false;

    s->has_frame_dt = GetFloatOr(g, "FrameTimeDeltaInMsec", 0.0f, &s->frame_dt);
}
static void ApplyFrameScalars(const FrameScalars *s)
{
    NVSDK_NGX_Parameter *q = g_bridge.params;

    // OptiScaler's D3D12 CreateFeature path may initialise or replace entries
    // in the parameter table. Resource bindings installed while constructing
    // the frame therefore are not guaranteed to survive until EvaluateFeature
    // (the observed failure was "Color not exist"). NGX defines these as
    // per-evaluate inputs anyway, so bind the current neural-device textures at
    // the last possible point, alongside the per-frame scalar values.
    for (int i = 0; i < SLOT_COUNT; ++i)
        q->Set(kSlotKey[i], NeuralTex(i));

    static bool logged_resources = false;
    if (!logged_resources)
    {
        bool valid = true;
        for (int i = 0; i < SLOT_COUNT; ++i)
        {
            ID3D12Resource *bound = nullptr;
            if (q->Get(kSlotKey[i], &bound) != NGX_SUCCESS || bound != NeuralTex(i))
            {
                Log("[bridge] per-frame %s binding read-back failed: expected=%p actual=%p",
                    kSlotName[i], (void *)NeuralTex(i), (void *)bound);
                valid = false;
            }
        }
        if (valid)
            Log("[bridge] per-frame D3D12 resources bound: Color=%p Output=%p Depth=%p MV=%p",
                (void *)NeuralTex(SLOT_COLOR), (void *)NeuralTex(SLOT_OUTPUT),
                (void *)NeuralTex(SLOT_DEPTH), (void *)NeuralTex(SLOT_MV));
        logged_resources = true;
    }

    q->Set("Width",     g_bridge.render_w);
    q->Set("Height",    g_bridge.render_h);
    q->Set("DLSS.Render.Subrect.Dimensions.Width",  g_bridge.render_w);
    q->Set("DLSS.Render.Subrect.Dimensions.Height", g_bridge.render_h);
    if (s->has_jitter_x)  q->Set("Jitter.Offset.X", s->jitter_x);
    if (s->has_jitter_y)  q->Set("Jitter.Offset.Y", s->jitter_y);
    if (s->has_sharpness) q->Set("Sharpness",       s->sharpness);
    q->Set("MV.Scale.X",     s->mv_scale_x);
    q->Set("MV.Scale.Y",     s->mv_scale_y);
    q->Set("Pre.Exposure",   s->pre_exposure);
    q->Set("Exposure.Scale", s->exposure_scale);
    for (int i = 0; i < 8; ++i) q->Set(kSubrectBaseKeys[i], s->bases[i]);
    q->Set("Reset", s->reset);
    if (s->has_frame_dt) q->Set("FrameTimeDeltaInMsec", s->frame_dt);
}

// ---- recording -------------------------------------------------------------
static void VkImgBarrier(VkCommandBuffer c, VkImage img, VkImageAspectFlags aspect,
                         VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = img;
    b.subresourceRange    = { aspect, 0, 1, 0, 1 };
    g_disp->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &b);
}

static void MemBarrier(VkCommandBuffer c)
{
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    g_disp->CmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, 1, &b, 0, nullptr, 0, nullptr);
}

// game colour/MV image -> shared texture (size-compatible copy, or blit).
static void CopyColorLike(VkCommandBuffer c, const GameRes &src, int slot, bool blit)
{
    SharedTex &dst = g_bridge.tex[slot];
    const UINT w = umin(src.w, dst.w), h = umin(src.h, dst.h);

    VkImgBarrier(c, src.image, src.aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImgBarrier(c, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, dst.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (blit)
    {
        VkImageBlit r = {};
        r.srcSubresource = { src.aspect, src.mip, src.layer, 1 };
        r.srcOffsets[1]  = { (int32_t)w, (int32_t)h, 1 };
        r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.dstOffsets[1]  = { (int32_t)w, (int32_t)h, 1 };
        g_disp->CmdBlitImage(c, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    }
    else
    {
        VkImageCopy r = {};
        r.srcSubresource = { src.aspect, src.mip, src.layer, 1 };
        r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.extent         = { w, h, 1 };
        g_disp->CmdCopyImage(c, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }

    VkImgBarrier(c, src.image, src.aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    VkImgBarrier(c, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    dst.layout = VK_IMAGE_LAYOUT_GENERAL;
}

// game depth image -> shared R32F, via a depth-aspect buffer round-trip (a
// bit-exact reinterpret, valid for 32-bit float depth).
static void CopyDepth(VkCommandBuffer c, const GameRes &src, int frame_slot)
{
    SharedTex &dst = g_bridge.tex[SLOT_DEPTH];
    const UINT w = umin(src.w, dst.w), h = umin(src.h, dst.h);
    VkBuffer staging = g_bridge.vkframe[frame_slot].depth_scratch;

    VkImgBarrier(c, src.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy toBuf = {};
    toBuf.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, src.mip, src.layer, 1 };
    toBuf.imageExtent      = { w, h, 1 };
    g_disp->CmdCopyImageToBuffer(c, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &toBuf);
    VkImgBarrier(c, src.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    MemBarrier(c);

    VkImgBarrier(c, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, dst.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy toImg = {};
    toImg.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    toImg.imageExtent      = { w, h, 1 };
    g_disp->CmdCopyBufferToImage(c, staging, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &toImg);
    VkImgBarrier(c, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    dst.layout = VK_IMAGE_LAYOUT_GENERAL;
}

// The subresource range the game hands NGX often names both DEPTH and STENCIL
// aspects (BG3: aspect=0x6 on D24_UNORM_S8_UINT), and a combined-aspect view
// cannot legally be sampled -- which is why v0.1.4's "sample the game's own
// view" never engaged. The image itself is fine: make our own DEPTH-only view
// of it and sample that. Cached per frame slot, keyed on image+mip+layer (the
// game may rotate depth images); the caller only hands out a slot whose
// previous sandwich has retired, so replacing the old view is safe.
static VkImageView GetSampleableDepthView(Bridge::VkFrame &fr, const GameRes &dep)
{
    if (dep.aspect == VK_IMAGE_ASPECT_DEPTH_BIT && dep.view != VK_NULL_HANDLE)
        return dep.view;
    if (!(dep.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return VK_NULL_HANDLE;
    if (!g_disp->CreateImageView || !g_disp->DestroyImageView) return VK_NULL_HANDLE;

    if (fr.game_depth_view != VK_NULL_HANDLE && fr.game_depth_image == dep.image &&
        fr.game_depth_mip == dep.mip && fr.game_depth_layer == dep.layer)
        return fr.game_depth_view;

    if (fr.game_depth_view)
    { g_disp->DestroyImageView(g_bridge.device, fr.game_depth_view, nullptr); fr.game_depth_view = VK_NULL_HANDLE; }

    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image            = dep.image;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vi.format           = dep.fmt;
    vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, dep.mip, 1, dep.layer, 1 };
    if (g_disp->CreateImageView(g_bridge.device, &vi, nullptr, &fr.game_depth_view) != VK_SUCCESS)
    { fr.game_depth_view = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    fr.game_depth_image = dep.image;
    fr.game_depth_mip   = dep.mip;
    fr.game_depth_layer = dep.layer;
    return fr.game_depth_view;
}

// game depth (any format) -> shared R32F, sampled through a depth-only view.
static void DispatchDepthConvert(VkCommandBuffer c, VkImageView src_view, UINT sw, UINT sh, int frame_slot)
{
    SharedTex &dst = g_bridge.tex[SLOT_DEPTH];
    const UINT w = umin(sw, dst.w), h = umin(sh, dst.h);

    // Rebind every frame: the game may rotate depth images. The set for this
    // slot is idle -- the slot is only reused once its previous sandwich has
    // retired.
    VkDescriptorImageInfo si = {};
    si.imageView   = src_view;
    si.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo di = {};
    di.imageView   = g_depth_store_view;
    di.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wr[2] = {};
    wr[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet          = g_depth_desc[frame_slot];
    wr[0].dstBinding      = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo      = &si;
    wr[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[1].dstSet          = g_depth_desc[frame_slot];
    wr[1].dstBinding      = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wr[1].pImageInfo      = &di;
    g_disp->UpdateDescriptorSets(g_bridge.device, 2, wr, 0, nullptr);

    VkImgBarrier(c, dst.image, VK_IMAGE_ASPECT_COLOR_BIT, dst.layout, VK_IMAGE_LAYOUT_GENERAL);
    dst.layout = VK_IMAGE_LAYOUT_GENERAL;

    g_disp->CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, g_depth_pipe);
    g_disp->CmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, g_depth_pl, 0, 1,
                                  &g_depth_desc[frame_slot], 0, nullptr);
    const uint32_t extent[2] = { w, h };
    g_disp->CmdPushConstants(c, g_depth_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(extent), extent);
    g_disp->CmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    MemBarrier(c);
}

static UINT DxgiBytesPerTexel(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return 4;
    default: return 0;
    }
}

static void RecordDirectInputCopies(VkCommandBuffer c)
{
    if (!g_mgpu.direct_vulkan_upload) return;
    const int inputs[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    for (int slot : inputs)
    {
        SharedTex &src = g_bridge.tex[slot];
        const CrossPlane &p = g_mgpu.plane[slot];
        const UINT bytes = DxgiBytesPerTexel(p.format);
        if (!bytes) continue;
        VkImgBarrier(c, src.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r = {};
        r.bufferOffset = p.offset;
        r.bufferRowLength = p.row_pitch / bytes;
        r.bufferImageHeight = p.height;
        r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageExtent = { p.width, p.height, 1 };
        g_disp->CmdCopyImageToBuffer(c, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     g_mgpu.cross_vk[CROSS_UPLOAD], 1, &r);
        VkImgBarrier(c, src.image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }
}

// shared Output -> the game's own Output image. The body carries no begin/end
// so it can be recorded either inline into the game's command buffer (sync
// sandwich) or into the bridge's own copy_out buffer (async path).
static void RecordCopyOutBody(VkCommandBuffer c, VkImage game_out, VkFormat game_fmt, UINT w, UINT h)
{
    SharedTex &src = g_bridge.tex[SLOT_OUTPUT];
    const UINT bytes_per_texel = DxgiBytesPerTexel(g_mgpu.plane[SLOT_OUTPUT].format);
    const bool host_result = g_mgpu.direct_vulkan_download && bytes_per_texel != 0;
    const bool direct = host_result && game_fmt == src.fmtvk;
    const bool blit = game_fmt != src.fmtvk;
    const UINT cw = umin(w, src.w), ch = umin(h, src.h);

    if (!direct)
        VkImgBarrier(c, src.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                     host_result ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                 : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImgBarrier(c, game_out, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    if (host_result)
    {
        const CrossPlane &p = g_mgpu.plane[SLOT_OUTPUT];
        VkBufferImageCopy r = {};
        r.bufferOffset = p.offset;
        r.bufferRowLength = p.row_pitch / bytes_per_texel;
        r.bufferImageHeight = p.height;
        r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.imageExtent = { cw, ch, 1 };
        VkImage target = direct ? game_out : src.image;
        g_disp->CmdCopyBufferToImage(c, g_mgpu.cross_vk[CROSS_DOWNLOAD], target,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        if (!direct)
            VkImgBarrier(c, src.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    if (blit)
    {
        VkImageBlit r = {};
        r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.srcOffsets[1]  = { (int32_t)cw, (int32_t)ch, 1 };
        r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.dstOffsets[1]  = { (int32_t)cw, (int32_t)ch, 1 };
        g_disp->CmdBlitImage(c, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             game_out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    }
    else if (!direct)
    {
        VkImageCopy r = {};
        r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.extent         = { cw, ch, 1 };
        g_disp->CmdCopyImage(c, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             game_out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }

    // mode=2 self-test: flood the game's output magenta right after the
    // copy-back. Screen turns magenta -> the copy-back lands where and when
    // the game reads it, and any remaining blandness is the D3D12 side.
    // Screen unchanged -> the write path or its timing is still wrong.
    if (g_cfg.mode == 2 && g_disp->CmdClearColorImage)
    {
        VkClearColorValue mag = {};
        mag.float32[0] = 10.0f; mag.float32[2] = 10.0f; mag.float32[3] = 1.0f;
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        g_disp->CmdClearColorImage(c, game_out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &mag, 1, &rr);
    }

    if (!direct)
        VkImgBarrier(c, src.image, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    VkImgBarrier(c, game_out, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

static void RecordCopyOut(VkCommandBuffer c, VkImage game_out, VkFormat game_fmt, UINT w, UINT h)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_disp->BeginCommandBuffer(c, &bi);
    RecordCopyOutBody(c, game_out, game_fmt, w, h);
    g_disp->EndCommandBuffer(c);
}

// ---- which evaluates the bridge mirrors ------------------------------------
// Every NGX Vulkan evaluate arrives through the same entry point whatever
// feature it belongs to, and DLSS Ray Reconstruction (DLSSD) hands over the
// very same Color / Depth / MotionVectors / Output resources super sampling
// does -- so reading those four cannot tell the two apart. What only Ray
// Reconstruction carries is its denoiser guidance: albedo, normals, roughness,
// the view matrices.
//
// This matters because the bridge's private feature is SuperSampling. Mirroring
// a Ray Reconstruction frame onto it upscales the still-noisy colour input and
// then copies that over the game's properly denoised output -- on screen,
// exactly as if denoising had been switched off (reported on RTX Remix, where
// super resolution worked and Ray Reconstruction came out noisy). So recognise
// those frames and leave them entirely alone.
//
// Both the "DLSSD."-prefixed spellings and the older bare ones are probed, and
// the key that matched is logged once so a title that still slips through can
// be identified from its log alone.
static const char *const kRayReconstructionKeys[] = {
    "DLSSD.NormalRoughness",     "DLSSD.Normals",           "DLSSD.Roughness",
    "DLSSD.DiffuseAlbedo",       "DLSSD.SpecularAlbedo",     "DLSSD.SpecularHitDistance",
    "DLSSD.WorldToViewMatrix",   "DLSSD.ViewToClipMatrix",
    "NormalRoughness",           "GBuffer.Normals",          "GBuffer.Roughness",
    "DiffuseAlbedo",             "SpecularAlbedo",           "SpecularHitDistance",
    "WorldToViewMatrix",         "ViewToClipMatrix",
};

static const char *BridgeRayReconstructionInput(const NVSDK_NGX_Parameter *p)
{
    if (p == nullptr) return nullptr;
    for (const char *k : kRayReconstructionKeys)
    {
        void *v = nullptr;
        if (p->Get(k, &v) == NGX_SUCCESS && v != nullptr) return k;
    }
    return nullptr;
}

// ---- evaluate hook ---------------------------------------------------------
// Called (from the NGX detour in the main .cpp) after the game's own evaluate
// has been forwarded. Reads the contract, (re)builds if needed, records the
// input copies into the game's command buffer and arms the pending handoff.
static void BridgeVkFrameLocked(VkCommandBuffer cmd, const NVSDK_NGX_Parameter *p,
                                const GameRes &col, const GameRes &out,
                                const GameRes &dep, const GameRes &mv);
static void ArmFamilyProbe(VkCommandBuffer cmd);

static void BridgeVkFrame(VkCommandBuffer cmd, const NVSDK_NGX_Parameter *p)
{
    // A teardown requested while ON the worker thread (BridgeDisable can be
    // reached from inside the evaluate core) was deferred; finish it here on
    // the game's thread, where joining the worker is legal.
    if (g_release_deferred)
    { InterlockedExchange(&g_release_deferred, 0); FrameLock(); D3D12ReleaseResources(); FrameUnlock(); }

    if (g_bridge.disabled || cmd == VK_NULL_HANDLE || p == nullptr) return;
    if (BridgeFrameGenerationActive()) return;   // DLSS-G owns the frame's pacing;
                                                 // announced once by the latch
    if (BridgeInFaultCooldown()) return;         // transient-fault pause; announced
                                                 // by the fault handler
    if (g_bridge_hold_until != 0)                // cooldown just expired
    { g_bridge_hold_until = 0; Log("[bridge] fault cooldown over; resuming bridged frames"); }
    if (!EnsureVkDevice(cmd)) { BridgeFail("no vulkan device"); return; }

    GameRes col, out, dep, mv;
    if (!ReadGameRes(p, "Color", &col) || !ReadGameRes(p, "Output", &out) ||
        !ReadGameRes(p, "Depth", &dep) || !ReadGameRes(p, "MotionVectors", &mv))
        return;   // not a DLSS super-sampling evaluate we can mirror; stand aside

    if (const char *rr = BridgeRayReconstructionInput(p))
    {
        static bool said = false;
        if (!said)
        { said = true;
          Log("[bridge] Ray Reconstruction evaluate (carries \"%s\"); standing aside. The bridge "
              "mirrors DLSS super sampling only -- the game's denoised frame is left untouched.", rr); }
        return;
    }

    // Hold off before touching the first frame. When DLSS is (re)enabled the
    // game brings its features up in a burst -- SuperSampling first, evaluates
    // flowing immediately, FrameGeneration a few milliseconds later. The X4
    // retest log for v0.1.9 shows the whole race: first frame bridged at
    // .537, the FG create latch at .544, dead at FG's next present. Creation-
    // time latching alone loses by those 7 ms, so the first bridgeable
    // evaluate only starts a clock: every frame is forwarded untouched until
    // the grace expires, and an FG create landing inside it (400x margin)
    // means no frame was ever touched. The residual risk is FG enabled in the
    // menu of a session that has ALREADY been bridging for longer than the
    // grace -- the latch still stops the next frame, but the transition frame
    // may collide once; the relaunch is then protected end to end.
    {
        const ULONGLONG kArmGraceMs = 3000;
        static ULONGLONG first_tick;
        if (first_tick == 0)
        {
            first_tick = GetTickCount64();
            Log("[bridge] first bridgeable evaluate; forwarding untouched for %llu ms in case "
                "Frame Generation comes up in the same settings burst", kArmGraceMs);
        }
        if (GetTickCount64() - first_tick < kArmGraceMs)
        {
            // Nothing is recorded during the grace, but the submit carrying
            // this very command buffer tells which queue family runs the
            // DLSS pass -- learn it now, so the first real frame already
            // knows whether a blit is allowed.
            if (g_bridge.vk_queue_flags == 0) { FrameLock(); ArmFamilyProbe(cmd); FrameUnlock(); }
            return;
        }
    }

    if (!g_bridge.session_ready)
    {
        if (!D3D12InitSession(g_bridge.game_luid, g_bridge.luid_valid)) return;   // disables on failure
        // The timeline-semaphore imports drive the asynchronous copy-back; the
        // in-frame handoff never waits on them. A host whose device lacks
        // VK_KHR_external_semaphore_win32 (the ReShade add-on) skips them.
        if (kHostHasTimelineImport || !g_cfg.sync)
        {
            if (!VkImportFence(&g_bridge.sem_in,  g_shared_fence_in,  "fence_in") ||
                !VkImportFence(&g_bridge.sem_out, g_shared_fence_out, "fence_out"))
            { BridgeDisable("could not import the shared fences into Vulkan"); return; }
        }
    }
    EnsureSyncObjects();
    if (!kHostAsyncFallback && !g_sync_active)
    { BridgeDisable("the in-frame handoff could not be set up and this host has no asynchronous fallback"); return; }

    FrameLock();
    BridgeVkFrameLocked(cmd, p, col, out, dep, mv);
    FrameUnlock();
}

// ---- slot bookkeeping (under g_frame_cs) -----------------------------------
// Whether the GPU is done with everything a slot recorded. Nothing here waits.
static bool SlotRetired(Bridge::VkFrame &fr)
{
    if (fr.submitted && fr.retire && g_disp->GetFenceStatus)
    {
        if (g_disp->GetFenceStatus(g_bridge.device, fr.retire) != VK_SUCCESS) return false;
        g_disp->ResetFences(g_bridge.device, 1, &fr.retire);
        fr.submitted = false;
        return true;
    }
    if (fr.abandoned) return true;
    return fr.ev_done && g_disp->GetEventStatus(g_bridge.device, fr.ev_done) == VK_EVENT_SET;
}

// Slots that still reference bridge resources on the GPU, or may yet.
static int BridgeSlotsBusy()
{
    int n = 0;
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        const Bridge::VkFrame &fr = g_bridge.vkframe[i];
        if (g_sync_active) { if (fr.in_flight) ++n; }
        else if (fr.submitted && fr.retire && g_disp->GetFenceStatus &&
                 g_disp->GetFenceStatus(g_bridge.device, fr.retire) != VK_SUCCESS) ++n;
    }
    if (!g_sync_active && g_bridge.pending) ++n;
    return n;
}

// The worker's verdict on one frame, applied on the game's thread.
static void BridgeJudge(Bridge::VkFrame &fr, int slot)
{
    g_worker_healthy = false;
    switch (fr.result)
    {
    case 0:
        g_bridge.consecutive_fails = 0;
        g_worker_healthy = true;
        if (dlss_bridge::IsStrictNeural(g_cfg) &&
            InterlockedCompareExchange(&g_neural_only_latched, 1, 0) == 0)
            Log("[policy] neural-only mode latched after the first completed private result; "
                "game-side DLSS fallback is now disabled for this process");
        break;
    case 2:  Log("[bridge] disabling to protect the game");
             BridgeDisable("NGX raised an exception in the D3D12 evaluate"); return;
    case 1:  BridgeFail("evaluate"); break;             // frame_ready drops; rebuilt next frame
    case 4:  BridgeFail("d3d12 command list"); break;
    case 32:
        // The transfer/evaluate is still retiring. In latched strict mode the
        // sandwich copies the last completed neural image; it never restores
        // the current game-side DLSS result.
        Log("[policy] neural result missed its slot; repeating the last neural image");
        break;
    case 8:
        if (fr.abandoned)
        {
            // A discarded recording says nothing about the bridge.
            static int said = 0;
            if (said++ < 3)
                Log("[bridge] slot %d: the game re-recorded command buffer %p before its sandwich ran; "
                    "that frame is dropped", slot, (void *)fr.cmd);
        }
        else
        {
            static int said = 0;
            if (said++ < 5)
                Log("[bridge] slot %d: the GPU did not reach our copies in command buffer %p within %llu ms "
                    "(%s; %lld unrelated submit(s) seen meanwhile). Its bridged output is dropped.",
                    slot, (void *)fr.cmd, (unsigned long long)kInputsTimeoutMs,
                    fr.submitted ? "its submit was seen" : "no submit carrying it was seen",
                    (long long)g_bridge.unmatched_submits);
            BridgeFail("input handoff");
        }
        break;
    case 16: Log("[bridge] the D3D12 evaluate did not finish within 1 s");
             BridgeFail("evaluate completion"); break;
    }
}

// Runs at every evaluate: apply the verdicts the worker has delivered since
// the last one, and release the slots whose sandwich the GPU has finished.
static void BridgeCollectSlots()
{
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        Bridge::VkFrame &fr = g_bridge.vkframe[i];
        if (g_sync_active)
        {
            if (!fr.in_flight || !fr.worker_done) continue;
            if (!fr.judged)
            {
                fr.judged = true;
                BridgeJudge(fr, i);
                if (g_bridge.disabled) return;
            }
            if (fr.matching || !SlotRetired(fr)) continue;
            fr.in_flight = false;
        }
        else if (fr.submitted && fr.retire && g_disp->GetFenceStatus &&
                 g_disp->GetFenceStatus(g_bridge.device, fr.retire) == VK_SUCCESS)
        {
            g_disp->ResetFences(g_bridge.device, 1, &fr.retire);
            fr.submitted = false;
        }
    }
}

static void RecountSlots();
static void NoteFrameBridged(UINT64 v);

static void BridgeVkFrameLocked(VkCommandBuffer cmd, const NVSDK_NGX_Parameter *p,
                                const GameRes &col, const GameRes &out,
                                const GameRes &dep, const GameRes &mv)
{
    BridgeCollectSlots();
    if (g_bridge.disabled) return;

    // The async path drives one frame at a time: while the previous one's
    // submit has not been seen, stand aside -- and give up on it after a few
    // evaluates, since a submit that has not shown up by then never will.
    if (!g_sync_active && g_bridge.pending)
    {
        if (++g_bridge.pending_skips < 4) { RecountSlots(); return; }
        Log("[bridge] no submit carrying command buffer %p was seen within %d evaluates; that frame is dropped",
            (void *)g_bridge.pending_cmd, g_bridge.pending_skips);
        g_bridge.pending = false;
        g_bridge.pending_skips = 0;
    }

    if (ContractChanged(col, out, dep, mv))
    {
        // Never release what a recorded sandwich still references: hold the
        // rebuild (forwarding frames untouched) until every slot has retired.
        const int busy = BridgeSlotsBusy();
        if (busy > 0)
        {
            if (g_bridge.rebuild_busy++ == 0)
                Log("[bridge] resources must be rebuilt but %d bridged frame(s) are still in flight; "
                    "forwarding frames untouched until they retire", busy);
            if (g_bridge.rebuild_busy > 600)
                BridgeDisable("bridged frames never retired (their command buffers were never seen to complete)");
            RecountSlots();
            return;
        }
        g_bridge.rebuild_busy = 0;
        if (!RebuildResources(p, col, out, dep, mv)) { RecountSlots(); return; }
    }
    if (!g_bridge.frame_ready) { RecountSlots(); return; }

    // A free slot -- or strict mode repeats the last neural image. Never wait
    // here: the sandwich that still owns the slot may be parked on the GPU behind
    // work this very thread has not submitted yet.
    const int slot = g_bridge.vk_slot;
    Bridge::VkFrame &fr = g_bridge.vkframe[slot];
    if (g_sync_active ? fr.in_flight : (fr.submitted && fr.retire))
    {
        if (g_bridge.no_slot_skips++ == 0)
            Log(dlss_bridge::IsStrictNeural(g_cfg) && g_neural_only_latched
                ? "[bridge] no free frame slot (%d bridged frames in flight); repeating the last neural image. "
                  "Repeats silently while the GPU stays more than %d frames behind."
                : "[bridge] no free frame slot (%d bridged frames in flight); forwarding this frame untouched. "
                  "Repeats silently while the GPU stays more than %d frames behind.",
                Bridge::kFrames, Bridge::kFrames);
        if (g_sync_active && dlss_bridge::IsStrictNeural(g_cfg) && g_neural_only_latched)
        {
            // Queue order keeps the shared image stable: the preceding admitted
            // command waits for its D3D12 writer, while a following admitted
            // command cannot release its input event until after this copy.
            NoteFrameTouched();
            RecordCopyOutBody(cmd, out.image, out.fmt, out.w, out.h);
        }
        RecountSlots();
        return;
    }
    if (g_sync_active)   // slot retired: nothing on the GPU references its events
    {
        g_disp->ResetEvent(g_bridge.device, fr.ev_inputs);
        g_disp->ResetEvent(g_bridge.device, fr.ev_output);
        g_disp->ResetEvent(g_bridge.device, fr.ev_done);
    }

    // vkCmdBlitImage needs a graphics queue. Which family runs the DLSS pass
    // is only known from a submit carrying one of its command buffers, so
    // nothing that would need a blit is recorded before that: the buffer is
    // handed to the submit matcher untouched and the family read off its
    // submit (normally settled during the start-up grace, at no cost). On a
    // family without graphics (idTech 8: async compute) formats that would
    // still need a blit cannot be bridged at all. v0.1.13 recorded its first
    // sandwich, blit included, before the family was known -- that blit on
    // idTech's compute queue is what froze the game (issue #3).
    VkFormat cvk; DXGI_FORMAT cdx; bool cblit; MapColor(col.fmt, &cvk, &cdx, &cblit);
    const bool mvblit     = mv.fmt  != g_bridge.tex[SLOT_MV].fmtvk;
    const bool outblit    = out.fmt != g_bridge.tex[SLOT_OUTPUT].fmtvk;
    const bool needs_blit = cblit || mvblit || outblit;
    if (needs_blit && g_bridge.vk_queue_flags == 0)
    {
        ArmFamilyProbe(cmd);
        RecountSlots();
        return;
    }
    if (needs_blit && !(g_bridge.vk_queue_flags & VK_QUEUE_GRAPHICS_BIT))
    {
        static bool said = false;
        if (!said)
        { said = true;
          Warn("the game submits DLSS on a queue family without graphics (flags 0x%X) and its formats "
               "(color %d, mv %d, output %d) would need a blit, which such a queue cannot run; frames "
               "are forwarded untouched.", g_bridge.vk_queue_flags, (int)col.fmt, (int)mv.fmt, (int)out.fmt); }
        RecountSlots();
        return;
    }

    const bool float_depth = IsFloatDepth(dep.fmt);
    bool depth_compute = false;
    VkImageView depth_view = VK_NULL_HANDLE;
    if (!float_depth)
    {
        depth_view    = GetSampleableDepthView(fr, dep);
        depth_compute = depth_view != VK_NULL_HANDLE &&
                        EnsureDepthPipeline() && g_depth_store_view != VK_NULL_HANDLE;
        static bool said = false;
        if (!said) { said = true;
            if (depth_compute)
                Log("[bridge] depth format %d (aspect 0x%X) fed through the compute conversion pass%s",
                    (int)dep.fmt, dep.aspect,
                    depth_view == dep.view ? "" : " via a bridge-made depth-only view");
            else
                Warn("depth format %d cannot be copied or sampled (view=%p aspect=0x%X); DLSS runs "
                     "WITHOUT depth -- expect ghosting.", (int)dep.fmt, (void *)dep.view, dep.aspect); }
    }

    FrameScalars scalars;
    CaptureFrameScalars(p, &scalars);
    if (!g_sync_active) ApplyFrameScalars(&scalars);   // consumed on this thread at the submit

    // From here the bridge records into the game's own command buffer -- the
    // session's history is no longer FG-compatible (see NoteVkFeatureCreate).
    NoteFrameTouched();

    // Record the input copies straight into the game's command buffer.
    CopyColorLike(cmd, col, SLOT_COLOR, cblit);
    CopyColorLike(cmd, mv, SLOT_MV, mvblit);
    if (float_depth)        CopyDepth(cmd, dep, slot);
    else if (depth_compute) DispatchDepthConvert(cmd, depth_view, dep.w, dep.h, slot);
    RecordDirectInputCopies(cmd);

    // During discovery/bootstrap the game output seeds the shared target. Once
    // strict neural-only mode latches, preserve the last completed neural image
    // instead. A missed result then repeats neural output rather than silently
    // displaying the game's original upscaler.
    if (g_sync_active && !g_neural_only_latched)
        CopyColorLike(cmd, out, SLOT_OUTPUT, outblit);

    fr.cmd       = cmd;
    fr.primary   = VK_NULL_HANDLE;
    fr.matching  = false;
    InterlockedExchange(&fr.abandoned, 0);

    if (g_sync_active)
    {
        // The sandwich: tell the worker the inputs are on the GPU, park the
        // game's command buffer until the worker reports the D3D12 result,
        // then copy that result back in place -- all before the game's own
        // post-processing (recorded right after this hook) reads the output.
        // ev_done at the tail is how the slot learns it has retired without
        // depending on any submit being seen.
        g_disp->CmdSetEvent(cmd, fr.ev_inputs, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        g_disp->CmdWaitEvents(cmd, 1, &fr.ev_output, VK_PIPELINE_STAGE_HOST_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 1, &mb, 0, nullptr, 0, nullptr);
        RecordCopyOutBody(cmd, out.image, out.fmt, out.w, out.h);
        g_disp->CmdSetEvent(cmd, fr.ev_done, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        fr.value     = ++g_bridge.timeline;
        fr.scalars   = scalars;
        fr.result    = 0;
        fr.judged    = false;
        fr.in_flight = true;
        InterlockedExchange(&fr.worker_done, 0);
        g_bridge.vk_slot = (slot + 1) % Bridge::kFrames;
        RecountSlots();
        WorkQueuePush(slot);          // armed now; no submit has to be guessed
        NoteFrameBridged(fr.value);
    }
    else
    {
        g_bridge.pending           = true;
        g_bridge.pending_slot      = slot;
        g_bridge.pending_skips     = 0;
        g_bridge.pending_cmd       = cmd;
        g_bridge.pending_primary   = VK_NULL_HANDLE;
        g_bridge.pending_out_image = out.image;
        g_bridge.pending_out_fmt   = out.fmt;
        g_bridge.pending_out_w     = out.w;
        g_bridge.pending_out_h     = out.h;
        RecountSlots();
    }
}

// ---- the queue-submit handoff ---------------------------------------------
// Cheap gates for the layer hooks, recounted under g_frame_cs whenever slot
// state changes: how many slots still await the submit carrying their buffer,
// and how many are between record and retire at all.
static volatile LONG g_match_wanted;
static volatile LONG g_slots_in_flight;

static void RecountSlots()
{
    LONG wanted = 0, flying = 0;
    if (g_sync_active)
        for (int i = 0; i < Bridge::kFrames; ++i)
        {
            const Bridge::VkFrame &fr = g_bridge.vkframe[i];
            if (!fr.in_flight) continue;
            ++flying;
            if (!fr.submitted && !fr.matching) ++wanted;
        }
    else if (g_bridge.pending) { ++wanted; ++flying; }
    if (g_bridge.probe_cmd != VK_NULL_HANDLE) { ++wanted; ++flying; }
    InterlockedExchange(&g_match_wanted, wanted);
    InterlockedExchange(&g_slots_in_flight, flying);
}
static bool BridgeWantsSubmitMatch() { return g_match_wanted != 0; }

static void NoteFrameBridged(UINT64 v)
{
    const UINT64 n = ++g_bridge.frames_done;
    // Liveness in the default log; per-frame detail only when configured.
    if (n == 1)
        Log(g_sync_active
            ? "[bridge] first frame bridged: in-frame sandwich armed (copies -> D3D12 evaluate -> in-place copy-back)"
            : "[bridge] first frame bridged end to end: input copies, D3D12 evaluate and "
              "output copy-back queued on the GPU");
    if (n == 100 || n % 1000 == 0 || (g_cfg.verbose && (n <= 10 || n % 60 == 0)))
        Log("[bridge] %llu frames bridged (timeline %llu; %llu forwarded untouched for lack of a free "
            "slot; %lld unrelated submits passed while a match was awaited)",
            (unsigned long long)n, (unsigned long long)v,
            (unsigned long long)g_bridge.no_slot_skips, (long long)g_bridge.unmatched_submits);
    if (n == 100 || n % 1000 == 0)
        D3D12ProbeEvalDetour("at heartbeat");
}

// game_eval=0 (see the config): whether this frame's game-side evaluate can be
// skipped. Only in the sync sandwich (the async copy-back is never seen, so
// there the game's evaluate IS the picture), only in mode 0, only while the
// last judged worker frame was clean, and only when the frame path is certain
// to record: a free slot, no rebuild pending. frame_ready drops on any
// BridgeFail, so one bad frame closes the gate before the next evaluate.
static bool BridgeSkipGameEvaluate(const NVSDK_NGX_Parameter *p)
{
    const bool strict_latched = dlss_bridge::IsStrictNeural(g_cfg) &&
                                g_neural_only_latched != 0;
    if (!(g_cfg.mode == 0 && g_sync_active && !BridgeFrameGenerationActive()))
        return false;
    // Strict mode stays neural-only after its first completed result. Slot
    // pressure records a copy of the last neural image, so it does not reopen
    // the original game evaluate. Legacy game_eval=0 retains its conservative gate.
    if (!strict_latched && !(g_cfg.game_eval == 0 && g_worker_healthy &&
          !g_bridge.disabled && g_bridge.frame_ready && !BridgeInFaultCooldown()))
        return false;
    GameRes col, out, dep, mv;
    if (!ReadGameRes(p, "Color", &col) || !ReadGameRes(p, "Output", &out) ||
        !ReadGameRes(p, "Depth", &dep) || !ReadGameRes(p, "MotionVectors", &mv))
        return false;
    FrameLock();
    const bool contract_ok = g_bridge.rebuild_busy == 0 && !ContractChanged(col, out, dep, mv);
    const bool ok = contract_ok && (strict_latched ||
                    !g_bridge.vkframe[g_bridge.vk_slot].in_flight);
    FrameUnlock();
    return ok;
}

static bool SubmitSignal(VkQueue queue, VkSemaphore sem, UINT64 value)
{
    VkTimelineSemaphoreSubmitInfo ts = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    ts.signalSemaphoreValueCount = 1;
    ts.pSignalSemaphoreValues    = &value;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext                = &ts;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &sem;
    g_internal_submit = 1;
    VkResult r = g_disp->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    g_internal_submit = 0;
    return r == VK_SUCCESS;
}

// Fence-only submit, queued behind the game's own: the fence signals once the
// game's command buffer (carrying the whole sandwich) has fully retired.
static bool SubmitFenceOnly(VkQueue queue, VkFence fence)
{
    g_internal_submit = 1;
    VkResult r = g_disp->QueueSubmit(queue, 0, nullptr, fence);
    g_internal_submit = 0;
    return r == VK_SUCCESS;
}

static bool SubmitCopyOut(VkQueue queue, VkCommandBuffer cmd, VkSemaphore sem, UINT64 value, VkFence retire)
{
    VkTimelineSemaphoreSubmitInfo ts = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    ts.waitSemaphoreValueCount = 1;
    ts.pWaitSemaphoreValues    = &value;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext                = &ts;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &sem;
    si.pWaitDstStageMask    = &stage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    g_internal_submit = 1;
    VkResult r = g_disp->QueueSubmit(queue, 1, &si, retire);
    g_internal_submit = 0;
    return r == VK_SUCCESS;
}

// ---- submit matching --------------------------------------------------------
static bool SlotWantsMatch(const Bridge::VkFrame &fr, int i)
{
    if (g_sync_active) return fr.in_flight && !fr.submitted && !fr.matching;
    return g_bridge.pending && g_bridge.pending_slot == i && !fr.matching;
}
static bool SlotOwnsBuffer(const Bridge::VkFrame &fr, VkCommandBuffer cb)
{
    if (g_sync_active) return cb == fr.cmd || (fr.primary && cb == fr.primary);
    return cb == g_bridge.pending_cmd || (g_bridge.pending_primary && cb == g_bridge.pending_primary);
}

// ---- the queue-family probe (under g_frame_cs) -----------------------------
// A command buffer the evaluate hook saw but recorded nothing into. The submit
// that carries it names the queue family the DLSS pass runs on, which decides
// whether a blit may ever be recorded. Re-armed with the newest buffer on
// every untouched evaluate; dropped when the game re-records that buffer.
static const uint32_t kProbeBit = 1u << 31;
static void NoteMatchedQueue(VkQueue queue, uint32_t family, bool tracked, bool via_primary);

static void ArmFamilyProbe(VkCommandBuffer cmd)
{
    if (cmd == VK_NULL_HANDLE || g_bridge.probe_matching || g_bridge.probe_cmd == cmd) return;
    static bool said = false;
    if (!said) { said = true;
        Log("[bridge] learning which queue family runs the DLSS pass from its own submit "
            "before recording anything into its command buffer"); }
    g_bridge.probe_cmd     = cmd;
    g_bridge.probe_primary = VK_NULL_HANDLE;
    RecountSlots();
}

static void ResolveFamilyProbe(VkQueue queue, uint32_t family, bool tracked)
{
    NoteMatchedQueue(queue, family, tracked, g_bridge.probe_primary != VK_NULL_HANDLE);
    if (g_bridge.vk_queue_flags == 0)
    {
        // An untracked queue, or no family properties to read: nothing says
        // the family lacks graphics, so blits stay allowed as before v0.1.13.
        g_bridge.vk_queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        Log("[bridge] the capabilities of the queue family running the DLSS pass could not be "
            "read; assuming it can run a blit");
    }
    g_bridge.probe_cmd     = VK_NULL_HANDLE;
    g_bridge.probe_primary = VK_NULL_HANDLE;
}

static uint32_t ClaimBuffer(VkCommandBuffer cb)
{
    uint32_t mask = 0;
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        Bridge::VkFrame &fr = g_bridge.vkframe[i];
        if (SlotWantsMatch(fr, i) && SlotOwnsBuffer(fr, cb)) { fr.matching = true; mask |= 1u << i; }
    }
    if (g_bridge.probe_cmd != VK_NULL_HANDLE && !g_bridge.probe_matching &&
        (cb == g_bridge.probe_cmd || (g_bridge.probe_primary && cb == g_bridge.probe_primary)))
    { g_bridge.probe_matching = true; mask |= kProbeBit; }
    return mask;
}
// Before the game's submit goes down the chain: claim the slots whose buffers
// it carries, so no slot can be retired and reused under the submit (the GPU
// may finish the buffer before this thread gets to queue its fence behind it).
static uint32_t BridgeMatchSubmit(uint32_t count, const VkSubmitInfo *submits)
{
    uint32_t mask = 0;
    if (submits == nullptr || g_bridge.device == VK_NULL_HANDLE) return 0;
    FrameLock();
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < submits[i].commandBufferCount && submits[i].pCommandBuffers; ++j)
            mask |= ClaimBuffer(submits[i].pCommandBuffers[j]);
    if (mask) RecountSlots();
    FrameUnlock();
    return mask;
}
static uint32_t BridgeMatchSubmit2(uint32_t count, const VkSubmitInfo2 *submits)
{
    uint32_t mask = 0;
    if (submits == nullptr || g_bridge.device == VK_NULL_HANDLE) return 0;
    FrameLock();
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < submits[i].commandBufferInfoCount && submits[i].pCommandBufferInfos; ++j)
            mask |= ClaimBuffer(submits[i].pCommandBufferInfos[j].commandBuffer);
    if (mask) RecountSlots();
    FrameUnlock();
    return mask;
}

// "queue family 2" / "an untracked queue" / "a queue (family index not
// exposed by the host)" in the log lines that name a queue.
static const char *DescribeQueue(char *buf, size_t n, uint32_t family, bool tracked)
{
    if (family == kUnknownFamily)
        snprintf(buf, n, "%s", tracked ? "a queue (family index not exposed by the host)" : "an untracked queue");
    else
        snprintf(buf, n, tracked ? "queue family %u" : "an untracked queue, family %u", family);
    return buf;
}
// For a host that learns about submits one command buffer at a time (the
// ReShade add-on's execute_command_list event).
static uint32_t BridgeMatchOne(VkCommandBuffer cb)
{
    if (cb == VK_NULL_HANDLE || g_bridge.device == VK_NULL_HANDLE) return 0;
    FrameLock();
    const uint32_t mask = ClaimBuffer(cb);
    if (mask) RecountSlots();
    FrameUnlock();
    return mask;
}
static void BridgeNoteUnmatchedSubmit(uint32_t family, bool tracked)
{
    const LONG64 n = InterlockedIncrement64(&g_bridge.unmatched_submits);
    char where[64];
    if (g_cfg.verbose && n <= 8)
        Log("[bridge] a submit on %s passed while bridged work still awaited its own submit (%lld so far)",
            DescribeQueue(where, sizeof where, family, tracked), (long long)n);
}

#ifndef DLSS5VK_ADDON_HOST
static uint32_t QueueFamilyFlags(uint32_t family)
{
    InstanceDispatch *in = BridgeInstance();
    if (in == nullptr || in->GetPhysicalDeviceQueueFamilyProperties == nullptr ||
        g_bridge.phys == VK_NULL_HANDLE) return 0;
    VkQueueFamilyProperties props[16];
    uint32_t n = 16;
    in->GetPhysicalDeviceQueueFamilyProperties(g_bridge.phys, &n, props);
    return family < n ? props[family].queueFlags : 0;
}
static uint32_t HostQueueFamilyFlags(VkQueue, uint32_t family, bool tracked)
{
    return tracked ? QueueFamilyFlags(family) : 0;
}
#else
// ReShade hands out each queue's capabilities with the queue itself.
static uint32_t HostQueueFamilyFlags(VkQueue queue, uint32_t, bool)
{
    return AddonQueueFlags(queue);
}
#endif
static void NoteMatchedQueue(VkQueue queue, uint32_t family, bool tracked, bool via_primary)
{
    g_bridge.vk_queue = queue;
    static bool said = false;
    if (said) return;
    said = true;
    const uint32_t flags = HostQueueFamilyFlags(queue, family, tracked);
    if (flags) g_bridge.vk_queue_flags = flags;
    char where[64];
    Log("[bridge] the DLSS command buffer is submitted on %s (%s%s%s%s)%s; %lld unrelated submit(s) "
        "had passed before it",
        DescribeQueue(where, sizeof where, family, tracked),
        flags ? "" : "flags unknown",
        flags & VK_QUEUE_GRAPHICS_BIT ? "graphics " : "",
        flags & VK_QUEUE_COMPUTE_BIT  ? "compute "  : "",
        flags & VK_QUEUE_TRANSFER_BIT ? "transfer"  : "",
        via_primary ? " -- via the primary that executes it" : "",
        (long long)g_bridge.unmatched_submits);
}

// Sync path, one matched slot: its sandwich is on the queue now. Drop a
// fence-only submit behind it to learn precisely when the slot has retired
// (ev_done is the fallback when no submit is ever seen).
static void SyncSlotSubmitted(VkQueue queue, uint32_t family, bool tracked, int slot)
{
    Bridge::VkFrame &fr = g_bridge.vkframe[slot];
    NoteMatchedQueue(queue, family, tracked, fr.primary != VK_NULL_HANDLE);
    // A host that learns of the submit before it goes down (the add-on) must
    // not queue anything: a fence submitted ahead of the sandwich would signal
    // before it ran and the slot would be recycled under it. The slot then
    // retires through ev_done at the sandwich's tail instead.
    if (!kHostSubmitsBehindGame) return;
    if (!EnsureRetireFences()) return;
    if (fr.retire && !fr.submitted && SubmitFenceOnly(queue, fr.retire)) fr.submitted = true;
}

// Async path (sync = 0): drive the frame now that its inputs are on the queue.
static void AsyncDriveFrame(VkQueue queue, uint32_t family, bool tracked, int slot)
{
    g_bridge.pending = false;
    if (g_bridge.disabled || !g_bridge.frame_ready) return;
    if (!tracked)
    {
        static bool said = false;
        if (!said) { said = true;
            Log("[bridge] the DLSS command buffer is submitted on a queue the layer never saw handed "
                "out; the async copy-back cannot follow it (use sync = 1)"); }
        return;
    }
    if (!EnsureVkFrames(family)) { BridgeFail("vk frame resources"); return; }
    if (g_bridge.vk_queue_family != family)
    {
        static bool said = false;
        if (!said) { said = true;
            Log("[bridge] the DLSS command buffer is submitted on queue family %u but the bridge's "
                "command pool is on family %u; the async copy-back is skipped (use sync = 1)",
                family, g_bridge.vk_queue_family); }
        return;
    }
    NoteMatchedQueue(queue, family, tracked, g_bridge.pending_primary != VK_NULL_HANDLE);

    Bridge::VkFrame &fr = g_bridge.vkframe[slot];
    const UINT64 v = ++g_bridge.timeline;

    // 1. inputs are ready once the game's submit (carrying our copies) drains.
    if (!SubmitSignal(queue, g_bridge.sem_in, v)) { BridgeFail("signal fence_in"); return; }

    // 2. the D3D12 evaluate: wait fence_in, evaluate (add-on inserts here),
    //    signal fence_out.
    if (!D3D12RunEvaluate(v)) return;   // logs / disables internally

    // 3. copy the neural result back into the game's Output image.
    VkCommandBuffer c = fr.copy_out;
    g_disp->ResetCommandBuffer(c, 0);
    RecordCopyOut(c, g_bridge.pending_out_image, g_bridge.pending_out_fmt,
                  g_bridge.pending_out_w, g_bridge.pending_out_h);
    if (!SubmitCopyOut(queue, c, g_bridge.sem_out, v, fr.retire))
    { BridgeFail("copy-back submit"); return; }

    fr.submitted = true;
    g_bridge.consecutive_fails = 0;
    g_bridge.vk_slot = (slot + 1) % Bridge::kFrames;
    NoteFrameBridged(v);
}

// Runs on the submitting thread, right after a submit carrying bridged work
// (claimed by BridgeMatchSubmit) has been forwarded down the chain.
static void BridgeOnGameSubmitDone(VkQueue queue, uint32_t family, bool tracked, uint32_t slots, bool ok)
{
    FrameLock();
    if (slots & kProbeBit)
    {
        g_bridge.probe_matching = false;
        if (ok) ResolveFamilyProbe(queue, family, tracked);
    }
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        if (!(slots & (1u << i))) continue;
        Bridge::VkFrame &fr = g_bridge.vkframe[i];
        fr.matching = false;
        if (!ok) continue;                     // the submit failed: nothing went out
        if (g_sync_active) SyncSlotSubmitted(queue, family, tracked, i);
        else               AsyncDriveFrame(queue, family, tracked, i);
    }
    RecountSlots();
    FrameUnlock();
}

// The game begins a command buffer. One the bridge has work in that was never
// submitted is being re-recorded, i.e. that recording is gone: release the
// slot (and stop the worker waiting on it) without ever seeing a submit.
static void BridgeOnCommandBufferBegin(VkCommandBuffer cb)
{
    if (g_slots_in_flight == 0 || cb == VK_NULL_HANDLE) return;
    FrameLock();
    bool changed = false;
    if (g_bridge.probe_cmd == cb && !g_bridge.probe_matching)
    { g_bridge.probe_cmd = VK_NULL_HANDLE; g_bridge.probe_primary = VK_NULL_HANDLE; changed = true; }
    for (int i = 0; i < Bridge::kFrames; ++i)
    {
        Bridge::VkFrame &fr = g_bridge.vkframe[i];
        if (fr.matching) continue;             // a submit carrying it is under way
        if (g_sync_active)
        {
            if (!fr.in_flight || fr.submitted || fr.abandoned || cb != fr.cmd) continue;
            InterlockedExchange(&fr.abandoned, 1);
            changed = true;
        }
        else if (g_bridge.pending && g_bridge.pending_slot == i && cb == g_bridge.pending_cmd)
        {
            g_bridge.pending = false;
            changed = true;
            static bool said = false;
            if (!said) { said = true;
                Log("[bridge] the game re-recorded command buffer %p before submitting it; that frame is dropped",
                    (void *)cb); }
        }
    }
    if (changed) RecountSlots();
    FrameUnlock();
}

// The DLSS pass lives in a secondary command buffer: the submit to look for
// is the one carrying its primary.
static void BridgeOnExecuteCommands(VkCommandBuffer primary, uint32_t count, const VkCommandBuffer *secondaries)
{
    if (g_match_wanted == 0 || secondaries == nullptr) return;
    FrameLock();
    bool found = false;
    for (uint32_t k = 0; k < count; ++k)
    {
        if (g_bridge.probe_cmd != VK_NULL_HANDLE && !g_bridge.probe_matching && secondaries[k] == g_bridge.probe_cmd)
        { g_bridge.probe_primary = primary; found = true; }
        for (int i = 0; i < Bridge::kFrames; ++i)
        {
            Bridge::VkFrame &fr = g_bridge.vkframe[i];
            if (g_sync_active)
            {
                if (!fr.in_flight || fr.submitted || fr.matching || secondaries[k] != fr.cmd) continue;
                fr.primary = primary;
            }
            else
            {
                if (!g_bridge.pending || g_bridge.pending_slot != i || secondaries[k] != g_bridge.pending_cmd) continue;
                g_bridge.pending_primary = primary;
            }
            found = true;
        }
    }
    FrameUnlock();
    if (found)
    {
        static bool said = false;
        if (!said) { said = true;
            Log("[bridge] the DLSS pass is recorded into a secondary command buffer; the submit matcher "
                "now looks for its primary %p", (void *)primary); }
    }
}

// ---- teardown --------------------------------------------------------------
static void VkReleaseSharedTex(int slot)
{
    SharedTex &t = g_bridge.tex[slot];
    VkDevice dev = g_bridge.device;
    if (dev && g_disp)
    {
        if (slot == SLOT_DEPTH && g_depth_store_view && g_disp->DestroyImageView)
        { g_disp->DestroyImageView(dev, g_depth_store_view, nullptr); g_depth_store_view = VK_NULL_HANDLE; }
        if (t.image)  g_disp->DestroyImage(dev, t.image, nullptr);
        if (t.memory) g_disp->FreeMemory(dev, t.memory, nullptr);
    }
    t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE; t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (t.nt_handle) { CloseHandle(t.nt_handle); t.nt_handle = nullptr; }
    if (t.tex12) { t.tex12->Release(); t.tex12 = nullptr; }
}

// Full teardown. Forward-declared in d3d12_session.inc and reached from
// BridgeDisable, so it must be safe at any stage of bring-up (any handle may be
// null) and release both the Vulkan aliases and the D3D12 session.
static void D3D12ReleaseResources()
{
    // BridgeDisable can be reached from the worker itself (the evaluate core's
    // command-list bring-up can disable on a GPU stall). The worker cannot
    // join itself; park the request and let the game thread run the release.
    if (g_worker_thread && GetCurrentThreadId() == g_worker_tid)
    { InterlockedExchange(&g_release_deferred, 1); return; }

    // Stop the sync worker first: it may be mid-use of the D3D12 queue, and
    // the game's submitted command buffers may be parked on our events.
    if (g_worker_thread)
    {
        InterlockedExchange(&g_worker_stop, 1);
        SetEvent(g_worker_wake);
        if (WaitForSingleObject(g_worker_thread, 2000) != WAIT_OBJECT_0)
            Log("[bridge] the sync worker did not stop within 2 s");
        CloseHandle(g_worker_thread); g_worker_thread = nullptr; g_worker_tid = 0;
    }
    if (g_worker_wake)        { CloseHandle(g_worker_wake);        g_worker_wake = nullptr; }
    if (g_worker_fence_event) { CloseHandle(g_worker_fence_event); g_worker_fence_event = nullptr; }
    g_wq_head = g_wq_tail = 0;
    const bool was_sync = g_sync_active;
    g_sync_active = false;
    g_worker_healthy = false;

    // Unpark any WaitEvents still on the GPU, then give those command buffers
    // a bounded moment to retire before their events (and everything else)
    // are destroyed: the fence where a submit was seen, ev_done otherwise.
    if (g_bridge.device && g_disp)
        for (int i = 0; i < Bridge::kFrames; ++i)
        {
            Bridge::VkFrame &fr = g_bridge.vkframe[i];
            if (fr.ev_output && g_disp->SetEvent) g_disp->SetEvent(g_bridge.device, fr.ev_output);
        }
    if (g_bridge.device && g_disp)
    {
        const ULONGLONG t0 = GetTickCount64();
        for (int i = 0; i < Bridge::kFrames; ++i)
        {
            Bridge::VkFrame &fr = g_bridge.vkframe[i];
            if (fr.submitted && fr.retire)
            {
                g_disp->WaitForFences(g_bridge.device, 1, &fr.retire, VK_TRUE, 1000ull * 1000 * 1000);
                fr.submitted = false;
            }
            else if (was_sync && fr.in_flight && !fr.abandoned && fr.ev_done && g_disp->GetEventStatus)
                while (g_disp->GetEventStatus(g_bridge.device, fr.ev_done) == VK_EVENT_RESET &&
                       GetTickCount64() - t0 < 1000)
                    Sleep(1);
            fr.in_flight = false;
            fr.matching  = false;
        }
    }

    D3D12DrainGpu();
    VkDevice dev = g_bridge.device;

    if (g_bridge.feature && g_bridge.release_feature)
    { g_bridge.release_feature(g_bridge.feature); g_bridge.feature = nullptr; }

    D3D12ReleaseMultiGpuFrameResources();
    for (int i = 0; i < SLOT_COUNT; ++i) VkReleaseSharedTex(i);

    if (dev && g_disp)
    {
        for (int i = 0; i < Bridge::kFrames; ++i)
        {
            Bridge::VkFrame &fr = g_bridge.vkframe[i];
            if (fr.retire)            { g_disp->DestroyFence(dev, fr.retire, nullptr);         fr.retire = VK_NULL_HANDLE; }
            if (fr.depth_scratch)     { g_disp->DestroyBuffer(dev, fr.depth_scratch, nullptr); fr.depth_scratch = VK_NULL_HANDLE; }
            if (fr.depth_scratch_mem) { g_disp->FreeMemory(dev, fr.depth_scratch_mem, nullptr);fr.depth_scratch_mem = VK_NULL_HANDLE; }
            if (fr.ev_inputs && g_disp->DestroyEvent) { g_disp->DestroyEvent(dev, fr.ev_inputs, nullptr); }
            if (fr.ev_output && g_disp->DestroyEvent) { g_disp->DestroyEvent(dev, fr.ev_output, nullptr); }
            if (fr.ev_done   && g_disp->DestroyEvent) { g_disp->DestroyEvent(dev, fr.ev_done,   nullptr); }
            fr.ev_inputs = VK_NULL_HANDLE; fr.ev_output = VK_NULL_HANDLE; fr.ev_done = VK_NULL_HANDLE;
            if (fr.game_depth_view && g_disp->DestroyImageView)
            { g_disp->DestroyImageView(dev, fr.game_depth_view, nullptr); fr.game_depth_view = VK_NULL_HANDLE; }
            fr.copy_out = VK_NULL_HANDLE; fr.submitted = false; fr.depth_scratch_size = 0;
            fr.cmd = VK_NULL_HANDLE; fr.primary = VK_NULL_HANDLE;
            fr.in_flight = false; fr.matching = false; fr.judged = false;
            InterlockedExchange(&fr.worker_done, 0);
            InterlockedExchange(&fr.abandoned, 0);
        }
        DestroyDepthPipeline();
        for (int direction = 0; direction < CROSS_COUNT; ++direction)
        {
            if (g_mgpu.cross_vk[direction])
            { g_disp->DestroyBuffer(dev, g_mgpu.cross_vk[direction], nullptr); g_mgpu.cross_vk[direction] = VK_NULL_HANDLE; }
            if (g_mgpu.cross_vk_memory[direction])
            { g_disp->FreeMemory(dev, g_mgpu.cross_vk_memory[direction], nullptr); g_mgpu.cross_vk_memory[direction] = VK_NULL_HANDLE; }
        }
        g_mgpu.direct_vulkan_upload = false;
        g_mgpu.direct_vulkan_download = false;
        if (g_bridge.vk_pool) { g_disp->DestroyCommandPool(dev, g_bridge.vk_pool, nullptr); g_bridge.vk_pool = VK_NULL_HANDLE; }
        if (g_bridge.sem_in)  { g_disp->DestroySemaphore(dev, g_bridge.sem_in,  nullptr);   g_bridge.sem_in  = VK_NULL_HANDLE; }
        if (g_bridge.sem_out) { g_disp->DestroySemaphore(dev, g_bridge.sem_out, nullptr);   g_bridge.sem_out = VK_NULL_HANDLE; }
    }
    g_bridge.vk_ready = false;

    if (g_shared_fence_in)  { CloseHandle(g_shared_fence_in);  g_shared_fence_in  = nullptr; }
    if (g_shared_fence_out) { CloseHandle(g_shared_fence_out); g_shared_fence_out = nullptr; }

    #define REL(x) if (g_bridge.x) { g_bridge.x->Release(); g_bridge.x = nullptr; }
    REL(fence_in); REL(fence_out); REL(fence_gpu);
    REL(timing_readback); REL(timing_heap);
    g_bridge.timing_frequency = 0;
    REL(list);
    for (int i = 0; i < Bridge::kFrames; ++i) REL(alloc[i]);
    REL(queue); REL(adapter3); REL(dev12);
    #undef REL
    if (g_bridge.fence_event) { CloseHandle(g_bridge.fence_event); g_bridge.fence_event = nullptr; }

    #define MREL(x) if (g_mgpu.x) { g_mgpu.x->Release(); g_mgpu.x = nullptr; }
    MREL(game_fence); MREL(game_list); MREL(game_alloc); MREL(game_queue);
    MREL(neural_download_list); MREL(neural_download_alloc);
    MREL(neural_copy_list); MREL(neural_copy_alloc); MREL(neural_copy_queue); MREL(game_dev);
    #undef MREL
    if (g_mgpu.game_event) { CloseHandle(g_mgpu.game_event); g_mgpu.game_event = nullptr; }
    g_mgpu.active = false; g_mgpu.game_value = 0;
    g_mgpu_pending_neural = 0; g_mgpu_dropped_outputs = 0;
    g_mgpu_pending_game = 0; g_mgpu_game_repeats = 0; g_mgpu_output_timeout = false;

    g_bridge.session_ready = false;
    g_bridge.frame_ready   = false;
    g_bridge.pending       = false;
    g_bridge.rebuild_busy  = 0;
    g_bridge.probe_cmd = VK_NULL_HANDLE; g_bridge.probe_primary = VK_NULL_HANDLE; g_bridge.probe_matching = false;
    RecountSlots();
}

// The game is about to destroy a VkDevice (called from the layer, before the
// real vkDestroyDevice). If it is the device the bridge attached to, everything
// the bridge holds on it -- imported images and semaphores, the command pool,
// the per-frame events, and a worker that may still be mid-evaluate -- has to
// be released while the device is still alive; releasing after it dies is how
// these bridges hang or crash on quit. The bridge is left re-armable, so a game
// that simply recreates its device (a resolution or HDR change) is picked up
// again on its next evaluate.
static void BridgeDeviceLost(VkDevice dev)
{
    if (dev == VK_NULL_HANDLE || dev != g_bridge.device) return;
    Log("[bridge] the game is destroying the Vulkan device the bridge is attached to; "
        "releasing the bridge first");
    FrameLock();   // the command-buffer hooks may still be looking at the slots
    D3D12ReleaseResources();
    g_bridge.device     = VK_NULL_HANDLE;
    g_bridge.phys       = VK_NULL_HANDLE;
    g_bridge.instance   = VK_NULL_HANDLE;
    g_bridge.pending    = false;
    g_bridge.vk_slot    = 0;
    g_bridge.vk_queue_flags = 0;
    g_disp              = nullptr;
    g_sync_init_done    = false;   // the events are re-created on the next device
    g_depth_pipe_failed = false;
    FrameUnlock();
}
