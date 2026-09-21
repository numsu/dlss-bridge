// dlss5-vk-bridge -- a Vulkan port of NIGos' DLSS 5 DX11 Bridge.
//
// A DLSS 5 (Neural Rendering) add-on such as RenoDX's ReShade add-on only
// detours the *D3D12* NGX evaluate. Vulkan games never make that call, so the
// neural pass never runs for them. This bridge closes that gap: it intercepts
// the game's NGX Vulkan evaluate, mirrors the same DLSS contract onto a private
// D3D12 device, and runs a second NGX evaluate there -- the call the add-on
// detours -- copying the neural result back into the game's own output image.
//
// The injected hook host enables the interop extensions the game did not ask
// for, intercepts the Vulkan submission boundary, and hooks NGX Vulkan feature
// creation/evaluation. The launch session is external to the game directory.
//
// Original DLSS 5 DX11 Bridge (c) 2026 NIGos, MIT. This port (c) 2026 Alan Z.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR

#include <windows.h>
#if defined(DLSS5VK_HOOK_HOST)
#include <tlhelp32.h>
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <malloc.h>     // _resetstkoflw
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ngx.h"        // pulls <vulkan/vulkan.h> and <vulkan/vulkan_win32.h>

// D3D12 / NGX function typedefs (real d3d12.h types; ngx.h stays Vulkan-only).
// Declared before bridge.h because the Bridge struct holds these pointers.
typedef NVSDK_NGX_Result (*PFN_NGX_D3D12_Init_Ext)(
    unsigned long long, const wchar_t *, ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_NGX_D3D12_Init_ProjectID)(
    const char *, int, const char *, const wchar_t *, ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(
    ID3D12GraphicsCommandList *, int, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(
    ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *,
    PFN_NVSDK_NGX_ProgressCallback);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle *);

#include "bridge.h"
#include "dlss_off_gate.h"
#include "dlss_bridge/runtime.hpp"
#include "dlss_bridge/components.hpp"

// Release builds generate this header from the package version. Direct source
// builds use an explicit development label.
#if __has_include("build_version.hpp")
#include "build_version.hpp"
#else
#define DLSS_BRIDGE_VERSION "dev"
#endif
#define DLSS5VK_VERSION_STRING DLSS_BRIDGE_VERSION

// ---------------------------------------------------------------------------
// module-wide state
// ---------------------------------------------------------------------------
static HMODULE          g_self;
static HMODULE          g_optiscaler_module;
static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_hook_cs;
static wchar_t          g_log_path[MAX_PATH];
static bool             g_log_ready;
static INIT_ONCE        g_runtime_once = INIT_ONCE_STATIC_INIT;
static bool EnsureRuntimeInitialized();

// ---------------------------------------------------------------------------
// configuration (dlss5-vk-bridge.cfg, next to the DLL)
// ---------------------------------------------------------------------------
using dlss_bridge::RuntimeConfig;
static RuntimeConfig g_cfg;
static dlss_bridge::RuntimeComposition g_components;
static void Log(const char *fmt, ...);
static void Warn(const char *fmt, ...);

enum class NgxInitKind : LONG { None = 0, ApplicationId = 1, ProjectId = 2 };
struct NgxInitIdentity {
    volatile LONG valid;
    NgxInitKind kind;
    unsigned long long application_id;
    int engine_type;
    int sdk_version;
    char project_id[128];
    char engine_version[64];
};
static NgxInitIdentity g_ngx_identity{};

static FeatureCreateContract g_feature_contracts[32]{};

static bool ReadCreateUInt(const NVSDK_NGX_Parameter *p, const char *key, unsigned int *value)
{
    return p && p->Get(key, value) == NGX_SUCCESS;
}

static void ClearFgHandle(const NVSDK_NGX_Handle *handle);

static void CaptureFeatureContract(int feature, const NVSDK_NGX_Parameter *p,
                                   const NVSDK_NGX_Handle *handle)
{
    if (!handle) return;
    ClearFgHandle(handle);
    FeatureCreateContract contract{};
    contract.handle = handle;
    contract.feature = feature;
    contract.valid =
        ReadCreateUInt(p, "Width", &contract.width) && contract.width &&
        ReadCreateUInt(p, "Height", &contract.height) && contract.height &&
        ReadCreateUInt(p, "OutWidth", &contract.out_width) && contract.out_width &&
        ReadCreateUInt(p, "OutHeight", &contract.out_height) && contract.out_height &&
        ReadCreateUInt(p, "PerfQualityValue", &contract.quality) &&
        ReadCreateUInt(p, "DLSS.Feature.Create.Flags", &contract.flags);
    contract.has_output_subrects =
        ReadCreateUInt(p, "DLSS.Enable.Output.Subrects", &contract.output_subrects);
    // Ray Reconstruction creation modes: opportunistic like subrects. These
    // spellings come from the DLSS 3.5 integration guides (the vendored SDK
    // snapshot does not define them); absent keys simply stay disabled and no
    // value is ever invented.
    contract.has_denoise_mode =
        ReadCreateUInt(p, "DLSS.Denoise.Mode", &contract.denoise_mode);
    contract.has_roughness_mode =
        ReadCreateUInt(p, "DLSS.Roughness.Mode", &contract.roughness_mode);
    contract.has_use_hw_depth =
        ReadCreateUInt(p, "DLSS.Use.HW.Depth", &contract.use_hw_depth);
    FeatureCreateContract *slot = nullptr;
    for (FeatureCreateContract &item : g_feature_contracts)
        if (item.handle == handle) { slot = &item; break; }
    if (!slot)
        for (FeatureCreateContract &item : g_feature_contracts)
            if (!item.handle) { slot = &item; break; }
    if (slot) *slot = contract;
    else contract.valid = false;
    if (contract.valid)
        Log("[contract] captured NGX feature %d handle=%p: %ux%u -> %ux%u quality=%u flags=0x%X%s",
            feature, (void *)handle, contract.width, contract.height,
            contract.out_width, contract.out_height, contract.quality, contract.flags,
            contract.has_output_subrects ? " with output-subrect policy" : "");
    else
        Log("[contract] NGX feature %d handle=%p is missing mandatory creation parameters",
            feature, (void *)handle);
    if (!slot) Warn("feature contract table is full; handle=%p cannot be bridged", (void *)handle);
}

static const FeatureCreateContract *FindFeatureContract(const NVSDK_NGX_Handle *handle)
{
    if (!handle) return nullptr;
    for (const FeatureCreateContract &contract : g_feature_contracts)
        if (contract.handle == handle) return contract.valid ? &contract : nullptr;
    return nullptr;
}

// Frame Generation coexistence: FG features are never bridged (their
// presentation-time interpolation contract is outside the in-frame executor),
// but they are no longer refused either. Tracked handles forward untouched so
// FG titles keep native frame generation while Super Resolution frames bridge.
// Creates are serialized on g_hook_cs, so plain slot scans are sufficient.
static const NVSDK_NGX_Handle *g_fg_handles[32]{};

static void NoteFgCreate(const NVSDK_NGX_Handle *handle)
{
    if (!handle) return;
    for (const NVSDK_NGX_Handle *slot : g_fg_handles)
        if (slot == handle) return;
    for (const NVSDK_NGX_Handle *&slot : g_fg_handles)
        if (!slot)
        {
            slot = handle;
            Log("[contract] NGX Frame Generation handle=%p tracked passthrough (never bridged; "
                "Super Resolution frames continue to bridge)", (void *)handle);
            return;
        }
    // Fixed table, unbounded game behavior: evict the oldest entry rather
    // than forwarding untracked (an untracked FG handle would fall through
    // to contract lookup and disable the bridge).
    Log("[contract] FG handle table full; evicting oldest for handle=%p", (void *)handle);
    for (int i = 0; i + 1 < 32; ++i) g_fg_handles[i] = g_fg_handles[i + 1];
    g_fg_handles[31] = handle;
}

// A released or recycled address must never keep an FG classification: an SR
// handle allocated at a former FG address would otherwise bypass bridging
// forever. SR contracts self-heal through geometry comparison; FG has no
// such check, so its registry is maintained explicitly.
static void ClearFgHandle(const NVSDK_NGX_Handle *handle)
{
    if (!handle) return;
    for (const NVSDK_NGX_Handle *&slot : g_fg_handles)
        if (slot == handle) { slot = nullptr; return; }
}

static bool IsFgHandle(const NVSDK_NGX_Handle *handle)
{
    if (!handle) return false;
    for (const NVSDK_NGX_Handle *slot : g_fg_handles)
        if (slot == handle) return true;
    return false;
}

// Idle-notice state: the bridge is inert while the game never creates an NGX
// feature (DLSS off in the game's settings). Every successful game create on
// any API bumps g_game_create_count; Vulkan presents and D3D12 accepted
// submissions prove frames are flowing. Once activity passes the gate with
// zero creates, a single log line tells the user to enable DLSS.
static volatile LONG g_game_create_count = 0;
static volatile LONG g_dlss_off_logged = 0;
static volatile LONGLONG g_total_presents = 0;

static void NoteGameCreateObserved()
{
    InterlockedIncrement(&g_game_create_count);
}

static void MaybeLogDlssOff(unsigned long long activity, const char *activity_name)
{
    // Plain read first: after the one-shot fires this stays a single volatile
    // load on the present/submission hot path instead of locked operations.
    if (g_dlss_off_logged) return;
    if (!DlssOffShouldLog(activity,
                           InterlockedCompareExchange(&g_game_create_count, 0, 0),
                           InterlockedCompareExchange(&g_dlss_off_logged, 0, 0)))
        return;
    if (InterlockedCompareExchange(&g_dlss_off_logged, 1, 0) != 0) return;
    Log("[bridge] %llu %s observed with no NGX feature created; DLSS appears "
        "disabled in-game, so the bridge is idle. Enable DLSS in the game's "
        "graphics settings to activate neural rendering.",
        activity, activity_name);
}

static void CaptureNgxApplicationIdentity(unsigned long long application_id, int sdk_version)
{
    g_ngx_identity.kind = NgxInitKind::ApplicationId;
    g_ngx_identity.application_id = application_id;
    g_ngx_identity.sdk_version = sdk_version;
    g_ngx_identity.project_id[0] = 0;
    g_ngx_identity.engine_version[0] = 0;
    MemoryBarrier();
    InterlockedExchange(&g_ngx_identity.valid, 1);
}

static void CaptureNgxProjectIdentity(const char *project_id, int engine_type,
                                      const char *engine_version, int sdk_version)
{
    g_ngx_identity.kind = NgxInitKind::ProjectId;
    g_ngx_identity.application_id = 0;
    g_ngx_identity.engine_type = engine_type;
    g_ngx_identity.sdk_version = sdk_version;
    strncpy_s(g_ngx_identity.project_id, project_id ? project_id : "", _TRUNCATE);
    strncpy_s(g_ngx_identity.engine_version, engine_version ? engine_version : "", _TRUNCATE);
    MemoryBarrier();
    InterlockedExchange(&g_ngx_identity.valid, 1);
}

static int ActiveFrameCount()
{
    return g_cfg.ring_slots;
}

static int ActiveViewportCount()
{
    const int n = g_cfg.max_viewports;
    if (n < 1) return 1;
    if (n > MAX_VIEWPORTS) return MAX_VIEWPORTS;
    return n;
}

// Per-viewport NGX handle -> viewport index. The first evaluate from an unseen
// handle claims a free viewport when max_viewports allows; beyond that the
// frame is forwarded untouched. Viewport 0 preserves the original
// single-feature behavior when max_viewports == 1.
static int ResolveViewport(const NVSDK_NGX_Handle *handle)
{
    if (!handle) return -1;
    for (int i = 0; i < ActiveViewportCount(); ++i)
        if (g_bridge.viewports[i].game_handle == handle) return i;
    for (int i = 0; i < ActiveViewportCount(); ++i)
        if (g_bridge.viewports[i].game_handle == nullptr)
        {
            g_bridge.viewports[i].game_handle = handle;
            if (i > 0)
                Log("[bridge] split-screen viewport %d claimed by game handle=%p", i, (void *)handle);
            return i;
        }
    return -1;
}

// Once the first private neural frame completes, strict mode never executes
// the game-side upscaler again. Startup calls only establish and seed the
// private path. A failed strict frame repeats the last neural image.
// Latched per viewport (BridgeViewport::neural_latched) so split-screen
// histories warm independently; the global below mirrors "any viewport
// latched" for legacy telemetry.
static volatile LONG g_neural_only_latched;

// OptiScaler owns the neural pass inside the private D3D12 evaluate. It accepts
// one virtual key, so reserve F24 as an internal signal and expose a chord that
// is available on ordinary keyboards and unlikely to overlap a game binding.
// The pulse surrounds one evaluate, which makes the mode transition occur at a
// frame boundary without stopping capture, transport, or private DLSS.
static volatile LONG g_toggle_chord_down;
static volatile LONG g_neural_effect_visible = 1;
static const int kNeuralToggleVirtualKey = VK_F24;

// ---------------------------------------------------------------------------
// logging (8 MB cap, matches the DX11 bridge)
// ---------------------------------------------------------------------------
static void LogPath()
{
    GetModuleFileNameW(g_self, g_log_path, MAX_PATH);
    if (wchar_t *s = wcsrchr(g_log_path, L'\\'))
        wcscpy_s(s + 1, MAX_PATH - (s + 1 - g_log_path), L"dlss5-vk-bridge.log");
    g_log_ready = true;
}

static void LogV(const char *tag, const char *fmt, va_list ap)
{
    if (!g_log_ready) return;
    EnterCriticalSection(&g_log_cs);

    FILE *f = nullptr;
    if (_wfopen_s(&f, g_log_path, L"a") == 0 && f)
    {
        // "a" reports position 0 until the first write; seek to learn the size.
        _fseeki64(f, 0, SEEK_END);
        long long here = _ftelli64(f);
        if (here > 8 * 1024 * 1024)
        {
            fclose(f); f = nullptr;
            if (_wfopen_s(&f, g_log_path, L"w") != 0) f = nullptr;
            here = 0;
        }
        if (f && here == 0) fputs("dlss5-vk-bridge log\n", f);
    }
    if (f)
    {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "[%02d:%02d:%02d.%03d]%s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, tag);
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void Log(const char *fmt, ...)  { va_list ap; va_start(ap, fmt); LogV("",      fmt, ap); va_end(ap); }
static void Warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); LogV(" WARN:", fmt, ap); va_end(ap); }

static bool BeginNeuralTogglePulse()
{
    const bool chord =
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 &&
        (GetAsyncKeyState('N') & 0x8000) != 0;
    if (!chord)
    {
        InterlockedExchange(&g_toggle_chord_down, 0);
        return false;
    }
    if (InterlockedExchange(&g_toggle_chord_down, 1) != 0) return false;

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(kNeuralToggleVirtualKey);
    if (SendInput(1, &input, sizeof(input)) != 1)
    {
        Warn("[control] Ctrl+Shift+N was pressed, but the neural toggle signal failed (Win32 %lu)",
             GetLastError());
        return false;
    }
    const LONG enabled = InterlockedCompareExchange(&g_neural_effect_visible, 0, 0);
    Log("[control] Ctrl+Shift+N: requesting neural effect %s",
        enabled ? "hidden" : "visible");
    return true;
}

static void EndNeuralTogglePulse(bool active)
{
    if (!active) return;
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(kNeuralToggleVirtualKey);
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(1, &input, sizeof(input)) != 1)
        Warn("[control] neural toggle key release failed (Win32 %lu)", GetLastError());
    const LONG previous = InterlockedCompareExchange(&g_neural_effect_visible, 0, 0);
    InterlockedExchange(&g_neural_effect_visible, previous ? 0 : 1);
    Log("[control] neural A/B input delivered; requested effect=%s; model and private DLSS remain warm",
        previous ? "hidden" : "visible");
}

