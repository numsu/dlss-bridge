// The D3D11 half of the bridge: capture the game's NGX D3D11 Super Resolution
// contract, move pixels across the D3D11/D3D12 boundary through shared NT
// handles, run the private D3D12 NGX evaluate (the call OptiScaler detours),
// and copy the neural result back before downstream rendering.
//
// Unlike Vulkan there is no submit-split handoff: the game hands the bridge an
// ID3D11DeviceContext whose immediate-context queue runs inline, so copy /
// evaluate / copy-back happen synchronously on the game thread inside the
// intercepted EvaluateFeature. Temporal order falls out of the hook's own
// serialisation (calls are serialised on g_hook_cs while forwarding); split-
// screen viewports interleave freely because each owns a private D3D12 feature
// and shared textures keyed by game NGX handle identity.
//
// Shared textures are created once on the D3D12 side (see D3D12MakeShared-
// TextureVp) and opened in the game's D3D11 device via OpenSharedResource.
// Both APIs alias the same memory; no host memcpy is on the hot path.

#pragma once

#ifdef __has_include
#if __has_include(<d3d11.h>)
#include <d3d11.h>
#define DLSS_BRIDGE_HAS_D3D11 1
#endif
#endif
#ifndef DLSS_BRIDGE_HAS_D3D11
// Forward-only declarations so the file still parses where the MinGW D3D11
// headers are unavailable (e.g. host-side syntax checks). The D3D11 build
// always has the real headers.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11Texture2D;
typedef int DXGI_FORMAT_ALIAS_CHECK;
#endif

// -- NGX D3D11 entry points (hooked in the game) ---------------------------
// Signatures match NVIDIA's nvsdk_ngx_d3d11.h: the context drives inline
// copies; the parameter block carries the same Width/Height/OutWidth/
// OutHeight/PerfQualityValue/flags contract as Vulkan.
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_Create)(
    ID3D11DeviceContext *, int, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_Evaluate)(
    ID3D11DeviceContext *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
    PFN_NVSDK_NGX_ProgressCallback);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_Init)(
    unsigned long long, const wchar_t *, ID3D11Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D11_Init_ProjectID)(
    const char *, int, const char *, const wchar_t *, ID3D11Device *, int, const void *);

struct D3d11GameRes
{
    ID3D11Resource *res;
    UINT w, h;
    DXGI_FORMAT fmt;
};

static bool ReadD3d11Res(const NVSDK_NGX_Parameter *p, const char *key, D3d11GameRes *out)
{
#ifdef DLSS_BRIDGE_HAS_D3D11
    if (!p || !out) return false;
    ID3D11Resource *res = nullptr;
    if (p->Get(key, &res) != NGX_SUCCESS || !res) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || !tex)
        return false;
    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    tex->Release();
    if (!desc.Width || !desc.Height) return false;
    out->res = res;
    out->w = desc.Width;
    out->h = desc.Height;
    out->fmt = desc.Format;
    return true;
#else
    (void)p; (void)key; (void)out;
    return false;
#endif
}

// D3D11 game formats are already DXGI, so the alias is identity for the known
// neural-compatible set. Unknown formats fall back to RGBA16F conversion via
// the D3D11 device's format-support query; frames that cannot be converted
// are forwarded untouched rather than guessed.
static bool MapD3d11Color(DXGI_FORMAT game, DXGI_FORMAT *neural, bool *need_convert)
{
    static const DXGI_FORMAT kDirect[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UNORM,
        DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R32_FLOAT,
    };
    for (DXGI_FORMAT f : kDirect)
        if (f == game) { *neural = game; *need_convert = false; return true; }
    *neural = DXGI_FORMAT_R16G16B16A16_FLOAT;
    *need_convert = true;
    return true;
}

// Per-viewport D3D11 aliases of the D3D12 shared textures. Opened lazily on
// first use via ID3D11Device::OpenSharedResource; released with the viewport.
static ID3D11Resource *g_d3d11_alias[MAX_VIEWPORTS][SLOT_COUNT] = {};
static ID3D11Device *g_d3d11_device = nullptr;

static void D3d11ReleaseViewportAliases(int vp)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return;
    for (int s = 0; s < SLOT_COUNT; ++s)
        if (g_d3d11_alias[vp][s]) { g_d3d11_alias[vp][s]->Release(); g_d3d11_alias[vp][s] = nullptr; }
}

