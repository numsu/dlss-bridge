// The D3D12 capture half of the bridge: capture the game's NGX D3D12 Super
// Resolution contract, move pixels across the game/private D3D12 boundary
// through shared NT handles, run the private D3D12 NGX evaluate (the call
// OptiScaler detours), and copy the neural result back before downstream
// rendering.
//
// v1 is synchronous and same-adapter only, mirroring the D3D11 path: the game
// hands the bridge an open ID3D12GraphicsCommandList, so copy-in is recorded
// into that list at Evaluate time, the game thread host-blocks while the
// private session evaluates on its own device/queue, and copy-back is recorded
// into the still-open list before Evaluate returns. The game thread itself is
// the barrier, so no Vulkan-style submit-split handoff is needed and temporal
// order falls out of the hook's own serialisation. Split-screen viewports
// interleave freely: each owns a private D3D12 feature and shared textures
// keyed by game NGX handle identity.
//
// Same-API reentrancy is the one hazard the Vulkan/D3D11 paths never face:
// the game and the private session export the same NVSDK_NGX_D3D12_* names.
// Three layers keep them apart: (1) TryHookModule never patches the
// session-local OptiScaler module, so the private session's own calls travel
// through unhooked code; (2) the private call below runs under a thread-local
// in-bridge guard that makes any re-entered detour forward without bridging;
// (3) bridge-owned D3D12 feature handles never appear in the game contract
// table, so a stray entry still cannot arm a frame.
//
// v1 limits, all fail-safe (stand aside, forward untouched): float depth only
// (R32_FLOAT/R32_TYPELESS; no compute depth conversion yet), same-adapter
// only (secondary-GPU + D3D12 capture stands aside), direct lists (bundles
// are not classified).

#pragma once

struct D3d12GameRes
{
    ID3D12Resource *res;
    UINT w, h;
    DXGI_FORMAT fmt;
};

static bool ReadD3d12Res(const NVSDK_NGX_Parameter *p, const char *key, D3d12GameRes *out)
{
    if (!p || !out) return false;
    ID3D12Resource *res = nullptr;
    if (p->Get(key, &res) != NGX_SUCCESS || !res) return false;
    D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;
    if (!desc.Width || !desc.Height) return false;
    out->res = res;
    out->w = (UINT)desc.Width;
    out->h = desc.Height;
    out->fmt = desc.Format;
    return true;
}

// v1 depth policy: the shared depth texture is R32_FLOAT and CopyResource
// requires identical formats, so only float depth bridges. Anything else is
// forwarded untouched; a compute conversion pass is future work mirroring
// Vulkan's DispatchDepthConvert.
static bool D3d12DepthBridgeable(DXGI_FORMAT fmt)
{
    return fmt == DXGI_FORMAT_R32_FLOAT || fmt == DXGI_FORMAT_R32_TYPELESS;
}

// Per-viewport game-device aliases of the bridge-created shared textures.
// Opened lazily via ID3D12Device::OpenSharedResource; released with the
// viewport. (Neural-device textures need no game-side alias.)
static ID3D12Resource *g_d3d12_alias[MAX_VIEWPORTS][SLOT_COUNT] = {};
// RR input aliases (declared here, before the deferred-release pool that
// retires them, rather than at the RR capture code further down).
static ID3D12Resource *g_rr_alias[MAX_VIEWPORTS][RR_SLOT_COUNT] = {};
static ID3D12Device *g_d3d12_game_device = nullptr;
// Completion fence on the game's device, signalled on the game's own queues
// after every accepted submission (see the queue-observation section below).
// Retired generations and private feature releases may only happen after the
// covering value completes.
static ID3D12Fence *g_d3d12_game_fence = nullptr;
static volatile LONG64 g_d3d12_game_fence_next = 1;
// Rendering-activity signal for the DLSS-off idle notice: every intercepted
// submission on a hooked queue, counted even before any NGX feature exists
// (the game fence is only created on the first evaluate, so its covering
// value cannot serve this purpose). Probes submit far fewer lists than the
// gate threshold; real rendering reaches it within seconds.
static volatile LONG64 g_d3d12_submissions_observed = 0;

static void D3d12ReleaseViewportAliases(int vp)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return;
    for (int s = 0; s < SLOT_COUNT; ++s)
        if (g_d3d12_alias[vp][s]) { g_d3d12_alias[vp][s]->Release(); g_d3d12_alias[vp][s] = nullptr; }
}

// Full game-device teardown: called from D3D12ReleaseResources (forward-
// declared in vk_interop.inc) so global teardown does not leak the adopted
// game device reference or its shared aliases.
static void D3d12ReleaseAllGameAliases()
{
    for (int v = 0; v < MAX_VIEWPORTS; ++v) D3d12ReleaseViewportAliases(v);
    if (g_d3d12_game_fence) { g_d3d12_game_fence->Release(); g_d3d12_game_fence = nullptr; }
    if (g_d3d12_game_device) { g_d3d12_game_device->Release(); g_d3d12_game_device = nullptr; }
    // The queue-observation vtable patches are append-only and outlive the
    // device: their objects are the game's, and restoring a dead object's
    // vptr would be a use-after-free. The copies live until process exit.
}

// ---- game-queue submission observation -------------------------------------
// The fence above is signalled only on the game's own queues, and only after
// the game has accepted a submission: that is the only order D3D12 gives us,
// and it is exactly the order "every list submitted so far has executed"
// needs. A command list never exposes its queue, so the queue is observed
// through its creator: the D3D12CreateDevice hook (application IAT, injected
// host) reports each device at creation, and per-object vtable slot patches
// then intercept the device's CreateCommandQueue and each queue's
// ExecuteCommandLists. A patch re-points ONE live object's vptr at a private
// copy of its vtable with a single slot swapped; no system module code is
// modified. The patches are append-only and are never restored: an object the
// game has already freed must never be written to (restoring a dead object's
// vptr is a use-after-free), and a freed object's memory cannot keep
// dispatching into our detour, because any reuse overwrites the vptr first.
// The ~1 KB vtable copies therefore live until process exit. If the create
// hook is absent (hosts without IAT patching), no submission is ever
// observed, the covering value stays 0, and retirement falls back to the
// legacy immediate release.
struct D3d12VtableSlotPatch
{
    void **object;
    void **original_vptr;
    void **vtable_copy;
    int    slot;
    void  *original_entry;
};
struct D3d12QueueHook
{
    D3d12VtableSlotPatch patch;
    ID3D12Device *device; // device the queue was created on (not owned)
};
static D3d12VtableSlotPatch g_d3d12_device_hooks[8] = {};
static volatile LONG        g_d3d12_device_hook_count;
static D3d12QueueHook       g_d3d12_queue_hooks[8] = {};
static volatile LONG        g_d3d12_queue_hook_count;

