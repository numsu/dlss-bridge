#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_real;

static BOOL CALLBACK LoadSystemXInput(PINIT_ONCE, PVOID, PVOID *)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 18 >= MAX_PATH) return TRUE;
    wcscat_s(path, L"\\XINPUT9_1_0.dll");
    g_real = LoadLibraryW(path);
    return TRUE;
}

template <typename T>
static T Resolve(const char *name)
{
    InitOnceExecuteOnce(&g_once, LoadSystemXInput, nullptr, nullptr);
    if (!g_real) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    auto proc = reinterpret_cast<T>(GetProcAddress(g_real, name));
    if (!proc) SetLastError(ERROR_PROC_NOT_FOUND);
    return proc;
}

extern "C" DWORD WINAPI Proxy_XInputGetState(DWORD user, XINPUT_STATE *state)
{
    using Fn = decltype(&XInputGetState);
    auto fn = Resolve<Fn>("XInputGetState");
    return fn ? fn(user, state) : ERROR_DEVICE_NOT_CONNECTED;
}

extern "C" DWORD WINAPI Proxy_XInputSetState(DWORD user, XINPUT_VIBRATION *vibration)
{
    using Fn = decltype(&XInputSetState);
    auto fn = Resolve<Fn>("XInputSetState");
    return fn ? fn(user, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}