static bool LoadConfig()
{
    wchar_t path[MAX_PATH] = {};
    // The launcher points at the policy in its isolated session.
    if (GetEnvironmentVariableW(L"DLSS_BRIDGE_CONFIG", path, MAX_PATH) == 0)
    {
        GetModuleFileNameW(g_self, path, MAX_PATH);
        if (wchar_t *s = wcsrchr(path, L'\\'))
            wcscpy_s(s + 1, MAX_PATH - (s + 1 - path), L"dlss5-vk-bridge.cfg");
    }

    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || f == nullptr)
    {
        Log("[cfg] no runtime policy found; using compiled defaults");
        return true;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64] = {}, value[128] = {};
        char *content = line;
        while (*content == ' ' || *content == '\t') ++content;
        if (*content == '#' || *content == '\r' || *content == '\n' || *content == 0) continue;
        if (sscanf_s(line, " %63[^= ] = %127[^\r\n]", key, (unsigned)sizeof(key),
                     value, (unsigned)sizeof(value)) != 2)
        { Warn("[cfg] malformed policy line: %s", content); fclose(f); return false; }
        char *begin = value;
        while (*begin == ' ' || *begin == '\t') ++begin;
        char *tail = begin + strlen(begin);
        while (tail > begin && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!g_cfg.Apply(key, begin))
        { Warn("[cfg] invalid or unknown setting %s=%s", key, begin); fclose(f); return false; }
    }
    fclose(f);
    if (!g_cfg.Valid())
    { Warn("[cfg] invalid runtime policy (neural_pipeline_frames must be smaller than ring_slots; max_viewports 1..4)"); return false; }
    Log("[cfg] verbose=%d execution_mode=%s "
        "compute_adapter=%s ring_slots=%d neural_pipeline_frames=%d latency_budget_ms=%d "
        "output_transport=%s neural_queue_mode=%s gpu_timestamps=%d",
        g_cfg.verbose,
        dlss_bridge::ExecutionModeName(g_cfg.execution_mode), g_cfg.compute_adapter.text,
        g_cfg.ring_slots, g_cfg.neural_pipeline_frames,
        g_cfg.latency_budget_ms,
        dlss_bridge::OutputTransportName(g_cfg.output_transport),
        dlss_bridge::NeuralQueueModeName(g_cfg.neural_queue_mode), g_cfg.gpu_timestamps);
    Log("[cfg] active frame/transport ring slots=%d viewports=%d", ActiveFrameCount(), ActiveViewportCount());
    return true;
}

// ---------------------------------------------------------------------------
// Frame Generation has a different presentation-time contract from the
// temporal upscaling operation implemented here, so FG features are never
// bridged. They are forwarded untouched and tracked (see g_fg_handles) so FG
// titles keep native frame generation while Super Resolution frames bridge.
// ---------------------------------------------------------------------------

// Implemented by vk_interop.inc after the host-specific Vulkan wrappers. A
// submitted game frame waits on the bridge's output event on the GPU. Keep
// vkQueuePresentKHR out of the driver until the worker has produced that
// output, otherwise NVIDIA's present path and the private D3D12 recorder can
// acquire the driver's internal locks in opposite order.
static void BridgeWaitBeforePresent(VkQueue queue);

// ---------------------------------------------------------------------------
// the three halves (order matters: D3D12 session, then the host, then interop)
//
// The injected hook, Vulkan layer, and ReShade add-on builds supply the same
// seam to vk_interop.inc: device dispatch, submit and command-buffer events,
// and a handful of kHost* traits.
// ---------------------------------------------------------------------------
#include "d3d12_session.inc"
#include "frame_pacing.inc"
#include "d3d11_host.inc"
#include "d3d12_host.inc"
#if defined(DLSS5VK_HOOK_HOST)
#include "child_follow.inc"
#include "hook_host.inc"
#elif defined(DLSS5VK_ADDON_HOST)
#include "addon_host.inc"
#else
#include "vk_layer.inc"
#endif
#include "vk_interop.inc"
#include "d3d11_interop.inc"
#include "d3d12_interop.inc"

// ---------------------------------------------------------------------------
// NGX Vulkan evaluate hook
//
// The game reaches NVSDK_NGX_VULKAN_EvaluateFeature through whichever NGX
// module the loader gave it; several modules can export it and forward to one
// another. Every real one is patched with a 14-byte absolute jump to a distinct
// detour, and a thread-local nesting depth means only the outermost call -- the
// game's -- arms a bridged frame. No trampoline or disassembler: the original
// bytes are restored around each forwarded call.
// ---------------------------------------------------------------------------
struct Hook
{
    void         *target;
    void         *detour;
    unsigned char orig[14];
    bool          installed;
};

static const int kMaxHooks = 12;
static Hook      g_eval_hooks[kMaxHooks];      // NVSDK_NGX_VULKAN_EvaluateFeature
static int       g_eval_count;
static Hook      g_create_hooks[kMaxHooks];    // NVSDK_NGX_VULKAN_CreateFeature
static int       g_create_count;
static Hook      g_create1_hooks[kMaxHooks];   // NVSDK_NGX_VULKAN_CreateFeature1
static int       g_create1_count;
static Hook      g_init_ext_hooks[kMaxHooks];  // NVSDK_NGX_VULKAN_Init_Ext
static int       g_init_ext_count;
static Hook      g_init_ext2_hooks[kMaxHooks]; // NVSDK_NGX_VULKAN_Init_Ext2
static int       g_init_ext2_count;
static Hook      g_init_project_hooks[kMaxHooks]; // NVSDK_NGX_VULKAN_Init_ProjectID
static int       g_init_project_count;
static Hook      g_d3d11_eval_hooks[kMaxHooks];   // NVSDK_NGX_D3D11_EvaluateFeature
static int       g_d3d11_eval_count;
static Hook      g_d3d11_create_hooks[kMaxHooks]; // NVSDK_NGX_D3D11_CreateFeature
static int       g_d3d11_create_count;
static int       g_lift_d3d11_eval_depth;
static int       g_lift_d3d11_create_depth;
static Hook      g_d3d12_eval_hooks[kMaxHooks];   // NVSDK_NGX_D3D12_EvaluateFeature
static int       g_d3d12_eval_count;
static Hook      g_d3d12_create_hooks[kMaxHooks]; // NVSDK_NGX_D3D12_CreateFeature
static int       g_d3d12_create_count;
static Hook      g_d3d12_init_ext_hooks[kMaxHooks];     // NVSDK_NGX_D3D12_Init_Ext
static int       g_d3d12_init_ext_count;
static Hook      g_d3d12_init_project_hooks[kMaxHooks]; // NVSDK_NGX_D3D12_Init_ProjectID
static int       g_d3d12_init_project_count;
static int       g_lift_d3d12_eval_depth;
static int       g_lift_d3d12_create_depth;
static int       g_lift_d3d12_init_depth;
// ReleaseFeature detours only clear handle classifications and forward; the
// exports are optional (absent on older NGX builds) and simply unhooked then.
static Hook      g_vk_release_hooks[kMaxHooks];    // NVSDK_NGX_VULKAN_ReleaseFeature
static int       g_vk_release_count;
static Hook      g_d3d11_release_hooks[kMaxHooks]; // NVSDK_NGX_D3D11_ReleaseFeature
static int       g_d3d11_release_count;
static Hook      g_d3d12_release_hooks[kMaxHooks]; // NVSDK_NGX_D3D12_ReleaseFeature
static int       g_d3d12_release_count;
static int       g_lift_release_depth;
static __declspec(thread) int g_ngx_nest;

static void WriteCode(void *dst, const void *src, size_t n)
{
    DWORD old = 0;
    if (VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old))
    {
        memcpy(dst, src, n);
        VirtualProtect(dst, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), dst, n);
    }
}

static void BuildJump(unsigned char code[14], void *dst)
{
    code[0] = 0xFF; code[1] = 0x25;               // jmp qword ptr [rip+0]
    code[2] = code[3] = code[4] = code[5] = 0x00;
    memcpy(code + 6, &dst, 8);
}

static void HookInstall(Hook *h)
{
    memcpy(h->orig, h->target, 14);
    unsigned char jmp[14];
    BuildJump(jmp, h->detour);
    WriteCode(h->target, jmp, 14);
    h->installed = true;
}
static void HookRestore(Hook *h)  { if (h->installed) WriteCode(h->target, h->orig, 14); }
static void HookReinstall(Hook *h){ if (h->installed) { unsigned char j[14]; BuildJump(j, h->detour); WriteCode(h->target, j, 14); } }

// Lift / restore every installed evaluate patch at once. The game's evaluate is
// forwarded with ALL of them removed, not just the module being called through.
//
// Un-patching only the module being called leaves the other hooks live inside
// a forwarded call. NGX and Streamline modules can call through one another,
// so another live detour can recursively re-enter the bridge. Removing every
// evaluate patch for the duration preserves the complete forwarded call chain;
// the nesting depth still arms the bridge on the outermost call alone.
//
// The two hook classes lift differently:
//
//   * Forwarding an EVALUATE lifts the evaluate patches only. The CreateFeature
//     patches stay hot on purpose: creates come from the game on its own
//     schedule (enabling Frame Generation mid-session, for instance), and if
//     they were lifted here, a create landing during any forwarded evaluate
//     would run unobserved -- with evaluates streaming every frame, that race
//     would miss the feature create that the bridge must explicitly classify.
//     A concurrent create hits its still-installed detour instead and
//     serialises on the same critical section.
//   * Forwarding a CREATE lifts everything. A create chains through the same
//     interposer modules as an evaluate and can run warm-up work behind it;
//     no patch of this DLL's may be live while that original code runs.
//
// Each class is depth-counted (under g_hook_cs, which is held across every
// forward) so a nested forward -- a create reached from inside a forwarded
// evaluate's interposer chain, or the reverse -- reinstalls a class only when
// the outermost lift of that class returns.
static int g_lift_eval_depth;
static int g_lift_create_depth;

