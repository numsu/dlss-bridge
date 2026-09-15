// The private D3D12 NGX session. This half is almost identical to the DLSS 5
// DX11 Bridge -- the D3D12/NGX API is the same whichever graphics API the game
// uses, and it is the call a DLSS 5 add-on detours. What differs from the D3D11
// original is only at the edges: the D3D12 device is chosen by LUID to sit on
// the same physical GPU as the game's Vulkan device (cross-API shared handles
// require it), the shared textures and fences export NT handles for Vulkan to
// import, and there is no D3D11 alias -- the aliasing happens in vk_interop.inc.

// ---- SEH-guarded NGX calls ------------------------------------------------
// Undocumented exports reached with signatures recovered from the D3D11 bridge;
// a wrong guess must land in the log, not take the game down. Each guard is its
// own function with no C++ objects so __try is legal.
#define NGX_EXCEPTION_MARKER 0x7FFFFFFF

static NVSDK_NGX_Result SafeInitExt(PFN_NGX_D3D12_Init_Ext fn, unsigned long long app,
                                    const wchar_t *path, ID3D12Device *dev, int ver,
                                    const void *info, DWORD *code)
{
    *code = 0;
    __try { return fn(app, path, dev, ver, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeInitProjectID(PFN_NGX_D3D12_Init_ProjectID fn, const char *proj,
                                          int engine_type, const char *engine_version,
                                          const wchar_t *path, ID3D12Device *dev, int ver,
                                          const void *info, DWORD *code)
{
    *code = 0;
    __try { return fn(proj, engine_type, engine_version, path, dev, ver, info); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeAllocParams(PFN_AllocateParameters fn, NVSDK_NGX_Parameter **out, DWORD *code)
{
    *code = 0;
    __try { return fn(out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeCreateFeature(PFN_D3D12CreateFeature fn, ID3D12GraphicsCommandList *list,
                                          NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out, DWORD *code)
{
    *code = 0;
    __try { return fn(list, 1 /*SuperSampling*/, p, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeEvaluate(PFN_D3D12EvaluateFeature fn, ID3D12GraphicsCommandList *list,
                                     const NVSDK_NGX_Handle *feature, const NVSDK_NGX_Parameter *p,
                                     DWORD *code)
{
    *code = 0;
    __try { return fn(list, feature, p, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

// NGX answers in hex; names taken from nvsdk_ngx_defs.h so a report does not
// need the header to be read.
static const char *NgxResultName(NVSDK_NGX_Result r)
{
    if (r == NGX_SUCCESS) return "Success";
    if ((static_cast<unsigned>(r) & 0xFFF00000u) != 0xBAD00000u) return "not an NGX result";
    static const char *k[] = {
        "Fail", "FeatureNotSupported", "PlatformError", "FeatureAlreadyExists",
        "FeatureNotFound", "InvalidParameter", "ScratchBufferTooSmall",
        "NotInitialized", "UnsupportedInputFormat", "RWFlagMissing", "MissingInput",
        "UnableToInitializeFeature", "OutOfDate", "OutOfGPUMemory",
        "UnsupportedFormat", "UnableToWriteToAppDataPath", "UnsupportedParameter",
        "Denied", "NotImplemented",
    };
    const unsigned n = static_cast<unsigned>(r) & 0xFFFFFu;
    return n < _countof(k) ? k[n] : "unknown";
}

// ---- NGX snippet search paths ---------------------------------------------
// The dispatcher looks for feature snippets (nvngx_dlss.dll & co.) in the
// session's data path and the OTA cache keyed by its app id. A private session
// under a made-up app id finds neither -- CreateFeature then fails with
// FeatureNotFound (seen on 610.74: SuperSampling.Available=0,
// FeatureInitResult=0xBAD00004). NGX's official escape is the
// FeatureCommonInfo PathListInfo handed to Init: point it at the directories
// of the NGX modules the game itself already loaded -- exactly where the
// working snippets live. Struct layout mirrors nvsdk_ngx_defs.h.
struct NgxPathListInfo { const wchar_t *const *Path; unsigned int Length; };
struct NgxLoggingInfo  { void *Callback; int MinimumLoggingLevel; bool DisableOtherLoggingSinks; };
struct NgxFeatureCommonInfo
{
    NgxPathListInfo PathListInfo;
    void           *InternalData;
    NgxLoggingInfo  LoggingInfo;
};

static HMODULE g_ngx12_module;              // module the D3D12 entry points resolved from
static NgxFeatureCommonInfo g_ngx_common;   // static: NGX may keep the pointers
static wchar_t        g_ngx_paths[8][MAX_PATH];
static const wchar_t *g_ngx_path_ptrs[8];
static UINT64         g_mgpu_pending_neural;
static UINT64         g_mgpu_dropped_outputs;
static UINT64         g_mgpu_pending_game;
static UINT64         g_mgpu_game_repeats;
static bool           g_mgpu_output_timeout;
static int            g_mgpu_pending_timing_slot = -1;
static bool           g_mgpu_pending_timing_unified;
static ULONGLONG      g_mgpu_pending_submitted_at;
static UINT64         g_mgpu_pending_gpu_calibration;
static UINT64         g_mgpu_pending_cpu_calibration;
static UINT64         g_mgpu_pending_submit_qpc;

static void NgxAddModuleDir(HMODULE m, unsigned int *n)
{
    wchar_t buf[MAX_PATH];
    if (m == nullptr || *n >= 8 || GetModuleFileNameW(m, buf, MAX_PATH) == 0) return;
    wchar_t *s = wcsrchr(buf, L'\\');
    if (s == nullptr) return;
    *s = L'\0';
    for (unsigned int i = 0; i < *n; ++i) if (_wcsicmp(g_ngx_paths[i], buf) == 0) return;
    wcscpy_s(g_ngx_paths[*n], buf);
    g_ngx_path_ptrs[*n] = g_ngx_paths[*n];
    ++*n;
}

static const NgxFeatureCommonInfo *NgxBuildCommonInfo()
{
    unsigned int n = 0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    HMODULE mods[1024]; DWORD needed = 0;
    if (enum_modules && enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
            if (GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_EvaluateFeature") ||
                GetProcAddress(mods[i], "NVSDK_NGX_D3D12_EvaluateFeature"))
                NgxAddModuleDir(mods[i], &n);
    NgxAddModuleDir(GetModuleHandleW(nullptr), &n);   // the game dir: games ship their NGX bits there

    for (unsigned int i = 0; i < n; ++i) Log("[bridge] NGX search path: %ls", g_ngx_paths[i]);
    g_ngx_common.PathListInfo.Path   = g_ngx_path_ptrs;
    g_ngx_common.PathListInfo.Length = n;
    g_ngx_common.InternalData        = nullptr;
    g_ngx_common.LoggingInfo         = {};
    return &g_ngx_common;
}

// Logs the SuperSampling capability block; returns Available (-1 if unknowable).
static int D3D12QueryNgxCaps()
{
    if (g_ngx12_module == nullptr) return -1;
    auto get_caps = reinterpret_cast<PFN_AllocateParameters>(
        GetProcAddress(g_ngx12_module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    if (get_caps == nullptr) { Log("[bridge] no GetCapabilityParameters export to probe"); return -1; }

    DWORD code = 0;
    NVSDK_NGX_Parameter *cap = nullptr;
    NVSDK_NGX_Result rc = SafeAllocParams(get_caps, &cap, &code);
    if (code || rc != NGX_SUCCESS || cap == nullptr)
    { Log("[bridge] GetCapabilityParameters gave nothing (result 0x%08X, code 0x%08X)", rc, code); return -1; }

    int avail = -1, init_res = 0, needs_drv = -1, maj = 0, minor = 0;
    cap->Get("SuperSampling.Available", &avail);
    cap->Get("SuperSampling.FeatureInitResult", &init_res);
    cap->Get("SuperSampling.NeedsUpdatedDriver", &needs_drv);
    cap->Get("SuperSampling.MinDriverVersionMajor", &maj);
    cap->Get("SuperSampling.MinDriverVersionMinor", &minor);
    Log("[bridge] NGX caps: SuperSampling.Available=%d FeatureInitResult=0x%08X "
        "NeedsUpdatedDriver=%d MinDriver=%d.%d", avail, init_res, needs_drv, maj, minor);

    typedef NVSDK_NGX_Result (*PFN_DestroyParameters)(NVSDK_NGX_Parameter *);
    auto destroy = reinterpret_cast<PFN_DestroyParameters>(
        GetProcAddress(g_ngx12_module, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (destroy) destroy(cap);
    return avail;
}

static void D3D12ReleaseResources();

static void BridgeDisable(const char *why)
{
    if (g_bridge.disabled) return;
    g_bridge.disabled = true;
    Warn("stopped: %s. Subsequent game DLSS evaluations will fail closed.", why);
    D3D12ReleaseResources();
}

static void BridgeFail(const char *what)
{
    g_bridge.frame_ready = false;
    if (++g_bridge.consecutive_fails >= 3)
    {
        Log("[bridge] disabled after repeated failures (%s); evaluation now fails closed.", what);
        BridgeDisable(what);
    }
}

// ---- adapter selection by LUID --------------------------------------------
// The D3D12 device must be on the same physical GPU as the game's Vulkan
// device, or the shared NT handles cannot be opened across the two APIs. The
// game's device LUID comes from VkPhysicalDeviceIDProperties (captured in
// vk_interop.inc); here it picks the DXGI adapter that matches.
static IDXGIAdapter1 *PickAdapterByLuid(const uint8_t luid[VK_LUID_SIZE], bool luid_valid)
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
                             GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create == nullptr) return nullptr;

    IDXGIFactory1 *factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return nullptr;

    IDXGIAdapter1 *chosen = nullptr;
    IDXGIAdapter1 *ad = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &ad) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 d = {};
        ad->GetDesc1(&d);
        if (luid_valid && memcmp(&d.AdapterLuid, luid, sizeof(LUID)) == 0)
        {
            chosen = ad;   // exact GPU the game's Vulkan device is on
            Log("[bridge] D3D12 adapter matched by LUID: %ls", d.Description);
            break;
        }
        if (!luid_valid && chosen == nullptr && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            chosen = ad;   // no stable identity was available during bootstrap
            chosen->AddRef();
        }
        ad->Release();
    }
    if (chosen && !luid_valid)
        Log("[bridge] no Vulkan device LUID; using the first hardware adapter. If the "
            "machine has more than one GPU this may be the wrong one.");
    factory->Release();
    return chosen;
}

static IDXGIAdapter1 *PickAdapterByIndex(UINT wanted)
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
                             GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!create) return nullptr;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return nullptr;
    IDXGIAdapter1 *ad = nullptr;
    if (factory->EnumAdapters1(wanted, &ad) != S_OK) ad = nullptr;
    if (ad)
    {
        DXGI_ADAPTER_DESC1 d = {};
        ad->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { ad->Release(); ad = nullptr; }
        else Log("[mgpu] neural DXGI adapter %u: %ls LUID=%08X:%08X", wanted,
                 d.Description, (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart);
    }
    factory->Release();
    return ad;
}

static IDXGIAdapter1 *PickAdapterByExactLuid(uint32_t high, uint32_t low, UINT *found_index)
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
                             GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!create) return nullptr;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return nullptr;
    IDXGIAdapter1 *found = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &found) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 d = {};
        found->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (uint32_t)d.AdapterLuid.HighPart == high && (uint32_t)d.AdapterLuid.LowPart == low)
        {
            if (found_index) *found_index = i;
            Log("[adapter] selected stable LUID %08X:%08X: %ls", high, low, d.Description);
            factory->Release();
            return found;
        }
        found->Release();
        found = nullptr;
    }
    factory->Release();
    return nullptr;
}

static IDXGIAdapter1 *PickFirstOtherAdapter(const LUID &game_luid, UINT *found_index)
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
                             GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (!create) return nullptr;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory)
        return nullptr;
    IDXGIAdapter1 *found = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &found) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 d = {};
        found->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            d.VendorId == 0x10DE &&
            memcmp(&d.AdapterLuid, &game_luid, sizeof(LUID)) != 0)
        {
            if (found_index) *found_index = i;
            Log("[adapter] auto-selected secondary DXGI adapter %u: %ls LUID=%08X:%08X", i,
                d.Description, (unsigned)d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart);
            factory->Release();
            return found;
        }
        found->Release();
        found = nullptr;
    }
    factory->Release();
    return nullptr;
}

static ID3D12Device *InteropDevice()
{
    return g_mgpu.active ? static_cast<ID3D12Device *>(g_mgpu.game_dev) : g_bridge.dev12;
}

// ---- command submission ----------------------------------------------------
static bool D3D12BeginCommands()
{
    const int slot = g_bridge.frame_slot;
    const UINT64 retire = g_bridge.alloc_fence[slot];
    if (retire != 0 && g_bridge.fence_gpu->GetCompletedValue() < retire)
    {
        g_bridge.fence_gpu->SetEventOnCompletion(retire, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 2000) != WAIT_OBJECT_0 ||
            g_bridge.fence_gpu->GetCompletedValue() < retire)
        {
            Log("[bridge] the GPU did not retire allocator slot %d within 2 s.", slot);
            BridgeDisable("the GPU stopped completing work");
            return false;
        }
    }
    if (g_bridge.alloc[slot] == nullptr) return false;
    if (FAILED(g_bridge.alloc[slot]->Reset())) return false;
    return SUCCEEDED(g_bridge.list->Reset(g_bridge.alloc[slot], nullptr));
}

// Submits, records which fence value retires the allocator, advances the ring.
static UINT64 D3D12EndCommands()
{
    g_bridge.list->Close();
    ID3D12CommandList *lists[] = { g_bridge.list };
    g_bridge.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++g_bridge.gpu_value;
    g_bridge.queue->Signal(g_bridge.fence_gpu, v);
    g_bridge.alloc_fence[g_bridge.frame_slot] = v;
    g_bridge.frame_slot = (g_bridge.frame_slot + 1) % ActiveFrameCount();
    return v;
}


static bool D3D12WaitNeural(UINT64 value, const char *stage, DWORD timeout);

struct PipelineSubmitTask
{
    ID3D12GraphicsCommandList *list;
    UINT64 fence;
    int slot;
};
static PipelineSubmitTask g_pipeline_submit_q[Bridge::kMaxFrames + 1];
static volatile LONG g_pipeline_submit_head;
static volatile LONG g_pipeline_submit_tail;
static volatile LONG g_pipeline_submit_stop;
static HANDLE g_pipeline_submit_wake;
static HANDLE g_pipeline_submit_thread;

static DWORD WINAPI D3D12PipelineSubmitProc(void *)
{
    for (;;)
    {
        WaitForSingleObject(g_pipeline_submit_wake, INFINITE);
        for (;;)
        {
            if (g_pipeline_submit_head == g_pipeline_submit_tail)
            {
                if (g_pipeline_submit_stop) return 0;
                break;
            }
            const PipelineSubmitTask task =
                g_pipeline_submit_q[g_pipeline_submit_head];
            MemoryBarrier();
            g_pipeline_submit_head =
                (g_pipeline_submit_head + 1) % (ActiveFrameCount() + 1);

            ID3D12CommandList *lists[] = { task.list };
            const ULONGLONG started = GetTickCount64();
            g_bridge.queue->ExecuteCommandLists(1, lists);
            const ULONGLONG submit_ms = GetTickCount64() - started;
            if (FAILED(g_bridge.queue->Signal(g_bridge.fence_gpu, task.fence)))
                Log("[pipeline] failed to signal producer fence %llu",
                    (unsigned long long)task.fence);
            if (submit_ms >= 8)
                Log("[pipeline] driver submission for producer slot %d blocked %llums",
                    task.slot, (unsigned long long)submit_ms);
        }
    }
}

static bool D3D12StartPipelineSubmitter()
{
    if (g_pipeline_submit_thread) return true;
    g_pipeline_submit_head = g_pipeline_submit_tail = 0;
    InterlockedExchange(&g_pipeline_submit_stop, 0);
    g_pipeline_submit_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_pipeline_submit_wake) return false;
    g_pipeline_submit_thread = CreateThread(
        nullptr, 0, D3D12PipelineSubmitProc, nullptr, 0, nullptr);
    if (!g_pipeline_submit_thread)
    {
        CloseHandle(g_pipeline_submit_wake);
        g_pipeline_submit_wake = nullptr;
        return false;
    }
    SetThreadPriority(g_pipeline_submit_thread, THREAD_PRIORITY_HIGHEST);
    Log("[pipeline] dedicated ordered D3D12 submission thread started");
    return true;
}

static void D3D12StopPipelineSubmitter()
{
    if (g_pipeline_submit_thread)
    {
        InterlockedExchange(&g_pipeline_submit_stop, 1);
        SetEvent(g_pipeline_submit_wake);
        if (WaitForSingleObject(g_pipeline_submit_thread, 5000) != WAIT_OBJECT_0)
            Log("[pipeline] submission thread did not drain within 5 s");
        CloseHandle(g_pipeline_submit_thread);
        g_pipeline_submit_thread = nullptr;
    }
    if (g_pipeline_submit_wake)
    {
        CloseHandle(g_pipeline_submit_wake);
        g_pipeline_submit_wake = nullptr;
    }
    g_pipeline_submit_head = g_pipeline_submit_tail = 0;
}

static bool D3D12BeginPipelineCommands(int slot,
                                        ID3D12GraphicsCommandList **out)
{
    if (!D3D12StartPipelineSubmitter() || !g_bridge.alloc[slot] ||
        !g_mgpu.pipeline_list[slot])
        return false;
    const UINT64 retire = g_bridge.alloc_fence[slot];
    if (retire && g_bridge.fence_gpu->GetCompletedValue() < retire)
    {
        if (!D3D12WaitNeural(retire, "pipeline command allocator", 2000))
            return false;
    }
    if (FAILED(g_bridge.alloc[slot]->Reset()) ||
        FAILED(g_mgpu.pipeline_list[slot]->Reset(g_bridge.alloc[slot], nullptr)))
        return false;
    *out = g_mgpu.pipeline_list[slot];
    return true;
}

static UINT64 D3D12EnqueuePipelineCommands(
    ID3D12GraphicsCommandList *list, int slot)
{
    if (FAILED(list->Close())) return 0;
    const LONG next = (g_pipeline_submit_tail + 1) % (ActiveFrameCount() + 1);
    if (next == g_pipeline_submit_head)
    {
        Log("[pipeline] internal submission queue overflow");
        return 0;
    }
    const UINT64 fence = ++g_bridge.gpu_value;
    g_bridge.alloc_fence[slot] = fence;
    g_pipeline_submit_q[g_pipeline_submit_tail] = { list, fence, slot };
    MemoryBarrier();
    g_pipeline_submit_tail = next;
    SetEvent(g_pipeline_submit_wake);
    return fence;
}


static void D3D12InitTimingQueries()
{
    if (!g_cfg.gpu_timestamps || !g_bridge.dev12 || !g_bridge.queue) return;

    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = ActiveFrameCount() * 4;
    if (FAILED(g_bridge.dev12->CreateQueryHeap(&qd, __uuidof(ID3D12QueryHeap),
            reinterpret_cast<void **>(&g_bridge.timing_heap))))
        return;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeof(UINT64) * qd.Count;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_bridge.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
            reinterpret_cast<void **>(&g_bridge.timing_readback))) ||
        FAILED(g_bridge.queue->GetTimestampFrequency(&g_bridge.timing_frequency)))
    {
        if (g_bridge.timing_readback)
        { g_bridge.timing_readback->Release(); g_bridge.timing_readback = nullptr; }
        g_bridge.timing_heap->Release();
        g_bridge.timing_heap = nullptr;
        g_bridge.timing_frequency = 0;
        return;
    }
    Log("[profile] GPU 1 direct-queue timestamps enabled at %llu Hz",
        (unsigned long long)g_bridge.timing_frequency);
}