static bool D3d12PatchObjectSlot(void *object, int slot, void *detour,
                                 D3d12VtableSlotPatch *p)
{
    if (!object || !detour) return false;
    void **vptr = *reinterpret_cast<void ** *>(object);
    if (!vptr) return false;
    const size_t entries = 128; // far beyond any d3d12 object vtable; the
                                // over-read stays inside d3d12.dll's .rdata
    void **copy = reinterpret_cast<void **>(VirtualAlloc(nullptr,
        entries * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!copy) return false;
    if (slot < 0 || slot >= (int)entries) { VirtualFree(copy, 0, MEM_RELEASE); return false; }
    memcpy(copy, vptr, entries * sizeof(void *));
    p->object          = reinterpret_cast<void **>(object);
    p->original_vptr   = vptr;
    p->vtable_copy     = copy;
    p->slot            = slot;
    p->original_entry  = vptr[slot];
    copy[slot]         = detour;
    InterlockedExchangePointer(reinterpret_cast<void *volatile *>(object), copy);
    return true;
}

// vtable slot indices, 0-based, from the SDK d3d12.h vtbl layout (the vtbl
// structs only exist in C mode of the header, so the offsets are pinned here
// and verified against it):
//   ID3D12Device.CreateCommandQueue      = 8  (IUnknown 3 + ID3D12Object 4 + GetNodeCount)
//   ID3D12CommandQueue.ExecuteCommandLists = 10 (IUnknown 3 + ID3D12Object 4 + GetDevice
//                                            + UpdateTileMappings + CopyTileMappings)
static const int kD3d12CreateCommandQueueSlot = 8;
static const int kD3d12ExecuteCommandListsSlot = 10;

static void D3d12HookGameQueue(ID3D12CommandQueue *queue);

static HRESULT STDMETHODCALLTYPE D3d12DetourCreateCommandQueue(
    ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue **out)
{
    void *original = nullptr;
    for (int i = 0; i < (int)_countof(g_d3d12_device_hooks); ++i)
        if (g_d3d12_device_hooks[i].object == reinterpret_cast<void **>(device))
            { original = g_d3d12_device_hooks[i].original_entry; break; }
    // original is always found: the detour is only reachable through the
    // vtable copy that its saved record owns (the patch and the record are
    // written together, and object-memory reuse overwrites the vptr before
    // any dispatch). E_FAIL below is a dead-code tripwire, not a path.
    const HRESULT hr = original
        ? reinterpret_cast<decltype(&D3d12DetourCreateCommandQueue)>(original)(device, type, out)
        : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) D3d12HookGameQueue(*out);
    return hr;
}

// Every accepted submission orders behind a fresh fence value: when that
// value completes, every list submitted up to and including this one has
// executed. Only queues of the ADOPTED game device feed the fence, so queues
// of a replaced device (or a probe) can never signal a stale fence.
static void STDMETHODCALLTYPE D3d12DetourExecuteCommandLists(
    ID3D12CommandQueue *queue, UINT list_count, ID3D12CommandList *const *lists)
{
    D3d12QueueHook *hook = nullptr;
    for (int i = 0; i < (int)_countof(g_d3d12_queue_hooks); ++i)
        if (g_d3d12_queue_hooks[i].patch.object == reinterpret_cast<void **>(queue))
            { hook = &g_d3d12_queue_hooks[i]; break; }
    if (hook)
        reinterpret_cast<decltype(&D3d12DetourExecuteCommandLists)>(
            hook->patch.original_entry)(queue, list_count, lists);
    ID3D12Fence *fence = g_d3d12_game_fence;
    if (fence && queue && hook && hook->device == g_d3d12_game_device)
        queue->Signal(fence, InterlockedIncrement64(&g_d3d12_game_fence_next) - 1);
    if (hook)
        MaybeLogDlssOff((unsigned long long)InterlockedIncrement64(
                            &g_d3d12_submissions_observed),
                        "game-queue submissions");
}

static void D3d12HookGameQueue(ID3D12CommandQueue *queue)
{
    if (!queue) return;
    for (int i = 0; i < (int)_countof(g_d3d12_queue_hooks); ++i)
        if (g_d3d12_queue_hooks[i].patch.object == reinterpret_cast<void **>(queue)) return;
    ID3D12Device *dev = nullptr;
    if (FAILED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&dev))) || !dev) return;
    if (g_d3d12_queue_hook_count >= (LONG)_countof(g_d3d12_queue_hooks)) return;
    if (!D3d12PatchObjectSlot(queue, kD3d12ExecuteCommandListsSlot,
                              (void *)D3d12DetourExecuteCommandLists,
                              &g_d3d12_queue_hooks[g_d3d12_queue_hook_count].patch))
        return;
    g_d3d12_queue_hooks[g_d3d12_queue_hook_count].device = dev;
    ++g_d3d12_queue_hook_count;
    static volatile LONG hook_log = 0;
    if (InterlockedIncrement(&hook_log) == 1)
        Log("[d3d12] game-queue submission tracking active (queue completion fence)");
}

// From the D3D12CreateDevice hook (injected host only): every device created
// in this process is a candidate game device (probe devices get replaced by
// the real one; their queues simply stop feeding the fence through the
// adoption check). Append-only: a dead device is never touched again.
static void D3d12NoteGameDeviceCreated(ID3D12Device *device)
{
    if (!device || g_d3d12_in_bridge) return; // the private session's own device
    for (int i = 0; i < (int)_countof(g_d3d12_device_hooks); ++i)
        if (g_d3d12_device_hooks[i].object == reinterpret_cast<void **>(device)) return;
    if (g_d3d12_device_hook_count >= (LONG)_countof(g_d3d12_device_hooks)) return;
    if (!D3d12PatchObjectSlot(device, kD3d12CreateCommandQueueSlot,
                              (void *)D3d12DetourCreateCommandQueue,
                              &g_d3d12_device_hooks[g_d3d12_device_hook_count]))
    {
        static volatile LONG said = 0;
        if (InterlockedIncrement(&said) == 1)
            Log("[d3d12] device command-queue tracking unavailable; "
                "retirement uses the legacy release policy");
        return;
    }
    ++g_d3d12_device_hook_count;
}