#ifdef DLSS_BRIDGE_HAS_D3D11
static bool D3d11EnsureDevice(ID3D11DeviceContext *ctx)
{
    if (g_d3d11_device && g_bridge.d3d11_ctx == ctx) return true;
    if (!ctx) return false;
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (g_d3d11_device && g_d3d11_device != dev)
    {
        Log("[d3d11] game device changed; releasing D3D11 shared aliases");
        for (int v = 0; v < MAX_VIEWPORTS; ++v) D3d11ReleaseViewportAliases(v);
        g_d3d11_device->Release();
        g_d3d11_device = nullptr;
    }
    if (!g_d3d11_device)
    {
        g_d3d11_device = dev; // adopted (no Release: owned by the game)
        g_d3d11_device->AddRef();
        dev->Release();
        D3D11HostNoteInit();
    }
    else
        dev->Release();
    g_bridge.d3d11_device = g_d3d11_device;
    g_bridge.d3d11_ctx = ctx;
    // Match the D3D12 session to the game's adapter via DXGI LUID so shared
    // NT handles open on the same physical GPU.
    IDXGIDevice *dxgi_dev = nullptr;
    if (SUCCEEDED(g_d3d11_device->QueryInterface(__uuidof(IDXGIDevice),
                                                 reinterpret_cast<void **>(&dxgi_dev))) && dxgi_dev)
    {
        IDXGIAdapter *ad = nullptr;
        if (SUCCEEDED(dxgi_dev->GetAdapter(&ad)) && ad)
        {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(ad->GetDesc(&desc)))
            {
                memcpy(g_bridge.game_luid, &desc.AdapterLuid, sizeof(LUID) < VK_LUID_SIZE ? sizeof(LUID) : VK_LUID_SIZE);
                g_bridge.luid_valid = true;
            }
            ad->Release();
        }
        dxgi_dev->Release();
    }
    return true;
}

static bool D3d11EnsureAlias(int vp, int slot)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS || slot < 0 || slot >= SLOT_COUNT) return false;
    if (g_d3d11_alias[vp][slot] || !g_d3d11_device) return g_d3d11_alias[vp][slot] != nullptr;
    HANDLE h = g_bridge.viewports[vp].tex[slot].nt_handle;
    if (!h) return false;
    ID3D11Resource *alias = nullptr;
    HRESULT hr = g_d3d11_device->OpenSharedResource(h, __uuidof(ID3D11Resource),
                                                    reinterpret_cast<void **>(&alias));
    if (FAILED(hr) || !alias)
    {
        Log("[d3d11] vp%d %s OpenSharedResource failed 0x%08X", vp, kSlotName[slot], hr);
        return false;
    }
    g_d3d11_alias[vp][slot] = alias;
    return true;
}
#endif

static bool D3d11ViewportContractChanged(int vp, const D3d11GameRes &col,
                                         const D3d11GameRes &out, const D3d11GameRes &dep,
                                         const D3d11GameRes &mv)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return true;
    const BridgeViewport &v = g_bridge.viewports[vp];
    return !v.frame_ready
        || v.slot_w[SLOT_COLOR] != col.w || v.slot_h[SLOT_COLOR] != col.h
        || v.slot_w[SLOT_OUTPUT] != out.w || v.slot_h[SLOT_OUTPUT] != out.h
        || v.slot_w[SLOT_DEPTH] != dep.w || v.slot_h[SLOT_DEPTH] != dep.h
        || v.slot_w[SLOT_MV] != mv.w || v.slot_h[SLOT_MV] != mv.h
        || v.game_dxgi[SLOT_COLOR] != col.fmt || v.game_dxgi[SLOT_OUTPUT] != out.fmt
        || v.game_dxgi[SLOT_DEPTH] != dep.fmt || v.game_dxgi[SLOT_MV] != mv.fmt;
}