static void D3D12TimingStampOn(ID3D12GraphicsCommandList *list, int slot, unsigned point)
{
    if (!g_bridge.timing_heap) return;
    list->EndQuery(g_bridge.timing_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                   (UINT)(slot * 4 + point));
}

static void D3D12TimingStamp(int slot, unsigned point)
{
    D3D12TimingStampOn(g_bridge.list, slot, point);
}

static void D3D12TimingResolveOn(ID3D12GraphicsCommandList *list, int slot)
{
    if (!g_bridge.timing_heap || !g_bridge.timing_readback) return;
    const UINT first = (UINT)(slot * 4);
    list->ResolveQueryData(g_bridge.timing_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                           first, 4, g_bridge.timing_readback,
                           sizeof(UINT64) * first);
}

static void D3D12TimingResolve(int slot)
{
    D3D12TimingResolveOn(g_bridge.list, slot);
}

static void D3D12TimingResolveNeural(int slot)
{
    if (!g_bridge.timing_heap || !g_bridge.timing_readback) return;
    const UINT first = (UINT)(slot * 4);
    g_bridge.list->ResolveQueryData(g_bridge.timing_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                    first, 2, g_bridge.timing_readback,
                                    sizeof(UINT64) * first);
}

static double D3D12TimingReadNeural(int slot)
{
    if (!g_bridge.timing_readback || !g_bridge.timing_frequency) return 0.0;
    const SIZE_T begin = sizeof(UINT64) * (SIZE_T)(slot * 4);
    D3D12_RANGE range = { begin, begin + sizeof(UINT64) * 2 };
    void *mapped = nullptr;
    if (FAILED(g_bridge.timing_readback->Map(0, &range, &mapped)) || !mapped) return 0.0;
    const UINT64 *t = reinterpret_cast<const UINT64 *>(
        reinterpret_cast<const BYTE *>(mapped) + begin);
    const double ms = (double)(t[1] - t[0]) * 1000.0 / (double)g_bridge.timing_frequency;
    D3D12_RANGE written = { 0, 0 };
    g_bridge.timing_readback->Unmap(0, &written);
    return ms;
}

struct PipelineProfileWindow
{
    double upload_ms[256], neural_wall_ms[256], neural_gpu_ms[256], download_ms[256], total_ms[256];
    unsigned count;
    UINT64 fresh, repeated, max_output_age;
    ULONGLONG window_start;
};
static PipelineProfileWindow g_profile = {};

static double ProfilePercentile(const double *values, unsigned count, double fraction)
{
    double sorted[256];
    for (unsigned i = 0; i < count; ++i) sorted[i] = values[i];
    for (unsigned i = 1; i < count; ++i)
    {
        const double value = sorted[i];
        unsigned j = i;
        while (j && sorted[j - 1] > value) { sorted[j] = sorted[j - 1]; --j; }
        sorted[j] = value;
    }
    unsigned index = (unsigned)((count - 1) * fraction + 0.5);
    return sorted[index];
}

static void ProfileRepeat(UINT64 timeline, int transport_slot)
{
    ++g_profile.repeated;
    if (!g_profile.window_start) g_profile.window_start = GetTickCount64();
    if (transport_slot < 0 || transport_slot >= ActiveFrameCount()) return;
    const UINT64 source = g_mgpu.frame[transport_slot].output_timeline;
    const UINT64 age = source && timeline >= source ? timeline - source : 0;
    if (age > g_profile.max_output_age) g_profile.max_output_age = age;
}