static void LiftEvalHooks()
{
    if (++g_lift_eval_depth != 1) return;
    for (int i = 0; i < g_eval_count; ++i) HookRestore(&g_eval_hooks[i]);
}
static void UnliftEvalHooks()
{
    if (--g_lift_eval_depth != 0) return;
    for (int i = 0; i < g_eval_count; ++i) HookReinstall(&g_eval_hooks[i]);
}
static void LiftCreateHooks()
{
    if (++g_lift_create_depth != 1) return;
    for (int i = 0; i < g_create_count;  ++i) HookRestore(&g_create_hooks[i]);
    for (int i = 0; i < g_create1_count; ++i) HookRestore(&g_create1_hooks[i]);
}
static void UnliftCreateHooks()
{
    if (--g_lift_create_depth != 0) return;
    for (int i = 0; i < g_create_count;  ++i) HookReinstall(&g_create_hooks[i]);
    for (int i = 0; i < g_create1_count; ++i) HookReinstall(&g_create1_hooks[i]);
}

static const DWORD kStatusStackOverflow = 0xC00000FDu;

// ---------------------------------------------------------------------------
// fault forensics
//
// A contained fault records the faulting module and offset, the accessed
// address, and the state of every hook before lifted patches are restored.
// This distinguishes a graphics-runtime fault from a stale module or another
// interposer rewriting the same export.
// ---------------------------------------------------------------------------
static const wchar_t *ModuleName(HMODULE m);

struct FaultInfo
{
    DWORD     code;       // exception code, 0 = no fault
    void     *addr;       // faulting instruction
    bool      has_av;     // av_op/av_target below are valid
    ULONG_PTR av_op;      // 0 read / 1 write / 8 DEP execute
    void     *av_target;  // the address the instruction touched
};

static int FaultCapture(FaultInfo *fi, EXCEPTION_POINTERS *ep)
{
    fi->code = ep->ExceptionRecord->ExceptionCode;
    fi->addr = ep->ExceptionRecord->ExceptionAddress;
    if (fi->code == 0xC0000005u && ep->ExceptionRecord->NumberParameters >= 2)
    {
        fi->has_av    = true;
        fi->av_op     = ep->ExceptionRecord->ExceptionInformation[0];
        fi->av_target = reinterpret_cast<void *>(ep->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void LogFaultSite(const FaultInfo *fi)
{
    HMODULE m = nullptr;
    if (fi->addr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(fi->addr), &m) && m)
        Log("[hook]   fault site: %ls+0x%llX (%p)", ModuleName(m),
            (unsigned long long)(reinterpret_cast<ULONG_PTR>(fi->addr) -
                                 reinterpret_cast<ULONG_PTR>(m)), fi->addr);
    else
        Log("[hook]   fault site: %p -- NOT IN ANY LOADED MODULE (a call or jump landed "
            "in unmapped or freed code)", fi->addr);
    if (fi->has_av)
        Log("[hook]   access violation: %s address %p",
            fi->av_op == 0 ? "reading" : fi->av_op == 1 ? "writing" :
            fi->av_op == 8 ? "executing (DEP)" : "touching", fi->av_target);
}

// Audit one hook array while the fault is fresh: g_hook_cs held, the lifted
// state still in place. Returns the number of anomalies logged.
static int AuditHookArray(const char *what, Hook *hooks, int count, bool lifted)
{
    int bad = 0;
    for (int i = 0; i < count; ++i)
    {
        Hook *h = &hooks[i];
        if (!h->installed) continue;
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(h->target), &m) || !m)
        {
            Log("[hook]   audit: %s hook @ %p -- its module is NO LONGER LOADED; the "
                "patched code was unloaded out from under the bridge", what, h->target);
            ++bad;
            continue;
        }
        unsigned char jmp[14];
        BuildJump(jmp, h->detour);
        const unsigned char *live = static_cast<const unsigned char *>(h->target);
        const unsigned char *want = lifted ? h->orig : jmp;
        if (memcmp(live, want, 14) == 0) continue;
        if (memcmp(live, lifted ? jmp : h->orig, 14) == 0)
            Log("[hook]   audit: %s hook @ %p in %ls -- entry in the WRONG state (%s)",
                what, h->target, ModuleName(m),
                lifted ? "the bridge's jump where original bytes were restored"
                       : "original bytes where the bridge's jump should be");
        else
            Log("[hook]   audit: %s hook @ %p in %ls -- entry REWRITTEN by another "
                "interposer: %02X %02X %02X %02X %02X %02X %02X %02X ...",
                what, h->target, ModuleName(m),
                live[0], live[1], live[2], live[3], live[4], live[5], live[6], live[7]);
        ++bad;
    }
    return bad;
}

static void AuditHooksAtFault()
{
    int bad = 0;
    bad += AuditHookArray("evaluate", g_eval_hooks,    g_eval_count,    g_lift_eval_depth   > 0);
    bad += AuditHookArray("create",   g_create_hooks,  g_create_count,  g_lift_create_depth > 0);
    bad += AuditHookArray("create1",  g_create1_hooks, g_create1_count, g_lift_create_depth > 0);
    bad += AuditHookArray("d3d11-evaluate", g_d3d11_eval_hooks, g_d3d11_eval_count, g_lift_d3d11_eval_depth > 0);
    bad += AuditHookArray("d3d11-create", g_d3d11_create_hooks, g_d3d11_create_count, g_lift_d3d11_create_depth > 0);
    bad += AuditHookArray("d3d12-evaluate", g_d3d12_eval_hooks, g_d3d12_eval_count, g_lift_d3d12_eval_depth > 0);
    bad += AuditHookArray("d3d12-create", g_d3d12_create_hooks, g_d3d12_create_count, g_lift_d3d12_create_depth > 0);
    bad += AuditHookArray("d3d12-init", g_d3d12_init_ext_hooks, g_d3d12_init_ext_count, g_lift_d3d12_init_depth > 0);
    bad += AuditHookArray("d3d12-init-project", g_d3d12_init_project_hooks, g_d3d12_init_project_count, g_lift_d3d12_init_depth > 0);
    if (bad == 0)
        Log("[hook]   audit: every hooked entry point is loaded and in the expected "
            "state; the fault is not a patch-integrity problem");
}

static NVSDK_NGX_Result SafeCallVkEvaluate(PFN_NGX_VK_Evaluate fn, VkCommandBuffer cmd,
                                           const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *p,
                                           PFN_NVSDK_NGX_ProgressCallback cb, FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(cmd, feat, p, cb); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        // Catching a stack overflow does not put the guard page back. Restore
        // it before another deep call on this thread can terminate the process.
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

// Forward one call through the module at hook index idx, original bytes restored
// around it. Serialised so two threads never see a half-patched entry point.
static NVSDK_NGX_Result ForwardVkEvaluateVia(int idx, VkCommandBuffer cmd,
                                             const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *p,
                                             PFN_NVSDK_NGX_ProgressCallback cb)
{
    Hook &h = g_eval_hooks[idx];
    if (reinterpret_cast<uintptr_t>(h.target) < 0x10000)
    {
        // A detour with no hook slot behind it (only reachable through a stale
        // jump this load refused to adopt). Nothing can be forwarded; say so
        // once instead of silently swallowing the game's DLSS call.
        static LONG warned = 0;
        if (InterlockedExchange(&warned, 1) == 0)
            Warn("[hook] an evaluate arrived through a detour with no hook behind it; failing the call");
        return NGX_FAIL;
    }

    EnterCriticalSection(&g_hook_cs);
    LiftEvalHooks();
    FaultInfo fi;
    NVSDK_NGX_Result r = SafeCallVkEvaluate(reinterpret_cast<PFN_NGX_VK_Evaluate>(h.target),
                                            cmd, feat, p, cb, &fi);
    if (fi.code) AuditHooksAtFault();   // before the lifted patches go back on
    UnliftEvalHooks();
    LeaveCriticalSection(&g_hook_cs);
    if (fi.code)
    {
        Log("[hook] forwarded NGX Vulkan evaluate raised 0x%08X%s", fi.code,
            fi.code == kStatusStackOverflow ? " (stack overflow)" : "");
        LogFaultSite(&fi);
        BridgeDisable("the game's NGX evaluate faulted while the bridge was forwarding it");
    }
    return r;
}

static NVSDK_NGX_Result BridgedVkEvaluate(int idx, VkCommandBuffer cmd, const NVSDK_NGX_Handle *feat,
                                          const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback cb)
{
    LARGE_INTEGER evaluate_started = {}, forwarded_done = {}, capture_done = {};
    QueryPerformanceCounter(&evaluate_started);
    ++g_ngx_nest;
    const bool outer = (g_ngx_nest == 1);

    // A worker-side failure cannot join its own handoff thread, so teardown is
    // deferred to the next game-thread evaluate. Finish it before the disabled
    // gate; fail-closed sessions otherwise never re-enter BridgeVkFrame.
    if (outer && g_release_deferred)
    {
        InterlockedExchange(&g_release_deferred, 0);
        FrameLock();
        D3D12ReleaseResources();
        FrameUnlock();
    }

    // Frame Generation features are never bridged: forward untouched without
    // touching viewport, contract, or frame state. This check precedes the
    // fail-closed gate below on purpose: native FG keeps running even after
    // the SR path has failed.
    if (IsFgHandle(feat))
    {
        --g_ngx_nest;
        return ForwardVkEvaluateVia(idx, cmd, feat, p, cb);
    }

    // An attached bridge that has failed remains fail-closed for the process.
    if (outer && g_bridge.disabled)
    {
        --g_ngx_nest;
        return NGX_FAIL;
    }

    // During bootstrap we forward while learning/building the private session.
    // Once strict mode latches, the game evaluate stays suppressed. Frames that
    // cannot enter the worker ring repeat the last completed neural image; an
    // NGX failure here prevents the game from submitting that repeat command.
    // Skip state is per-viewport so split-screen histories latch independently.
    const bool rr_frame = BridgeRayReconstructionInput(p) != nullptr;
    const int skip_vp = (!rr_frame && feat) ? ResolveViewport(feat) : -1;
    const bool skip_game = outer && !rr_frame && skip_vp >= 0 && BridgeSkipGameEvaluateVp(skip_vp, p);
    NVSDK_NGX_Result r = NGX_SUCCESS;
    if (skip_game)
    {
        static bool said = false;
        if (!said) { said = true;
            Log("[policy] skipping the game-side evaluate; the private neural result is authoritative"); }
    }
    else
        r = ForwardVkEvaluateVia(idx, cmd, feat, p, cb);
    QueryPerformanceCounter(&forwarded_done);
    if (outer && !g_bridge.disabled)
    {
        FaultInfo fi;
        memset(&fi, 0, sizeof(fi));
        __try { BridgeVkFrame(cmd, feat, p); }
        __except (FaultCapture(&fi, GetExceptionInformation()))
        { BridgeFrameUnlockAll();   // never leave the submit hooks locked out
          Log("[bridge] frame path faulted 0x%08X; disabling to protect the game", fi.code);
          LogFaultSite(&fi);
          BridgeDisable("the bridge frame path raised an exception"); }
    }
    QueryPerformanceCounter(&capture_done);
    --g_ngx_nest;
    const double elapsed = CpuElapsedMs(evaluate_started);
    if (outer && elapsed >= 8.0)
    {
        LARGE_INTEGER frequency = {};
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart)
        {
            const double scale = 1000.0 / (double)frequency.QuadPart;
            Log("[stall] game-thread NGX evaluate %.3fms (game call %.3fms, bridge capture %.3fms, setup %.3fms) bridge-frame=%llu thread=%lu",
                elapsed,
                (double)(forwarded_done.QuadPart - evaluate_started.QuadPart) * scale,
                (double)(capture_done.QuadPart - forwarded_done.QuadPart) * scale,
                elapsed - (double)(capture_done.QuadPart - evaluate_started.QuadPart) * scale,
                (unsigned long long)g_bridge.timeline, GetCurrentThreadId());
        }
    }
    if (outer) NoteNgxPace(feat, evaluate_started);
    return r;
}

#define EVAL_DETOUR(N) \
    static NVSDK_NGX_Result Detour_VK_Eval_##N(VkCommandBuffer c, const NVSDK_NGX_Handle *f, \
        const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback cb) \
    { return BridgedVkEvaluate(N, c, f, p, cb); }
EVAL_DETOUR(0) EVAL_DETOUR(1) EVAL_DETOUR(2)  EVAL_DETOUR(3)
EVAL_DETOUR(4) EVAL_DETOUR(5) EVAL_DETOUR(6)  EVAL_DETOUR(7)
EVAL_DETOUR(8) EVAL_DETOUR(9) EVAL_DETOUR(10) EVAL_DETOUR(11)