static bool D3d11RebuildViewport(int vp, const FeatureCreateContract &contract,
                                 const D3d11GameRes &col, const D3d11GameRes &out,
                                 const D3d11GameRes &dep, const D3d11GameRes &mv)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return false;
    BridgeViewport &v = g_bridge.viewports[vp];
    Log("[d3d11] vp%d (re)building resources for %ux%u -> %ux%u", vp, col.w, col.h, out.w, out.h);
    D3D12DrainGpu();
    v.frame_ready = false;
    if (v.feature && g_bridge.release_feature)
    { g_bridge.release_feature(v.feature); v.feature = nullptr; }
    D3d11ReleaseViewportAliases(vp);
    for (int i = 0; i < SLOT_COUNT; ++i) VkReleaseSharedTexVp(vp, i);
    if (InterlockedExchange(&v.neural_latched, 0) != 0)
        Log("[policy] vp%d resource contract changed; rewarming before neural-only relatches", vp);

    struct Plan { int slot; UINT w, h; DXGI_FORMAT dxgi; bool uav; } plan[SLOT_COUNT];
    DXGI_FORMAT cdx; bool cconv; MapD3d11Color(col.fmt, &cdx, &cconv);
    DXGI_FORMAT odx; bool oconv; MapD3d11Color(out.fmt, &odx, &oconv);
    if (g_cfg.output_transport == dlss_bridge::OutputTransport::R11G11B10Float)
    { odx = DXGI_FORMAT_R11G11B10_FLOAT; oconv = (out.fmt != odx); }
    DXGI_FORMAT mdx; bool mconv; MapD3d11Color(mv.fmt, &mdx, &mconv);
    plan[SLOT_COLOR] = { SLOT_COLOR, col.w, col.h, cdx, false };
    plan[SLOT_OUTPUT] = { SLOT_OUTPUT, out.w, out.h, odx, true };
    plan[SLOT_DEPTH] = { SLOT_DEPTH, dep.w, dep.h, DXGI_FORMAT_R32_FLOAT, true };
    plan[SLOT_MV] = { SLOT_MV, mv.w, mv.h, mdx, false };
    if (cconv || oconv || mconv)
        Log("[d3d11] vp%d format conversion required (color %d, output %d, mv %d); "
            "conversion runs on the D3D11 immediate context", vp, (int)col.fmt, (int)out.fmt, (int)mv.fmt);

    for (const Plan &pl : plan)
    {
        if (!D3D12MakeSharedTextureVp(vp, pl.slot, pl.w, pl.h, pl.dxgi, pl.uav))
        { ViewportFail(vp, "shared texture"); return false; }
        if (g_mgpu.active && !D3D12MakeNeuralTextureVp(vp, pl.slot, pl.w, pl.h, pl.dxgi, pl.uav))
        { ViewportFail(vp, "neural texture"); return false; }
#ifdef DLSS_BRIDGE_HAS_D3D11
        if (!D3d11EnsureAlias(vp, pl.slot)) { ViewportFail(vp, "d3d11 shared alias"); return false; }
#endif
        v.slot_w[pl.slot] = pl.w; v.slot_h[pl.slot] = pl.h;
    }
    if (!D3D12BuildCrossBuffer()) { ViewportFail(vp, "cross-adapter buffer"); return false; }
    if (!v.params)
    {
        DWORD acode = 0;
        if (SafeAllocParams(g_bridge.alloc_params, &v.params, &acode) != NGX_SUCCESS || !v.params)
        { ViewportFail(vp, "allocate parameters"); return false; }
    }
    for (int i = 0; i < SLOT_COUNT; ++i)
        v.params->Set(kSlotKey[i], NeuralTexVp(vp, i));
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

