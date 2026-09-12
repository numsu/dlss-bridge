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
                                          const wchar_t *path, ID3D12Device *dev, int ver,
                                          const void *info, DWORD *code)
{
    *code = 0;
    __try { return fn(proj, 0 /*ENGINE_CUSTOM*/, "1.0", path, dev, ver, info); }
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
    if (dlss_bridge::IsStrictNeural(g_cfg) && g_neural_only_latched)
    {
        // Preserve the hook as a fail-closed gate. Unhooking here would silently
        // resume the game-side upscaler, violating the latched neural-only policy.
        Warn("stopped: %s. Neural-only mode is latched; subsequent game DLSS evaluates "
             "will fail instead of falling back to the original path.", why);
        D3D12ReleaseResources();
        return;
    }
    Warn("stopped during bootstrap: %s. See dlss5-vk-bridge.log for detail.", why);
    BridgeUnhookAll();
    D3D12ReleaseResources();
}

static void BridgeFail(const char *what)
{
    g_bridge.frame_ready = false;
    if (++g_bridge.consecutive_fails >= 3)
    {
        Log("[bridge] disabled after repeated failures (%s); policy determines whether "
            "bootstrap unhooks or latched neural-only mode fails closed.", what);
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
    g_bridge.frame_slot = (g_bridge.frame_slot + 1) % Bridge::kFrames;
    return v;
}


static void D3D12InitTimingQueries()
{
    if (!g_cfg.verbose || !g_bridge.dev12 || !g_bridge.queue) return;

    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = Bridge::kFrames * 4;
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

static void D3D12TimingStamp(int slot, unsigned point)
{
    if (!g_bridge.timing_heap) return;
    g_bridge.list->EndQuery(g_bridge.timing_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                            (UINT)(slot * 4 + point));
}

static void D3D12TimingResolve(int slot)
{
    if (!g_bridge.timing_heap || !g_bridge.timing_readback) return;
    const UINT first = (UINT)(slot * 4);
    g_bridge.list->ResolveQueryData(g_bridge.timing_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                    first, 4, g_bridge.timing_readback,
                                    sizeof(UINT64) * first);
}

static void D3D12TimingReport(int slot)
{
    if (!g_bridge.timing_readback || !g_bridge.timing_frequency) return;
    const SIZE_T begin = sizeof(UINT64) * (SIZE_T)(slot * 4);
    D3D12_RANGE range = { begin, begin + sizeof(UINT64) * 4 };
    void *mapped = nullptr;
    if (FAILED(g_bridge.timing_readback->Map(0, &range, &mapped)) || !mapped) return;
    const UINT64 *t = reinterpret_cast<const UINT64 *>(
        reinterpret_cast<const BYTE *>(mapped) + begin);
    const double scale = 1000.0 / (double)g_bridge.timing_frequency;
    const double upload_ms = (double)(t[1] - t[0]) * scale;
    const double neural_ms = (double)(t[2] - t[1]) * scale;
    const double download_ms = (double)(t[3] - t[2]) * scale;
    D3D12_RANGE written = { 0, 0 };
    g_bridge.timing_readback->Unmap(0, &written);
    static unsigned reports = 0;
    if (reports++ < 32)
        Log("[profile] GPU 1 upload=%.3fms neural=%.3fms download=%.3fms total=%.3fms",
            upload_ms, neural_ms, download_ms, upload_ms + neural_ms + download_ms);
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

static bool NeuralCopyBegin()
{
    return g_mgpu.neural_copy_alloc && g_mgpu.neural_copy_list &&
           SUCCEEDED(g_mgpu.neural_copy_alloc->Reset()) &&
           SUCCEEDED(g_mgpu.neural_copy_list->Reset(g_mgpu.neural_copy_alloc, nullptr));
}

static UINT64 NeuralCopyEnd()
{
    if (FAILED(g_mgpu.neural_copy_list->Close())) return 0;
    ID3D12CommandList *lists[] = { g_mgpu.neural_copy_list };
    g_mgpu.neural_copy_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++g_bridge.gpu_value;
    if (FAILED(g_mgpu.neural_copy_queue->Signal(g_bridge.fence_gpu, v))) return 0;
    return v;
}

static bool NeuralDownloadBegin()
{
    return g_mgpu.neural_download_alloc && g_mgpu.neural_download_list &&
           SUCCEEDED(g_mgpu.neural_download_alloc->Reset()) &&
           SUCCEEDED(g_mgpu.neural_download_list->Reset(g_mgpu.neural_download_alloc, nullptr));
}

static UINT64 NeuralDownloadEnd()
{
    if (FAILED(g_mgpu.neural_download_list->Close())) return 0;
    ID3D12CommandList *lists[] = { g_mgpu.neural_download_list };
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
    for (int i = 0; i < Bridge::kFrames; ++i) g_bridge.alloc_fence[i] = 0;
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
    for (int direction = 0; direction < CROSS_COUNT; ++direction)
    {
        if (g_mgpu.game_buffer[direction])
        { g_mgpu.game_buffer[direction]->Release(); g_mgpu.game_buffer[direction] = nullptr; }
        if (g_mgpu.neural_buffer[direction])
        { g_mgpu.neural_buffer[direction]->Release(); g_mgpu.neural_buffer[direction] = nullptr; }
        if (g_mgpu.game_heap[direction])
        { g_mgpu.game_heap[direction]->Release(); g_mgpu.game_heap[direction] = nullptr; }
        if (g_mgpu.neural_heap[direction])
        { g_mgpu.neural_heap[direction]->Release(); g_mgpu.neural_heap[direction] = nullptr; }
        if (g_mgpu.host_memory[direction])
        { VirtualFree(g_mgpu.host_memory[direction], 0, MEM_RELEASE); g_mgpu.host_memory[direction] = nullptr; }
    }
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
    for (int direction = 0; direction < CROSS_COUNT; ++direction)
    {
        const char *name = direction == CROSS_UPLOAD ? "upload" : "download";
        g_mgpu.host_memory[direction] = VirtualAlloc(nullptr, g_mgpu.host_bytes,
                                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_mgpu.host_memory[direction])
        {
            Log("[mgpu] VirtualAlloc(%s, %llu) failed", name,
                (unsigned long long)g_mgpu.host_bytes);
            neural3->Release();
            return false;
        }

        // Commit and fault every page before either GPU sees the allocation.
        // VirtualAlloc reserves committed address space but leaves first-touch
        // work to the page fault path, where it would otherwise hit a frame.
        memset(g_mgpu.host_memory[direction], 0, g_mgpu.host_bytes);

        hr = g_mgpu.game_dev->OpenExistingHeapFromAddress(
            g_mgpu.host_memory[direction], __uuidof(ID3D12Heap),
            reinterpret_cast<void **>(&g_mgpu.game_heap[direction]));
        if (SUCCEEDED(hr))
            hr = neural3->OpenExistingHeapFromAddress(
                g_mgpu.host_memory[direction], __uuidof(ID3D12Heap),
                reinterpret_cast<void **>(&g_mgpu.neural_heap[direction]));
        if (FAILED(hr) || !g_mgpu.game_heap[direction] || !g_mgpu.neural_heap[direction])
        {
            Log("[mgpu] OpenExistingHeapFromAddress(%s) failed 0x%08X", name, hr);
            neural3->Release();
            return false;
        }

        hr = g_mgpu.game_dev->CreatePlacedResource(
            g_mgpu.game_heap[direction], 0, &bd, D3D12_RESOURCE_STATE_COMMON,
            nullptr, __uuidof(ID3D12Resource),
            reinterpret_cast<void **>(&g_mgpu.game_buffer[direction]));
        if (SUCCEEDED(hr))
            hr = g_bridge.dev12->CreatePlacedResource(
                g_mgpu.neural_heap[direction], 0, &bd, D3D12_RESOURCE_STATE_COMMON,
                nullptr, __uuidof(ID3D12Resource),
                reinterpret_cast<void **>(&g_mgpu.neural_buffer[direction]));
        if (FAILED(hr) || !g_mgpu.game_buffer[direction] || !g_mgpu.neural_buffer[direction])
        {
            Log("[mgpu] %s cross-buffer placed resource failed 0x%08X", name, hr);
            neural3->Release();
            return false;
        }
    }
    neural3->Release();
    Log("[mgpu] directional cross-adapter buffers ready: %llu bytes each",
        (unsigned long long)g_mgpu.host_bytes);
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

static bool GameBeginCommands()
{
    return SUCCEEDED(g_mgpu.game_alloc->Reset()) &&
           SUCCEEDED(g_mgpu.game_list->Reset(g_mgpu.game_alloc, nullptr));
}

static bool GameEndAndWait(const char *which, DWORD timeout)
{
    if (FAILED(g_mgpu.game_list->Close())) return false;
    ID3D12CommandList *lists[] = { g_mgpu.game_list };
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
    // OptiScaler owns the DLSS-NR insertion point. Prefer its exported D3D12
    // surface even when Vulkan already loaded the driver dispatcher; calling
    // _nvngx.dll directly bypasses OptiScaler's neural pass.
    if (HMODULE m = GetModuleHandleW(L"OptiScaler.dll"))
        if (NgxHasFullD3D12Set(m))
        { Log("[bridge] NGX D3D12 entry points from %ls (DLSS-NR host)", NgxModuleName(m)); return m; }

    // Fall back to the driver dispatcher if OptiScaler is unavailable.
    if (HMODULE m = GetModuleHandleW(L"_nvngx.dll"))
        if (NgxHasFullD3D12Set(m))
        { Log("[bridge] NGX D3D12 entry points from %ls", NgxModuleName(m)); return m; }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return nullptr;
    HMODULE mods[1024]; DWORD needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
    const DWORD n = needed / sizeof(HMODULE);

    for (DWORD i = 0; i < n; ++i)
        if (NgxHasFullD3D12Set(mods[i]))
        { Log("[bridge] NGX D3D12 entry points from %ls", NgxModuleName(mods[i])); return mods[i]; }

    // No module carries the complete surface: name the near misses so the log
    // pinpoints the missing export on whatever driver produced it.
    for (DWORD i = 0; i < n; ++i)
    {
        if (GetProcAddress(mods[i], "NVSDK_NGX_D3D12_CreateFeature") == nullptr &&
            GetProcAddress(mods[i], "NVSDK_NGX_D3D12_Init_Ext") == nullptr) continue;
        char missing[256] = "";
        for (const char *name : kNgxD3D12Names)
            if (GetProcAddress(mods[i], name) == nullptr)
            {
                if (missing[0]) strcat_s(missing, ", ");
                strcat_s(missing, name);
            }
        Log("[bridge] %ls has an NGX D3D12 surface but lacks: %s",
            NgxModuleName(mods[i]), missing[0] ? missing : "(an init entry point)");
    }
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
    else if (selector.kind == dlss_bridge::SelectorKind::Uuid ||
             selector.kind == dlss_bridge::SelectorKind::Pci)
    {
        Log("[adapter] selector %s must be resolved to a DXGI LUID by the platform controller",
            selector.text);
        goto fail;
    }
    else if (g_cfg.execution_mode == dlss_bridge::ExecutionMode::SecondaryGpu)
    {
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

        // The SDK version constant is undocumented and has moved between NGX
        // releases, so it is negotiated. A fault (not a refusal) means NGX died
        // inside itself; stop at the first one rather than call back in.
        DWORD code = 0; bool inited = false, faulted = false;
        for (int ver = 0x13; ver <= 0x16 && !inited && !faulted; ++ver)
        {
            if (init_ext)
            {
                NVSDK_NGX_Result r = SafeInitExt(init_ext, 0x1000000ULL, data_path, g_bridge.dev12, ver, common, &code);
                if (code) { Log("[bridge] D3D12 Init_Ext(0x%02X) raised 0x%08X", ver, code); faulted = true; }
                else { Log("[bridge] D3D12 Init_Ext(0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused"); if (r == NGX_SUCCESS) inited = true; }
            }
            if (!inited && !faulted && init_proj)
            {
                NVSDK_NGX_Result r = SafeInitProjectID(init_proj,
                    "a0f57b54-1daf-4934-90ae-c4035c19df04", data_path, g_bridge.dev12, ver, common, &code);
                if (code) { Log("[bridge] D3D12 Init_ProjectID(0x%02X) raised 0x%08X", ver, code); faulted = true; }
                else { Log("[bridge] D3D12 Init_ProjectID(0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused"); if (r == NGX_SUCCESS) inited = true; }
            }
        }
        if (faulted) { Log("[bridge] NGX faulted opening its D3D12 session; stopping."); goto fail; }
        if (!inited) { Log("[bridge] no NGX D3D12 init entry point was accepted by this driver"); goto fail; }
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
            g_mgpu.game_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_mgpu.game_alloc));
            if (g_mgpu.game_alloc)
                g_mgpu.game_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_mgpu.game_alloc,
                    nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_mgpu.game_list));
            if (g_mgpu.game_list) g_mgpu.game_list->Close();

            CreateCopyQueue(g_bridge.dev12, &g_mgpu.neural_copy_queue, "neural-adapter");
            g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_mgpu.neural_copy_alloc));
            if (g_mgpu.neural_copy_alloc)
                g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_mgpu.neural_copy_alloc,
                    nullptr, __uuidof(ID3D12GraphicsCommandList),
                    reinterpret_cast<void **>(&g_mgpu.neural_copy_list));
            if (g_mgpu.neural_copy_list) g_mgpu.neural_copy_list->Close();
            g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_mgpu.neural_download_alloc));
            if (g_mgpu.neural_download_alloc)
                g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_mgpu.neural_download_alloc,
                    nullptr, __uuidof(ID3D12GraphicsCommandList),
                    reinterpret_cast<void **>(&g_mgpu.neural_download_list));
            if (g_mgpu.neural_download_list) g_mgpu.neural_download_list->Close();

            hr = g_mgpu.game_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                               reinterpret_cast<void **>(&g_mgpu.game_fence));
            g_mgpu.game_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_mgpu.game_queue || !g_mgpu.game_alloc || !g_mgpu.game_list ||
                !g_mgpu.neural_copy_queue || !g_mgpu.neural_copy_alloc || !g_mgpu.neural_copy_list ||
                !g_mgpu.neural_download_alloc || !g_mgpu.neural_download_list ||
                FAILED(hr) || !g_mgpu.game_fence || !g_mgpu.game_event)
            { Log("[mgpu] copy-queue infrastructure creation failed"); goto fail; }
        }
        for (int i = 0; i < Bridge::kFrames; ++i)
            g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&g_bridge.alloc[i]));
        if (g_bridge.alloc[0])
            g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bridge.alloc[0], nullptr,
                __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g_bridge.list));
        if (g_bridge.list) g_bridge.list->Close();
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
            (void *)g_bridge.fence_out, Bridge::kFrames);
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
static unsigned int CopyUIntParam(const NVSDK_NGX_Parameter *src, const char *key,
                                  unsigned int fallback, bool *copied = nullptr)
{
    unsigned int v = 0;
    bool from_game = src != nullptr && src->Get(key, &v) == NGX_SUCCESS;
    if (!from_game) v = fallback;
    if (copied) *copied = from_game;
    g_bridge.params->Set(key, v);
    return v;
}

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