static void *const g_eval_detours[kMaxHooks] = {
    (void *)Detour_VK_Eval_0, (void *)Detour_VK_Eval_1, (void *)Detour_VK_Eval_2,  (void *)Detour_VK_Eval_3,
    (void *)Detour_VK_Eval_4, (void *)Detour_VK_Eval_5, (void *)Detour_VK_Eval_6,  (void *)Detour_VK_Eval_7,
    (void *)Detour_VK_Eval_8, (void *)Detour_VK_Eval_9, (void *)Detour_VK_Eval_10, (void *)Detour_VK_Eval_11,
};

// ---------------------------------------------------------------------------
// NGX Vulkan create hook. Supported creates are forwarded and captured;
// unsupported feature contracts are rejected explicitly.
// The forwarding contract is the evaluate's: all patches lifted for the
// duration, serialised on the same critical section, exceptions contained.
// One difference: a create that FAULTS must report failure, not success -- a
// success would hand the game a garbage feature handle it will then use.
// ---------------------------------------------------------------------------
static NVSDK_NGX_Result SafeCallVkCreate(PFN_NGX_VK_Create fn, VkCommandBuffer cmd, int feature,
                                         NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out, FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(cmd, feature, p, out); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

static NVSDK_NGX_Result SafeCallVkCreate1(PFN_NGX_VK_Create1 fn, VkDevice dev, VkCommandBuffer cmd,
                                          int feature, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out,
                                          FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(dev, cmd, feature, p, out); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

static NVSDK_NGX_Result ForwardVkCreateVia(Hook *h, bool one, VkDevice dev, VkCommandBuffer cmd,
                                           int feature, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    if (reinterpret_cast<uintptr_t>(h->target) < 0x10000)
    {
        static LONG warned = 0;
        if (InterlockedExchange(&warned, 1) == 0)
            Warn("[hook] a feature create arrived through a detour with no hook behind it; "
                 "failing it rather than inventing a handle.");
        return NGX_FAIL;
    }

    EnterCriticalSection(&g_hook_cs);
    LiftEvalHooks();
    LiftCreateHooks();
    FaultInfo fi;
    NVSDK_NGX_Result r = one
        ? SafeCallVkCreate1(reinterpret_cast<PFN_NGX_VK_Create1>(h->target), dev, cmd, feature, p, out, &fi)
        : SafeCallVkCreate (reinterpret_cast<PFN_NGX_VK_Create >(h->target),      cmd, feature, p, out, &fi);
    if (fi.code) AuditHooksAtFault();   // before the lifted patches go back on
    UnliftCreateHooks();
    UnliftEvalHooks();
    LeaveCriticalSection(&g_hook_cs);
    if (fi.code)
    {
        Log("[hook] forwarded NGX Vulkan create (feature %d) raised 0x%08X%s", feature, fi.code,
            fi.code == kStatusStackOverflow ? " (stack overflow)" : "");
        LogFaultSite(&fi);
        BridgeDisable("the game's NGX feature creation faulted while the bridge was forwarding it");
    }
    return r;
}

static NVSDK_NGX_Result BridgedVkCreate(int idx, VkCommandBuffer cmd, int feature,
                                        NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    NVSDK_NGX_Result result = ForwardVkCreateVia(
        &g_create_hooks[idx], false, VK_NULL_HANDLE, cmd, feature, p, out);
    if (result == NGX_SUCCESS && out)
    {
        NoteGameCreateObserved();
        if (feature == NVSDK_NGX_FEATURE_FRAME_GENERATION) NoteFgCreate(*out);
        else CaptureFeatureContract(feature, p, *out);
    }
    return result;
}
static NVSDK_NGX_Result BridgedVkCreate1(int idx, VkDevice dev, VkCommandBuffer cmd, int feature,
                                         NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    NVSDK_NGX_Result result = ForwardVkCreateVia(
        &g_create1_hooks[idx], true, dev, cmd, feature, p, out);
    if (result == NGX_SUCCESS && out)
    {
        NoteGameCreateObserved();
        if (feature == NVSDK_NGX_FEATURE_FRAME_GENERATION) NoteFgCreate(*out);
        else CaptureFeatureContract(feature, p, *out);
    }
    return result;
}

#define CREATE_DETOUR(N) \
    static NVSDK_NGX_Result Detour_VK_Create_##N(VkCommandBuffer c, int ft, \
        NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o) \
    { return BridgedVkCreate(N, c, ft, p, o); } \
    static NVSDK_NGX_Result Detour_VK_Create1_##N(VkDevice d, VkCommandBuffer c, int ft, \
        NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o) \
    { return BridgedVkCreate1(N, d, c, ft, p, o); }
CREATE_DETOUR(0) CREATE_DETOUR(1) CREATE_DETOUR(2)  CREATE_DETOUR(3)
CREATE_DETOUR(4) CREATE_DETOUR(5) CREATE_DETOUR(6)  CREATE_DETOUR(7)
CREATE_DETOUR(8) CREATE_DETOUR(9) CREATE_DETOUR(10) CREATE_DETOUR(11)

static void *const g_create_detours[kMaxHooks] = {
    (void *)Detour_VK_Create_0, (void *)Detour_VK_Create_1, (void *)Detour_VK_Create_2,  (void *)Detour_VK_Create_3,
    (void *)Detour_VK_Create_4, (void *)Detour_VK_Create_5, (void *)Detour_VK_Create_6,  (void *)Detour_VK_Create_7,
    (void *)Detour_VK_Create_8, (void *)Detour_VK_Create_9, (void *)Detour_VK_Create_10, (void *)Detour_VK_Create_11,
};
static void *const g_create1_detours[kMaxHooks] = {
    (void *)Detour_VK_Create1_0, (void *)Detour_VK_Create1_1, (void *)Detour_VK_Create1_2,  (void *)Detour_VK_Create1_3,
    (void *)Detour_VK_Create1_4, (void *)Detour_VK_Create1_5, (void *)Detour_VK_Create1_6,  (void *)Detour_VK_Create1_7,
    (void *)Detour_VK_Create1_8, (void *)Detour_VK_Create1_9, (void *)Detour_VK_Create1_10, (void *)Detour_VK_Create1_11,
};

// A game can create Vulkan before NGX loads. The later NGX Init_Ext call
// supplies the already-created handles, which lets the injected host adopt the
// real render device instead of guessing from opaque handles.
// All Init_Ext patches are lifted while forwarding because NGX modules may
// forward this call through one another just like Create/Evaluate.
static int g_lift_init_ext_depth;
static void LiftInitExtHooks()
{
    if (++g_lift_init_ext_depth != 1) return;
    for (int i = 0; i < g_init_ext_count; ++i) HookRestore(&g_init_ext_hooks[i]);
    for (int i = 0; i < g_init_ext2_count; ++i) HookRestore(&g_init_ext2_hooks[i]);
    for (int i = 0; i < g_init_project_count; ++i) HookRestore(&g_init_project_hooks[i]);
}
static void UnliftInitExtHooks()
{
    if (--g_lift_init_ext_depth != 0) return;
    for (int i = 0; i < g_init_ext_count; ++i) HookReinstall(&g_init_ext_hooks[i]);
    for (int i = 0; i < g_init_ext2_count; ++i) HookReinstall(&g_init_ext2_hooks[i]);
    for (int i = 0; i < g_init_project_count; ++i) HookReinstall(&g_init_project_hooks[i]);
}

static NVSDK_NGX_Result ForwardVkInitExt(int idx, unsigned long long app_id,
                                         const wchar_t *data_path, VkInstance instance,
                                         VkPhysicalDevice phys, VkDevice device, int version,
                                         const void *feature_info)
{
    Hook &h = g_init_ext_hooks[idx];
#if defined(DLSS5VK_HOOK_HOST)
    HostCaptureNgxInit(instance, phys, device);
#endif
    EnterCriticalSection(&g_hook_cs);
    LiftInitExtHooks();
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NGX_VK_Init>(h.target)(
        app_id, data_path, instance, phys, device, version, feature_info);
    if (result == NGX_SUCCESS) CaptureNgxApplicationIdentity(app_id, version);
    UnliftInitExtHooks();
    LeaveCriticalSection(&g_hook_cs);
    return result;
}

#define INIT_EXT_DETOUR(N) \
    static NVSDK_NGX_Result Detour_VK_InitExt_##N(unsigned long long a, const wchar_t *p, \
        VkInstance i, VkPhysicalDevice ph, VkDevice d, int v, const void *f) \
    { return ForwardVkInitExt(N, a, p, i, ph, d, v, f); }
INIT_EXT_DETOUR(0) INIT_EXT_DETOUR(1) INIT_EXT_DETOUR(2)  INIT_EXT_DETOUR(3)
INIT_EXT_DETOUR(4) INIT_EXT_DETOUR(5) INIT_EXT_DETOUR(6)  INIT_EXT_DETOUR(7)
INIT_EXT_DETOUR(8) INIT_EXT_DETOUR(9) INIT_EXT_DETOUR(10) INIT_EXT_DETOUR(11)

static void *const g_init_ext_detours[kMaxHooks] = {
    (void *)Detour_VK_InitExt_0, (void *)Detour_VK_InitExt_1, (void *)Detour_VK_InitExt_2,
    (void *)Detour_VK_InitExt_3, (void *)Detour_VK_InitExt_4, (void *)Detour_VK_InitExt_5,
    (void *)Detour_VK_InitExt_6, (void *)Detour_VK_InitExt_7, (void *)Detour_VK_InitExt_8,
    (void *)Detour_VK_InitExt_9, (void *)Detour_VK_InitExt_10, (void *)Detour_VK_InitExt_11,
};

static NVSDK_NGX_Result ForwardVkInitExt2(
    int idx, unsigned long long app_id, const wchar_t *data_path,
    VkInstance instance, VkPhysicalDevice phys, VkDevice device,
    PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
    int version, const void *feature_info)
{
    Hook &h = g_init_ext2_hooks[idx];
#if defined(DLSS5VK_HOOK_HOST)
    HostCaptureNgxInit(instance, phys, device);
#endif
    EnterCriticalSection(&g_hook_cs);
    LiftInitExtHooks();
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NGX_VK_Init_Ext2>(h.target)(
        app_id, data_path, instance, phys, device, gipa, gdpa, version, feature_info);
    if (result == NGX_SUCCESS) CaptureNgxApplicationIdentity(app_id, version);
    UnliftInitExtHooks();
    LeaveCriticalSection(&g_hook_cs);
    return result;
}

#define INIT_EXT2_DETOUR(N) \
    static NVSDK_NGX_Result Detour_VK_InitExt2_##N( \
        unsigned long long a, const wchar_t *p, VkInstance i, VkPhysicalDevice ph, \
        VkDevice d, PFN_vkGetInstanceProcAddr gi, PFN_vkGetDeviceProcAddr gd, \
        int v, const void *f) \
    { return ForwardVkInitExt2(N, a, p, i, ph, d, gi, gd, v, f); }
INIT_EXT2_DETOUR(0) INIT_EXT2_DETOUR(1) INIT_EXT2_DETOUR(2)  INIT_EXT2_DETOUR(3)
INIT_EXT2_DETOUR(4) INIT_EXT2_DETOUR(5) INIT_EXT2_DETOUR(6)  INIT_EXT2_DETOUR(7)
INIT_EXT2_DETOUR(8) INIT_EXT2_DETOUR(9) INIT_EXT2_DETOUR(10) INIT_EXT2_DETOUR(11)

static void *const g_init_ext2_detours[kMaxHooks] = {
    (void *)Detour_VK_InitExt2_0, (void *)Detour_VK_InitExt2_1, (void *)Detour_VK_InitExt2_2,
    (void *)Detour_VK_InitExt2_3, (void *)Detour_VK_InitExt2_4, (void *)Detour_VK_InitExt2_5,
    (void *)Detour_VK_InitExt2_6, (void *)Detour_VK_InitExt2_7, (void *)Detour_VK_InitExt2_8,
    (void *)Detour_VK_InitExt2_9, (void *)Detour_VK_InitExt2_10, (void *)Detour_VK_InitExt2_11,
};

static NVSDK_NGX_Result ForwardVkInitProject(
    int idx, const char *project_id, int engine_type, const char *engine_version,
    const wchar_t *data_path, VkInstance instance, VkPhysicalDevice phys,
    VkDevice device, int version, const void *feature_info)
{
    Hook &h = g_init_project_hooks[idx];
#if defined(DLSS5VK_HOOK_HOST)
    HostCaptureNgxInit(instance, phys, device);
#endif
    EnterCriticalSection(&g_hook_cs);
    LiftInitExtHooks();
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NGX_VK_Init_ProjectID>(h.target)(
        project_id, engine_type, engine_version, data_path,
        instance, phys, device, version, feature_info);
    if (result == NGX_SUCCESS)
        CaptureNgxProjectIdentity(project_id, engine_type, engine_version, version);
    UnliftInitExtHooks();
    LeaveCriticalSection(&g_hook_cs);
    return result;
}

#define INIT_PROJECT_DETOUR(N) \
    static NVSDK_NGX_Result Detour_VK_InitProject_##N( \
        const char *p, int e, const char *ev, const wchar_t *d, VkInstance i, \
        VkPhysicalDevice ph, VkDevice vkd, int v, const void *f) \
    { return ForwardVkInitProject(N, p, e, ev, d, i, ph, vkd, v, f); }