// The fence value covering every submission accepted so far (0 = nothing
// observed: no in-flight game work, legacy release applies).
static UINT64 D3d12CoveringFenceValue()
{
    return g_d3d12_game_fence_next > 1 ? g_d3d12_game_fence_next - 1 : 0;
}

// ---- deferred release of game-referenced generations -----------------------
// The same-adapter D3D12 path records its copies (and the neural evaluate
// itself) into the game's own command lists, which the game submits on its own
// queue. A generation -- or the private feature -- is therefore only safe to
// release once that queue has actually completed the list that referenced it.
// Counting frames cannot prove that, and D3D12DrainGpu only covers the
// bridge's private queue, so completion is tracked on the real consuming
// queue: a fence created on the game device is signalled with
// ID3D12CommandQueue::Signal after every submission the game accepts
// (observed through the queue-observation section above). A generation --
// or the private feature -- is retired during some frame's evaluate, and its
// last reference always lives in an EARLIER frame's list, which the game has
// already submitted by then (a frame cannot be recorded before the previous
// one was presented, and presenting requires submission) -- so the fence
// value of the most recent accepted submission covers it. A retired
// generation carries that covering value; release is a non-blocking
// GetCompletedValue poll per frame, and the bounded waits below
// (rebuild/teardown) log and proceed only after their timeout, when the queue
// has either drained or the game is stuck. Fence completion is monotonic in
// value across queues, so one fence covers split-screen viewports sharing a
// device. If no submission was observed yet (covering value 0) nothing of
// the bridge's work is in flight on the game's queue, and the legacy
// immediate release applies. The depth store view is released immediately
// (bridge timeline, drained by D3D12DrainGpu); only the texture handles are
// deferred.
struct D3d12RetiredSR
{
    SharedTex        tex[SLOT_COUNT];
    ID3D12Resource *alias[SLOT_COUNT];
    bool             valid;
    UINT64           fence_value; // game-queue signal covering this generation's last use
};
struct D3d12RetiredRR
{
    SharedTex        tex[RR_SLOT_COUNT];
    ID3D12Resource *alias[RR_SLOT_COUNT];
    bool             valid;
    UINT64           fence_value; // game-queue signal covering this generation's last use
};
static D3d12RetiredSR g_d3d12_retired_sr[MAX_VIEWPORTS];
static D3d12RetiredRR g_d3d12_retired_rr[MAX_VIEWPORTS];

// True once the game's queue has completed everything the given signal covers
// (a zero value means no signal was ever recorded, so nothing can be pending).
static bool D3d12GameFenceComplete(UINT64 value)
{
    return value == 0 || g_d3d12_game_fence == nullptr ||
           g_d3d12_game_fence->GetCompletedValue() >= value;
}

// Bounded wait on the game's queue. Returns true on completion; on timeout the
// caller logs and proceeds -- at that point the queue has either drained or
// the game is wedged, and parking the viewport forever would be the worse
// failure.
static bool D3d12WaitGameFence(UINT64 value, DWORD timeout_ms)
{
    if (D3d12GameFenceComplete(value)) return true;
    const DWORD start = GetTickCount();
    while (!D3d12GameFenceComplete(value))
    {
        if (GetTickCount() - start >= timeout_ms) return false;
        Sleep(1);
    }
    return true;
}

static void D3d12ReleaseRetiredSR(int vp)
{
    D3d12RetiredSR &r = g_d3d12_retired_sr[vp];
    if (!r.valid) return;
    r.valid = false;
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (r.alias[i]) { r.alias[i]->Release(); r.alias[i] = nullptr; }
        VkReleaseSharedTexHandles(&r.tex[i]);
    }
}

static void D3d12ReleaseRetiredRR(int vp)
{
    D3d12RetiredRR &r = g_d3d12_retired_rr[vp];
    if (!r.valid) return;
    r.valid = false;
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        if (r.alias[s]) { r.alias[s]->Release(); r.alias[s] = nullptr; }
        VkReleaseSharedTexHandles(&r.tex[s]);
    }
}

// Release the previously retired generation only after the game's queue has
// completed the signal covering it (bounded wait; the timeout logs). Then move
// the current generation's handles into the pool tagged with the signal that
// covers their last use.
static void D3d12RetireSR(int vp)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return;
    BridgeViewport &v = g_bridge.viewports[vp];
    if (g_d3d12_retired_sr[vp].valid)
    {
        if (!D3d12WaitGameFence(g_d3d12_retired_sr[vp].fence_value, 5000))
            Log("[d3d12] vp%d game queue did not complete within 5 s before retiring a prior "
                "generation; releasing it anyway", vp);
        D3d12ReleaseRetiredSR(vp);
    }
    if (g_depth_store_view[vp] && g_disp && g_disp->DestroyImageView && g_bridge.device)
    { g_disp->DestroyImageView(g_bridge.device, g_depth_store_view[vp], nullptr); g_depth_store_view[vp] = VK_NULL_HANDLE; }
    D3d12RetiredSR &r = g_d3d12_retired_sr[vp];
    r.valid = true;
    r.fence_value = D3d12CoveringFenceValue();
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        r.tex[i] = v.tex[i];           v.tex[i] = SharedTex{};
        r.alias[i] = g_d3d12_alias[vp][i]; g_d3d12_alias[vp][i] = nullptr;
    }
}

static void D3d12RetireRR(int vp)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return;
    BridgeViewport &v = g_bridge.viewports[vp];
    if (g_d3d12_retired_rr[vp].valid)
    {
        if (!D3d12WaitGameFence(g_d3d12_retired_rr[vp].fence_value, 5000))
            Log("[d3d12] vp%d game queue did not complete within 5 s before retiring a prior "
                "RR generation; releasing it anyway", vp);
        D3d12ReleaseRetiredRR(vp);
    }
    D3d12RetiredRR &r = g_d3d12_retired_rr[vp];
    r.valid = true;
    r.fence_value = D3d12CoveringFenceValue();
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        r.tex[s] = v.rr_tex[s];            v.rr_tex[s] = SharedTex{};
        r.alias[s] = g_rr_alias[vp][s];    g_rr_alias[vp][s] = nullptr;
    }
}