static void ProfileAdd(double upload_ms, double neural_wall_ms, double neural_gpu_ms,
                       double download_ms, double total_ms)
{
    const unsigned i = g_profile.count++;
    g_profile.upload_ms[i] = upload_ms;
    g_profile.neural_wall_ms[i] = neural_wall_ms;
    g_profile.neural_gpu_ms[i] = neural_gpu_ms;
    g_profile.download_ms[i] = download_ms;
    g_profile.total_ms[i] = total_ms;
    ++g_profile.fresh;
    if (!g_profile.window_start) g_profile.window_start = GetTickCount64();
    if (g_profile.count != 256) return;
    const ULONGLONG now = GetTickCount64();
    const double seconds = (double)(now - g_profile.window_start) / 1000.0;
    const double fresh_fps = seconds > 0.0 ? 256.0 / seconds : 0.0;
    const double repeat_rate = (g_profile.fresh + g_profile.repeated) ?
        100.0 * (double)g_profile.repeated / (double)(g_profile.fresh + g_profile.repeated) : 0.0;
    Log("[profile] fresh=%.1ffps repeats=%llu (%.2f%%) max-output-age=%llu frames; p50/p95/p99 ms: "
        "upload %.2f/%.2f/%.2f, neural-wall %.2f/%.2f/%.2f, neural-gpu %.2f/%.2f/%.2f, "
        "download %.2f/%.2f/%.2f, total %.2f/%.2f/%.2f",
        fresh_fps, (unsigned long long)g_profile.repeated, repeat_rate,
        (unsigned long long)g_profile.max_output_age,
        ProfilePercentile(g_profile.upload_ms, 256, .50), ProfilePercentile(g_profile.upload_ms, 256, .95), ProfilePercentile(g_profile.upload_ms, 256, .99),
        ProfilePercentile(g_profile.neural_wall_ms, 256, .50), ProfilePercentile(g_profile.neural_wall_ms, 256, .95), ProfilePercentile(g_profile.neural_wall_ms, 256, .99),
        ProfilePercentile(g_profile.neural_gpu_ms, 256, .50), ProfilePercentile(g_profile.neural_gpu_ms, 256, .95), ProfilePercentile(g_profile.neural_gpu_ms, 256, .99),
        ProfilePercentile(g_profile.download_ms, 256, .50), ProfilePercentile(g_profile.download_ms, 256, .95), ProfilePercentile(g_profile.download_ms, 256, .99),
        ProfilePercentile(g_profile.total_ms, 256, .50), ProfilePercentile(g_profile.total_ms, 256, .95), ProfilePercentile(g_profile.total_ms, 256, .99));
    g_profile.count = 0;
    g_profile.fresh = 0;
    g_profile.repeated = 0;
    g_profile.max_output_age = 0;
    g_profile.window_start = now;
}

static bool D3D12TimingReadPipeline(int slot, double *upload_ms,
                                    double *neural_ms, double *download_ms,
                                    UINT64 *gpu_start = nullptr,
                                    UINT64 *gpu_end = nullptr)
{
    if (!g_bridge.timing_readback || !g_bridge.timing_frequency) return false;
    const SIZE_T begin = sizeof(UINT64) * (SIZE_T)(slot * 4);
    D3D12_RANGE range = { begin, begin + sizeof(UINT64) * 4 };
    void *mapped = nullptr;
    if (FAILED(g_bridge.timing_readback->Map(0, &range, &mapped)) || !mapped) return false;
    const UINT64 *t = reinterpret_cast<const UINT64 *>(
        reinterpret_cast<const BYTE *>(mapped) + begin);
    const double scale = 1000.0 / (double)g_bridge.timing_frequency;
    *upload_ms = (double)(t[1] - t[0]) * scale;
    *neural_ms = (double)(t[2] - t[1]) * scale;
    *download_ms = (double)(t[3] - t[2]) * scale;
    if (gpu_start) *gpu_start = t[0];
    if (gpu_end) *gpu_end = t[3];
    D3D12_RANGE written = { 0, 0 };
    g_bridge.timing_readback->Unmap(0, &written);
    return true;
}

static double D3D12TimingReport(int slot)
{
    double upload_ms = 0.0, neural_ms = 0.0, download_ms = 0.0;
    if (!D3D12TimingReadPipeline(slot, &upload_ms, &neural_ms, &download_ms))
        return 0.0;
    static unsigned reports = 0;
    if (reports++ < 32)
        Log("[profile] GPU 1 upload=%.3fms neural=%.3fms download=%.3fms total=%.3fms",
            upload_ms, neural_ms, download_ms, upload_ms + neural_ms + download_ms);
    return neural_ms;
}

static void D3D12CaptureClockCalibration(UINT64 *gpu, UINT64 *cpu, UINT64 *submitted)
{
    *gpu = *cpu = *submitted = 0;
    LARGE_INTEGER qpc = {};
    if (SUCCEEDED(g_bridge.queue->GetClockCalibration(gpu, cpu)) &&
        QueryPerformanceCounter(&qpc))
        *submitted = (UINT64)qpc.QuadPart;
}

static HRESULT CreateCopyQueue(ID3D12Device *device, ID3D12CommandQueue **out,
                               const char *label)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    HRESULT hr = device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                                             reinterpret_cast<void **>(out));
    if (FAILED(hr))
    {
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                                         reinterpret_cast<void **>(out));
        Log("[mgpu] %s high-priority copy queue unavailable; normal-priority fallback 0x%08X",
            label, hr);
    }
    else
    {
        Log("[mgpu] %s copy queue created at high priority", label);
    }
    return hr;
}

static HRESULT CreateDirectQueue(ID3D12Device *device, ID3D12CommandQueue **out,
                                 const char *label)
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    HRESULT hr = device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                                             reinterpret_cast<void **>(out));
    if (FAILED(hr))
    {
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        hr = device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue),
                                         reinterpret_cast<void **>(out));
        Log("[mgpu] %s high-priority direct queue unavailable; normal-priority fallback 0x%08X",
            label, hr);
    }
    else
    {
        Log("[mgpu] %s direct queue created at high priority", label);
    }
    return hr;
}

static bool NeuralCopyBegin(int slot)
{
    return g_mgpu.neural_copy_alloc[slot] && g_mgpu.neural_copy_list[slot] &&
           SUCCEEDED(g_mgpu.neural_copy_alloc[slot]->Reset()) &&
           SUCCEEDED(g_mgpu.neural_copy_list[slot]->Reset(g_mgpu.neural_copy_alloc[slot], nullptr));
}

static UINT64 NeuralCopyEnd(int slot)
{
    if (FAILED(g_mgpu.neural_copy_list[slot]->Close())) return 0;
    ID3D12CommandList *lists[] = { g_mgpu.neural_copy_list[slot] };
    g_mgpu.neural_copy_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g_bridge.gpu_value;
    if (FAILED(g_mgpu.neural_copy_queue->Signal(g_bridge.fence_gpu, v))) return 0;
    return v;
}

static bool NeuralDownloadBegin(int slot)
{
    return g_mgpu.neural_download_alloc[slot] && g_mgpu.neural_download_list[slot] &&
           SUCCEEDED(g_mgpu.neural_download_alloc[slot]->Reset()) &&
           SUCCEEDED(g_mgpu.neural_download_list[slot]->Reset(g_mgpu.neural_download_alloc[slot], nullptr));
}

static UINT64 NeuralDownloadEnd(int slot)
{
    if (FAILED(g_mgpu.neural_download_list[slot]->Close())) return 0;
    ID3D12CommandList *lists[] = { g_mgpu.neural_download_list[slot] };
    g_mgpu.neural_copy_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g_bridge.gpu_value;
    if (FAILED(g_mgpu.neural_copy_queue->Signal(g_bridge.fence_gpu, v))) return 0;
    return v;
}

static void D3D12DrainGpu()
{
    if (g_bridge.queue == nullptr || g_bridge.fence_gpu == nullptr) return;
    const UINT64 v = ++g_bridge.gpu_value;
    g_bridge.queue->Signal(g_bridge.fence_gpu, v);
    if (g_bridge.fence_gpu->GetCompletedValue() < v && g_bridge.fence_event)
    {
        g_bridge.fence_gpu->SetEventOnCompletion(v, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 5000) != WAIT_OBJECT_0)
            Log("[bridge] timed out draining the D3D12 queue before teardown");
    }
    for (int i = 0; i < ActiveFrameCount(); ++i) g_bridge.alloc_fence[i] = 0;
}

static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (res == nullptr) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

// ---- shared resource creation (D3D12 side) --------------------------------
// The texture is created on the D3D12 side with HEAP_FLAG_SHARED and
// ALLOW_SIMULTANEOUS_ACCESS (so the two APIs need no cross-API state
// transitions) and an NT shared handle is exported for Vulkan to import. Depth
// and the Output always carry a UAV; Output because DLSS writes it through one.
static bool D3D12MakeSharedTexture(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    SharedTex &t = g_bridge.tex[slot];

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                               : D3D12_RESOURCE_FLAG_NONE);

    ID3D12Device *interop = InteropDevice();
    HRESULT hr = interop->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t.tex12));
    if (FAILED(hr)) { Log("[bridge] %s: CreateCommittedResource failed 0x%08X", kSlotName[slot], hr); return false; }

    hr = interop->CreateSharedHandle(t.tex12, nullptr, GENERIC_ALL, nullptr, &t.nt_handle);
    if (FAILED(hr)) { Log("[bridge] %s: CreateSharedHandle failed 0x%08X", kSlotName[slot], hr); return false; }

    {
        wchar_t n[64];
        _snwprintf_s(n, _TRUNCATE, L"dlss5-vk-bridge %hs", kSlotName[slot]);
        t.tex12->SetName(n);
    }
    t.w = w; t.h = h; t.fmt12 = fmt;
    Log("[bridge] %-6s %ux%u fmt=%u  D3D12 shared texture created (handle=%p)",
        kSlotName[slot], w, h, fmt, t.nt_handle);
    return true;
}

static bool D3D12CreateSharedFence(ID3D12Fence **out, HANDLE *handle)
{
    ID3D12Device *interop = InteropDevice();
    HRESULT hr = interop->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                             __uuidof(ID3D12Fence), reinterpret_cast<void **>(out));
    if (FAILED(hr)) { Log("[bridge] CreateFence(SHARED) failed 0x%08X", hr); return false; }
    hr = interop->CreateSharedHandle(*out, nullptr, GENERIC_ALL, nullptr, handle);
    if (FAILED(hr)) { Log("[bridge] CreateSharedHandle(fence) failed 0x%08X", hr); return false; }
    return true;
}

static ID3D12Resource *NeuralTex(int slot)
{
    return g_mgpu.active ? g_mgpu.neural_tex[slot] : g_bridge.tex[slot].tex12;
}

static void D3D12ReleaseMultiGpuFrameResources()
{
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (g_mgpu.neural_tex[i]) { g_mgpu.neural_tex[i]->Release(); g_mgpu.neural_tex[i] = nullptr; }
        g_mgpu.plane[i] = {};
    }
    for (int slot = 0; slot < Bridge::kMaxFrames; ++slot)
    {
        CrossFrameTransport &fr = g_mgpu.frame[slot];
        for (int direction = 0; direction < CROSS_COUNT; ++direction)
        {
            if (fr.neural_staging[direction])
            {
                D3D12_RANGE no_cpu_writes = { 0, 0 };
                fr.neural_staging[direction]->Unmap(0, &no_cpu_writes);
                fr.neural_staging[direction]->Release();
                fr.neural_staging[direction] = nullptr;
                fr.neural_staging_mapped[direction] = nullptr;
            }
            if (fr.game_buffer[direction]) { fr.game_buffer[direction]->Release(); fr.game_buffer[direction] = nullptr; }
            if (fr.neural_buffer[direction]) { fr.neural_buffer[direction]->Release(); fr.neural_buffer[direction] = nullptr; }
            if (fr.game_heap[direction]) { fr.game_heap[direction]->Release(); fr.game_heap[direction] = nullptr; }
            if (fr.neural_heap[direction]) { fr.neural_heap[direction]->Release(); fr.neural_heap[direction] = nullptr; }
            if (fr.host_memory[direction]) { VirtualFree(fr.host_memory[direction], 0, MEM_RELEASE); fr.host_memory[direction] = nullptr; }
        }
        if (fr.vulkan_input_memory)
        { VirtualFree(fr.vulkan_input_memory, 0, MEM_RELEASE); fr.vulkan_input_memory = nullptr; }
        if (fr.vulkan_output_memory)
        { VirtualFree(fr.vulkan_output_memory, 0, MEM_RELEASE); fr.vulkan_output_memory = nullptr; }
        fr.generation = fr.upload_fence = fr.output_fence = fr.output_timeline = 0;
        fr.producer_fence = fr.producer_timeline = fr.producer_sequence = 0;
        fr.producer_timing_slot = -1;
        fr.output_valid = fr.producer_valid = fr.producer_presented = false;
    }
    g_mgpu.producer_sequence = 0;
    g_mgpu.next_present_sequence = 1;
    g_mgpu.host_bytes = 0;
}