INIT_PROJECT_DETOUR(0) INIT_PROJECT_DETOUR(1) INIT_PROJECT_DETOUR(2) INIT_PROJECT_DETOUR(3)
INIT_PROJECT_DETOUR(4) INIT_PROJECT_DETOUR(5) INIT_PROJECT_DETOUR(6) INIT_PROJECT_DETOUR(7)
INIT_PROJECT_DETOUR(8) INIT_PROJECT_DETOUR(9) INIT_PROJECT_DETOUR(10) INIT_PROJECT_DETOUR(11)

static void *const g_init_project_detours[kMaxHooks] = {
    (void *)Detour_VK_InitProject_0, (void *)Detour_VK_InitProject_1,
    (void *)Detour_VK_InitProject_2, (void *)Detour_VK_InitProject_3,
    (void *)Detour_VK_InitProject_4, (void *)Detour_VK_InitProject_5,
    (void *)Detour_VK_InitProject_6, (void *)Detour_VK_InitProject_7,
    (void *)Detour_VK_InitProject_8, (void *)Detour_VK_InitProject_9,
    (void *)Detour_VK_InitProject_10, (void *)Detour_VK_InitProject_11,
};

// ---------------------------------------------------------------------------
// NGX D3D11 hooks: bg3_dx11.exe and other Direct3D 11 titles.
//
// Same 14-byte absolute-jump patching and lift-all-while-forwarding contract
// as Vulkan. The D3D11 evaluate is synchronous on the game's immediate
// context, so the bridged frame runs inline (see d3d11_interop.inc) rather
// than through the Vulkan submit-split worker. Hook tables are separate so a
// mixed-API process can bridge Vulkan and D3D11 viewports concurrently.
// ---------------------------------------------------------------------------
static void LiftD3d11EvalHooks()
{
    if (++g_lift_d3d11_eval_depth != 1) return;
    for (int i = 0; i < g_d3d11_eval_count; ++i) HookRestore(&g_d3d11_eval_hooks[i]);
}
static void UnliftD3d11EvalHooks()
{
    if (--g_lift_d3d11_eval_depth != 0) return;
    for (int i = 0; i < g_d3d11_eval_count; ++i) HookReinstall(&g_d3d11_eval_hooks[i]);
}
static void LiftD3d11CreateHooks()
{
    if (++g_lift_d3d11_create_depth != 1) return;
    for (int i = 0; i < g_d3d11_create_count; ++i) HookRestore(&g_d3d11_create_hooks[i]);
}
static void UnliftD3d11CreateHooks()
{
    if (--g_lift_d3d11_create_depth != 0) return;
    for (int i = 0; i < g_d3d11_create_count; ++i) HookReinstall(&g_d3d11_create_hooks[i]);
}

static NVSDK_NGX_Result SafeCallD3d11Evaluate(PFN_NGX_D3D11_Evaluate fn,
    ID3D11DeviceContext *ctx, const NVSDK_NGX_Handle *feat,
    const NVSDK_NGX_Parameter *p, FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(ctx, feat, p, nullptr); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

static NVSDK_NGX_Result ForwardD3d11EvaluateVia(int idx, ID3D11DeviceContext *ctx,
    const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *p)
{
    Hook &h = g_d3d11_eval_hooks[idx];
    if (reinterpret_cast<uintptr_t>(h.target) < 0x10000) return NGX_FAIL;
    EnterCriticalSection(&g_hook_cs);
    LiftD3d11EvalHooks();
    FaultInfo fi;
    NVSDK_NGX_Result r = SafeCallD3d11Evaluate(
        reinterpret_cast<PFN_NGX_D3D11_Evaluate>(h.target), ctx, feat, p, &fi);
    if (fi.code) AuditHooksAtFault();
    UnliftD3d11EvalHooks();
    LeaveCriticalSection(&g_hook_cs);
    if (fi.code)
    {
        Log("[hook] forwarded NGX D3D11 evaluate raised 0x%08X", fi.code);
        LogFaultSite(&fi);
        BridgeDisable("the game's NGX D3D11 evaluate faulted while forwarding");
    }
    return r;
}

static NVSDK_NGX_Result BridgedD3d11Evaluate(int idx, ID3D11DeviceContext *ctx,
    const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *p,
    PFN_NVSDK_NGX_ProgressCallback cb)
{
    (void)cb;
    ++g_ngx_nest;
    const bool outer = (g_ngx_nest == 1);
    if (outer && g_release_deferred)
    {
        InterlockedExchange(&g_release_deferred, 0);
        FrameLock();
        D3D12ReleaseResources();
        FrameUnlock();
    }
    if (IsFgHandle(feat))
    {
        --g_ngx_nest;
        return ForwardD3d11EvaluateVia(idx, ctx, feat, p);
    }
    if (outer && g_bridge.disabled) { --g_ngx_nest; return NGX_FAIL; }
    const int vp = feat ? ResolveViewport(feat) : -1;
    const bool skip_game = outer && vp >= 0 && BridgeD3D11SkipGameEvaluate(vp, p);
    NVSDK_NGX_Result r = NGX_SUCCESS;
    if (!skip_game)
        r = ForwardD3d11EvaluateVia(idx, ctx, feat, p);
    else
    {
        static bool said = false;
        if (!said) { said = true;
            Log("[policy] vp%d (d3d11) skipping game-side evaluate; private neural result authoritative", vp); }
    }
    if (outer && !g_bridge.disabled && r == NGX_SUCCESS)
    {
        FaultInfo fi;
        memset(&fi, 0, sizeof(fi));
        __try { BridgeD3D11Frame(ctx, feat, p); }
        __except (FaultCapture(&fi, GetExceptionInformation()))
        { BridgeFrameUnlockAll();
          Log("[bridge] d3d11 frame path faulted 0x%08X; disabling", fi.code);
          BridgeDisable("the d3d11 bridge frame path raised"); }
    }
    --g_ngx_nest;
    return r;
}

#define D3D11_EVAL_DETOUR(N) \
    static NVSDK_NGX_Result Detour_D3D11_Eval_##N(ID3D11DeviceContext *c, const NVSDK_NGX_Handle *f, \
        const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback cb) \
    { return BridgedD3d11Evaluate(N, c, f, p, cb); }
D3D11_EVAL_DETOUR(0) D3D11_EVAL_DETOUR(1) D3D11_EVAL_DETOUR(2)  D3D11_EVAL_DETOUR(3)
D3D11_EVAL_DETOUR(4) D3D11_EVAL_DETOUR(5) D3D11_EVAL_DETOUR(6)  D3D11_EVAL_DETOUR(7)
D3D11_EVAL_DETOUR(8) D3D11_EVAL_DETOUR(9) D3D11_EVAL_DETOUR(10) D3D11_EVAL_DETOUR(11)

static void *const g_d3d11_eval_detours[kMaxHooks] = {
    (void *)Detour_D3D11_Eval_0, (void *)Detour_D3D11_Eval_1, (void *)Detour_D3D11_Eval_2,  (void *)Detour_D3D11_Eval_3,
    (void *)Detour_D3D11_Eval_4, (void *)Detour_D3D11_Eval_5, (void *)Detour_D3D11_Eval_6,  (void *)Detour_D3D11_Eval_7,
    (void *)Detour_D3D11_Eval_8, (void *)Detour_D3D11_Eval_9, (void *)Detour_D3D11_Eval_10, (void *)Detour_D3D11_Eval_11,
};

static NVSDK_NGX_Result SafeCallD3d11Create(PFN_NGX_D3D11_Create fn,
    ID3D11DeviceContext *ctx, int feature, NVSDK_NGX_Parameter *p,
    NVSDK_NGX_Handle **out, FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(ctx, feature, p, out); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

static NVSDK_NGX_Result ForwardD3d11CreateVia(Hook *h, ID3D11DeviceContext *ctx,
    int feature, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    if (reinterpret_cast<uintptr_t>(h->target) < 0x10000) return NGX_FAIL;
    EnterCriticalSection(&g_hook_cs);
    LiftD3d11EvalHooks();
    LiftD3d11CreateHooks();
    FaultInfo fi;
    NVSDK_NGX_Result r = SafeCallD3d11Create(
        reinterpret_cast<PFN_NGX_D3D11_Create>(h->target), ctx, feature, p, out, &fi);
    if (fi.code) AuditHooksAtFault();
    UnliftD3d11CreateHooks();
    UnliftD3d11EvalHooks();
    LeaveCriticalSection(&g_hook_cs);
    if (fi.code) BridgeDisable("the game's NGX D3D11 feature creation faulted");
    return r;
}

static NVSDK_NGX_Result BridgedD3d11Create(int idx, ID3D11DeviceContext *ctx, int feature,
    NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    // Ensure the runtime composition covers D3D11 before the first create.
    if (!g_components.capture || strcmp(g_components.capture->id, "d3d11") != 0)
    {
        dlss_bridge::RuntimeComposition dual{};
        if (dual.Configure(D3D11HostId()) && dual.SelectTransport(false))
        {
            g_components = dual;
            Log("[components] D3D11 capture activated (host injected-d3d11)");
        }
    }
    NVSDK_NGX_Result result = ForwardD3d11CreateVia(
        &g_d3d11_create_hooks[idx], ctx, feature, p, out);
    if (result == NGX_SUCCESS && out)
    {
        NoteGameCreateObserved();
        if (feature == NVSDK_NGX_FEATURE_FRAME_GENERATION) NoteFgCreate(*out);
        else CaptureFeatureContract(feature, p, *out);
    }
    return result;
}

#define D3D11_CREATE_DETOUR(N) \
    static NVSDK_NGX_Result Detour_D3D11_Create_##N(ID3D11DeviceContext *c, int ft, \
        NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o) \
    { return BridgedD3d11Create(N, c, ft, p, o); }
D3D11_CREATE_DETOUR(0) D3D11_CREATE_DETOUR(1) D3D11_CREATE_DETOUR(2)  D3D11_CREATE_DETOUR(3)
D3D11_CREATE_DETOUR(4) D3D11_CREATE_DETOUR(5) D3D11_CREATE_DETOUR(6)  D3D11_CREATE_DETOUR(7)
D3D11_CREATE_DETOUR(8) D3D11_CREATE_DETOUR(9) D3D11_CREATE_DETOUR(10) D3D11_CREATE_DETOUR(11)

static void *const g_d3d11_create_detours[kMaxHooks] = {
    (void *)Detour_D3D11_Create_0, (void *)Detour_D3D11_Create_1, (void *)Detour_D3D11_Create_2,  (void *)Detour_D3D11_Create_3,
    (void *)Detour_D3D11_Create_4, (void *)Detour_D3D11_Create_5, (void *)Detour_D3D11_Create_6,  (void *)Detour_D3D11_Create_7,
    (void *)Detour_D3D11_Create_8, (void *)Detour_D3D11_Create_9, (void *)Detour_D3D11_Create_10, (void *)Detour_D3D11_Create_11,
};

// ---------------------------------------------------------------------------
// NGX D3D12 hooks: DX12 titles with native Super Resolution.
//
// Same 14-byte absolute-jump patching and lift-all-while-forwarding contract
// as Vulkan/D3D11. Two differences are D3D12-specific: (1) the session-local
// OptiScaler module is never hooked (see TryHookModule), so the private
// neural session's own NGX calls travel through unhooked code; (2) a
// thread-local in-bridge guard makes any re-entered detour -- e.g. a game
// interposer forwarding through the private module -- forward without
// bridging instead of recursing. The D3D12 evaluate runs synchronously on the
// game thread (see d3d12_interop.inc), so no worker is involved.
// ---------------------------------------------------------------------------
static void LiftD3d12EvalHooks()
{
    if (++g_lift_d3d12_eval_depth != 1) return;
    for (int i = 0; i < g_d3d12_eval_count; ++i) HookRestore(&g_d3d12_eval_hooks[i]);
}
static void UnliftD3d12EvalHooks()
{
    if (--g_lift_d3d12_eval_depth != 0) return;
    for (int i = 0; i < g_d3d12_eval_count; ++i) HookReinstall(&g_d3d12_eval_hooks[i]);
}
static void LiftD3d12CreateHooks()
{
    if (++g_lift_d3d12_create_depth != 1) return;
    for (int i = 0; i < g_d3d12_create_count; ++i) HookRestore(&g_d3d12_create_hooks[i]);
}
static void UnliftD3d12CreateHooks()
{
    if (--g_lift_d3d12_create_depth != 0) return;
    for (int i = 0; i < g_d3d12_create_count; ++i) HookReinstall(&g_d3d12_create_hooks[i]);
}
static void LiftD3d12InitHooks()
{
    if (++g_lift_d3d12_init_depth != 1) return;
    for (int i = 0; i < g_d3d12_init_ext_count; ++i) HookRestore(&g_d3d12_init_ext_hooks[i]);
    for (int i = 0; i < g_d3d12_init_project_count; ++i) HookRestore(&g_d3d12_init_project_hooks[i]);
}
static void UnliftD3d12InitHooks()
{
    if (--g_lift_d3d12_init_depth != 0) return;
    for (int i = 0; i < g_d3d12_init_ext_count; ++i) HookReinstall(&g_d3d12_init_ext_hooks[i]);
    for (int i = 0; i < g_d3d12_init_project_count; ++i) HookReinstall(&g_d3d12_init_project_hooks[i]);
}

