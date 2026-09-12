// dlss5-vk-bridge -- a Vulkan port of NIGos' DLSS 5 DX11 Bridge.
//
// A DLSS 5 (Neural Rendering) add-on such as RenoDX's ReShade add-on only
// detours the *D3D12* NGX evaluate. Vulkan games never make that call, so the
// neural pass never runs for them. This bridge closes that gap: it intercepts
// the game's NGX Vulkan evaluate, mirrors the same DLSS contract onto a private
// D3D12 device, and runs a second NGX evaluate there -- the call the add-on
// detours -- copying the neural result back into the game's own output image.
//
// Two pieces cooperate:
//   * a Vulkan layer (vk_layer.inc) that enables the interop extensions the
//     game never asked for and exposes the vkQueueSubmit handoff point;
//   * an NGX Vulkan evaluate hook (this file) that arms each bridged frame.
//
// Original DLSS 5 DX11 Bridge (c) 2026 NIGos, MIT. This port (c) 2026 Alan Z.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR

#include <windows.h>
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
#include "dlss_bridge/runtime.hpp"

// One string for the log banner, the ReShade add-on registration and the
// release notes; version.rc carries the same numbers.
#define DLSS5VK_VERSION_STRING "v0.3.0"

// ---------------------------------------------------------------------------
// module-wide state
// ---------------------------------------------------------------------------
static HMODULE          g_self;
static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_hook_cs;
static wchar_t          g_log_path[MAX_PATH];
static bool             g_log_ready;

// ---------------------------------------------------------------------------
// configuration (dlss5-vk-bridge.cfg, next to the DLL)
// ---------------------------------------------------------------------------
using dlss_bridge::RuntimeConfig;
static RuntimeConfig g_cfg;

// Once the first private neural frame completes, strict mode never executes
// the game-side upscaler again. Startup calls are observation/bootstrap, not a
// displayed fallback path. A failed strict frame repeats the last neural image.
static volatile LONG g_neural_only_latched;

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

static void LoadConfig()
{
    wchar_t path[MAX_PATH] = {};
    // Every launch frontend can point at one resolved policy. Existing game-
    // local deployments remain compatible when the variable is absent.
    if (GetEnvironmentVariableW(L"DLSS_BRIDGE_CONFIG", path, MAX_PATH) == 0)
    {
        GetModuleFileNameW(g_self, path, MAX_PATH);
        if (wchar_t *s = wcsrchr(path, L'\\'))
            wcscpy_s(s + 1, MAX_PATH - (s + 1 - path), L"dlss5-vk-bridge.cfg");
    }

    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || f == nullptr)
    {
        // write a documented default so users have something to edit.
        if (_wfopen_s(&f, path, L"w") == 0 && f)
        {
            fputs("# dlss5 portable runtime configuration\n"
                  "# execution_mode auto | same_gpu | secondary_gpu\n"
                  "# compute_adapter auto | game | index:N | luid:HIGH:LOW\n"
                  "# require_neural_result=1 permits bootstrap, then never displays game DLSS\n"
                  "mode = 0\nflags = -1\nsubrects = 1\nverbose = 0\nsync = 1\n"
                  "execution_mode = auto\ncompute_adapter = auto\n"
                  "require_neural_result = 1\nring_slots = 3\nlatency_budget_ms = 16\n"
                  "output_transport = native\nneural_queue_mode = split\n", f);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char key[64] = {}, value[128] = {};
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf_s(line, " %63[^= ] = %127[^\r\n]", key, (unsigned)sizeof(key),
                     value, (unsigned)sizeof(value)) != 2) continue;
        char *begin = value;
        while (*begin == ' ' || *begin == '\t') ++begin;
        char *tail = begin + strlen(begin);
        while (tail > begin && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!g_cfg.Apply(key, begin)) Warn("[cfg] ignored invalid or unknown setting %s=%s", key, begin);
    }
    fclose(f);
    Log("[cfg] mode=%d flags=%d subrects=%d verbose=%d sync=%d execution_mode=%s "
        "compute_adapter=%s require_neural_result=%d ring_slots=%d latency_budget_ms=%d "
        "output_transport=%s neural_queue_mode=%s",
        g_cfg.mode, g_cfg.flags, g_cfg.subrects, g_cfg.verbose, g_cfg.sync,
        dlss_bridge::ExecutionModeName(g_cfg.execution_mode), g_cfg.compute_adapter.text,
        g_cfg.require_neural_result, g_cfg.ring_slots, g_cfg.latency_budget_ms,
        dlss_bridge::OutputTransportName(g_cfg.output_transport),
        dlss_bridge::NeuralQueueModeName(g_cfg.neural_queue_mode));
    if (g_cfg.ring_slots != Bridge::kFrames)
        Warn("[cfg] ring_slots=%d requested; working Vulkan backend currently provides %d",
             g_cfg.ring_slots, Bridge::kFrames);
}

