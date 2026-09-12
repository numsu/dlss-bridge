#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_real;

static BOOL CALLBACK LoadSystemWinHttp(PINIT_ONCE, PVOID, PVOID *)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 13 >= MAX_PATH) return TRUE;
    wcscat_s(path, L"\\winhttp.dll");
    g_real = LoadLibraryW(path);
    return TRUE;
}

template <typename T>
static T Resolve(const char *name)
{
    InitOnceExecuteOnce(&g_once, LoadSystemWinHttp, nullptr, nullptr);
    if (!g_real) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    auto proc = reinterpret_cast<T>(GetProcAddress(g_real, name));
    if (!proc) SetLastError(ERROR_PROC_NOT_FOUND);
    return proc;
}

#define FORWARD_BOOL(name, params, args) \
    extern "C" BOOL WINAPI Proxy_##name params { \
        using Fn = decltype(&name); auto fn = Resolve<Fn>(#name); \
        return fn ? fn args : FALSE; \
    }

#define FORWARD_HANDLE(name, params, args) \
    extern "C" HINTERNET WINAPI Proxy_##name params { \
        using Fn = decltype(&name); auto fn = Resolve<Fn>(#name); \
        return fn ? fn args : nullptr; \
    }

FORWARD_BOOL(WinHttpAddRequestHeaders,
    (HINTERNET h, LPCWSTR headers, DWORD length, DWORD modifiers),
    (h, headers, length, modifiers))
FORWARD_BOOL(WinHttpCloseHandle, (HINTERNET h), (h))
FORWARD_HANDLE(WinHttpConnect,
    (HINTERNET session, LPCWSTR server, INTERNET_PORT port, DWORD reserved),
    (session, server, port, reserved))
FORWARD_BOOL(WinHttpCrackUrl,
    (LPCWSTR url, DWORD length, DWORD flags, LPURL_COMPONENTS components),
    (url, length, flags, components))
FORWARD_BOOL(WinHttpGetIEProxyConfigForCurrentUser,
    (WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *config), (config))
FORWARD_BOOL(WinHttpGetProxyForUrl,
    (HINTERNET session, LPCWSTR url, WINHTTP_AUTOPROXY_OPTIONS *options,
     WINHTTP_PROXY_INFO *info),
    (session, url, options, info))
FORWARD_HANDLE(WinHttpOpen,
    (LPCWSTR agent, DWORD access, LPCWSTR proxy, LPCWSTR bypass, DWORD flags),
    (agent, access, proxy, bypass, flags))
FORWARD_HANDLE(WinHttpOpenRequest,
    (HINTERNET connect, LPCWSTR verb, LPCWSTR object, LPCWSTR version,
     LPCWSTR referrer, LPCWSTR *accept, DWORD flags),
    (connect, verb, object, version, referrer, accept, flags))
FORWARD_BOOL(WinHttpQueryDataAvailable,
    (HINTERNET request, LPDWORD available), (request, available))
FORWARD_BOOL(WinHttpQueryHeaders,
    (HINTERNET request, DWORD level, LPCWSTR name, LPVOID buffer,
     LPDWORD length, LPDWORD index),
    (request, level, name, buffer, length, index))
FORWARD_BOOL(WinHttpQueryOption,
    (HINTERNET h, DWORD option, LPVOID buffer, LPDWORD length),
    (h, option, buffer, length))
FORWARD_BOOL(WinHttpReadData,
    (HINTERNET request, LPVOID buffer, DWORD bytes, LPDWORD read),
    (request, buffer, bytes, read))
FORWARD_BOOL(WinHttpReceiveResponse,
    (HINTERNET request, LPVOID reserved), (request, reserved))
FORWARD_BOOL(WinHttpSendRequest,
    (HINTERNET request, LPCWSTR headers, DWORD headers_length,
     LPVOID optional, DWORD optional_length, DWORD total_length,
     DWORD_PTR context),
    (request, headers, headers_length, optional, optional_length, total_length,
     context))
FORWARD_BOOL(WinHttpSetOption,
    (HINTERNET h, DWORD option, LPVOID buffer, DWORD length),
    (h, option, buffer, length))
FORWARD_BOOL(WinHttpSetTimeouts,
    (HINTERNET h, int resolve, int connect, int send, int receive),
    (h, resolve, connect, send, receive))
FORWARD_BOOL(WinHttpWriteData,
    (HINTERNET request, LPCVOID buffer, DWORD bytes, LPDWORD written),
    (request, buffer, bytes, written))

extern "C" WINHTTP_STATUS_CALLBACK WINAPI Proxy_WinHttpSetStatusCallback(
    HINTERNET h, WINHTTP_STATUS_CALLBACK callback, DWORD flags,
    DWORD_PTR reserved)
{
    using Fn = decltype(&WinHttpSetStatusCallback);
    auto fn = Resolve<Fn>("WinHttpSetStatusCallback");
    return fn ? fn(h, callback, flags, reserved)
              : WINHTTP_INVALID_STATUS_CALLBACK;
}