static NVSDK_NGX_Result SafeCallD3d12Evaluate(PFN_D3D12EvaluateFeature fn,
    ID3D12GraphicsCommandList *list, const NVSDK_NGX_Handle *feat,
    const NVSDK_NGX_Parameter *p, FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(list, feat, p, nullptr); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

static NVSDK_NGX_Result ForwardD3d12EvaluateVia(int idx, ID3D12GraphicsCommandList *list,
    const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *p)
{
    Hook &h = g_d3d12_eval_hooks[idx];
    if (reinterpret_cast<uintptr_t>(h.target) < 0x10000) return NGX_FAIL;
    EnterCriticalSection(&g_hook_cs);
    LiftD3d12EvalHooks();
    FaultInfo fi;
    NVSDK_NGX_Result r = SafeCallD3d12Evaluate(
        reinterpret_cast<PFN_D3D12EvaluateFeature>(h.target), list, feat, p, &fi);
    if (fi.code) AuditHooksAtFault();
    UnliftD3d12EvalHooks();
    LeaveCriticalSection(&g_hook_cs);
    if (fi.code)
    {
        Log("[hook] forwarded NGX D3D12 evaluate raised 0x%08X", fi.code);
        LogFaultSite(&fi);
        BridgeDisable("the game's NGX D3D12 evaluate faulted while forwarding");
    }
    return r;
}

static NVSDK_NGX_Result BridgedD3d12Evaluate(int idx, ID3D12GraphicsCommandList *list,
    const NVSDK_NGX_Handle *feat, const NVSDK_NGX_Parameter *p,
    PFN_NVSDK_NGX_ProgressCallback cb)
{
    (void)cb;
    // Re-entered from the private neural session (same API names on both
    // sides): forward without bridging instead of recursing.
    if (g_d3d12_in_bridge) return ForwardD3d12EvaluateVia(idx, list, feat, p);
    ++g_ngx_nest;
    const bool outer = (g_ngx_nest == 1);
    if (outer && g_release_deferred)
    {
        InterlockedExchange(&g_release_deferred, 0);
        FrameLock();
        D3D12ReleaseResources();
        FrameUnlock();
    }
    if (IsFgHandle(feat))
    {
        --g_ngx_nest;
        return ForwardD3d12EvaluateVia(idx, list, feat, p);
    }
    if (outer && g_bridge.disabled) { --g_ngx_nest; return NGX_FAIL; }
    const int vp = feat ? ResolveViewport(feat) : -1;
    const bool skip_game = outer && vp >= 0 && BridgeD3D12SkipGameEvaluate(vp, p);
    NVSDK_NGX_Result r = NGX_FAIL;
    if (!skip_game)
    {
        r = ForwardD3d12EvaluateVia(idx, list, feat, p);
        if (outer && !g_bridge.disabled && r == NGX_SUCCESS)
        {
            FaultInfo fi;
            memset(&fi, 0, sizeof(fi));
            __try { BridgeD3D12Frame(list, feat, p); }
            __except (FaultCapture(&fi, GetExceptionInformation()))
            { BridgeFrameUnlockAll();
              Log("[bridge] d3d12 frame path faulted 0x%08X; disabling", fi.code);
              BridgeDisable("the d3d12 bridge frame path raised"); }
        }
    }
    else
    {
        // Latched: the private neural result is authoritative, but only if it
        // actually writes the output. Run it first; on any failure the
        // game's own evaluate still has to run, or downstream rendering would
        // consume an unwritten/stale Output.
        static bool said = false;
        if (!said) { said = true;
            Log("[policy] vp%d (d3d12) skipping game-side evaluate; private neural result authoritative", vp); }
        bool wrote = false;
        FaultInfo fi;
        memset(&fi, 0, sizeof(fi));
        __try { wrote = BridgeD3D12Frame(list, feat, p); }
        __except (FaultCapture(&fi, GetExceptionInformation()))
        { BridgeFrameUnlockAll();
          Log("[bridge] d3d12 frame path faulted 0x%08X; disabling", fi.code);
          BridgeDisable("the d3d12 bridge frame path raised"); }
        if (wrote)
        {
            r = NGX_SUCCESS;
        }
        else
        {
            static bool said_fb = false;
            if (!said_fb) { said_fb = true;
                Log("[d3d12] vp%d private evaluate produced no output; "
                    "falling back to the game-side evaluate", vp); }
            r = ForwardD3d12EvaluateVia(idx, list, feat, p);
        }
    }
    --g_ngx_nest;
    return r;
}

#define D3D12_EVAL_DETOUR(N) \
    static NVSDK_NGX_Result Detour_D3D12_Eval_##N(ID3D12GraphicsCommandList *l, const NVSDK_NGX_Handle *f, \
        const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback cb) \
    { return BridgedD3d12Evaluate(N, l, f, p, cb); }
D3D12_EVAL_DETOUR(0) D3D12_EVAL_DETOUR(1) D3D12_EVAL_DETOUR(2)  D3D12_EVAL_DETOUR(3)
D3D12_EVAL_DETOUR(4) D3D12_EVAL_DETOUR(5) D3D12_EVAL_DETOUR(6)  D3D12_EVAL_DETOUR(7)
D3D12_EVAL_DETOUR(8) D3D12_EVAL_DETOUR(9) D3D12_EVAL_DETOUR(10) D3D12_EVAL_DETOUR(11)

static void *const g_d3d12_eval_detours[kMaxHooks] = {
    (void *)Detour_D3D12_Eval_0, (void *)Detour_D3D12_Eval_1, (void *)Detour_D3D12_Eval_2,  (void *)Detour_D3D12_Eval_3,
    (void *)Detour_D3D12_Eval_4, (void *)Detour_D3D12_Eval_5, (void *)Detour_D3D12_Eval_6,  (void *)Detour_D3D12_Eval_7,
    (void *)Detour_D3D12_Eval_8, (void *)Detour_D3D12_Eval_9, (void *)Detour_D3D12_Eval_10, (void *)Detour_D3D12_Eval_11,
};

static NVSDK_NGX_Result SafeCallD3d12Create(PFN_D3D12CreateFeature fn,
    ID3D12GraphicsCommandList *list, int feature, NVSDK_NGX_Parameter *p,
    NVSDK_NGX_Handle **out, FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(list, feature, p, out); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

static NVSDK_NGX_Result ForwardD3d12CreateVia(Hook *h, ID3D12GraphicsCommandList *list,
    int feature, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    if (reinterpret_cast<uintptr_t>(h->target) < 0x10000) return NGX_FAIL;
    EnterCriticalSection(&g_hook_cs);
    LiftD3d12EvalHooks();
    LiftD3d12CreateHooks();
    FaultInfo fi;
    NVSDK_NGX_Result r = SafeCallD3d12Create(
        reinterpret_cast<PFN_D3D12CreateFeature>(h->target), list, feature, p, out, &fi);
    if (fi.code) AuditHooksAtFault();
    UnliftD3d12CreateHooks();
    UnliftD3d12EvalHooks();
    LeaveCriticalSection(&g_hook_cs);
    if (fi.code) BridgeDisable("the game's NGX D3D12 feature creation faulted");
    return r;
}

static NVSDK_NGX_Result BridgedD3d12Create(int idx, ID3D12GraphicsCommandList *list, int feature,
    NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    // Re-entered from the private neural session (same API names on both
    // sides): forward raw -- no component switch, no contract capture, no FG
    // note -- or our own creation parameters would arm a game viewport.
    if (g_d3d12_in_bridge) return ForwardD3d12CreateVia(&g_d3d12_create_hooks[idx], list, feature, p, out);
    // Ensure the runtime composition covers D3D12 before the first create.
    if (!g_components.capture || strcmp(g_components.capture->id, "ngx-d3d12") != 0)
    {
        dlss_bridge::RuntimeComposition dual{};
        if (dual.Configure(D3D12HostId()) && dual.SelectTransport(false))
        {
            g_components = dual;
            Log("[components] D3D12 capture activated (host injected-d3d12)");
        }
    }
    NVSDK_NGX_Result result = ForwardD3d12CreateVia(
        &g_d3d12_create_hooks[idx], list, feature, p, out);
    if (result == NGX_SUCCESS && out)
    {
        NoteGameCreateObserved();
        if (feature == NVSDK_NGX_FEATURE_FRAME_GENERATION) NoteFgCreate(*out);
        else CaptureFeatureContract(feature, p, *out);
    }
    return result;
}

#define D3D12_CREATE_DETOUR(N) \
    static NVSDK_NGX_Result Detour_D3D12_Create_##N(ID3D12GraphicsCommandList *l, int ft, \
        NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o) \
    { return BridgedD3d12Create(N, l, ft, p, o); }
D3D12_CREATE_DETOUR(0) D3D12_CREATE_DETOUR(1) D3D12_CREATE_DETOUR(2)  D3D12_CREATE_DETOUR(3)
D3D12_CREATE_DETOUR(4) D3D12_CREATE_DETOUR(5) D3D12_CREATE_DETOUR(6)  D3D12_CREATE_DETOUR(7)
D3D12_CREATE_DETOUR(8) D3D12_CREATE_DETOUR(9) D3D12_CREATE_DETOUR(10) D3D12_CREATE_DETOUR(11)

static void *const g_d3d12_create_detours[kMaxHooks] = {
    (void *)Detour_D3D12_Create_0, (void *)Detour_D3D12_Create_1, (void *)Detour_D3D12_Create_2,  (void *)Detour_D3D12_Create_3,
    (void *)Detour_D3D12_Create_4, (void *)Detour_D3D12_Create_5, (void *)Detour_D3D12_Create_6,  (void *)Detour_D3D12_Create_7,
    (void *)Detour_D3D12_Create_8, (void *)Detour_D3D12_Create_9, (void *)Detour_D3D12_Create_10, (void *)Detour_D3D12_Create_11,
};

// Game-side NGX initialization: forward, then capture the application/project
// identity the private D3D12 session reuses at bring-up. No device adoption --
// the evaluate entry point already hands the bridge the game command list.
static NVSDK_NGX_Result ForwardD3d12InitExt(int idx, unsigned long long app_id,
    const wchar_t *data_path, ID3D12Device *device, int version, const void *feature_info)
{
    Hook &h = g_d3d12_init_ext_hooks[idx];
    (void)device;
    EnterCriticalSection(&g_hook_cs);
    LiftD3d12InitHooks();
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NGX_D3D12_Init_Ext>(h.target)(
        app_id, data_path, device, version, feature_info);
    if (result == NGX_SUCCESS) CaptureNgxApplicationIdentity(app_id, version);
    UnliftD3d12InitHooks();
    LeaveCriticalSection(&g_hook_cs);
    return result;
}

#define D3D12_INIT_EXT_DETOUR(N) \
    static NVSDK_NGX_Result Detour_D3D12_InitExt_##N(unsigned long long a, const wchar_t *p, \
        ID3D12Device *d, int v, const void *f) \
    { return ForwardD3d12InitExt(N, a, p, d, v, f); }
D3D12_INIT_EXT_DETOUR(0) D3D12_INIT_EXT_DETOUR(1) D3D12_INIT_EXT_DETOUR(2)  D3D12_INIT_EXT_DETOUR(3)
D3D12_INIT_EXT_DETOUR(4) D3D12_INIT_EXT_DETOUR(5) D3D12_INIT_EXT_DETOUR(6)  D3D12_INIT_EXT_DETOUR(7)
D3D12_INIT_EXT_DETOUR(8) D3D12_INIT_EXT_DETOUR(9) D3D12_INIT_EXT_DETOUR(10) D3D12_INIT_EXT_DETOUR(11)

static void *const g_d3d12_init_ext_detours[kMaxHooks] = {
    (void *)Detour_D3D12_InitExt_0, (void *)Detour_D3D12_InitExt_1, (void *)Detour_D3D12_InitExt_2,
    (void *)Detour_D3D12_InitExt_3, (void *)Detour_D3D12_InitExt_4, (void *)Detour_D3D12_InitExt_5,
    (void *)Detour_D3D12_InitExt_6, (void *)Detour_D3D12_InitExt_7, (void *)Detour_D3D12_InitExt_8,
    (void *)Detour_D3D12_InitExt_9, (void *)Detour_D3D12_InitExt_10, (void *)Detour_D3D12_InitExt_11,
};