// Defined with the NGX hook below. BridgeDisable calls it so a bridge that has
// stopped also stops intercepting: the game's DLSS calls go back to running
// exactly as they would without this DLL loaded at all.
static void BridgeUnhookAll();

// ---------------------------------------------------------------------------
// Frame Generation stand-aside
//
// Streamline's DLSS Frame Generation (sl.dlss_g) pairs every present with that
// frame's DLSS evaluate and interpolates between the two most recent frames at
// present time. The bridge breaks both halves of that pairing: it stalls the
// game's queue mid-submit (the sync sandwich) and runs a second, private DLSS
// evaluate each frame inside the same process. X4: Foundations (#1) showed
// what that costs: one bridged frame, then a null dereference inside
// sl.dlss_g.dll's present-time bookkeeping (minidump: sl.dlss_g+0x3d320,
// v2.7.30, reached from vkQueuePresentKHR through the driver, the frame after
// the bridge's first sandwich).
//
// The bridge cannot make FG's internal bookkeeping robust, so while FG is
// active it must not bridge at all. The latch below is set the moment the game
// creates an NGX FrameGeneration feature -- the earliest reliable signal.
// (Module presence is NOT such a signal: Streamline preloads nvngx_dlssg.dll
// to probe support long before the user's setting is known; both X4 logs show
// it resident before any VkInstance existed.) Feature creation, by contrast,
// happens only when FG is actually enabled, and always before FG's first
// evaluate -- so the latch is in place before the first frame the bridge
// would otherwise touch. It never clears: a mid-session FG toggle-off leaves
// NGX state the bridge has no way to re-validate, so standing back up is a
// game restart.
// ---------------------------------------------------------------------------
static volatile LONG g_fg_active;
static volatile LONG g_frames_touched;   // a sandwich was recorded into a game frame

static bool BridgeFrameGenerationActive() { return g_fg_active != 0; }
static void NoteFrameTouched()            { g_frames_touched = 1; }

// Transient-fault cooldown. The X4 #1 forensics pinned the one-off contained
// evaluate fault inside ReShade64.dll itself (+0x1EAF11, reading null+0x440)
// with every bridge patch audited intact -- a cooperating interposer having a
// moment, not corrupted state on this side. Standing down for the whole
// session over it (v0.1.10/11) cost the add-on the session; the game demons-
// trably renders on fine after the containment. So the first contained fault
// only pauses bridged frames for a cooldown; forwarding continues untouched
// every frame, and a second fault stands the bridge down for real.
static volatile LONG      g_eval_fault_count;
static volatile ULONGLONG g_bridge_hold_until;   // tick until which frames pass untouched

static bool BridgeInFaultCooldown()
{
    return g_bridge_hold_until != 0 && GetTickCount64() < g_bridge_hold_until;
}

// Called with the feature id of every NGX Vulkan feature the game creates,
// BEFORE the create is forwarded. Returns true when the create must be
// REFUSED (fail, never forwarded): a FrameGeneration create arriving after
// the bridge has already recorded into this session's frames. Forwarded, FG
// would come up over a history of stalled, double-evaluated frames, and its
// present-time bookkeeping crashes on exactly that -- X4 #1 hit it twice
// (3000 and 2000 bridged frames in, the log's last line the old stand-aside
// notice). Refused, the FG feature never exists, its bookkeeping never arms,
// and the bridge keeps working; a relaunch with FG enabled gets FG, because
// the create then lands before any frame is touched and takes the stand-aside
// branch below.
static bool NoteVkFeatureCreate(int feature)
{
    const int kNGXFeatureFrameGeneration = 11;   // NVSDK_NGX_Feature_FrameGeneration
    if (feature != kNGXFeatureFrameGeneration) return false;
    if (g_frames_touched != 0)
    {
        static LONG warned = 0;
        if (InterlockedExchange(&warned, 1) == 0)
            Log("[bridge] REFUSING a mid-session NGX Frame Generation create: the bridge "
                "has already recorded into this session's frames, and FG's present-time "
                "bookkeeping cannot absorb that mixed history. The refusal fails the "
                "create in the most standard way NGX has (FeatureNotSupported, null "
                "handle). If the game dies moments after this line anyway, its own FG "
                "enable path did not survive being told no either -- change Frame "
                "Generation only from a fresh launch.");
        return true;
    }
    if (InterlockedExchange(&g_fg_active, 1) == 0)
        Log("[bridge] the game is creating an NGX Frame Generation feature (DLSS-G). "
            "The bridge stands aside for this session: FG pairs each present with the "
            "frame's DLSS evaluate, and the bridge's in-frame stall plus private second "
            "evaluate breaks that pairing inside sl.dlss_g. Restart the game with Frame "
            "Generation disabled to use the bridge.");
    return false;
}