static bool D3D12MakeNeuralTexture(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    HRESULT hr = g_bridge.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&g_mgpu.neural_tex[slot]));
    if (FAILED(hr)) Log("[mgpu] %s neural texture creation failed 0x%08X", kSlotName[slot], hr);
    return SUCCEEDED(hr);
}

static D3D12_RESOURCE_DESC CrossBufferDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    return d;
}

static D3D12_RESOURCE_DESC StagingBufferDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d = CrossBufferDesc(bytes);
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    return d;
}

static bool D3D12MakeNeuralStaging(CrossFrameTransport &fr, int slot, int direction)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = direction == CROSS_UPLOAD ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc = StagingBufferDesc(g_mgpu.host_bytes);
    const D3D12_RESOURCE_STATES initial = direction == CROSS_UPLOAD
        ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    HRESULT hr = g_bridge.dev12->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&fr.neural_staging[direction]));
    if (FAILED(hr))
    {
        Log("[mgpu] frame %d neural %s staging creation failed 0x%08X", slot,
            direction == CROSS_UPLOAD ? "upload" : "readback", hr);
        return false;
    }
    D3D12_RANGE cpu_read = direction == CROSS_DOWNLOAD
        ? D3D12_RANGE{ 0, (SIZE_T)g_mgpu.host_bytes } : D3D12_RANGE{ 0, 0 };
    hr = fr.neural_staging[direction]->Map(
        0, &cpu_read, &fr.neural_staging_mapped[direction]);
    if (FAILED(hr) || !fr.neural_staging_mapped[direction])
    {
        Log("[mgpu] frame %d neural %s staging map failed 0x%08X", slot,
            direction == CROSS_UPLOAD ? "upload" : "readback", hr);
        return false;
    }
    return true;
}

static bool D3D12BuildCrossBuffer()
{
    if (!g_mgpu.active) return true;
    UINT64 cursor = 0;
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        D3D12_RESOURCE_DESC td = g_mgpu.neural_tex[i]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT rows = 0; UINT64 row_bytes = 0, total = 0;
        g_bridge.dev12->GetCopyableFootprints(&td, 0, 1, cursor, &fp, &rows, &row_bytes, &total);
        g_mgpu.plane[i] = { fp.Offset, fp.Footprint.RowPitch, fp.Footprint.Width,
                            fp.Footprint.Height, fp.Footprint.Format };
        cursor = fp.Offset + total;
    }
    g_mgpu.host_bytes = (SIZE_T)((cursor + 65535ull) & ~65535ull);

    ID3D12Device3 *neural3 = nullptr;
    HRESULT hr = g_bridge.dev12->QueryInterface(__uuidof(ID3D12Device3), reinterpret_cast<void **>(&neural3));
    if (FAILED(hr) || !neural3) { Log("[mgpu] neural device has no ID3D12Device3 0x%08X", hr); return false; }
    D3D12_RESOURCE_DESC bd = CrossBufferDesc(g_mgpu.host_bytes);

    const UINT64 generation = ++g_mgpu.transport_generation;
    g_mgpu.producer_sequence = 0;
    g_mgpu.next_present_sequence = 1;
    for (int slot = 0; slot < ActiveFrameCount(); ++slot)
    {
        CrossFrameTransport &fr = g_mgpu.frame[slot];
        fr.generation = generation;
        fr.producer_timing_slot = -1;
        fr.output_valid = fr.producer_valid = fr.producer_presented = false;
        if (!D3D12MakeNeuralStaging(fr, slot, CROSS_UPLOAD) ||
            !D3D12MakeNeuralStaging(fr, slot, CROSS_DOWNLOAD))
        { neural3->Release(); return false; }
        for (int direction = 0; direction < CROSS_COUNT; ++direction)
        {
            const char *name = direction == CROSS_UPLOAD ? "upload" : "download";
            fr.host_memory[direction] = VirtualAlloc(nullptr, g_mgpu.host_bytes,
                                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!fr.host_memory[direction])
            {
                Log("[mgpu] frame %d VirtualAlloc(%s, %llu) failed", slot, name,
                    (unsigned long long)g_mgpu.host_bytes);
                neural3->Release(); return false;
            }
            memset(fr.host_memory[direction], 0, g_mgpu.host_bytes);
            hr = g_mgpu.game_dev->OpenExistingHeapFromAddress(
                fr.host_memory[direction], __uuidof(ID3D12Heap),
                reinterpret_cast<void **>(&fr.game_heap[direction]));
            if (SUCCEEDED(hr))
                hr = neural3->OpenExistingHeapFromAddress(
                    fr.host_memory[direction], __uuidof(ID3D12Heap),
                    reinterpret_cast<void **>(&fr.neural_heap[direction]));
            if (FAILED(hr) || !fr.game_heap[direction] || !fr.neural_heap[direction])
            {
                Log("[mgpu] frame %d OpenExistingHeapFromAddress(%s) failed 0x%08X", slot, name, hr);
                neural3->Release(); return false;
            }
            hr = g_mgpu.game_dev->CreatePlacedResource(
                fr.game_heap[direction], 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&fr.game_buffer[direction]));
            if (SUCCEEDED(hr))
                hr = g_bridge.dev12->CreatePlacedResource(
                    fr.neural_heap[direction], 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&fr.neural_buffer[direction]));
            if (FAILED(hr) || !fr.game_buffer[direction] || !fr.neural_buffer[direction])
            {
                Log("[mgpu] frame %d %s cross-buffer placed resource failed 0x%08X", slot, name, hr);
                neural3->Release(); return false;
            }
        }
    }
    neural3->Release();
    Log("[mgpu] %d-frame directional transport ring ready: %llu bytes per direction per frame",
        ActiveFrameCount(), (unsigned long long)g_mgpu.host_bytes);
    return true;
}