// D3D11 frame: inline copy / neural evaluate / copy-back on the game thread.
// The D3D12 evaluate reuses the same per-viewport private feature as Vulkan;
// only the transport edges differ (immediate-context copies, no Vulkan events).
static void BridgeD3D11Frame(ID3D11DeviceContext *ctx, const NVSDK_NGX_Handle *feature,
                             const NVSDK_NGX_Parameter *p)
{
    if (g_bridge.disabled || !ctx || !p) return;
#ifndef DLSS_BRIDGE_HAS_D3D11
    (void)feature;
    return;
#else
    D3d11GameRes col{}, out{}, dep{}, mv{};
    if (!ReadD3d11Res(p, "Color", &col) || !ReadD3d11Res(p, "Output", &out) ||
        !ReadD3d11Res(p, "Depth", &dep) || !ReadD3d11Res(p, "MotionVectors", &mv))
        return; // not a mirrorable super-sampling evaluate; stand aside
    if (BridgeRayReconstructionInput(p)) return; // denoiser guidance present; leave untouched

    const FeatureCreateContract *contract = FindFeatureContract(feature);
    if (!contract)
    {
        BridgeDisable("the D3D11 DLSS feature has no complete captured creation contract");
        return;
    }
    const int vp = ResolveViewport(feature);
    if (vp < 0) return; // viewport budget exhausted; forward untouched
    if (!D3d11EnsureDevice(ctx)) { ViewportFail(vp, "d3d11 device"); return; }
    if (!g_bridge.session_ready && !D3D12InitSession(g_bridge.game_luid, g_bridge.luid_valid))
        return;

    FrameLock();
    if (D3d11ViewportContractChanged(vp, col, out, dep, mv) &&
        !D3d11RebuildViewport(vp, *contract, col, out, dep, mv))
    { FrameUnlock(); return; }
    BridgeViewport &v = g_bridge.viewports[vp];
    if (!v.frame_ready) { FrameUnlock(); return; }

    FrameScalars scalars;
    CaptureFrameScalarsVp(vp, p, &scalars);

    // Inputs: game -> shared (same-adapter fast path). Cross-adapter staging
    // reuses the directional host ring via the shared D3D12 session.
    for (int s = 0; s < SLOT_COUNT; ++s)
        if (!D3d11EnsureAlias(vp, s)) { FrameUnlock(); ViewportFail(vp, "d3d11 alias"); return; }
    const D3d11GameRes *srcs[SLOT_COUNT] = { &col, nullptr, &dep, &mv };
    const int copy_slots[] = { SLOT_COLOR, SLOT_DEPTH, SLOT_MV };
    for (int k = 0; k < 3; ++k)
    {
        const int s = copy_slots[k];
        ctx->CopyResource(g_d3d11_alias[vp][s], srcs[s]->res);
    }
    if (v.neural_latched == 0)
        ctx->CopyResource(g_d3d11_alias[vp][SLOT_OUTPUT], out.res);
    ctx->Flush();

    // Neural evaluate on the shared D3D12 queue (ordered with Vulkan frames
    // when both APIs are live in one process, e.g. mixed-API engines).
    ApplyFrameScalarsVp(vp, &scalars);
    g_bridge.fence_in->Signal(++g_bridge.timeline);
    LONG result = 0;
    D3D12RunEvaluateCoreVp(g_bridge.timeline, &result, 0, vp);
    if (result == 0 && g_bridge.fence_out->GetCompletedValue() < g_bridge.timeline)
    {
        g_bridge.fence_out->SetEventOnCompletion(g_bridge.timeline, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 1000) != WAIT_OBJECT_0)
            result = 16;
    }
    if (result == 0)
    {
        v.consecutive_fails = 0;
        ctx->CopyResource(out.res, g_d3d11_alias[vp][SLOT_OUTPUT]);
        if (InterlockedCompareExchange(&v.neural_latched, 1, 0) == 0)
        {
            InterlockedExchange(&g_neural_only_latched, 1);
            Log("[policy] vp%d (d3d11) neural-only latched; game-side DLSS suppressed", vp);
        }
        ++v.frames_done;
    }
    else if (result == 32)
    {
        ctx->CopyResource(out.res, g_d3d11_alias[vp][SLOT_OUTPUT]);
        Log("[policy] vp%d (d3d11) repeating last neural image", vp);
    }
    else
    {
        ViewportFail(vp, result == 2 ? "NGX exception in D3D12 evaluate" : "evaluate");
    }
    FrameUnlock();
#endif
}

static bool BridgeD3D11SkipGameEvaluate(int vp, const NVSDK_NGX_Parameter *p)
{
    if (vp < 0 || vp >= MAX_VIEWPORTS) return false;
    if (g_bridge.viewports[vp].neural_latched == 0) return false;
    D3d11GameRes col{}, out{}, dep{}, mv{};
    if (!ReadD3d11Res(p, "Color", &col) || !ReadD3d11Res(p, "Output", &out) ||
        !ReadD3d11Res(p, "Depth", &dep) || !ReadD3d11Res(p, "MotionVectors", &mv))
        return false;
    return !D3d11ViewportContractChanged(vp, col, out, dep, mv);
}