// ---------------------------------------------------------------------------
// the three halves (order matters: d3d12 session, then the host, then interop)
//
// Two hosts build from this file. The Vulkan layer (dlss5-vk-bridge.dll, the
// v0.1 line) interposes the loader chain; the ReShade add-on
// (dlss5-vk-bridge.addon64, the v0.2 line, DLSS5VK_ADDON_HOST) is loaded by
// ReShade next to the DLSS 5 add-on it feeds. Either supplies the same seam
// to vk_interop.inc: the device dispatch, the submit / command-buffer
// notifications and a handful of kHost* traits.
// ---------------------------------------------------------------------------
#include "d3d12_session.inc"
#if defined(DLSS5VK_HOOK_HOST)
#include "hook_host.inc"
#elif defined(DLSS5VK_PROXY_HOST)
#include "proxy_host.inc"
#elif defined(DLSS5VK_ADDON_HOST)
#include "addon_host.inc"
#else
#include "vk_layer.inc"
#endif
#include "vk_interop.inc"

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
#if defined(DLSS5VK_HOOK_HOST)
static Hook      g_init_ext_hooks[kMaxHooks];  // NVSDK_NGX_VULKAN_Init_Ext
static int       g_init_ext_count;
static Hook      g_init_ext2_hooks[kMaxHooks]; // NVSDK_NGX_VULKAN_Init_Ext2
static int       g_init_ext2_count;
#endif
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
// Un-patching only the module being called leaves the other four live inside
// the forwarded call, and these modules call each other: with Streamline in the
// process the game reaches sl.common.dll, which forwards into _nvngx.dll, which
// dispatches into nvngx_dlss.dll -- each hop re-entering a detour, and any of
// those detours' "original" bytes possibly being another interposer's jump
// rather than real code. X4: Foundations reported exactly what that costs: the
// forwarded evaluate raising 0xC00000FD, a stack overflow, once per launch,
// with the game dying immediately after. Removing every patch for the duration
// makes the whole forwarded call tree run byte for byte as it would with this
// DLL absent, whatever its shape; the nesting depth still arms the bridge on
// the outermost call alone.
//
// The two hook classes lift differently:
//
//   * Forwarding an EVALUATE lifts the evaluate patches only. The CreateFeature
//     patches stay hot on purpose: creates come from the game on its own
//     schedule (enabling Frame Generation mid-session, for instance), and if
//     they were lifted here, a create landing during any forwarded evaluate
//     would run unobserved -- with evaluates streaming every frame, that race
//     would miss the one signal the FG stand-aside depends on. A concurrent
//     create hits its still-installed detour instead and serialises on the
//     same critical section.
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
// X4 #1 (v0.1.10) had a forwarded evaluate raise an access violation ~55
// frames into an otherwise healthy session -- contained by design, but the
// log recorded only the exception code, which pins nothing. A contained
// fault now logs where the faulting instruction lives (module+offset, or
// "not in any loaded module" -- that alone convicts a jump into freed code),
// what it touched, and an audit of every hooked entry point taken BEFORE the
// lifted patches go back on: is the module still loaded, and are the bytes
// on the entry the expected ones for the current lift depth. Another
// interposer rewriting or unhooking these same exports is exactly what such
// an audit catches red-handed.
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
        // Catching a stack overflow does not put the guard page back. Leave it
        // consumed and the next deep call on this thread takes the process down
        // with no exception at all -- which is what the game did a moment after
        // this line in the X4: Foundations report.
        if (fi->code == kStatusStackOverflow) _resetstkoflw();
        return NGX_SUCCESS;
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
            Warn("[hook] an evaluate arrived through a detour with no hook behind it; "
                 "returning success without forwarding.");
        return NGX_SUCCESS;
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
        // The game's own DLSS call faulted while we were holding it. The X4 #1
        // forensics showed this can be a transient in a neighbouring interposer
        // (ReShade64.dll reading null+0x440, every bridge patch intact) with
        // the game rendering on fine after containment -- so the first fault
        // only pauses bridged frames for a cooldown, with forwarding running
        // untouched throughout. A second fault means it was not transient:
        // stop, un-patch, and let the game have its own entry points back.
        if (InterlockedIncrement(&g_eval_fault_count) == 1)
        {
            const ULONGLONG kFaultCooldownMs = 5000;
            g_bridge_hold_until = GetTickCount64() + kFaultCooldownMs;
            Warn("a forwarded evaluate faulted but was contained; pausing bridged frames "
                 "for %llu ms, then resuming (a second fault stands the bridge down for "
                 "the session). The game renders normally.", kFaultCooldownMs);
        }
        else
            BridgeDisable("the game's NGX evaluate faulted twice while the bridge was forwarding it");
    }
    return r;
}