static void CopyTextureToCross(ID3D12GraphicsCommandList *list, ID3D12Resource *texture,
                               ID3D12Resource *buffer, const CrossPlane &p)
{
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = buffer; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = p.offset;
    dst.PlacedFootprint.Footprint.Format = p.format;
    dst.PlacedFootprint.Footprint.Width = p.width;
    dst.PlacedFootprint.Footprint.Height = p.height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = p.row_pitch;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

static void CopyCrossToTexture(ID3D12GraphicsCommandList *list, ID3D12Resource *buffer,
                               ID3D12Resource *texture, const CrossPlane &p)
{
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = buffer; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = p.offset;
    src.PlacedFootprint.Footprint.Format = p.format;
    src.PlacedFootprint.Footprint.Width = p.width;
    src.PlacedFootprint.Footprint.Height = p.height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = p.row_pitch;
    dst.pResource = texture; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

static bool WaitFence(ID3D12Fence *fence, UINT64 value, HANDLE event, DWORD timeout, const char *which)
{
    if (fence->GetCompletedValue() >= value) return true;
    const ULONGLONG started = GetTickCount64();
    const ULONGLONG deadline = started + timeout;
    for (;;)
    {
        if (FAILED(fence->SetEventOnCompletion(value, event)))
        { Log("[mgpu] could not arm the fence event for %s", which); return false; }

        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now >= deadline ? 0 : (DWORD)(deadline - now);
        if (WaitForSingleObject(event, remaining) != WAIT_OBJECT_0)
        {
            Log("[mgpu] timed out waiting for %s after %llums (deadline %lums)", which,
                (unsigned long long)(GetTickCount64() - started), (unsigned long)timeout);
            return false;
        }

        // A timed-out SetEventOnCompletion remains registered. Reusing the
        // auto-reset event can therefore wake this wait for an older value.
        // Treat that as a wake-up hint and keep waiting for the value we armed.
        if (fence->GetCompletedValue() >= value) return true;
        if (GetTickCount64() >= deadline)
        {
            Log("[mgpu] timed out waiting for %s after a stale fence wake at %llums "
                "(deadline %lums)", which,
                (unsigned long long)(GetTickCount64() - started), (unsigned long)timeout);
            return false;
        }
    }
}

// A late secondary-GPU result must not park the game's graphics queue for an
// arbitrary scheduling cycle. A normal 1080p host-staged frame on the test
// machine takes 23-27 ms, so two configured frame budgets provide useful
// headroom while turning a bus-contention miss into one repeated image instead
// of the previous 60 ms hitch. Keep a floor for faster configured targets and
// a ceiling so a mistaken profile cannot create a quarter-second stall.
static DWORD NeuralPipelineDeadlineMs()
{
    const int configured = g_cfg.latency_budget_ms > 0 ? g_cfg.latency_budget_ms : 16;
    unsigned long long deadline = (unsigned long long)configured * 2ull;
    if (deadline < 32ull) deadline = 32ull;
    if (deadline > 250ull) deadline = 250ull;
    return (DWORD)deadline;
}

static bool GameBeginCommands(int slot)
{
    return g_mgpu.game_alloc[slot] && g_mgpu.game_list[slot] &&
           SUCCEEDED(g_mgpu.game_alloc[slot]->Reset()) &&
           SUCCEEDED(g_mgpu.game_list[slot]->Reset(g_mgpu.game_alloc[slot], nullptr));
}

static bool GameEndAndWait(int slot, const char *which, DWORD timeout)
{
    if (FAILED(g_mgpu.game_list[slot]->Close())) return false;
    ID3D12CommandList *lists[] = { g_mgpu.game_list[slot] };
    g_mgpu.game_queue->ExecuteCommandLists(1, lists);
    UINT64 v = ++g_mgpu.game_value;
    if (FAILED(g_mgpu.game_queue->Signal(g_mgpu.game_fence, v))) return false;
    if (WaitFence(g_mgpu.game_fence, v, g_mgpu.game_event, timeout, which)) return true;
    g_mgpu_pending_game = v;
    g_mgpu_game_repeats = 1;
    Log("[mgpu] game queue fence target=%llu completed=%llu; falling back without rebuilding",
        (unsigned long long)v,
        (unsigned long long)g_mgpu.game_fence->GetCompletedValue());
    return false;
}

static bool D3D12WaitNeural(UINT64 value, const char *stage, DWORD timeout = 4000)
{
    const bool ok = WaitFence(g_bridge.fence_gpu, value, g_bridge.fence_event, timeout, stage);
    if (!ok)
    {
        const UINT64 completed = g_bridge.fence_gpu ? g_bridge.fence_gpu->GetCompletedValue() : 0;
        const HRESULT removed = g_bridge.dev12 ? g_bridge.dev12->GetDeviceRemovedReason() : E_POINTER;
        Log("[mgpu] %s fence target=%llu completed=%llu device_status=0x%08X", stage,
            (unsigned long long)value, (unsigned long long)completed, (unsigned)removed);
    }
    return ok;
}

// The D3D12 entry points must all come from the NGX dispatcher (_nvngx.dll from
// the driver store). Feature snippets (nvngx_dlss*.dll, driver-downloaded *.bin
// modules) also export the per-feature Init/Create/Evaluate names, so a
// two-name fingerprint can land on a snippet that lacks the dispatcher-only
// services (AllocateParameters) -- seen on 610.74, where nvngx_dlssnr.dll
// enumerates ahead of _nvngx.dll. Demand the complete set.
static const char *const kNgxD3D12Names[] = {
    "NVSDK_NGX_D3D12_AllocateParameters",
    "NVSDK_NGX_D3D12_CreateFeature",
    "NVSDK_NGX_D3D12_EvaluateFeature",
    "NVSDK_NGX_D3D12_ReleaseFeature",
};

static bool NgxHasFullD3D12Set(HMODULE m)
{
    for (const char *name : kNgxD3D12Names)
        if (GetProcAddress(m, name) == nullptr) return false;
    return GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext") != nullptr ||
           GetProcAddress(m, "NVSDK_NGX_D3D12_Init_ProjectID") != nullptr;
}

static const wchar_t *NgxModuleName(HMODULE m)
{
    static wchar_t buf[MAX_PATH];
    GetModuleFileNameW(m, buf, MAX_PATH);
    const wchar_t *s = wcsrchr(buf, L'\\');
    return s ? s + 1 : buf;
}

static HMODULE FindNgxD3D12Loader()
{
    // Selecting any other NGX dispatcher would make a successful launch run
    // ordinary DLSS without the configured neural pass. The exact module
    // loaded from the isolated session is therefore mandatory.
    HMODULE m = g_optiscaler_module;
    if (m && NgxHasFullD3D12Set(m))
    {
        Log("[bridge] NGX D3D12 entry points from %ls (neural host)", NgxModuleName(m));
        return m;
    }
    Warn(m ? "the session OptiScaler module lacks the complete NGX D3D12 interface"
           : "the session OptiScaler module was not loaded");
    return nullptr;
}

// ---- evaluate detour probe -------------------------------------------------
// The whole point of this bridge is that a DLSS 5 Neural Rendering add-on
// detours NVSDK_NGX_D3D12_EvaluateFeature and inserts its pass there. The
// bridge itself never patches that function, so if its first bytes are (or
// become) a jump, an external party has attached -- and if they never do, the
// bridge's evaluate is plain driver DLSS with nothing riding on it. Logged at
// init and re-checked at the frame heartbeats.
static BYTE g_eval12_baseline[16];
static bool g_eval12_baseline_ok;
static bool g_eval12_detour_reported;

static bool LooksDetoured(const BYTE *p)
{
    if (p[0] == 0xE9) return true;                      // jmp rel32
    if (p[0] == 0xFF && p[1] == 0x25) return true;      // jmp [rip+disp32]
    if (p[0] == 0x48 && p[1] == 0xB8 &&                 // mov rax, imm64
        p[10] == 0xFF && p[11] == 0xE0) return true;    // jmp rax
    return false;
}

static void D3D12ProbeEvalDetour(const char *when)
{
    const BYTE *p = reinterpret_cast<const BYTE *>(g_bridge.eval_feature);
    if (p == nullptr) return;
    BYTE now[16];
    __try { memcpy(now, p, sizeof(now)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (!g_eval12_baseline_ok)
    {
        memcpy(g_eval12_baseline, now, sizeof(now));
        g_eval12_baseline_ok = true;
        Log("[bridge] D3D12 EvaluateFeature entry (%s): %02X %02X %02X %02X -- %s", when,
            now[0], now[1], now[2], now[3],
            LooksDetoured(now) ? "DETOURED: an add-on is attached to the D3D12 evaluate"
                               : "not detoured: no add-on attached (yet)");
        return;
    }
    if (!g_eval12_detour_reported && memcmp(now, g_eval12_baseline, sizeof(now)) != 0)
    {
        g_eval12_detour_reported = true;
        Log("[bridge] D3D12 EvaluateFeature entry CHANGED (%s): %02X %02X %02X %02X -> %02X %02X %02X %02X%s",
            when, g_eval12_baseline[0], g_eval12_baseline[1], g_eval12_baseline[2], g_eval12_baseline[3],
            now[0], now[1], now[2], now[3],
            LooksDetoured(now) ? " -- an add-on has hooked the D3D12 evaluate" : "");
    }
}

// Opens the D3D12 device (LUID-matched), initialises a second NGX session on it,
// and creates the queue, allocators, retire fence and the two shared cross-API
// fences. `luid`/`luid_valid` come from the game's VkPhysicalDevice.
static HANDLE g_shared_fence_in;
static HANDLE g_shared_fence_out;

static bool D3D12InitSession(const uint8_t luid[VK_LUID_SIZE], bool luid_valid)
{
    Log("");
    Log("################ bridge: opening D3D12 session ################");

    IDXGIAdapter1 *game_adapter = PickAdapterByLuid(luid, luid_valid);
    IDXGIAdapter1 *adapter = game_adapter;
    IDXGIAdapter1 *selected = nullptr;
    DXGI_ADAPTER_DESC1 gd = {};
    UINT selected_index = UINT_MAX;
    const dlss_bridge::AdapterSelector &selector = g_cfg.compute_adapter;
    if (game_adapter == nullptr) { Log("[bridge] no usable game DXGI adapter"); goto fail; }

    game_adapter->GetDesc1(&gd);
    if (g_cfg.execution_mode == dlss_bridge::ExecutionMode::SameGpu ||
        selector.kind == dlss_bridge::SelectorKind::Game)
    {
        Log("[adapter] execution policy selected the game GPU");
    }
    else if (selector.kind == dlss_bridge::SelectorKind::Index)
    {
        selected_index = (UINT)selector.index;
        selected = PickAdapterByIndex(selected_index);
    }
    else if (selector.kind == dlss_bridge::SelectorKind::Luid)
    {
        selected = PickAdapterByExactLuid(selector.luid_high, selector.luid_low, &selected_index);
    }
    else if (g_cfg.execution_mode == dlss_bridge::ExecutionMode::SecondaryGpu ||
             g_cfg.execution_mode == dlss_bridge::ExecutionMode::Auto)
    {
        // Auto prefers a usable non-game adapter and naturally remains on the
        // game adapter when the host has only one GPU.
        selected = PickFirstOtherAdapter(gd.AdapterLuid, &selected_index);
    }

    if (selected)
    {
        DXGI_ADAPTER_DESC1 nd = {};
        selected->GetDesc1(&nd);
        if (memcmp(&gd.AdapterLuid, &nd.AdapterLuid, sizeof(LUID)) != 0)
        {
            adapter = selected;
            g_mgpu.active = true;
            g_mgpu.neural_index = selected_index;
            Log("[mgpu] enabled: game LUID=%08X:%08X, neural LUID=%08X:%08X",
                (unsigned)gd.AdapterLuid.HighPart, (unsigned)gd.AdapterLuid.LowPart,
                (unsigned)nd.AdapterLuid.HighPart, (unsigned)nd.AdapterLuid.LowPart);
        }
        else
        {
            selected->Release();
            selected = nullptr;
            if (g_cfg.execution_mode == dlss_bridge::ExecutionMode::SecondaryGpu)
            {
                Log("[adapter] secondary_gpu was required but the selector resolved to the game adapter");
                goto fail;
            }
            Log("[adapter] compute selector resolved to the game adapter; using same-GPU path");
        }
    }
    else if (g_cfg.execution_mode == dlss_bridge::ExecutionMode::SecondaryGpu)
    {
        Log("[adapter] secondary_gpu was required but no usable secondary adapter matched %s",
            selector.text);
        goto fail;
    }

    if (!g_components.SelectTransport(g_mgpu.active) || !g_components.Ready())
    {
        Log("[components] no valid transport for the selected adapter topology");
        goto fail;
    }
    Log("[components] host=%s capture=%s executor=%s transport=%s",
        g_components.host->id, g_components.capture->id,
        g_components.executor->id, g_components.transport->id);

    {
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto create_device = d3d12 ? reinterpret_cast<HRESULT (WINAPI *)(
            IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **)>(
                GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
        if (create_device == nullptr) { Log("[bridge] no D3D12CreateDevice"); goto fail; }

        HRESULT hr = S_OK;
        if (g_mgpu.active)
        {
            hr = create_device(game_adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device3),
                               reinterpret_cast<void **>(&g_mgpu.game_dev));
            if (FAILED(hr) || !g_mgpu.game_dev)
            { Log("[mgpu] game-side D3D12Device3 creation failed 0x%08X", hr); goto fail; }
        }
        hr = create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                           reinterpret_cast<void **>(&g_bridge.dev12));
        if (FAILED(hr) || g_bridge.dev12 == nullptr)
        { Log("[bridge] D3D12CreateDevice failed 0x%08X", hr); goto fail; }

        adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&g_bridge.adapter3));

        HMODULE ngx = FindNgxD3D12Loader();
        if (ngx == nullptr) { Log("[bridge] no module carries the complete NGX D3D12 export set"); goto fail; }
        g_ngx12_module = ngx;

        auto init_ext = reinterpret_cast<PFN_NGX_D3D12_Init_Ext>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
        auto init_proj = reinterpret_cast<PFN_NGX_D3D12_Init_ProjectID>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));
        g_bridge.alloc_params    = reinterpret_cast<PFN_AllocateParameters>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
        g_bridge.create_feature  = reinterpret_cast<PFN_D3D12CreateFeature>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
        g_bridge.eval_feature    = reinterpret_cast<PFN_D3D12EvaluateFeature>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
        g_bridge.release_feature = reinterpret_cast<PFN_D3D12ReleaseFeature>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
        if (!g_bridge.alloc_params || !g_bridge.create_feature || !g_bridge.eval_feature)
        { Log("[bridge] required NGX D3D12 exports missing"); goto fail; }
        D3D12ProbeEvalDetour("at export resolve");

        // Data path stays the bridge's own directory: proven to pass Init on
        // hardware and certain to be writable (the log lives there). Snippet
        // discovery is PathListInfo's job, not the data path's.
        wchar_t data_path[MAX_PATH] = {};
        GetModuleFileNameW(g_self, data_path, MAX_PATH);
        if (wchar_t *s = wcsrchr(data_path, L'\\')) *(s + 1) = L'\0';
        Log("[bridge] NGX data path: %ls", data_path);

        const NgxFeatureCommonInfo *common = NgxBuildCommonInfo();

        if (InterlockedCompareExchange(&g_ngx_identity.valid, 0, 0) == 0)
        { Log("[bridge] the game has not completed NGX initialization; refusing to invent an identity"); goto fail; }
        MemoryBarrier();
        const NgxInitIdentity identity = g_ngx_identity;
        DWORD code = 0;
        NVSDK_NGX_Result init_result = NGX_EXCEPTION_MARKER;
        if (identity.kind == NgxInitKind::ApplicationId && init_ext)
            init_result = SafeInitExt(init_ext, identity.application_id, data_path,
                                      g_bridge.dev12, identity.sdk_version, common, &code);
        else if (identity.kind == NgxInitKind::ProjectId && init_proj)
            init_result = SafeInitProjectID(init_proj, identity.project_id, identity.engine_type,
                identity.engine_version, data_path, g_bridge.dev12, identity.sdk_version, common, &code);
        else
        { Log("[bridge] matching D3D12 NGX initialization export is unavailable"); goto fail; }
        Log("[bridge] D3D12 NGX init kind=%s version=0x%02X -> %s",
            identity.kind == NgxInitKind::ProjectId ? "project" : "application",
            identity.sdk_version, code ? "fault" : (init_result == NGX_SUCCESS ? "ok" : "refused"));
        if (code) { Log("[bridge] NGX D3D12 init raised 0x%08X", code); goto fail; }
        if (init_result != NGX_SUCCESS) { Log("[bridge] NGX refused the game's captured identity"); goto fail; }
        Log("[bridge] NGX D3D12 session initialised");
        D3D12ProbeEvalDetour("after D3D12 init");

        // A session that cannot host SuperSampling will refuse every
        // CreateFeature shape; learn that now instead of after building
        // the queue, textures and fences.
        if (D3D12QueryNgxCaps() == 0)
        { Log("[bridge] SuperSampling unavailable to this session despite explicit search paths"); goto fail; }

        if (FAILED(CreateDirectQueue(g_bridge.dev12, &g_bridge.queue, "neural evaluate")))
        { Log("[bridge] could not create the neural evaluate queue"); goto fail; }
        D3D12InitTimingQueries();
        if (g_mgpu.active)
        {
            // Vulkan-shared textures require a DIRECT queue on the game adapter.
            // High priority keeps this small copy from sitting behind unrelated
            // rendering while the Vulkan frame is parked waiting for it.
            if (FAILED(CreateDirectQueue(g_mgpu.game_dev, &g_mgpu.game_queue, "game-adapter copy")))
            { Log("[mgpu] could not create the game-adapter copy queue"); goto fail; }
            CreateCopyQueue(g_bridge.dev12, &g_mgpu.neural_copy_queue, "neural-adapter");
            for (int slot = 0; slot < ActiveFrameCount(); ++slot)
            {
                g_mgpu.game_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_mgpu.game_alloc[slot]));
                if (g_mgpu.game_alloc[slot])
                    g_mgpu.game_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                        g_mgpu.game_alloc[slot], nullptr, __uuidof(ID3D12GraphicsCommandList),
                        reinterpret_cast<void **>(&g_mgpu.game_list[slot]));
                if (g_mgpu.game_list[slot]) g_mgpu.game_list[slot]->Close();

                g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                    __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_mgpu.neural_copy_alloc[slot]));
                if (g_mgpu.neural_copy_alloc[slot])
                    g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                        g_mgpu.neural_copy_alloc[slot], nullptr, __uuidof(ID3D12GraphicsCommandList),
                        reinterpret_cast<void **>(&g_mgpu.neural_copy_list[slot]));
                if (g_mgpu.neural_copy_list[slot]) g_mgpu.neural_copy_list[slot]->Close();

                g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                    __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_mgpu.neural_download_alloc[slot]));
                if (g_mgpu.neural_download_alloc[slot])
                    g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
                        g_mgpu.neural_download_alloc[slot], nullptr, __uuidof(ID3D12GraphicsCommandList),
                        reinterpret_cast<void **>(&g_mgpu.neural_download_list[slot]));
                if (g_mgpu.neural_download_list[slot]) g_mgpu.neural_download_list[slot]->Close();
            }

            hr = g_mgpu.game_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                               reinterpret_cast<void **>(&g_mgpu.game_fence));
            g_mgpu.game_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool copy_resources_ready = g_mgpu.game_queue && g_mgpu.neural_copy_queue;
            for (int slot = 0; slot < ActiveFrameCount(); ++slot)
                copy_resources_ready = copy_resources_ready && g_mgpu.game_alloc[slot] &&
                    g_mgpu.game_list[slot] && g_mgpu.neural_copy_alloc[slot] &&
                    g_mgpu.neural_copy_list[slot] && g_mgpu.neural_download_alloc[slot] &&
                    g_mgpu.neural_download_list[slot];
            if (!copy_resources_ready || FAILED(hr) || !g_mgpu.game_fence || !g_mgpu.game_event)
            { Log("[mgpu] per-frame copy-queue infrastructure creation failed"); goto fail; }
        }
        for (int i = 0; i < ActiveFrameCount(); ++i)
            g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_bridge.alloc[i]));
        if (g_bridge.alloc[0])
            g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bridge.alloc[0], nullptr,
                __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_bridge.list));
        if (g_bridge.list) g_bridge.list->Close();
        if (g_mgpu.active)
        {
            bool pipeline_lists_ready = true;
            for (int slot = 0; slot < ActiveFrameCount(); ++slot)
            {
                if (!g_bridge.alloc[slot] ||
                    FAILED(g_bridge.dev12->CreateCommandList(
                        0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bridge.alloc[slot],
                        nullptr, __uuidof(ID3D12GraphicsCommandList),
                        reinterpret_cast<void **>(&g_mgpu.pipeline_list[slot]))))
                    pipeline_lists_ready = false;
                if (g_mgpu.pipeline_list[slot])
                    g_mgpu.pipeline_list[slot]->Close();
            }
            if (!pipeline_lists_ready)
            { Log("[pipeline] per-slot neural command list creation failed"); goto fail; }
        }
        g_bridge.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        hr = g_bridge.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                         reinterpret_cast<void **>(&g_bridge.fence_gpu));
        if (FAILED(hr)) { Log("[bridge] retire fence failed 0x%08X", hr); goto fail; }

        if (!D3D12CreateSharedFence(&g_bridge.fence_in,  &g_shared_fence_in))  goto fail;
        if (!D3D12CreateSharedFence(&g_bridge.fence_out, &g_shared_fence_out)) goto fail;

        NVSDK_NGX_Result rp = SafeAllocParams(g_bridge.alloc_params, &g_bridge.params, &code);
        if (code || rp != NGX_SUCCESS || g_bridge.params == nullptr)
        { Log("[bridge] AllocateParameters failed 0x%08X, %s", rp, NgxResultName(rp)); goto fail; }

        if (g_bridge.queue) g_bridge.queue->SetName(L"dlss5-vk-bridge NGX queue");
        if (g_bridge.list)  g_bridge.list->SetName(L"dlss5-vk-bridge NGX list");
        if (g_mgpu.game_queue) g_mgpu.game_queue->SetName(L"dlss5-vk-bridge game copy queue");
        if (g_mgpu.neural_copy_queue) g_mgpu.neural_copy_queue->SetName(L"dlss5-vk-bridge neural copy queue");

        Log("[bridge] session ready: queue=%p list=%p fence_in=%p fence_out=%p (%d frames)",
            (void *)g_bridge.queue, (void *)g_bridge.list, (void *)g_bridge.fence_in,
            (void *)g_bridge.fence_out, ActiveFrameCount());
        Log("############# bridge: session open #############\n");
        if (adapter != game_adapter) game_adapter->Release();
        adapter->Release();
        game_adapter = adapter = nullptr;
        g_bridge.session_ready = true;
        return true;
    }