// Per-frame (non-blocking) release of retired generations whose covering
// signal has completed on the game's queue.
static void D3d12AgeRetire(int vp)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return;
    if (g_d3d12_retired_sr[vp].valid && D3d12GameFenceComplete(g_d3d12_retired_sr[vp].fence_value))
        D3d12ReleaseRetiredSR(vp);
    if (g_d3d12_retired_rr[vp].valid && D3d12GameFenceComplete(g_d3d12_retired_rr[vp].fence_value))
        D3d12ReleaseRetiredRR(vp);
}

// At teardown the game is exiting; give each pending generation a bounded
// moment to drain, then free whatever remains.
static void D3d12FlushRetiredAll()
{
    for (int vp = 0; vp < MAX_VIEWPORTS; ++vp)
    {
        if (g_d3d12_retired_sr[vp].valid &&
            !D3d12WaitGameFence(g_d3d12_retired_sr[vp].fence_value, 2500))
            Log("[d3d12] vp%d game queue still incomplete at teardown; releasing the retired "
                "generation anyway", vp);
        D3d12ReleaseRetiredSR(vp);
        if (g_d3d12_retired_rr[vp].valid &&
            !D3d12WaitGameFence(g_d3d12_retired_rr[vp].fence_value, 2500))
            Log("[d3d12] vp%d game queue still incomplete at teardown; releasing the retired RR "
                "generation anyway", vp);
        D3d12ReleaseRetiredRR(vp);
    }
}

static bool D3d12EnsureDevice(ID3D12GraphicsCommandList *list)
{
    if (!list) return false;
    ID3D12Device *dev = nullptr;
    if (FAILED(list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&dev))) || !dev)
        return false;
    if (g_d3d12_game_device && g_d3d12_game_device != dev)
    {
        Log("[d3d12] game device changed; releasing D3D12 shared aliases");
        D3d12FlushRetiredAll(); // the retired aliases live on the old device
        for (int v = 0; v < MAX_VIEWPORTS; ++v) D3d12ReleaseViewportAliases(v);
        if (g_d3d12_game_fence)
        { g_d3d12_game_fence->Release(); g_d3d12_game_fence = nullptr; }
        g_d3d12_game_device->Release();
        g_d3d12_game_device = nullptr;
        // The old queues die with the old device, so nothing of the old
        // generations can still be in flight; the new fence restarts at 1.
    }
    if (!g_d3d12_game_device)
    {
        g_d3d12_game_device = dev; // adopted (no Release: owned by the game)
        g_d3d12_game_device->AddRef();
        dev->Release();
        D3D12HostNoteInit();
        if (!g_d3d12_game_fence &&
            FAILED(g_d3d12_game_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                     __uuidof(ID3D12Fence),
                                                     reinterpret_cast<void **>(&g_d3d12_game_fence))) ||
            !g_d3d12_game_fence)
        {
            Log("[d3d12] could not create the game-queue completion fence; generation "
                "retirement falls back to the bounded wait timeout");
            g_d3d12_game_fence = nullptr;
        }
        else
            g_d3d12_game_fence_next = 1;
    }
    else
        dev->Release();
    // Match the private D3D12 session to the game's adapter via its LUID so
    // shared NT handles open on the same physical GPU. D3D12 devices do not
    // expose IDXGIDevice (that query always fails); the LUID comes straight
    // from ID3D12Device::GetAdapterLuid instead.
    if (g_d3d12_game_device)
    {
        const LUID adapter_luid = g_d3d12_game_device->GetAdapterLuid();
        memcpy(g_bridge.game_luid, &adapter_luid,
               sizeof(LUID) < VK_LUID_SIZE ? sizeof(LUID) : VK_LUID_SIZE);
        g_bridge.luid_valid = true;
    }
    return true;
}

static ID3D12Device *D3D12GameDevice()
{
    return g_d3d12_game_device;
}

static bool D3d12EnsureAlias(int vp, int slot)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS || slot < 0 || slot >= SLOT_COUNT) return false;
    if (g_d3d12_alias[vp][slot] || !g_d3d12_game_device) return g_d3d12_alias[vp][slot] != nullptr;
    HANDLE h = g_bridge.viewports[vp].tex[slot].nt_handle;
    if (!h) return false;
    ID3D12Resource *alias = nullptr;
    HRESULT hr = g_d3d12_game_device->OpenSharedHandle(h, __uuidof(ID3D12Resource),
                                                       reinterpret_cast<void **>(&alias));
    if (FAILED(hr) || !alias)
    {
        Log("[d3d12] vp%d %s OpenSharedHandle failed 0x%08X", vp, kSlotName[slot], hr);
        return false;
    }
    g_d3d12_alias[vp][slot] = alias;
    return true;
}

// ---- Ray Reconstruction denoiser-guidance transport (D3D12 only) ----------
// RR inputs ride the same evaluate as Super Sampling (integrated DLSS 3.5
// mode) or a dedicated feature-13 handle; either way they resolve to a
// viewport like everything else. Each slot probes several historical key
// spellings (DLSSD.* vs GBuffer.* vs bare) and remembers the hit, so the
// private bind mirrors the game's own spelling back instead of guessing a
// canonical name. Shared textures are created in the game's own format, so
// CopyResource is always identical-format and no conversion pass is needed.
// Specular hit-distance is optional: absent slots are simply unbound.
// Key spellings are probed in order and the hit is mirrored back, so the
// list is a deliberate superset: GBuffer.* names are SDK-defined, DLSSD.*
// come from the DLSS 3.5 integration guides, and DLSS.Input.* is probed
// opportunistically for newer integrations (absent keys simply miss).
struct RRSlotProbe { const char *label; const char *keys[7]; };
static const RRSlotProbe kRRProbes[RR_SLOT_COUNT] = {
    { "DiffuseAlbedo",  { "DLSSD.DiffuseAlbedo", "GBuffer.DiffuseAlbedo", "DiffuseAlbedo", "DLSS.Input.DiffuseAlbedo", nullptr, nullptr, nullptr } },
    { "SpecularAlbedo", { "DLSSD.SpecularAlbedo", "GBuffer.SpecularAlbedo", "SpecularAlbedo", "DLSS.Input.SpecularAlbedo", nullptr, nullptr, nullptr } },
    { "Normals",        { "DLSSD.Normals", "DLSSD.NormalRoughness", "GBuffer.Normals", "NormalRoughness", nullptr, nullptr, nullptr } },
    { "Roughness",      { "DLSSD.Roughness", "GBuffer.Roughness", nullptr, nullptr, nullptr, nullptr, nullptr } },
    { "SpecularMvec",   { "GBuffer.SpecularMvec", "DLSSD.SpecularMvec", "MotionVectorsReflection", nullptr, nullptr, nullptr, nullptr } },
    { "HitDistance",    { "DLSSD.SpecularHitDistance", "RayTracingHitDistance", "SpecularHitDistance", nullptr, nullptr, nullptr, nullptr } },
};