static NVSDK_NGX_Result BridgedVkEvaluate(int idx, VkCommandBuffer cmd, const NVSDK_NGX_Handle *feat,
                                          const NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback cb)
{
    ++g_ngx_nest;
    const bool outer = (g_ngx_nest == 1);

    // A hard failure after strict mode has latched must not unhook and silently
    // resume the game's original DLSS. Report NGX failure to the game instead.
    if (outer && g_bridge.disabled && dlss_bridge::IsStrictNeural(g_cfg) &&
        g_neural_only_latched)
    {
        --g_ngx_nest;
        return NGX_FAIL;
    }

    // During bootstrap we forward while learning/building the private session.
    // Once strict mode latches, the game evaluate stays suppressed. Frames that
    // cannot enter the worker ring repeat the last completed neural image; an
    // NGX failure here prevents the game from submitting that repeat command.
    const bool rr_frame = BridgeRayReconstructionInput(p) != nullptr;
    const bool skip_game = outer && !rr_frame && BridgeSkipGameEvaluate(p);
    NVSDK_NGX_Result r = NGX_SUCCESS;
    if (skip_game)
    {
        static bool said = false;
        if (!said) { said = true;
            Log("[policy] skipping the game-side evaluate; the private neural result is authoritative"); }
    }
    else
        r = ForwardVkEvaluateVia(idx, cmd, feat, p, cb);
    if (outer && !g_bridge.disabled)
    {
        FaultInfo fi;
        memset(&fi, 0, sizeof(fi));
        __try { BridgeVkFrame(cmd, p); }
        __except (FaultCapture(&fi, GetExceptionInformation()))
        { BridgeFrameUnlockAll();   // never leave the submit hooks locked out
          Log("[bridge] frame path faulted 0x%08X; disabling to protect the game", fi.code);
          LogFaultSite(&fi);
          BridgeDisable("the bridge frame path raised an exception"); }
    }
    --g_ngx_nest;
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
// NGX Vulkan create hook -- only to observe the feature id (see the Frame
// Generation stand-aside above); every call is forwarded, nothing is altered.
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
    if (NoteVkFeatureCreate(feature))
    { if (out) *out = nullptr; return NGX_FAIL_FEATURE_NOT_SUPPORTED; }
    return ForwardVkCreateVia(&g_create_hooks[idx], false, VK_NULL_HANDLE, cmd, feature, p, out);
}
static NVSDK_NGX_Result BridgedVkCreate1(int idx, VkDevice dev, VkCommandBuffer cmd, int feature,
                                         NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    if (NoteVkFeatureCreate(feature))
    { if (out) *out = nullptr; return NGX_FAIL_FEATURE_NOT_SUPPORTED; }
    return ForwardVkCreateVia(&g_create1_hooks[idx], true, dev, cmd, feature, p, out);
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

#if defined(DLSS5VK_HOOK_HOST)
// NMS creates Vulkan before its local WinHTTP proxy loads this DLL. Its later
// NGX Init_Ext call supplies the already-created handles, which lets the IAT
// host adopt the real render device instead of guessing from opaque handles.
// All Init_Ext patches are lifted while forwarding because NGX proxy modules
// may forward this call through one another just like Create/Evaluate.
static int g_lift_init_ext_depth;
static void LiftInitExtHooks()
{
    if (++g_lift_init_ext_depth != 1) return;
    for (int i = 0; i < g_init_ext_count; ++i) HookRestore(&g_init_ext_hooks[i]);
    for (int i = 0; i < g_init_ext2_count; ++i) HookRestore(&g_init_ext2_hooks[i]);
}
static void UnliftInitExtHooks()
{
    if (--g_lift_init_ext_depth != 0) return;
    for (int i = 0; i < g_init_ext_count; ++i) HookReinstall(&g_init_ext_hooks[i]);
    for (int i = 0; i < g_init_ext2_count; ++i) HookReinstall(&g_init_ext2_hooks[i]);
}

static NVSDK_NGX_Result ForwardVkInitExt(int idx, unsigned long long app_id,
                                         const wchar_t *data_path, VkInstance instance,
                                         VkPhysicalDevice phys, VkDevice device, int version,
                                         const void *feature_info)
{
    Hook &h = g_init_ext_hooks[idx];
    HostCaptureNgxInit(instance, phys, device);
    EnterCriticalSection(&g_hook_cs);
    LiftInitExtHooks();
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NGX_VK_Init>(h.target)(
        app_id, data_path, instance, phys, device, version, feature_info);
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
    HostCaptureNgxInit(instance, phys, device);
    EnterCriticalSection(&g_hook_cs);
    LiftInitExtHooks();
    const NVSDK_NGX_Result result = reinterpret_cast<PFN_NGX_VK_Init_Ext2>(h.target)(
        app_id, data_path, instance, phys, device, gipa, gdpa, version, feature_info);
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
#endif

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

// Lift every patch for good. Reached from BridgeDisable, so a bridge that has
// given up leaves the game's NGX entry points exactly as it found them.
static void UnhookArray(Hook *hooks, int n)
{
    for (int i = 0; i < n; ++i)
        if (hooks[i].installed)
        {
            HookRestore(&hooks[i]);
            hooks[i].installed = false;
        }
}

static void BridgeUnhookAll()
{
    EnterCriticalSection(&g_hook_cs);
    UnhookArray(g_eval_hooks,    g_eval_count);
    UnhookArray(g_create_hooks,  g_create_count);
    UnhookArray(g_create1_hooks, g_create1_count);
#if defined(DLSS5VK_HOOK_HOST)
    UnhookArray(g_init_ext_hooks, g_init_ext_count);
    UnhookArray(g_init_ext2_hooks, g_init_ext2_count);
#endif
    LeaveCriticalSection(&g_hook_cs);
}

// True when an entry point already starts with one of THIS DLL's own jumps: a
// previous load of the bridge patched it and was unloaded without a chance to
// unpatch (the reload lands at the same base, so the old jump targets the new
// detours). Saving those 14 bytes as "original" makes every forwarded call
// jump back into the detour forever. The module pin in DllMain makes this
// state unreachable; if some day it is reached anyway, the original bytes are
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
#if defined(DLSS5VK_HOOK_HOST)
    for (int i = 0; i < kMaxHooks; ++i)
        if (dst == g_init_ext_detours[i] || dst == g_init_ext2_detours[i]) return true;
#endif
    return false;
}

static void InstallOne(Hook *hooks, int *count, void *const detours[], void *target,
                       const char *what, HMODULE m)
{
    for (int i = 0; i < *count; ++i) if (hooks[i].target == target) return;   // already hooked
    if (EntryIsOwnDetourJump(target))
    {
        Warn("[hook] %ls: NVSDK_NGX_VULKAN_%s entry already carries this bridge's own jump "
             "from an unloaded previous load; its original bytes are unrecoverable -- not "
             "hooking it.", ModuleName(m), what);
        return;                                                  // never install over it
    }
    if (*count >= kMaxHooks) return;
    Hook &h  = hooks[*count];
    h.target = target;
    h.detour = detours[*count];
    HookInstall(&h);
    Log("[hook] patched NVSDK_NGX_VULKAN_%s @ %p in %ls", what, target, ModuleName(m));
    ++*count;
}

static void TryHookModule(HMODULE m)
{
    if (g_bridge.disabled) return;                               // never re-patch after giving up
    if (m == g_self || m == GetModuleHandleW(nullptr)) return;   // never the host exe (integrity checks)

    auto eval   = GetProcAddress(m, "NVSDK_NGX_VULKAN_EvaluateFeature");
    auto create = GetProcAddress(m, "NVSDK_NGX_VULKAN_CreateFeature");
    auto create1 = GetProcAddress(m, "NVSDK_NGX_VULKAN_CreateFeature1");   // optional export
#if defined(DLSS5VK_HOOK_HOST)
    auto init_ext = GetProcAddress(m, "NVSDK_NGX_VULKAN_Init_Ext");
    auto init_ext2 = GetProcAddress(m, "NVSDK_NGX_VULKAN_Init_Ext2");
    if ((eval == nullptr || create == nullptr) && init_ext == nullptr && init_ext2 == nullptr) return;
#else
    if (eval == nullptr || create == nullptr) return;            // not a real NGX Vulkan module
#endif

    EnterCriticalSection(&g_hook_cs);
    if (eval && create)
    {
        InstallOne(g_eval_hooks,   &g_eval_count,   g_eval_detours,   (void *)eval,   "EvaluateFeature", m);
        InstallOne(g_create_hooks, &g_create_count, g_create_detours, (void *)create, "CreateFeature",   m);
    }
    if (create1)
        InstallOne(g_create1_hooks, &g_create1_count, g_create1_detours, (void *)create1, "CreateFeature1", m);
#if defined(DLSS5VK_HOOK_HOST)
    if (init_ext)
        InstallOne(g_init_ext_hooks, &g_init_ext_count, g_init_ext_detours, (void *)init_ext, "Init_Ext", m);
    if (init_ext2)
        InstallOne(g_init_ext2_hooks, &g_init_ext2_count, g_init_ext2_detours, (void *)init_ext2, "Init_Ext2", m);
#endif
    LeaveCriticalSection(&g_hook_cs);
}

static void ScanModules()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return;
    HMODULE mods[1024]; DWORD needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const DWORD n = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < n; ++i) TryHookModule(mods[i]);
}