static NVSDK_NGX_Result ForwardD3d12InitProject(int idx, const char *project_id,
    int engine_type, const char *engine_version, const wchar_t *data_path,
    ID3D12Device *device, int version, const void *feature_info)
{
    Hook &h = g_d3d12_init_project_hooks[idx];
    (void)device;
    EnterCriticalSection(&g_hook_cs);
    LiftD3d12InitHooks();
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NGX_D3D12_Init_ProjectID>(h.target)(
        project_id, engine_type, engine_version, data_path, device, version, feature_info);
    if (result == NGX_SUCCESS)
        CaptureNgxProjectIdentity(project_id, engine_type, engine_version, version);
    UnliftD3d12InitHooks();
    LeaveCriticalSection(&g_hook_cs);
    return result;
}

#define D3D12_INIT_PROJECT_DETOUR(N) \
    static NVSDK_NGX_Result Detour_D3D12_InitProject_##N( \
        const char *p, int e, const char *ev, const wchar_t *d, ID3D12Device *dev, \
        int v, const void *f) \
    { return ForwardD3d12InitProject(N, p, e, ev, d, dev, v, f); }
D3D12_INIT_PROJECT_DETOUR(0) D3D12_INIT_PROJECT_DETOUR(1) D3D12_INIT_PROJECT_DETOUR(2) D3D12_INIT_PROJECT_DETOUR(3)
D3D12_INIT_PROJECT_DETOUR(4) D3D12_INIT_PROJECT_DETOUR(5) D3D12_INIT_PROJECT_DETOUR(6) D3D12_INIT_PROJECT_DETOUR(7)
D3D12_INIT_PROJECT_DETOUR(8) D3D12_INIT_PROJECT_DETOUR(9) D3D12_INIT_PROJECT_DETOUR(10) D3D12_INIT_PROJECT_DETOUR(11)

static void *const g_d3d12_init_project_detours[kMaxHooks] = {
    (void *)Detour_D3D12_InitProject_0, (void *)Detour_D3D12_InitProject_1,
    (void *)Detour_D3D12_InitProject_2, (void *)Detour_D3D12_InitProject_3,
    (void *)Detour_D3D12_InitProject_4, (void *)Detour_D3D12_InitProject_5,
    (void *)Detour_D3D12_InitProject_6, (void *)Detour_D3D12_InitProject_7,
    (void *)Detour_D3D12_InitProject_8, (void *)Detour_D3D12_InitProject_9,
    (void *)Detour_D3D12_InitProject_10, (void *)Detour_D3D12_InitProject_11,
};

// ---------------------------------------------------------------------------
// ReleaseFeature detours: forward, then drop the handle's FG classification
// and invalidate the viewport that was bridged under it (a recreated feature
// at the same address rebuilds from the fresh creation contract; the stale
// private feature's history and any neural-only latch do not survive). All
// three APIs share one signature, one lift set, and one classification table.
// ---------------------------------------------------------------------------
typedef NVSDK_NGX_Result (*PFN_NGXReleaseFeature)(NVSDK_NGX_Handle *);

static NVSDK_NGX_Result SafeCallRelease(PFN_NGXReleaseFeature fn, NVSDK_NGX_Handle *h,
    FaultInfo *fi)
{
    memset(fi, 0, sizeof(*fi));
    __try { return fn(h); }
    __except (FaultCapture(fi, GetExceptionInformation()))
    {
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_FAIL;
    }
}

static void LiftReleaseHooks()
{
    if (++g_lift_release_depth != 1) return;
    for (int i = 0; i < g_vk_release_count; ++i) HookRestore(&g_vk_release_hooks[i]);
    for (int i = 0; i < g_d3d11_release_count; ++i) HookRestore(&g_d3d11_release_hooks[i]);
    for (int i = 0; i < g_d3d12_release_count; ++i) HookRestore(&g_d3d12_release_hooks[i]);
}
static void UnliftReleaseHooks()
{
    if (--g_lift_release_depth != 0) return;
    for (int i = 0; i < g_vk_release_count; ++i) HookReinstall(&g_vk_release_hooks[i]);
    for (int i = 0; i < g_d3d11_release_count; ++i) HookReinstall(&g_d3d11_release_hooks[i]);
    for (int i = 0; i < g_d3d12_release_count; ++i) HookReinstall(&g_d3d12_release_hooks[i]);
}

// A released game handle must not keep its bridge state: a game that
// recreates its feature at the same address would otherwise be evaluated
// against the previous feature's private history, and a still-set neural-only
// latch would suppress the new game feature. Drop the latch and force a clean
// rebuild on the next frame -- the rebuild then releases and recreates the
// private feature behind its normal queue-completion waits (D3D12DrainGpu for
// the bridge-queue paths, the game-queue fence for the D3D12 path), so the
// stale feature's temporal state never survives the recreate. Shared textures
// stay: a rebuild recreates them only when the geometry actually changed.
static void InvalidateBridgedFeature(const NVSDK_NGX_Handle *h)
{
    if (!h) return;
    int vp = -1;
    for (int i = 0; i < MAX_VIEWPORTS; ++i)
        if (g_bridge.viewports[i].game_handle == h) { vp = i; break; }
    if (vp < 0) return;
    FrameLock();
    BridgeViewport &v = g_bridge.viewports[vp];
    if (InterlockedExchange(&v.neural_latched, 0) != 0)
        Log("[policy] vp%d game feature released; neural-only relatches on the recreated feature", vp);
    v.frame_ready = false;
    v.rr_active = false;
    for (int s = 0; s < RR_SLOT_COUNT; ++s) v.rr_key[s] = nullptr;
    FrameUnlock();
}

static NVSDK_NGX_Result BridgedReleaseCommon(Hook *hooks, int count, int idx,
    NVSDK_NGX_Handle *h, const char *what)
{
    NVSDK_NGX_Result r = NGX_FAIL;
    if (idx >= 0 && idx < count && reinterpret_cast<uintptr_t>(hooks[idx].target) >= 0x10000)
    {
        EnterCriticalSection(&g_hook_cs);
        LiftReleaseHooks();
        FaultInfo fi;
        r = SafeCallRelease(reinterpret_cast<PFN_NGXReleaseFeature>(hooks[idx].target), h, &fi);
        if (fi.code) AuditHooksAtFault();
        UnliftReleaseHooks();
        LeaveCriticalSection(&g_hook_cs);
        if (fi.code)
        {
            Log("[hook] forwarded NGX %s release raised 0x%08X", what, fi.code);
            LogFaultSite(&fi);
        }
    }
    ClearFgHandle(h);
    InvalidateBridgedFeature(h);
    return r;
}

static NVSDK_NGX_Result BridgedVkRelease(int idx, NVSDK_NGX_Handle *h)
{ return BridgedReleaseCommon(g_vk_release_hooks, g_vk_release_count, idx, h, "VULKAN"); }
static NVSDK_NGX_Result BridgedD3d11Release(int idx, NVSDK_NGX_Handle *h)
{ return BridgedReleaseCommon(g_d3d11_release_hooks, g_d3d11_release_count, idx, h, "D3D11"); }
static NVSDK_NGX_Result BridgedD3d12Release(int idx, NVSDK_NGX_Handle *h)
{ return BridgedReleaseCommon(g_d3d12_release_hooks, g_d3d12_release_count, idx, h, "D3D12"); }

#define RELEASE_DETOUR(TAG, FN, N) \
    static NVSDK_NGX_Result Detour_##TAG##_Release_##N(NVSDK_NGX_Handle *h) { return FN(N, h); }
RELEASE_DETOUR(Vk, BridgedVkRelease, 0) RELEASE_DETOUR(Vk, BridgedVkRelease, 1)
RELEASE_DETOUR(Vk, BridgedVkRelease, 2) RELEASE_DETOUR(Vk, BridgedVkRelease, 3)
RELEASE_DETOUR(Vk, BridgedVkRelease, 4) RELEASE_DETOUR(Vk, BridgedVkRelease, 5)
RELEASE_DETOUR(Vk, BridgedVkRelease, 6) RELEASE_DETOUR(Vk, BridgedVkRelease, 7)
RELEASE_DETOUR(Vk, BridgedVkRelease, 8) RELEASE_DETOUR(Vk, BridgedVkRelease, 9)
RELEASE_DETOUR(Vk, BridgedVkRelease, 10) RELEASE_DETOUR(Vk, BridgedVkRelease, 11)
RELEASE_DETOUR(D3D11, BridgedD3d11Release, 0) RELEASE_DETOUR(D3D11, BridgedD3d11Release, 1)
RELEASE_DETOUR(D3D11, BridgedD3d11Release, 2) RELEASE_DETOUR(D3D11, BridgedD3d11Release, 3)
RELEASE_DETOUR(D3D11, BridgedD3d11Release, 4) RELEASE_DETOUR(D3D11, BridgedD3d11Release, 5)
RELEASE_DETOUR(D3D11, BridgedD3d11Release, 6) RELEASE_DETOUR(D3D11, BridgedD3d11Release, 7)
RELEASE_DETOUR(D3D11, BridgedD3d11Release, 8) RELEASE_DETOUR(D3D11, BridgedD3d11Release, 9)
RELEASE_DETOUR(D3D11, BridgedD3d11Release, 10) RELEASE_DETOUR(D3D11, BridgedD3d11Release, 11)
RELEASE_DETOUR(D3D12, BridgedD3d12Release, 0) RELEASE_DETOUR(D3D12, BridgedD3d12Release, 1)
RELEASE_DETOUR(D3D12, BridgedD3d12Release, 2) RELEASE_DETOUR(D3D12, BridgedD3d12Release, 3)
RELEASE_DETOUR(D3D12, BridgedD3d12Release, 4) RELEASE_DETOUR(D3D12, BridgedD3d12Release, 5)
RELEASE_DETOUR(D3D12, BridgedD3d12Release, 6) RELEASE_DETOUR(D3D12, BridgedD3d12Release, 7)
RELEASE_DETOUR(D3D12, BridgedD3d12Release, 8) RELEASE_DETOUR(D3D12, BridgedD3d12Release, 9)
RELEASE_DETOUR(D3D12, BridgedD3d12Release, 10) RELEASE_DETOUR(D3D12, BridgedD3d12Release, 11)

static void *const g_vk_release_detours[kMaxHooks] = {
    (void *)Detour_Vk_Release_0, (void *)Detour_Vk_Release_1, (void *)Detour_Vk_Release_2,
    (void *)Detour_Vk_Release_3, (void *)Detour_Vk_Release_4, (void *)Detour_Vk_Release_5,
    (void *)Detour_Vk_Release_6, (void *)Detour_Vk_Release_7, (void *)Detour_Vk_Release_8,
    (void *)Detour_Vk_Release_9, (void *)Detour_Vk_Release_10, (void *)Detour_Vk_Release_11,
};
static void *const g_d3d11_release_detours[kMaxHooks] = {
    (void *)Detour_D3D11_Release_0, (void *)Detour_D3D11_Release_1, (void *)Detour_D3D11_Release_2,
    (void *)Detour_D3D11_Release_3, (void *)Detour_D3D11_Release_4, (void *)Detour_D3D11_Release_5,
    (void *)Detour_D3D11_Release_6, (void *)Detour_D3D11_Release_7, (void *)Detour_D3D11_Release_8,
    (void *)Detour_D3D11_Release_9, (void *)Detour_D3D11_Release_10, (void *)Detour_D3D11_Release_11,
};
static void *const g_d3d12_release_detours[kMaxHooks] = {
    (void *)Detour_D3D12_Release_0, (void *)Detour_D3D12_Release_1, (void *)Detour_D3D12_Release_2,
    (void *)Detour_D3D12_Release_3, (void *)Detour_D3D12_Release_4, (void *)Detour_D3D12_Release_5,
    (void *)Detour_D3D12_Release_6, (void *)Detour_D3D12_Release_7, (void *)Detour_D3D12_Release_8,
    (void *)Detour_D3D12_Release_9, (void *)Detour_D3D12_Release_10, (void *)Detour_D3D12_Release_11,
};

// ---------------------------------------------------------------------------
// module discovery: hook every real NGX module that exports the Vulkan evaluate
// ---------------------------------------------------------------------------
static const wchar_t *ModuleName(HMODULE m)
{
    static wchar_t buf[MAX_PATH];
    GetModuleFileNameW(m, buf, MAX_PATH);
    const wchar_t *s = wcsrchr(buf, L'\\');
    return s ? s + 1 : buf;
}