// Capture RR texture inputs, recording the matched key per slot. Returns the
// hit count; a zero means an SR-only frame.
static int CaptureRRInputs(const NVSDK_NGX_Parameter *p, D3d12GameRes *res, const char **keys)
{
    int hits = 0;
    if (!p) return 0;
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        res[s] = {};
        keys[s] = nullptr;
        for (const char *const *k = kRRProbes[s].keys; *k; ++k)
        {
            ID3D12Resource *r = nullptr;
            if (p->Get(*k, &r) != NGX_SUCCESS || !r) continue;
            D3D12_RESOURCE_DESC desc = r->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue;
            if (!desc.Width || !desc.Height) continue;
            res[s].res = r;
            res[s].w = (UINT)desc.Width;
            res[s].h = desc.Height;
            res[s].fmt = desc.Format;
            keys[s] = *k;
            ++hits;
            break;
        }
    }
    return hits;
}

static bool D3d12EnsureRRAlias(int vp, int slot)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS || slot < 0 || slot >= RR_SLOT_COUNT) return false;
    if (g_rr_alias[vp][slot] || !g_d3d12_game_device) return g_rr_alias[vp][slot] != nullptr;
    HANDLE h = g_bridge.viewports[vp].rr_tex[slot].nt_handle;
    if (!h) return false;
    ID3D12Resource *alias = nullptr;
    HRESULT hr = g_d3d12_game_device->OpenSharedHandle(h, __uuidof(ID3D12Resource),
                                                       reinterpret_cast<void **>(&alias));
    if (FAILED(hr) || !alias)
    {
        Log("[d3d12] vp%d RR %s OpenSharedHandle failed 0x%08X", vp, kRRProbes[slot].label, hr);
        return false;
    }
    g_rr_alias[vp][slot] = alias;
    return true;
}

static void D3d12ReleaseViewportRR(int vp)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return;
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        if (g_rr_alias[vp][s]) { g_rr_alias[vp][s]->Release(); g_rr_alias[vp][s] = nullptr; }
        SharedTex &t = g_bridge.viewports[vp].rr_tex[s];
        if (t.nt_handle) { CloseHandle(t.nt_handle); t.nt_handle = nullptr; }
        if (t.tex12) { t.tex12->Release(); t.tex12 = nullptr; }
        t.image = VK_NULL_HANDLE; t.memory = VK_NULL_HANDLE;
        t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t.w = t.h = 0;
    }
}

static bool D3d12RRContractChanged(int vp, const D3d12GameRes *res, const char **keys)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return true;
    const BridgeViewport &v = g_bridge.viewports[vp];
    if (!v.rr_active) return true;
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        const bool want = res[s].res != nullptr;
        const bool have = v.rr_key[s] != nullptr;
        if (want != have) return true;
        if (!want) continue;
        if (v.rr_key[s] != keys[s]) return true;
        if (v.rr_slot_w[s] != res[s].w || v.rr_slot_h[s] != res[s].h) return true;
        if (v.rr_game_dxgi[s] != res[s].fmt) return true;
    }
    return false;
}

static bool D3d12RebuildRR(int vp, const D3d12GameRes *res, const char **keys)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return false;
    BridgeViewport &v = g_bridge.viewports[vp];
    Log("[d3d12] vp%d (re)building Ray Reconstruction inputs", vp);
    // Fresh parameter block first: removed slots must not keep their old
    // bindings (a released texture must never be evaluated), and vanished
    // key spellings must not linger in the slot metadata either.
    for (int s = 0; s < RR_SLOT_COUNT; ++s) v.rr_key[s] = nullptr;
    if (!D3D12RecreatePrivateParams(vp)) { ViewportFail(vp, "RR parameters"); return false; }
    D3d12RetireRR(vp); // defer the old RR generation's release; the game's
                       // in-flight list may still reference those textures
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        if (!res[s].res) continue;
        // Game-format shared texture: identical-format CopyResource needs no
        // conversion. Read-only for the neural pass, so no UAV.
        if (!D3D12MakeSharedTextureRef(v.rr_tex[s], vp, kRRProbes[s].label,
                                       res[s].w, res[s].h, res[s].fmt, false))
        { ViewportFail(vp, "RR shared texture"); return false; }
        if (!D3d12EnsureRRAlias(vp, s)) { ViewportFail(vp, "RR shared alias"); return false; }
        v.rr_key[s] = keys[s];
        v.rr_slot_w[s] = res[s].w;
        v.rr_slot_h[s] = res[s].h;
        v.rr_game_dxgi[s] = res[s].fmt;
    }
    v.rr_active = true;
    return true;
}

// Bind RR inputs on the private parameter block under the game's own key
// spellings. v1 is same-adapter only (multi-GPU + D3D12 capture stands aside
// before any RR state can exist), so the shared textures bind directly.
static void ApplyRRParamsVp(int vp)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return;
    BridgeViewport &v = g_bridge.viewports[vp];
    NVSDK_NGX_Parameter *q = v.params;
    if (!q || !v.rr_active) return;
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        if (!v.rr_key[s] || !v.rr_tex[s].tex12) continue;
        q->Set(v.rr_key[s], v.rr_tex[s].tex12);
    }
}