fail:
    if (adapter && adapter != game_adapter) adapter->Release();
    if (game_adapter) game_adapter->Release();
    BridgeDisable("the D3D12 session failed to start");
    Log("############# bridge: session failed #############");
    return false;
}

// Builds the D3D12 DLSS feature for the current render/output geometry. The
// texture params are the shared textures (imported into Vulkan elsewhere); the
// per-frame scalars are filled in later, before each evaluate.
// After a create failure, pull every diagnostic NGX and D3D12 will give: a
// removed device explains everything on its own; otherwise the capability
// block says whether this session may host SuperSampling at all and what the
// snippet's own init result was.
static void D3D12LogCreateFailureDetail()
{
    HRESULT drr = g_bridge.dev12 ? g_bridge.dev12->GetDeviceRemovedReason() : S_OK;
    if (drr != S_OK) { Log("[bridge] D3D12 device removed: 0x%08X", drr); return; }
    D3D12QueryNgxCaps();
}

static bool D3D12CreateDlssFeature(const FeatureCreateContract &contract)
{
    if (g_bridge.feature && g_bridge.release_feature)
    { g_bridge.release_feature(g_bridge.feature); g_bridge.feature = nullptr; }

    g_bridge.params->Set("Width",     contract.width);
    g_bridge.params->Set("Height",    contract.height);
    g_bridge.params->Set("OutWidth",  contract.out_width);
    g_bridge.params->Set("OutHeight", contract.out_height);
    g_bridge.params->Set("PerfQualityValue", contract.quality);
    g_bridge.params->Set("DLSS.Feature.Create.Flags", contract.flags);
    if (contract.has_output_subrects)
        g_bridge.params->Set("DLSS.Enable.Output.Subrects", contract.output_subrects);
    g_bridge.feature_flags = contract.flags;

    // These masks describe the single-node private D3D12 device rather than a
    // game policy, so they are determined by the executor.
    g_bridge.params->Set("CreationNodeMask", 1u);
    g_bridge.params->Set("VisibilityNodeMask", 1u);
    Log("[bridge] exact create contract: %ux%u -> %ux%u quality=%u flags=0x%X%s",
        contract.width, contract.height, contract.out_width, contract.out_height,
        contract.quality, contract.flags,
        contract.has_output_subrects ? " with output-subrect policy" : "");

    g_bridge.feature = nullptr;
    if (!D3D12BeginCommands()) { Log("[bridge] could not start a command list for create"); return false; }

    DWORD code = 0;
    NVSDK_NGX_Result rf = SafeCreateFeature(g_bridge.create_feature, g_bridge.list,
                                            g_bridge.params, &g_bridge.feature, &code);
    const UINT64 v = D3D12EndCommands();

    if (g_bridge.fence_gpu->GetCompletedValue() < v)
    {
        g_bridge.fence_gpu->SetEventOnCompletion(v, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 4000) != WAIT_OBJECT_0)
        { Log("[bridge] feature creation did not complete within 4 s"); BridgeDisable("creating the DLSS feature stalled"); return false; }
    }
    if (code)
    { Warn("creating the DLSS feature crashed (0x%08X)", code); BridgeDisable("the DLSS feature crashed"); return false; }
    if (rf != NGX_SUCCESS || g_bridge.feature == nullptr)
    {
        Log("[bridge] exact CreateFeature contract failed 0x%08X, %s", rf, NgxResultName(rf));
        D3D12LogCreateFailureDetail();
        return false;
    }

    Log("[bridge] feature ready: render %ux%u -> output %ux%u handle=%p",
        g_bridge.render_w, g_bridge.render_h, g_bridge.ngx_out_w, g_bridge.ngx_out_h,
        (void *)g_bridge.feature);
    g_bridge.need_reset = true;
    return true;
}

// The D3D12 half of one frame: wait for the Vulkan inputs, run the evaluate
// (where the DLSS 5 add-on inserts its pass), signal that the output is ready.
// The texture params are already bound; per-frame scalars are set by the caller.
//
// The core runs on the in-frame handoff worker thread (vk_interop.inc). It must
// not tear the bridge
// down itself -- teardown joins that worker -- so it only reports:
//   0 = ok, 1 = NGX refused, 2 = NGX raised an exception, 4 = no command list.
// It signals fence_out on EVERY path because the game's command buffer is
// blocked until that value lands, and skipping it would hang the GPU.
static bool UnifiedNeuralQueueActive()
{
    if (g_cfg.neural_queue_mode != dlss_bridge::NeuralQueueMode::Unified) return false;
    const bool supported = g_mgpu.direct_vulkan_upload && g_mgpu.direct_vulkan_download;
    static bool reported = false;
    if (!reported)
    {
        reported = true;
        if (supported)
            Log("[mgpu] unified neural DIRECT queue active: upload, evaluate, and download use one submission");
        else
            Log("[mgpu] unified neural queue requested but direct Vulkan host imports are incomplete; using split queues");
    }
    return supported;
}