// NGX and the DLSS 5 add-on can load well after the game starts, so scan
// eagerly for the first minute, then keep a slow watch for the whole session.
static DWORD WINAPI NgxWatch(void *)
{
#if defined(DLSS5VK_HOOK_HOST)
    for (int i = 0; i < 2000 && !g_bridge.disabled; ++i) { ScanModules(); Sleep(5); }
#else
    for (int i = 0; i < 240 && !g_bridge.disabled; ++i) { ScanModules(); Sleep(250); }
#endif
    while (!g_bridge.disabled) { ScanModules(); Sleep(2000); }
    return 0;
}

// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = inst;

        // Pin this module for the life of the process. A DLL that writes jump
        // patches into other modules and runs a watcher thread cannot be
        // unloaded safely, but the Vulkan loader unloads implicit layers
        // whenever the last instance is destroyed -- and a game may create and
        // destroy several instances back to back (X4: Foundations creates
        // three). Each unload left the patches jumping into a dead mapping;
        // each reload then landed at the same base, read its predecessor's
        // still-installed jump back as the "original bytes", and the first
        // forwarded evaluate jumped straight back into its own detour forever:
        // the 0xC00000FD stack overflow in both X4 crash reports. Pinned, every
        // later FreeLibrary is a no-op, so one load's patches, watcher and
        // layer state outlive every instance the process creates.
        HMODULE pin = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pin);

        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_hook_cs);
        InitializeCriticalSection(&g_disp_cs);   // owned by the host half
        InitializeCriticalSection(&g_frame_cs);  // the interop half's slot state
        LogPath();
        LoadConfig();