static D3D12_RESOURCE_BARRIER D3d12Trans(ID3D12Resource *res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

// The bridge owns every shared texture and every transition recorded against
// one, so COMMON is the exact baseline at frame boundaries. NGX is assumed to
// preserve entry states (it must: vanilla games' subsequent passes depend on
// it), which makes the post-evaluate transitions back to COMMON exact too.
static void D3d12TransitionNeuralForEvaluate(int vp, ID3D12GraphicsCommandList *list)
{
    BridgeViewport &v = g_bridge.viewports[vp];
    D3D12_RESOURCE_BARRIER barriers[SLOT_COUNT + RR_SLOT_COUNT];
    int n = 0;
    // Inputs are read by the neural compute (NON_PIXEL, mirroring the
    // executor's transition logic); a pixel-only state would not permit it.
    const int sr_slots[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    for (int k = 0; k < 3; ++k)
    {
        ID3D12Resource *tex = v.tex[sr_slots[k]].tex12;
        if (tex) barriers[n++] = D3d12Trans(tex, D3D12_RESOURCE_STATE_COMMON,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (v.tex[SLOT_OUTPUT].tex12)
        barriers[n++] = D3d12Trans(v.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_COMMON,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (v.rr_active)
        for (int s = 0; s < RR_SLOT_COUNT; ++s)
            if (v.rr_key[s] && v.rr_tex[s].tex12)
                barriers[n++] = D3d12Trans(v.rr_tex[s].tex12, D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (n) list->ResourceBarrier((UINT)n, barriers);
}

static void D3d12TransitionNeuralToCommon(int vp, ID3D12GraphicsCommandList *list)
{
    BridgeViewport &v = g_bridge.viewports[vp];
    // Inputs are read by the neural compute (NON_PIXEL, mirroring the
    // executor's transition logic); a pixel-only state would not permit it.
    D3D12_RESOURCE_BARRIER barriers[SLOT_COUNT + RR_SLOT_COUNT];
    int n = 0;
    const int sr_slots[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    for (int k = 0; k < 3; ++k)
    {
        ID3D12Resource *tex = v.tex[sr_slots[k]].tex12;
        if (tex) barriers[n++] = D3d12Trans(tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            D3D12_RESOURCE_STATE_COMMON);
    }
    if (v.tex[SLOT_OUTPUT].tex12)
        barriers[n++] = D3d12Trans(v.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_COMMON);
    if (v.rr_active)
        for (int s = 0; s < RR_SLOT_COUNT; ++s)
            if (v.rr_key[s] && v.rr_tex[s].tex12)
                barriers[n++] = D3d12Trans(v.rr_tex[s].tex12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COMMON);
    if (n) list->ResourceBarrier((UINT)n, barriers);
}

static bool D3d12ViewportContractChanged(int vp, const FeatureCreateContract &contract,
                                         const D3d12GameRes &col,
                                         const D3d12GameRes &out, const D3d12GameRes &dep,
                                         const D3d12GameRes &mv)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return true;
    const BridgeViewport &v = g_bridge.viewports[vp];
    return CreateContractDrifted(v.create_contract, contract)
        || !v.frame_ready
        || v.slot_w[SLOT_COLOR] != col.w || v.slot_h[SLOT_COLOR] != col.h
        || v.slot_w[SLOT_OUTPUT] != out.w || v.slot_h[SLOT_OUTPUT] != out.h
        || v.slot_w[SLOT_DEPTH] != dep.w || v.slot_h[SLOT_DEPTH] != dep.h
        || v.slot_w[SLOT_MV] != mv.w || v.slot_h[SLOT_MV] != mv.h
        || v.game_dxgi[SLOT_COLOR] != col.fmt || v.game_dxgi[SLOT_OUTPUT] != out.fmt
        || v.game_dxgi[SLOT_DEPTH] != dep.fmt || v.game_dxgi[SLOT_MV] != mv.fmt;
}

static bool D3d12RebuildViewport(int vp, const FeatureCreateContract &contract,
                                 const D3d12GameRes &col, const D3d12GameRes &out,
                                 const D3d12GameRes &dep, const D3d12GameRes &mv)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return false;
    BridgeViewport &v = g_bridge.viewports[vp];
    Log("[d3d12] vp%d (re)building resources for %ux%u -> %ux%u", vp, col.w, col.h, out.w, out.h);
    D3D12DrainGpu();
    v.frame_ready = false;
    // The private feature was last consumed by the game's queue (the neural
    // evaluate is recorded into the game's list), so its release follows the
    // covering signal, not just the bridge-queue drain above.
    if (v.feature && !D3d12WaitGameFence(D3d12CoveringFenceValue(), 5000))
        Log("[d3d12] vp%d game queue did not complete within 5 s before releasing the "
            "private feature; releasing it anyway", vp);
    if (v.feature && g_bridge.release_feature)
    { g_bridge.release_feature(v.feature); v.feature = nullptr; }
    D3d12RetireSR(vp); // defer the old generation's release; the game's in-flight
                       // list may still reference those shared textures/aliases
    if (InterlockedExchange(&v.neural_latched, 0) != 0)
        Log("[policy] vp%d resource contract changed; rewarming before neural-only relatches", vp);

    struct Plan { int slot; UINT w, h; DXGI_FORMAT dxgi; bool uav; } plan[SLOT_COUNT];
    DXGI_FORMAT cdx; bool cconv; MapDxgiIdentityColor(col.fmt, &cdx, &cconv);
    DXGI_FORMAT odx; bool oconv; MapDxgiIdentityColor(out.fmt, &odx, &oconv);
    DXGI_FORMAT mdx; bool mconv; MapDxgiIdentityColor(mv.fmt, &mdx, &mconv);
    if (cconv || oconv || mconv)
    {
        // CopyResource between incompatible formats is invalid (and the
        // packed-output override only applies when the game already emits it):
        // forward untouched rather than misinterpret data. No conversion pass
        // exists on this path yet.
        static LONG said = 0;
        if (InterlockedExchange(&said, 1) == 0)
            Log("[d3d12] vp%d color/output/mv formats need conversion (game %d/%d/%d); "
                "frames are forwarded untouched until a conversion pass exists",
                vp, (int)col.fmt, (int)out.fmt, (int)mv.fmt);
        ViewportFail(vp, "incompatible formats");
        return false;
    }
    plan[SLOT_COLOR] = { SLOT_COLOR, col.w, col.h, cdx, false };
    plan[SLOT_OUTPUT] = { SLOT_OUTPUT, out.w, out.h, odx, true };
    plan[SLOT_DEPTH] = { SLOT_DEPTH, dep.w, dep.h, DXGI_FORMAT_R32_FLOAT, true };
    plan[SLOT_MV] = { SLOT_MV, mv.w, mv.h, mdx, false };

    for (const Plan &pl : plan)
    {
        if (!D3D12MakeSharedTextureVp(vp, pl.slot, pl.w, pl.h, pl.dxgi, pl.uav))
        { ViewportFail(vp, "shared texture"); return false; }
        if (g_mgpu.active && !D3D12MakeNeuralTextureVp(vp, pl.slot, pl.w, pl.h, pl.dxgi, pl.uav))
        { ViewportFail(vp, "neural texture"); return false; }
        if (!D3d12EnsureAlias(vp, pl.slot)) { ViewportFail(vp, "d3d12 shared alias"); return false; }
        v.slot_w[pl.slot] = pl.w; v.slot_h[pl.slot] = pl.h;
    }
    if (!D3D12BuildCrossBuffer()) { ViewportFail(vp, "cross-adapter buffer"); return false; }
    v.create_contract = contract;
    if (!D3D12RecreatePrivateParams(vp)) { ViewportFail(vp, "allocate parameters"); return false; }
    if (!D3D12CreateDlssFeatureVp(contract, vp)) { ViewportFail(vp, "create feature"); return false; }
    v.game_dxgi[SLOT_COLOR] = col.fmt;
    v.game_dxgi[SLOT_OUTPUT] = out.fmt;
    v.game_dxgi[SLOT_DEPTH] = dep.fmt;
    v.game_dxgi[SLOT_MV] = mv.fmt;
    v.width = col.w; v.height = col.h;
    v.out_width = out.w; v.out_height = out.h;
    v.render_w = col.w; v.render_h = col.h;
    v.ngx_out_w = out.w; v.ngx_out_h = out.h;
    v.frame_ready = true;
    v.consecutive_fails = 0;
    return true;
}

// D3D12 frame: record copy-in into the game's open list, evaluate the private
// neural feature inline on that list, then record copy-back. The game thread
// itself is the barrier: downstream passes recorded after Evaluate returns
// can only read the already-completed neural output.
//
// Returns true only when a valid neural output was written into the game's
// Output resource. The detour relies on this: when the game-side evaluate is
// skipped (latched), a false return means the game's own evaluate must still
// run so downstream rendering never consumes an unwritten output.
static bool BridgeD3D12Frame(ID3D12GraphicsCommandList *list, const NVSDK_NGX_Handle *feature,
                             const NVSDK_NGX_Parameter *p)
{
    if (g_bridge.disabled || !list || !p) return false;
    D3d12GameRes col{}, out{}, dep{}, mv{};
    if (!ReadD3d12Res(p, "Color", &col) || !ReadD3d12Res(p, "Output", &out) ||
        !ReadD3d12Res(p, "Depth", &dep) || !ReadD3d12Res(p, "MotionVectors", &mv))
        return false; // not a mirrorable super-sampling evaluate; stand aside
    // Ray Reconstruction inputs no longer stand aside on D3D12: they are
    // transported and bound on the private feature below. (Vulkan keeps the
    // stand-aside; D3D11 has no RR in the SDK.)
    if (!D3d12DepthBridgeable(dep.fmt))
    {
        // Non-float depth has no v1 conversion path; forward untouched.
        static LONG said = 0;
        if (InterlockedExchange(&said, 1) == 0)
            Log("[d3d12] depth format %u is not R32_FLOAT; frames without convertible depth "
                "are forwarded untouched (depth conversion is planned)", (unsigned)dep.fmt);
        return false;
    }

    const FeatureCreateContract *contract = FindFeatureContract(feature);
    if (!contract || !contract->valid)
    {
        BridgeDisable("the D3D12 DLSS feature has no complete captured creation contract");
        return false;
    }
    const int vp = ResolveViewport(feature);
    if (vp < 0) return false; // viewport budget exhausted; forward untouched
    if (!D3d12EnsureDevice(list)) { ViewportFail(vp, "d3d12 device"); return false; }
    if (!g_bridge.session_ready && !D3D12InitSession(g_bridge.game_luid, g_bridge.luid_valid))
        return false;
    if (g_mgpu.active)
    {
        // Cross-device D3D12 capture needs an async submit-boundary handoff
        // (the Vulkan event-sandwich equivalent): a synchronously evaluated
        // private session cannot observe inputs the game recorded but has not
        // submitted. Forward untouched until that worker exists.
        static bool said = false;
        if (!said) { said = true;
            Log("[d3d12] secondary-GPU execution with D3D12 capture stands aside "
                "(async cross-adapter handoff is planned); forwarding untouched"); }
        return false;
    }

    // RR inputs are captured up front: their presence selects RR handling
    // for this frame, and any geometry/format drift rebuilds RR state.
    // Capture reads only the parameter block, so it runs outside the frame
    // lock like the SR resource reads above.
    D3d12GameRes rr_res[RR_SLOT_COUNT];
    const char *rr_keys[RR_SLOT_COUNT] = {};
    const int rr_hits = CaptureRRInputs(p, rr_res, rr_keys);

    FrameLock();
    D3d12AgeRetire(vp); // release the previous generation now that a full frame
                        // has passed (its game list has been submitted and drained)
    if (D3d12ViewportContractChanged(vp, *contract, col, out, dep, mv) &&
        !D3d12RebuildViewport(vp, *contract, col, out, dep, mv))
    { FrameUnlock(); return false; }
    BridgeViewport &v = g_bridge.viewports[vp];
    if (!v.frame_ready) { FrameUnlock(); return false; }
    if (rr_hits > 0 && (!v.rr_active || D3d12RRContractChanged(vp, rr_res, rr_keys)) &&
        !D3d12RebuildRR(vp, rr_res, rr_keys))
    { FrameUnlock(); return false; }
    if (rr_hits == 0 && v.rr_active)
    {
        // The game toggled denoiser guidance off mid-process: tear the
        // viewport down to a clean SR rebuild rather than evaluating with
        // stale RR bindings still in the parameter block. The feature was
        // last consumed by the game's queue, so its release follows the
        // covering signal; the RR textures the game's in-flight list may
        // still reference are retired.
        if (v.feature && !D3d12WaitGameFence(D3d12CoveringFenceValue(), 5000))
            Log("[d3d12] vp%d game queue did not complete within 5 s before releasing the "
                "private feature; releasing it anyway", vp);
        if (v.feature && g_bridge.release_feature)
        { g_bridge.release_feature(v.feature); v.feature = nullptr; }
        D3d12RetireRR(vp);
        v.rr_active = false;
        for (int s = 0; s < RR_SLOT_COUNT; ++s) v.rr_key[s] = nullptr;
        v.frame_ready = false;
        v.need_reset = true;
        Log("[d3d12] vp%d Ray Reconstruction inputs gone; rebuilding SR-only", vp);
        FrameUnlock();
        return false;
    }

    FrameScalars scalars;
    CaptureFrameScalarsVp(vp, p, &scalars);

    for (int s = 0; s < SLOT_COUNT; ++s)
        if (!D3d12EnsureAlias(vp, s)) { FrameUnlock(); ViewportFail(vp, "d3d12 alias"); return false; }
    // Game resources are copied in list order, so the neural inputs observe
    // exactly what downstream game passes will observe. States follow the
    // documented NGX D3D12 input contract the game demonstrably satisfies
    // (vanilla DLSS runs at this call): non-pixel (compute-readable) inputs,
    // UAV output. Each game resource is restored to that same state after the
    // copy, so neither the game nor NGX ever observes the COPY states.
    const D3d12GameRes *srcs[SLOT_COUNT] = { &col, nullptr, &dep, &mv };
    const int copy_slots[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    for (int k = 0; k < 3; ++k)
    {
        const int s = copy_slots[k];
        D3D12_RESOURCE_BARRIER pre[] = {
            D3d12Trans(srcs[s]->res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            D3d12Trans(g_d3d12_alias[vp][s], D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(2, pre);
        list->CopyResource(g_d3d12_alias[vp][s], srcs[s]->res);
        D3D12_RESOURCE_BARRIER post[] = {
            D3d12Trans(srcs[s]->res, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            D3d12Trans(g_d3d12_alias[vp][s], D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COMMON),
        };
        list->ResourceBarrier(2, post);
    }
    for (int s = 0; s < RR_SLOT_COUNT; ++s)
    {
        if (!rr_res[s].res || !v.rr_key[s]) continue;
        if (!D3d12EnsureRRAlias(vp, s)) { FrameUnlock(); ViewportFail(vp, "RR alias"); return false; }
        D3D12_RESOURCE_BARRIER pre[] = {
            D3d12Trans(rr_res[s].res, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            D3d12Trans(g_rr_alias[vp][s], D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(2, pre);
        list->CopyResource(g_rr_alias[vp][s], rr_res[s].res);
        D3D12_RESOURCE_BARRIER post[] = {
            D3d12Trans(rr_res[s].res, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            D3d12Trans(g_rr_alias[vp][s], D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COMMON),
        };
        list->ResourceBarrier(2, post);
    }
    if (v.neural_latched == 0)
    {
        D3D12_RESOURCE_BARRIER pre[] = {
            D3d12Trans(out.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            D3d12Trans(g_d3d12_alias[vp][SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(2, pre);
        list->CopyResource(g_d3d12_alias[vp][SLOT_OUTPUT], out.res);
        D3D12_RESOURCE_BARRIER post[] = {
            D3d12Trans(out.res, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            D3d12Trans(g_d3d12_alias[vp][SLOT_OUTPUT], D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COMMON),
        };
        list->ResourceBarrier(2, post);
    }

    // Private neural evaluate INLINE on the game's open list. Same-adapter
    // capture shares the game device, so OptiScaler's detoured evaluate
    // records the neural work in list order: inputs are current (copied
    // above), and downstream passes recorded after Evaluate returns consume
    // the completed neural output. A cross-device private queue could never
    // observe these unsubmitted inputs, which is why secondary-GPU D3D12
    // capture stands aside above.
    ApplyFrameScalarsVp(vp, &scalars);
    ApplyRRParamsVp(vp);
    D3d12TransitionNeuralForEvaluate(vp, list);
    const bool toggle_pulse = BeginNeuralTogglePulse();
    DWORD ecode = 0;
    NVSDK_NGX_Result er = NGX_FAIL;
    ++g_d3d12_in_bridge;
    if (v.feature && g_bridge.eval_feature)
        er = SafeEvaluate(g_bridge.eval_feature, list, v.feature, v.params, &ecode);
    --g_d3d12_in_bridge;
    EndNeuralTogglePulse(toggle_pulse);
    D3d12TransitionNeuralToCommon(vp, list);
    if (ecode || er != NGX_SUCCESS)
    {
        // The input copies above were still recorded into the game's list and
        // will be submitted; the shared textures they reference stay alive via
        // the deferred-release pool (released only after the game-queue fence
        // covering this frame completes).
        ViewportFail(vp, ecode ? "NGX exception in D3D12 evaluate" : "evaluate");
        FrameUnlock();
        return false;
    }
    v.consecutive_fails = 0;
    D3D12_RESOURCE_BARRIER back[] = {
        D3d12Trans(g_d3d12_alias[vp][SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_COPY_SOURCE),
        D3d12Trans(out.res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_DEST),
    };
    list->ResourceBarrier(2, back);
    list->CopyResource(out.res, g_d3d12_alias[vp][SLOT_OUTPUT]);
    D3D12_RESOURCE_BARRIER done[] = {
        D3d12Trans(g_d3d12_alias[vp][SLOT_OUTPUT], D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_COMMON),
        D3d12Trans(out.res, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(2, done);
    if (InterlockedCompareExchange(&v.neural_latched, 1, 0) == 0)
    {
        InterlockedExchange(&g_neural_only_latched, 1);
        Log("[policy] vp%d (d3d12) neural-only latched; game-side DLSS suppressed", vp);
    }
    ++v.frames_done;
    FrameUnlock();
    return true;
}

static bool BridgeD3D12SkipGameEvaluate(int vp, const NVSDK_NGX_Parameter *p)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return false;
    if (g_bridge.viewports[vp].neural_latched == 0) return false;
    D3d12GameRes col{}, out{}, dep{}, mv{};
    if (!ReadD3d12Res(p, "Color", &col) || !ReadD3d12Res(p, "Output", &out) ||
        !ReadD3d12Res(p, "Depth", &dep) || !ReadD3d12Res(p, "MotionVectors", &mv))
        return false;
    if (!D3d12DepthBridgeable(dep.fmt)) return false;
    // A missing/invalid stored contract means the viewport can never be
    // considered unchanged: skipping would suppress the game's evaluate.
    const FeatureCreateContract *contract = FindFeatureContract(g_bridge.viewports[vp].game_handle);
    if (!contract) return false;
    if (D3d12ViewportContractChanged(vp, *contract, col, out, dep, mv)) return false;
    // RR presence is part of the skip contract: a toggled denoiser must
    // rebuild, never evaluate under a stale binding.
    D3d12GameRes rr_res[RR_SLOT_COUNT];
    const char *rr_keys[RR_SLOT_COUNT] = {};
    const int rr_hits = CaptureRRInputs(p, rr_res, rr_keys);
    const bool rr_now = rr_hits > 0;
    if (rr_now != g_bridge.viewports[vp].rr_active) return false;
    if (rr_now && D3d12RRContractChanged(vp, rr_res, rr_keys)) return false;
    return true;
}
