#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_real;

static BOOL CALLBACK LoadDependencies(PINIT_ONCE, PVOID, PVOID *)
{
    // This callback runs on NMS's first WinMM call, outside DllMain/loader lock.
    // Load OptiScaler explicitly while this proxy's own DllMain hosts the bridge.
    wchar_t local[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, local, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(local, L'\\');
        if (slash) {
            slash[1] = L'\0';
            wcscat_s(local, L"OptiScaler.dll");
            LoadLibraryW(local);
        }
    }

    wchar_t system[MAX_PATH];
    n = GetSystemDirectoryW(system, MAX_PATH);
    if (n > 0 && n + 11 < MAX_PATH) {
        wcscat_s(system, L"\\winmm.dll");
        g_real = LoadLibraryW(system);
    }
    return TRUE;
}

template <typename T>
static T Resolve(const char *name)
{
    InitOnceExecuteOnce(&g_once, LoadDependencies, nullptr, nullptr);
    if (!g_real) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    auto proc = reinterpret_cast<T>(GetProcAddress(g_real, name));
    if (!proc) SetLastError(ERROR_PROC_NOT_FOUND);
    return proc;
}

extern "C" MMRESULT WINAPI Proxy_timeBeginPeriod(UINT period)
{
    using Fn = decltype(&timeBeginPeriod);
    auto fn = Resolve<Fn>("timeBeginPeriod");
    return fn ? fn(period) : TIMERR_NOCANDO;
}

extern "C" MMRESULT WINAPI Proxy_timeEndPeriod(UINT period)
{
    using Fn = decltype(&timeEndPeriod);
    auto fn = Resolve<Fn>("timeEndPeriod");
    return fn ? fn(period) : TIMERR_NOCANDO;
}