#if defined(DLSS5VK_HOOK_HOST)
        Log("dlss5-vk-bridge " DLSS5VK_VERSION_STRING " loaded -- in-process Vulkan hook host + NGX evaluate/create hooks.");
        CreateThread(nullptr, 0, VulkanHookWatch, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, NgxWatch, nullptr, 0, nullptr);
#elif defined(DLSS5VK_PROXY_HOST)
        Log("dlss5-vk-bridge " DLSS5VK_VERSION_STRING " loaded -- Vulkan loader proxy + NGX Vulkan evaluate/create hooks.");
        CreateThread(nullptr, 0, NgxWatch, nullptr, 0, nullptr);
#elif defined(DLSS5VK_ADDON_HOST)
        // ReShade calls AddonInit next; the NGX hooks start there, once the
        // registration went through, so a module ReShade rejects stays inert.
        Log("dlss5-vk-bridge " DLSS5VK_VERSION_STRING " loaded -- ReShade add-on host + NGX Vulkan evaluate/create hooks.");
#else
        Log("dlss5-vk-bridge " DLSS5VK_VERSION_STRING " loaded -- Vulkan layer + NGX Vulkan evaluate/create hooks.");
        CreateThread(nullptr, 0, NgxWatch, nullptr, 0, nullptr);
#endif
    }
    return TRUE;
}