static void RecordMultiGpuUpload(ID3D12GraphicsCommandList *list, int transport_slot)
{
    CrossFrameTransport &fr = g_mgpu.frame[transport_slot];
    const int inputs[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    ID3D12Resource *source = g_mgpu.direct_vulkan_upload
        ? fr.neural_staging[CROSS_UPLOAD] : fr.neural_buffer[CROSS_UPLOAD];
    if (!g_mgpu.direct_vulkan_upload)
        Barrier(list, source, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (int input : inputs)
    {
        Barrier(list, g_mgpu.neural_tex[input], D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
        CopyCrossToTexture(list, source,
                           g_mgpu.neural_tex[input], g_mgpu.plane[input]);
        Barrier(list, g_mgpu.neural_tex[input], D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON);
    }
    if (!g_mgpu.direct_vulkan_upload)
        Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON);
}

static void RecordMultiGpuDownload(ID3D12GraphicsCommandList *list, int transport_slot)
{
    CrossFrameTransport &fr = g_mgpu.frame[transport_slot];
    ID3D12Resource *destination = g_mgpu.direct_vulkan_download
        ? fr.neural_staging[CROSS_DOWNLOAD] : fr.neural_buffer[CROSS_DOWNLOAD];
    if (!g_mgpu.direct_vulkan_download)
        Barrier(list, destination, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, g_mgpu.neural_tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyTextureToCross(list, g_mgpu.neural_tex[SLOT_OUTPUT],
                       destination, g_mgpu.plane[SLOT_OUTPUT]);
    Barrier(list, g_mgpu.neural_tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    if (!g_mgpu.direct_vulkan_download)
        Barrier(list, destination, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON);
}

static bool PrepareNeuralUpload(int transport_slot)
{
    if (!g_mgpu.direct_vulkan_upload) return true;
    CrossFrameTransport &fr = g_mgpu.frame[transport_slot];
    if (!fr.vulkan_input_memory || !fr.neural_staging_mapped[CROSS_UPLOAD]) return false;
    const int inputs[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    BYTE *destination = static_cast<BYTE *>(fr.neural_staging_mapped[CROSS_UPLOAD]);
    const BYTE *source = static_cast<const BYTE *>(fr.vulkan_input_memory);
    for (int input : inputs)
    {
        const CrossPlane &plane = g_mgpu.plane[input];
        const SIZE_T bytes = (SIZE_T)plane.row_pitch * plane.height;
        if (plane.offset + bytes > g_mgpu.host_bytes) return false;
        memcpy(destination + plane.offset, source + plane.offset, bytes);
    }
    MemoryBarrier();
    return true;
}

static bool MultiGpuUploadInputs(int transport_slot, UINT64 *upload_value)
{
    *upload_value = 0;
    CrossFrameTransport &fr = g_mgpu.frame[transport_slot];
    if (fr.generation != g_mgpu.transport_generation) return false;
    if (!PrepareNeuralUpload(transport_slot)) return false;
    const int inputs[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    if (!g_mgpu.direct_vulkan_upload)
    {
        if (!GameBeginCommands(transport_slot)) return false;
        ID3D12GraphicsCommandList *list = g_mgpu.game_list[transport_slot];
        Barrier(list, fr.game_buffer[CROSS_UPLOAD], D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
        for (int input : inputs)
        {
            Barrier(list, g_bridge.tex[input].tex12, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            CopyTextureToCross(list, g_bridge.tex[input].tex12,
                               fr.game_buffer[CROSS_UPLOAD], g_mgpu.plane[input]);
            Barrier(list, g_bridge.tex[input].tex12, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
        }
        Barrier(list, fr.game_buffer[CROSS_UPLOAD], D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON);
        if (!GameEndAndWait(transport_slot, "game adapter input copy", 60)) return false;
    }
    if (!NeuralCopyBegin(transport_slot)) return false;
    RecordMultiGpuUpload(g_mgpu.neural_copy_list[transport_slot], transport_slot);
    *upload_value = NeuralCopyEnd(transport_slot);
    fr.upload_fence = *upload_value;
    return *upload_value != 0;
}

static bool PublishVulkanOutputFrom(int producer_slot, int consumer_slot)
{
    CrossFrameTransport &producer = g_mgpu.frame[producer_slot];
    CrossFrameTransport &consumer = g_mgpu.frame[consumer_slot];
    if (!g_mgpu.direct_vulkan_download) return producer_slot == consumer_slot;
    if (!producer.neural_staging_mapped[CROSS_DOWNLOAD] || !consumer.vulkan_output_memory)
        return false;
    const CrossPlane &plane = g_mgpu.plane[SLOT_OUTPUT];
    const SIZE_T bytes = (SIZE_T)plane.row_pitch * plane.height;
    if (plane.offset + bytes > g_mgpu.host_bytes) return false;
    memcpy(static_cast<BYTE *>(consumer.vulkan_output_memory) + plane.offset,
           static_cast<const BYTE *>(producer.neural_staging_mapped[CROSS_DOWNLOAD]) + plane.offset,
           bytes);
    MemoryBarrier();
    return true;
}

static bool PublishVulkanOutput(int transport_slot)
{
    return PublishVulkanOutputFrom(transport_slot, transport_slot);
}

// The caller must qualify evaluate_value before invoking this function. Once a
// download has been submitted its target allocation is immutable until the
// copy fence completes; Vulkan is never released to read an in-flight write.
static bool MultiGpuDownloadOutput(UINT64 timeline, UINT64 pipeline_value,
                                   int transport_slot)
{
    CrossFrameTransport &fr = g_mgpu.frame[transport_slot];
    if (fr.generation != g_mgpu.transport_generation) return false;

    // The DIRECT evaluation submission has already copied neural Output into
    // this frame's D3D-only producer allocation. Publish it to Vulkan only
    // after that submission's fence completed.
    if (!PublishVulkanOutput(transport_slot)) return false;
    fr.output_fence = pipeline_value;
    fr.output_timeline = timeline;
    fr.output_valid = true;
    if (g_mgpu.direct_vulkan_download)
        return SUCCEEDED(g_bridge.fence_out->Signal(timeline));

    if (!GameBeginCommands(transport_slot)) return false;
    ID3D12GraphicsCommandList *list = g_mgpu.game_list[transport_slot];
    Barrier(list, fr.game_buffer[CROSS_DOWNLOAD], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, g_bridge.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
    CopyCrossToTexture(list, fr.game_buffer[CROSS_DOWNLOAD],
                       g_bridge.tex[SLOT_OUTPUT].tex12, g_mgpu.plane[SLOT_OUTPUT]);
    Barrier(list, g_bridge.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, fr.game_buffer[CROSS_DOWNLOAD], D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    if (!GameEndAndWait(transport_slot, "game adapter output copy", 1000)) return false;
    return SUCCEEDED(g_bridge.fence_out->Signal(timeline));
}

static void BridgeReleaseVkOutputEarly(int transport_slot);

static int NeuralPipelineFrames()
{
    return g_cfg.neural_pipeline_frames;
}

// The direct cross-adapter route keeps several ordered NGX evaluations queued.
// NVIDIA/vkd3d can occasionally spend roughly two frame periods inside the
// private queue's submission even though the recorded GPU work is short.
// Waiting for that same submission in the same Vulkan frame turns the driver
// delay into a visible hitch. This path instead presents exactly the producer
// neural_pipeline_frames admissions behind the current frame. It never picks
// an arbitrary last result and never advances presentation past an incomplete
// producer: a genuine throughput deficit becomes explicit backpressure.
static void D3D12RunEvaluatePipelined(UINT64 timeline, LONG *result, int transport_slot)
{
    *result = 0;
    CrossFrameTransport &producer = g_mgpu.frame[transport_slot];
    if (producer.generation != g_mgpu.transport_generation ||
        (producer.producer_valid && !producer.producer_presented))
    {
        Log("[pipeline] transport slot %d was reused before its producer was presented",
            transport_slot);
        g_bridge.fence_out->Signal(timeline);
        *result = 4;
        return;
    }

    const UINT64 admission = g_mgpu.producer_sequence + 1;
    const int delay = NeuralPipelineFrames();
    if (admission > (UINT64)delay)
    {
        const UINT64 wanted = g_mgpu.next_present_sequence
            ? g_mgpu.next_present_sequence : 1;
        int source_slot = -1;
        for (int i = 0; i < ActiveFrameCount(); ++i)
            if (g_mgpu.frame[i].producer_valid &&
                !g_mgpu.frame[i].producer_presented &&
                g_mgpu.frame[i].producer_sequence == wanted)
            {
                source_slot = i;
                break;
            }
        if (source_slot < 0)
        {
            Log("[pipeline] ordered producer %llu is missing at admission %llu",
                (unsigned long long)wanted, (unsigned long long)admission);
            g_bridge.fence_out->Signal(timeline);
            *result = 4;
            return;
        }

        CrossFrameTransport &source = g_mgpu.frame[source_slot];
        const ULONGLONG wait_start = GetTickCount64();
        if (!D3D12WaitNeural(source.producer_fence,
                             "ordered neural pipeline backpressure", 2000))
        {
            g_bridge.fence_out->Signal(timeline);
            *result = 16;
            return;
        }
        const ULONGLONG waited = GetTickCount64() - wait_start;
        if (waited >= 8)
            Log("[pipeline] waited %llums for ordered producer %llu (source frame %llu)",
                (unsigned long long)waited, (unsigned long long)wanted,
                (unsigned long long)source.producer_timeline);

        if (!PublishVulkanOutputFrom(source_slot, transport_slot))
        {
            g_bridge.fence_out->Signal(timeline);
            *result = 4;
            return;
        }
        producer.output_fence = source.producer_fence;
        producer.output_timeline = source.producer_timeline;
        producer.output_valid = true;
        source.producer_presented = true;
        g_mgpu.next_present_sequence = wanted + 1;

        const double neural_gpu_ms = D3D12TimingReport(source.producer_timing_slot);
        ProfileAdd(0.0, (double)waited, neural_gpu_ms, 0.0, (double)waited);
    }

    // Release the Vulkan sandwich before entering vkd3d's submit path. The
    // current producer uses distinct upload/readback memory, and this slot
    // cannot retire on the game thread until WorkerRunSlot publishes its final
    // worker_done flag after the submission returns.
    if (FAILED(g_bridge.fence_out->Signal(timeline)))
    {
        *result = 4;
        return;
    }
    BridgeReleaseVkOutputEarly(transport_slot);

    ID3D12GraphicsCommandList *list = nullptr;
    if (!PrepareNeuralUpload(transport_slot) ||
        !D3D12BeginPipelineCommands(transport_slot, &list))
    {
        *result = 4;
        return;
    }

    const int timing_slot = transport_slot;
    D3D12TimingStampOn(list, timing_slot, 0);
    RecordMultiGpuUpload(list, transport_slot);
    D3D12TimingStampOn(list, timing_slot, 1);

    Barrier(list, NeuralTex(SLOT_COLOR),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, NeuralTex(SLOT_DEPTH),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, NeuralTex(SLOT_MV),     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, NeuralTex(SLOT_OUTPUT), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    DWORD code = 0;
    NVSDK_NGX_Result evaluated = NGX_SUCCESS;
    static const char kMark[] = "ngx evaluate";
    list->BeginEvent(1, kMark, sizeof(kMark));
    const bool toggle_pulse = BeginNeuralTogglePulse();
    evaluated = SafeEvaluate(g_bridge.eval_feature, list,
                             g_bridge.feature, g_bridge.params, &code);
    EndNeuralTogglePulse(toggle_pulse);
    list->EndEvent();

    Barrier(list, NeuralTex(SLOT_COLOR),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, NeuralTex(SLOT_DEPTH),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, NeuralTex(SLOT_MV),     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, NeuralTex(SLOT_OUTPUT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    D3D12TimingStampOn(list, timing_slot, 2);
    RecordMultiGpuDownload(list, transport_slot);
    D3D12TimingStampOn(list, timing_slot, 3);
    D3D12TimingResolveOn(list, timing_slot);

    const UINT64 fence = D3D12EnqueuePipelineCommands(list, transport_slot);
    if (!fence)
    {
        *result = 4;
        return;
    }

    producer.producer_fence = fence;
    producer.producer_timeline = timeline;
    producer.producer_sequence = admission;
    producer.producer_timing_slot = timing_slot;
    producer.producer_valid = true;
    producer.producer_presented = false;
    g_mgpu.producer_sequence = admission;

    if (code)
    {
        Log("[bridge] evaluate raised 0x%08X", code);
        *result = 2;
    }
    else if (evaluated != NGX_SUCCESS)
    {
        Log("[bridge] evaluate failed 0x%08X, %s", evaluated,
            NgxResultName(evaluated));
        *result = 1;
    }
}

static void D3D12RunEvaluateCore(UINT64 v, LONG *result, int transport_slot)
{
    if (g_mgpu.active && UnifiedNeuralQueueActive() &&
        g_mgpu.direct_vulkan_upload && g_mgpu.direct_vulkan_download)
    {
        D3D12RunEvaluatePipelined(v, result, transport_slot);
        return;
    }
    *result = 0;
    const ULONGLONG timing_start = GetTickCount64();
    ULONGLONG timing_uploaded = timing_start;
    ULONGLONG timing_evaluated = timing_start;
    UINT64 upload_value = 0;
    bool unified = false;
    int timing_slot = -1;
    if (g_mgpu.active)
    {
        g_mgpu_output_timeout = false;
        if (g_mgpu_pending_game != 0)
        {
            if (g_mgpu.game_fence->GetCompletedValue() < g_mgpu_pending_game)
            {
                ++g_mgpu_game_repeats;
                ProfileRepeat(v, transport_slot);
                g_bridge.fence_out->Signal(v);
                *result = 32;
                return;
            }
            Log("[mgpu] game queue recovered after %llu repeated neural frame(s)",
                (unsigned long long)g_mgpu_game_repeats);
            g_mgpu_pending_game = 0;
            g_mgpu_game_repeats = 0;
        }
        if (g_mgpu_pending_neural != 0)
        {
            if (g_bridge.fence_gpu->GetCompletedValue() < g_mgpu_pending_neural)
            {
                ++g_mgpu_dropped_outputs;
                ProfileRepeat(v, transport_slot);
                g_bridge.fence_out->Signal(v);
                *result = 32;
                return;
            }
            if (g_mgpu_pending_timing_slot >= 0)
            {
                double upload_ms = 0.0, neural_ms = 0.0, download_ms = 0.0;
                UINT64 gpu_start = 0, gpu_end = 0;
                if (g_mgpu_pending_timing_unified &&
                    D3D12TimingReadPipeline(g_mgpu_pending_timing_slot,
                                            &upload_ms, &neural_ms, &download_ms,
                                            &gpu_start, &gpu_end))
                {
                    const double gpu_ms = upload_ms + neural_ms + download_ms;
                    const ULONGLONG retired_at = GetTickCount64();
                    const ULONGLONG wall_ms = g_mgpu_pending_submitted_at
                        ? retired_at - g_mgpu_pending_submitted_at : 0;
                    double queue_start_ms = -1.0, observation_ms = -1.0;
                    LARGE_INTEGER qpc_now = {}, qpc_frequency = {};
                    if (g_mgpu_pending_submit_qpc && g_mgpu_pending_cpu_calibration &&
                        g_mgpu_pending_gpu_calibration && QueryPerformanceCounter(&qpc_now) &&
                        QueryPerformanceFrequency(&qpc_frequency) && qpc_frequency.QuadPart)
                    {
                        const double qpc_per_gpu_tick =
                            (double)qpc_frequency.QuadPart / (double)g_bridge.timing_frequency;
                        const double start_qpc = (double)g_mgpu_pending_cpu_calibration +
                            ((double)gpu_start - (double)g_mgpu_pending_gpu_calibration) *
                            qpc_per_gpu_tick;
                        const double end_qpc = (double)g_mgpu_pending_cpu_calibration +
                            ((double)gpu_end - (double)g_mgpu_pending_gpu_calibration) *
                            qpc_per_gpu_tick;
                        queue_start_ms = (start_qpc - (double)g_mgpu_pending_submit_qpc) *
                            1000.0 / (double)qpc_frequency.QuadPart;
                        observation_ms = ((double)qpc_now.QuadPart - end_qpc) *
                            1000.0 / (double)qpc_frequency.QuadPart;
                    }
                    Log("[mgpu] retired late submission: wall=%llums queue-start=%.3fms "
                        "gpu=%.3fms (upload=%.3f neural=%.3f download=%.3f) "
                        "post-gpu-observation=%.3fms",
                        (unsigned long long)wall_ms, queue_start_ms, gpu_ms,
                        upload_ms, neural_ms, download_ms, observation_ms);
                }
                g_mgpu_pending_timing_slot = -1;
                g_mgpu_pending_submitted_at = 0;
                g_mgpu_pending_gpu_calibration = 0;
                g_mgpu_pending_cpu_calibration = 0;
                g_mgpu_pending_submit_qpc = 0;
            }
            Log("[mgpu] GPU 1 recovered after %llu repeated neural frame(s)",
                (unsigned long long)g_mgpu_dropped_outputs);
            g_mgpu_pending_neural = 0;
            g_mgpu_dropped_outputs = 0;
        }

        unified = UnifiedNeuralQueueActive();
        if (unified)
        {
            if (!PrepareNeuralUpload(transport_slot) ||
                !D3D12BeginCommands())
            {
                g_bridge.fence_out->Signal(v);
                *result = 4;
                return;
            }
            timing_slot = g_bridge.frame_slot;
            D3D12TimingStamp(timing_slot, 0);
            RecordMultiGpuUpload(g_bridge.list, transport_slot);
            D3D12TimingStamp(timing_slot, 1);
        }
        else if (!MultiGpuUploadInputs(transport_slot, &upload_value))
        {
            g_bridge.fence_out->Signal(v);
            *result = g_mgpu_pending_game ? 32 : 4;
            return;
        }
        timing_uploaded = GetTickCount64();
    }
    if (g_mgpu.active && !unified &&
        FAILED(g_bridge.queue->Wait(g_bridge.fence_gpu, upload_value)))
    {
        g_bridge.fence_out->Signal(v); *result = 4; return;
    }
    if (!unified && !D3D12BeginCommands())
    {
        if (g_mgpu.active) g_bridge.fence_out->Signal(v);
        else g_bridge.queue->Signal(g_bridge.fence_out, v);
        *result = 4;
        return;
    }
    if (!unified)
    {
        timing_slot = g_bridge.frame_slot;
        D3D12TimingStamp(timing_slot, 0);
    }

    Barrier(g_bridge.list, NeuralTex(SLOT_COLOR),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, NeuralTex(SLOT_DEPTH),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, NeuralTex(SLOT_MV),     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, NeuralTex(SLOT_OUTPUT), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    DWORD code = 0;
    NVSDK_NGX_Result re = NGX_SUCCESS;
    static const char kMark[] = "ngx evaluate";
    g_bridge.list->BeginEvent(1, kMark, sizeof(kMark));
    const bool toggle_pulse = BeginNeuralTogglePulse();
    re = SafeEvaluate(g_bridge.eval_feature, g_bridge.list, g_bridge.feature, g_bridge.params, &code);
    EndNeuralTogglePulse(toggle_pulse);
    g_bridge.list->EndEvent();

    Barrier(g_bridge.list, NeuralTex(SLOT_COLOR),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, NeuralTex(SLOT_DEPTH),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, NeuralTex(SLOT_MV),     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, NeuralTex(SLOT_OUTPUT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (unified) D3D12TimingStamp(timing_slot, 2);
    else
    {
        D3D12TimingStamp(timing_slot, 1);
        if (g_mgpu.active)
            RecordMultiGpuDownload(g_bridge.list, transport_slot);
        D3D12TimingResolveNeural(timing_slot);
    }

    if (g_mgpu.active && unified)
    {
        RecordMultiGpuDownload(g_bridge.list, transport_slot);
        D3D12TimingStamp(timing_slot, 3);
        D3D12TimingResolve(timing_slot);
        const UINT64 pipeline_value = D3D12EndCommands();
        const ULONGLONG pipeline_submitted_at = GetTickCount64();
        timing_evaluated = GetTickCount64();
        if (!pipeline_value ||
            !D3D12WaitNeural(pipeline_value, "GPU 1 unified pipeline",
                             NeuralPipelineDeadlineMs()))
        {
            g_mgpu_pending_neural = pipeline_value;
            g_mgpu_pending_timing_slot = timing_slot;
            g_mgpu_pending_timing_unified = true;
            g_mgpu_pending_submitted_at = pipeline_submitted_at;
            ++g_mgpu_dropped_outputs;
            ProfileRepeat(v, transport_slot);
            g_mgpu_output_timeout = true;
            g_bridge.fence_out->Signal(v);
            *result = 32;
            return;
        }
        timing_evaluated = GetTickCount64();
        CrossFrameTransport &transport = g_mgpu.frame[transport_slot];
        if (!PublishVulkanOutput(transport_slot))
        {
            *result = 4;
            return;
        }
        transport.output_fence = pipeline_value;
        transport.output_timeline = v;
        transport.output_valid = true;
        const double neural_gpu_ms = D3D12TimingReport(timing_slot);
        const ULONGLONG prof_done = GetTickCount64();
        ProfileAdd((double)(timing_uploaded - timing_start),
                   (double)(timing_evaluated - timing_uploaded), neural_gpu_ms,
                   (double)(prof_done - timing_evaluated),
                   (double)(prof_done - timing_start));
        if (FAILED(g_bridge.fence_out->Signal(v)))
        {
            *result = 4;
            return;
        }
    }
    else
    {
        UINT64 gpu_calibration = 0, cpu_calibration = 0, submit_qpc = 0;
        D3D12CaptureClockCalibration(&gpu_calibration, &cpu_calibration, &submit_qpc);
        const UINT64 evaluate_value = D3D12EndCommands();
        const ULONGLONG evaluate_submitted_at = GetTickCount64();
        timing_evaluated = GetTickCount64();
        if (g_mgpu.active)
        {
            if (!D3D12WaitNeural(evaluate_value, "GPU 1 neural plus download", NeuralPipelineDeadlineMs()))
            {
                g_mgpu_pending_neural = evaluate_value;
                g_mgpu_pending_timing_slot = timing_slot;
                g_mgpu_pending_timing_unified = false;
                g_mgpu_pending_submitted_at = evaluate_submitted_at;
                g_mgpu_pending_gpu_calibration = gpu_calibration;
                g_mgpu_pending_cpu_calibration = cpu_calibration;
                g_mgpu_pending_submit_qpc = submit_qpc;
                ++g_mgpu_dropped_outputs;
                ProfileRepeat(v, transport_slot);
                g_mgpu_output_timeout = true;
                g_bridge.fence_out->Signal(v);
                *result = 32;
                return;
            }
            timing_evaluated = GetTickCount64();
            const double neural_gpu_ms = D3D12TimingReadNeural(timing_slot);
            if (!MultiGpuDownloadOutput(v, evaluate_value, transport_slot))
            {
                g_bridge.fence_out->Signal(v);
                *result = g_mgpu_pending_game ? 32 : 4;
                return;
            }
            const ULONGLONG prof_done = GetTickCount64();
            ProfileAdd((double)(timing_uploaded - timing_start),
                       (double)(timing_evaluated - timing_uploaded), neural_gpu_ms,
                       (double)(prof_done - timing_evaluated),
                       (double)(prof_done - timing_start));
        }
        else
        {
            g_bridge.queue->Signal(g_bridge.fence_out, v);
        }
    }

    if (g_mgpu.active)
    {
        static unsigned timing_frames = 0;
        const ULONGLONG timing_done = GetTickCount64();
        const ULONGLONG timing_total = timing_done - timing_start;
        if (timing_frames++ < 16 || timing_total >= 50)
        {
            if (unified)
                Log("[mgpu] %s unified frame timing: record=%llums gpu1=%llums total=%llums",
                    timing_total >= 50 ? "slow" : "startup",
                    (unsigned long long)(timing_evaluated - timing_start),
                    (unsigned long long)(timing_done - timing_evaluated),
                    (unsigned long long)timing_total);
            else
                Log("[mgpu] %s frame timing: upload=%llums evaluate=%llums download=%llums total=%llums",
                    timing_total >= 50 ? "slow" : "startup",
                    (unsigned long long)(timing_uploaded - timing_start),
                    (unsigned long long)(timing_evaluated - timing_uploaded),
                    (unsigned long long)(timing_done - timing_evaluated),
                    (unsigned long long)timing_total);
        }
    }

    if (code)              { Log("[bridge] evaluate raised 0x%08X", code); *result = 2; return; }
    if (re != NGX_SUCCESS) { Log("[bridge] evaluate failed 0x%08X, %s", re, NgxResultName(re)); *result = 1; return; }
}