static bool D3D12CreateDlssFeature(const NVSDK_NGX_Parameter *gp)
{
    if (g_bridge.feature && g_bridge.release_feature)
    { g_bridge.release_feature(g_bridge.feature); g_bridge.feature = nullptr; }

    g_bridge.params->Set("Width",     g_bridge.render_w);
    g_bridge.params->Set("Height",    g_bridge.render_h);
    g_bridge.params->Set("OutWidth",  g_bridge.ngx_out_w);
    g_bridge.params->Set("OutHeight", g_bridge.ngx_out_h);
    bool q_game = false, f_game = false, s_game = false;
    unsigned int quality = CopyUIntParam(gp, "PerfQualityValue", 2u, &q_game);

    // The create flags describe the game's own buffers (HDR, inverted depth,
    // low-res MV). Copy the game's value; force one only when configured to a
    // value other than -1. See the DX11 bridge for why -1 (copy) is the only
    // value guaranteed to be correct across titles.
    unsigned int flags;
    if (g_cfg.flags < 0) flags = CopyUIntParam(gp, "DLSS.Feature.Create.Flags", 0u, &f_game);
    else               { flags = (unsigned int)g_cfg.flags; g_bridge.params->Set("DLSS.Feature.Create.Flags", flags); }

    unsigned int subrects;
    if (gp) subrects = CopyUIntParam(gp, "DLSS.Enable.Output.Subrects", (unsigned)g_cfg.subrects, &s_game);
    else  { subrects = (unsigned)g_cfg.subrects; g_bridge.params->Set("DLSS.Enable.Output.Subrects", subrects); }
    g_bridge.params->Set("CreationNodeMask", 1u);
    g_bridge.params->Set("VisibilityNodeMask", 1u);
    g_bridge.params->Set("RTXValue", 0);

    Log("[bridge] create params: %ux%u -> %ux%u quality=%u(%s) flags=0x%X(%s) subrects=%u(%s)",
        g_bridge.render_w, g_bridge.render_h, g_bridge.ngx_out_w, g_bridge.ngx_out_h,
        quality, q_game ? "game" : "fallback", flags, f_game ? "game" : "fallback",
        subrects, s_game ? "game" : "cfg");

    // NGX refuses some parameter shapes outright (UnableToInitializeFeature)
    // without naming the offender. Retry the known repairs before giving up:
    // 1:1 geometry is only valid as DLAA, and output subrects are not accepted
    // everywhere.
    bool tried_dlaa = false, tried_nosub = false;
    for (;;)
    {
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
        if (code)      { Warn("creating the DLSS feature crashed (0x%08X). A hand-copied nvngx_*.dll "
                              "version mismatch is the usual cause.", code); BridgeDisable("the DLSS feature crashed"); return false; }
        if (rf == NGX_SUCCESS && g_bridge.feature != nullptr) break;
        Log("[bridge] CreateFeature failed 0x%08X, %s", rf, NgxResultName(rf));

        if (!tried_dlaa && quality != 5 &&
            g_bridge.render_w == g_bridge.ngx_out_w && g_bridge.render_h == g_bridge.ngx_out_h)
        {
            tried_dlaa = true; quality = 5;
            g_bridge.params->Set("PerfQualityValue", 5u);
            Log("[bridge] retrying: PerfQualityValue=5 (DLAA) for 1:1 geometry");
            continue;
        }
        if (!tried_nosub && subrects != 0)
        {
            tried_nosub = true; subrects = 0;
            g_bridge.params->Set("DLSS.Enable.Output.Subrects", 0u);
            Log("[bridge] retrying: output subrects disabled");
            continue;
        }
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
// The core runs on the game's thread (async path) or on the sync-handoff
// worker thread (vk_interop.inc). On the worker it must not tear the bridge
// down itself -- teardown joins that worker -- so it only reports:
//   0 = ok, 1 = NGX refused, 2 = NGX raised an exception, 4 = no command list.
// It signals fence_out on EVERY path: with sync=1 the game's command buffer is
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

static void RecordMultiGpuUpload(ID3D12GraphicsCommandList *list)
{
    const int inputs[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    Barrier(list, g_mgpu.neural_buffer[CROSS_UPLOAD], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (int slot : inputs)
    {
        Barrier(list, g_mgpu.neural_tex[slot], D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
        CopyCrossToTexture(list, g_mgpu.neural_buffer[CROSS_UPLOAD],
                           g_mgpu.neural_tex[slot], g_mgpu.plane[slot]);
        Barrier(list, g_mgpu.neural_tex[slot], D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON);
    }
    Barrier(list, g_mgpu.neural_buffer[CROSS_UPLOAD], D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
}

static void RecordMultiGpuDownload(ID3D12GraphicsCommandList *list)
{
    Barrier(list, g_mgpu.neural_buffer[CROSS_DOWNLOAD], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, g_mgpu.neural_tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    CopyTextureToCross(list, g_mgpu.neural_tex[SLOT_OUTPUT],
                       g_mgpu.neural_buffer[CROSS_DOWNLOAD], g_mgpu.plane[SLOT_OUTPUT]);
    Barrier(list, g_mgpu.neural_tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    Barrier(list, g_mgpu.neural_buffer[CROSS_DOWNLOAD], D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COMMON);
}

static bool MultiGpuUploadInputs(UINT64 timeline, bool inputs_cpu_ready, UINT64 *upload_value)
{
    *upload_value = 0;
    const int inputs[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    if (!g_mgpu.direct_vulkan_upload)
    {
        if ((!inputs_cpu_ready && FAILED(g_mgpu.game_queue->Wait(g_bridge.fence_in, timeline))) ||
            !GameBeginCommands()) return false;
        Barrier(g_mgpu.game_list, g_mgpu.game_buffer[CROSS_UPLOAD], D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_DEST);
        for (int slot : inputs)
        {
            Barrier(g_mgpu.game_list, g_bridge.tex[slot].tex12, D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            CopyTextureToCross(g_mgpu.game_list, g_bridge.tex[slot].tex12,
                               g_mgpu.game_buffer[CROSS_UPLOAD], g_mgpu.plane[slot]);
            Barrier(g_mgpu.game_list, g_bridge.tex[slot].tex12, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
        }
        Barrier(g_mgpu.game_list, g_mgpu.game_buffer[CROSS_UPLOAD], D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_COMMON);
        if (!GameEndAndWait("game adapter input copy", 60)) return false;
    }
    if (!NeuralCopyBegin()) return false;
    RecordMultiGpuUpload(g_mgpu.neural_copy_list);
    *upload_value = NeuralCopyEnd();
    return *upload_value != 0;
}

static bool MultiGpuDownloadOutput(UINT64 timeline, UINT64 evaluate_value)
{
    if (!NeuralDownloadBegin()) return false;
    RecordMultiGpuDownload(g_mgpu.neural_download_list);

    // Chain entirely on GPU 1: copy upload -> DIRECT evaluate -> copy download.
    // This preserves the stable copy-engine path without CPU scheduling gaps.
    if (FAILED(g_mgpu.neural_copy_queue->Wait(g_bridge.fence_gpu, evaluate_value))) return false;
    const UINT64 download_value = NeuralDownloadEnd();
    if (download_value == 0) return false;
    if (!D3D12WaitNeural(download_value, "GPU 1 chained pipeline", NeuralPipelineDeadlineMs()))
    {
        g_mgpu_pending_neural = download_value;
        g_mgpu_dropped_outputs = 1;
        g_mgpu_output_timeout = true;
        return false;
    }
    if (g_mgpu.direct_vulkan_download)
        return SUCCEEDED(g_bridge.fence_out->Signal(timeline));

    if (!GameBeginCommands()) return false;

    Barrier(g_mgpu.game_list, g_mgpu.game_buffer[CROSS_DOWNLOAD], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_mgpu.game_list, g_bridge.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
    CopyCrossToTexture(g_mgpu.game_list, g_mgpu.game_buffer[CROSS_DOWNLOAD],
                       g_bridge.tex[SLOT_OUTPUT].tex12, g_mgpu.plane[SLOT_OUTPUT]);
    Barrier(g_mgpu.game_list, g_bridge.tex[SLOT_OUTPUT].tex12, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_mgpu.game_list, g_mgpu.game_buffer[CROSS_DOWNLOAD], D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    if (!GameEndAndWait("game adapter output copy", 8)) return false;
    return SUCCEEDED(g_bridge.fence_out->Signal(timeline));
}

static void D3D12RunEvaluateCore(UINT64 v, LONG *result, bool inputs_cpu_ready)
{
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
                g_bridge.fence_out->Signal(v);
                *result = 32;
                return;
            }
            Log("[mgpu] GPU 1 recovered after %llu repeated neural frame(s)",
                (unsigned long long)g_mgpu_dropped_outputs);
            g_mgpu_pending_neural = 0;
            g_mgpu_dropped_outputs = 0;
        }

        unified = UnifiedNeuralQueueActive();
        if (unified)
        {
            if (!D3D12BeginCommands())
            {
                g_bridge.fence_out->Signal(v);
                *result = 4;
                return;
            }
            timing_slot = g_bridge.frame_slot;
            D3D12TimingStamp(timing_slot, 0);
            RecordMultiGpuUpload(g_bridge.list);
            D3D12TimingStamp(timing_slot, 1);
        }
        else if (!MultiGpuUploadInputs(v, inputs_cpu_ready, &upload_value))
        {
            g_bridge.fence_out->Signal(v);
            *result = g_mgpu_pending_game ? 32 : 4;
            return;
        }
        timing_uploaded = GetTickCount64();
    }
    else
    {
        if (!inputs_cpu_ready) g_bridge.queue->Wait(g_bridge.fence_in, v);
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

    Barrier(g_bridge.list, NeuralTex(SLOT_COLOR),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, NeuralTex(SLOT_DEPTH),  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, NeuralTex(SLOT_MV),     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, NeuralTex(SLOT_OUTPUT), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    DWORD code = 0;
    NVSDK_NGX_Result re = NGX_SUCCESS;
    static const char kMark[] = "ngx evaluate";
    g_bridge.list->BeginEvent(1, kMark, sizeof(kMark));
    if (g_cfg.mode != 1)
        re = SafeEvaluate(g_bridge.eval_feature, g_bridge.list, g_bridge.feature, g_bridge.params, &code);
    g_bridge.list->EndEvent();

    Barrier(g_bridge.list, NeuralTex(SLOT_COLOR),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, NeuralTex(SLOT_DEPTH),  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, NeuralTex(SLOT_MV),     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, NeuralTex(SLOT_OUTPUT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (unified) D3D12TimingStamp(timing_slot, 2);

    if (g_mgpu.active && unified)
    {
        RecordMultiGpuDownload(g_bridge.list);
        D3D12TimingStamp(timing_slot, 3);
        D3D12TimingResolve(timing_slot);
        const UINT64 pipeline_value = D3D12EndCommands();
        timing_evaluated = GetTickCount64();
        if (!D3D12WaitNeural(pipeline_value, "GPU 1 unified pipeline", NeuralPipelineDeadlineMs()))
        {
            g_mgpu_pending_neural = pipeline_value;
            g_mgpu_dropped_outputs = 1;
            g_mgpu_output_timeout = true;
            g_bridge.fence_out->Signal(v);
            *result = 32;
            return;
        }
        D3D12TimingReport(timing_slot);
        if (FAILED(g_bridge.fence_out->Signal(v)))
        {
            *result = 4;
            return;
        }
    }
    else
    {
        const UINT64 evaluate_value = D3D12EndCommands();
        timing_evaluated = GetTickCount64();
        if (g_mgpu.active)
        {
            if (!MultiGpuDownloadOutput(v, evaluate_value))
            {
                g_bridge.fence_out->Signal(v);
                *result = (g_mgpu_output_timeout || g_mgpu_pending_game) ? 32 : 4;
                return;
            }
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

// Async-path wrapper: runs the core on the calling (game) thread and applies
// the failure policy there, where tearing down is safe.
static bool D3D12RunEvaluate(UINT64 v)
{
    LONG result = 0;
    D3D12RunEvaluateCore(v, &result, false);
    switch (result)
    {
    case 0:  g_bridge.consecutive_fails = 0; return true;
    case 2:  Log("[bridge] disabling to protect the game");
             BridgeDisable("NGX raised an exception in the D3D12 evaluate"); return false;
    case 4:  BridgeFail("d3d12 command list"); return false;
    case 32: return false; // strict mode repeats the last completed neural output
    default: BridgeFail("evaluate"); return false;
    }
}