// True when an entry point already starts with one of THIS DLL's own jumps: a
// previous load of the bridge patched it and was unloaded without a chance to
// unpatch (the reload lands at the same base, so the old jump targets the new
// detours). Saving those 14 bytes as "original" makes every forwarded call
// jump back into the detour forever. Explicit initialization pins the module,
// making this state unreachable; if some day it is reached anyway, the original bytes are
// simply gone and the only honest move is to leave the export alone.
static bool EntryIsOwnDetourJump(const void *entry)
{
    const unsigned char *b = static_cast<const unsigned char *>(entry);
    if (!(b[0] == 0xFF && b[1] == 0x25 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0))
        return false;                                            // not a jmp qword ptr [rip+0]
    void *dst = nullptr; memcpy(&dst, b + 6, 8);
    for (int i = 0; i < kMaxHooks; ++i)
        if (dst == g_eval_detours[i] || dst == g_create_detours[i] || dst == g_create1_detours[i])
            return true;
    for (int i = 0; i < kMaxHooks; ++i)
        if (dst == g_init_ext_detours[i] || dst == g_init_ext2_detours[i] ||
            dst == g_init_project_detours[i]) return true;
    for (int i = 0; i < kMaxHooks; ++i)
        if (dst == g_d3d11_eval_detours[i] || dst == g_d3d11_create_detours[i])
            return true;
    for (int i = 0; i < kMaxHooks; ++i)
        if (dst == g_d3d12_eval_detours[i] || dst == g_d3d12_create_detours[i] ||
            dst == g_d3d12_init_ext_detours[i] || dst == g_d3d12_init_project_detours[i])
            return true;
    for (int i = 0; i < kMaxHooks; ++i)
        if (dst == g_vk_release_detours[i] || dst == g_d3d11_release_detours[i] ||
            dst == g_d3d12_release_detours[i])
            return true;
    return false;
}

static void InstallOne(Hook *hooks, int *count, void *const detours[], void *target,
                       const char *what, HMODULE m)
{
    for (int i = 0; i < *count; ++i) if (hooks[i].target == target) return;   // already hooked
    if (EntryIsOwnDetourJump(target))
    {
        Warn("[hook] %ls: NVSDK_NGX_%s entry already carries this bridge's own jump "
             "from an unloaded previous load; its original bytes are unrecoverable -- not "
             "hooking it.", ModuleName(m), what);
        return;                                                  // never install over it
    }
    if (*count >= kMaxHooks) return;
    Hook &h  = hooks[*count];
    h.target = target;
    h.detour = detours[*count];
    HookInstall(&h);
    Log("[hook] patched NVSDK_NGX_%s @ %p in %ls", what, target, ModuleName(m));
    ++*count;
}

static void TryHookModule(HMODULE m)
{
    if (g_bridge.disabled) return;                               // never re-patch after giving up
    if (m == g_self || m == GetModuleHandleW(nullptr)) return;   // never the host exe (integrity checks)
    // Never patch the session-local OptiScaler module: the private neural
    // session's own D3D12 NGX calls travel through it, and patching them
    // would route the bridge into itself. Vulkan/D3D11 exports never resolve
    // there, so this exclusion only ever bites for D3D12.
    if (g_optiscaler_module && m == g_optiscaler_module) return;

    auto eval   = GetProcAddress(m, "NVSDK_NGX_VULKAN_EvaluateFeature");
    auto create = GetProcAddress(m, "NVSDK_NGX_VULKAN_CreateFeature");
    auto create1 = GetProcAddress(m, "NVSDK_NGX_VULKAN_CreateFeature1");   // optional export
    auto init_ext = GetProcAddress(m, "NVSDK_NGX_VULKAN_Init_Ext");
    auto init_ext2 = GetProcAddress(m, "NVSDK_NGX_VULKAN_Init_Ext2");
    auto init_project = GetProcAddress(m, "NVSDK_NGX_VULKAN_Init_ProjectID");
    auto d3d11_eval = GetProcAddress(m, "NVSDK_NGX_D3D11_EvaluateFeature");
    auto d3d11_create = GetProcAddress(m, "NVSDK_NGX_D3D11_CreateFeature");
    auto d3d12_eval = GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature");
    auto d3d12_create = GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature");
    auto d3d12_init_ext = GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext");
    auto d3d12_init_project = GetProcAddress(m, "NVSDK_NGX_D3D12_Init_ProjectID");
    // ReleaseFeature exports are optional (absent on older NGX builds); each
    // API is hooked independently when present.
    auto vk_release = GetProcAddress(m, "NVSDK_NGX_VULKAN_ReleaseFeature");
    auto d3d11_release = GetProcAddress(m, "NVSDK_NGX_D3D11_ReleaseFeature");
    auto d3d12_release = GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature");
    if ((eval == nullptr || create == nullptr) && init_ext == nullptr &&
        init_ext2 == nullptr && init_project == nullptr &&
        d3d11_eval == nullptr && d3d11_create == nullptr &&
        d3d12_eval == nullptr && d3d12_create == nullptr &&
        d3d12_init_ext == nullptr && d3d12_init_project == nullptr) return;

    EnterCriticalSection(&g_hook_cs);
    if (eval && create)
    {
        InstallOne(g_eval_hooks,   &g_eval_count,   g_eval_detours,   (void *)eval,   "VULKAN_EvaluateFeature", m);
        InstallOne(g_create_hooks, &g_create_count, g_create_detours, (void *)create, "VULKAN_CreateFeature",   m);
    }
    if (create1)
        InstallOne(g_create1_hooks, &g_create1_count, g_create1_detours, (void *)create1, "VULKAN_CreateFeature1", m);
    if (init_ext)
        InstallOne(g_init_ext_hooks, &g_init_ext_count, g_init_ext_detours, (void *)init_ext, "VULKAN_Init_Ext", m);
    if (init_ext2)
        InstallOne(g_init_ext2_hooks, &g_init_ext2_count, g_init_ext2_detours, (void *)init_ext2, "VULKAN_Init_Ext2", m);
    if (init_project)
        InstallOne(g_init_project_hooks, &g_init_project_count, g_init_project_detours,
                   (void *)init_project, "VULKAN_Init_ProjectID", m);
    if (d3d11_eval)
        InstallOne(g_d3d11_eval_hooks, &g_d3d11_eval_count, g_d3d11_eval_detours,
                   (void *)d3d11_eval, "D3D11_EvaluateFeature", m);
    if (d3d11_create)
        InstallOne(g_d3d11_create_hooks, &g_d3d11_create_count, g_d3d11_create_detours,
                   (void *)d3d11_create, "D3D11_CreateFeature", m);
    if (d3d12_eval && d3d12_create)
    {
        InstallOne(g_d3d12_eval_hooks, &g_d3d12_eval_count, g_d3d12_eval_detours,
                   (void *)d3d12_eval, "D3D12_EvaluateFeature", m);
        InstallOne(g_d3d12_create_hooks, &g_d3d12_create_count, g_d3d12_create_detours,
                   (void *)d3d12_create, "D3D12_CreateFeature", m);
    }
    if (d3d12_init_ext)
        InstallOne(g_d3d12_init_ext_hooks, &g_d3d12_init_ext_count, g_d3d12_init_ext_detours,
                   (void *)d3d12_init_ext, "D3D12_Init_Ext", m);
    if (d3d12_init_project)
        InstallOne(g_d3d12_init_project_hooks, &g_d3d12_init_project_count, g_d3d12_init_project_detours,
                   (void *)d3d12_init_project, "D3D12_Init_ProjectID", m);
    if (vk_release)
        InstallOne(g_vk_release_hooks, &g_vk_release_count, g_vk_release_detours,
                   (void *)vk_release, "VULKAN_ReleaseFeature", m);
    if (d3d11_release)
        InstallOne(g_d3d11_release_hooks, &g_d3d11_release_count, g_d3d11_release_detours,
                   (void *)d3d11_release, "D3D11_ReleaseFeature", m);
    if (d3d12_release)
        InstallOne(g_d3d12_release_hooks, &g_d3d12_release_count, g_d3d12_release_detours,
                   (void *)d3d12_release, "D3D12_ReleaseFeature", m);
    LeaveCriticalSection(&g_hook_cs);
}

// Streamline coexistence: sl.* plugins (sl.dlss, sl.dlss_g, sl.reflex,
// sl.pace, sl.interposer) drive the same in-process NVSDK_NGX_* exports the
// bridge already patches, so Streamline-forwarded calls are intercepted with
// no separate backend. FG evaluates arriving on sl.dlss_g's present thread
// hit the same IsFgHandle passthrough, serialized on g_hook_cs like
// everything else. This probe only records which sl plugins are resident so
// logs attribute chains correctly; it never loads anything.
static volatile LONG g_streamline_seen = 0;

static void NoteStreamlinePresence()
{
    static const wchar_t *const kSlModules[] = {
        L"sl.interposer.dll", L"sl.dlss.dll", L"sl.dlss_d.dll",
        L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pace.dll",
    };
    LONG mask = 0;
    for (int i = 0; i < (int)_countof(kSlModules); ++i)
        if (GetModuleHandleW(kSlModules[i])) mask |= (1L << i);
    const LONG previous = InterlockedOr(&g_streamline_seen, mask);
    const LONG fresh = mask & ~previous;
    for (int i = 0; fresh && i < (int)_countof(kSlModules); ++i)
        if (fresh & (1L << i))
            Log("[hook] Streamline plugin resident: %ls (NGX-level interception covers "
                "its forwarded calls; no separate backend)", kSlModules[i]);
}

static void ScanModules()
{
    NoteStreamlinePresence();
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return;
    HMODULE mods[1024]; DWORD needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const DWORD n = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < n; ++i) TryHookModule(mods[i]);
}

// Normal library loading is intercepted immediately. A bounded scan covers
// startup modules loaded through lower-level APIs without leaving a permanent
// polling thread behind.
static DWORD WINAPI NgxWatch(void *)
{
    for (int i = 0; i < 240 && !g_bridge.disabled; ++i) { ScanModules(); Sleep(250); }
    return 0;
}

// Load the session-local OptiScaler during explicit initialization. The
// controller assembles this isolated directory before launch.
static bool LoadLocalOptiScaler()
{
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"OptiScaler.dll");
    g_optiscaler_module = GetModuleHandleW(L"OptiScaler.dll");
    if (g_optiscaler_module)
    {
        wchar_t loaded[MAX_PATH] = {};
        if (!GetModuleFileNameW(g_optiscaler_module, loaded, MAX_PATH) ||
            _wcsicmp(loaded, path) != 0)
        {
            Warn("a different OptiScaler.dll is already loaded from %ls; refusing it", loaded);
            g_optiscaler_module = nullptr;
            return false;
        }
    }
    else
        g_optiscaler_module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_optiscaler_module)
    {
        Log("loaded colocated OptiScaler.dll");
        return true;
    }
    Log("could not load colocated OptiScaler.dll (Win32 %lu)", GetLastError());
    return false;
}

// ---------------------------------------------------------------------------
static BOOL CALLBACK InitializeRuntimeOnce(PINIT_ONCE, PVOID, PVOID *)
{
    HMODULE pin = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&InitializeRuntimeOnce), &pin))
        return FALSE;
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_hook_cs);
    InitializeCriticalSection(&g_disp_cs);
    InitializeCriticalSection(&g_frame_cs);
#if defined(DLSS5VK_HOOK_HOST)
    InitializeCriticalSection(&g_iat_cs);
#endif
    LogPath();
    if (!LoadConfig()) return FALSE;
#if defined(DLSS5VK_HOOK_HOST)
    const char *host_id = "injected-vulkan";
    InitializeApplicationRoot();
#elif defined(DLSS5VK_ADDON_HOST)
    const char *host_id = "reshade-addon";
#else
    const char *host_id = "vulkan-windows";
#endif
    if (!g_components.Configure(host_id))
    {
        Log("[components] invalid static component composition for host %s", host_id);
        return FALSE;
    }
    if (!LoadLocalOptiScaler()) return FALSE;
#if defined(DLSS5VK_HOOK_HOST)
    Log("dlss5-vk-bridge " DLSS5VK_VERSION_STRING " initialized -- injected Vulkan+D3D11+D3D12 host "
        "(capture ngx-vulkan + d3d11 + ngx-d3d12, %d viewport(s)).", ActiveViewportCount());
    InstallVulkanHooks();
    if (ChildFollowEnabled())
        Log("[follow] child-process follow armed for target %hs; every spawned child "
            "(intermediates included) is force-suspended, injected, and resumed",
            g_cfg.target_executable);
    CreateThread(nullptr, 0, VulkanHookWatch, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, NgxWatch, nullptr, 0, nullptr);
#elif defined(DLSS5VK_ADDON_HOST)
    Log("dlss5-vk-bridge " DLSS5VK_VERSION_STRING " initialized -- ReShade add-on host.");
#else
    Log("dlss5-vk-bridge " DLSS5VK_VERSION_STRING " initialized -- Vulkan layer host.");
    CreateThread(nullptr, 0, NgxWatch, nullptr, 0, nullptr);
#endif
    return TRUE;
}

static bool EnsureRuntimeInitialized()
{
    return InitOnceExecuteOnce(&g_runtime_once, InitializeRuntimeOnce, nullptr, nullptr) != FALSE;
}

extern "C" __declspec(dllexport) DWORD WINAPI DLSSBridgeInitialize(void *)
{
    return EnsureRuntimeInitialized() ? 1u : 0u;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
