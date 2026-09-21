// Child-process follow: bridge a game the launcher spawns instead of the
// launcher itself (Rockstar Games Launcher -> GTA5_Enhanced.exe, Warframe
// Launcher.exe -> Warframe.x64.exe, ...).
//
// Enabled only by explicit profile opt-in ([launch] follow_children with a
// bare target_executable filename). While enabled, CreateProcessW/A imports in
// application modules are IAT-redirected through the same PatchLoaderIat
// mechanism as the loader hooks. Every spawned child is force-created
// suspended, gets the session hook DLL injected with the same
// remote-LoadLibrary + remote-DLSSBridgeInitialize technique as the session
// launcher, and is resumed iff the bridge added the suspension. Following the
// intermediate processes, not just the target, is what makes launcher chains
// work (Launcher.exe -> Helper.exe -> Game.exe): the bridge hooks NGX exports,
// which only the final renderer exports, so intermediates run the hook
// passively and exist to intercept the next CreateProcess down the chain.
// Injection failures run the child exactly as the launcher requested: a
// failed follow never hangs or kills the child, it only logs, so the game
// still starts (unbridged).
//
// The child inherits the launcher's environment, which already carries
// DLSS_BRIDGE_CONFIG/DLSS_BRIDGE_SESSION, so no environment surgery is needed.
// One level or many: every injected process carries the same follow rule, so
// grandchild renderers are intercepted the same way, at any depth.

#pragma once

static bool ChildFollowEnabled()
{
    if (!g_cfg.follow_children || !g_cfg.target_executable[0]) return false;
    return true;
}

// Basename of a path (either separator flavor), without mutating the input.
static const wchar_t *ChildBaseNameW(const wchar_t *path)
{
    if (!path) return L"";
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return base;
}

static const char *ChildBaseNameA(const char *path)
{
    if (!path) return "";
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

// First token of a command line into a caller buffer, honoring quotes. Used
// only when lpApplicationName is null. Never fails: worst case yields "".
static void ChildFirstTokenW(const wchar_t *cmd, wchar_t *out, size_t capacity)
{
    if (!out || !capacity) return;
    out[0] = 0;
    if (!cmd) return;
    while (*cmd == L' ' || *cmd == L'\t') ++cmd;
    size_t n = 0;
    if (*cmd == L'"')
    {
        ++cmd;
        while (*cmd && *cmd != L'"' && n + 1 < capacity) out[n++] = *cmd++;
    }
    else
    {
        while (*cmd && *cmd != L' ' && *cmd != L'\t' && n + 1 < capacity) out[n++] = *cmd++;
    }
    out[n] = 0;
}

static void ChildFirstTokenA(const char *cmd, char *out, size_t capacity)
{
    if (!out || !capacity) return;
    out[0] = 0;
    if (!cmd) return;
    while (*cmd == ' ' || *cmd == '\t') ++cmd;
    size_t n = 0;
    if (*cmd == '"')
    {
        ++cmd;
        while (*cmd && *cmd != '"' && n + 1 < capacity) out[n++] = *cmd++;
    }
    else
    {
        while (*cmd && *cmd != ' ' && *cmd != '\t' && n + 1 < capacity) out[n++] = *cmd++;
    }
    out[n] = 0;
}

// Narrow target name (from the UTF-8 policy file) against a wide child path.
// Both sides reduce to bare filenames before the case-insensitive compare.
static bool ChildNameMatchesW(const wchar_t *child_path)
{
    wchar_t want[MAX_PATH] = {};
    size_t converted = 0;
    mbstowcs_s(&converted, want, g_cfg.target_executable, _TRUNCATE);
    if (!want[0]) return false;
    return _wcsicmp(ChildBaseNameW(child_path), ChildBaseNameW(want)) == 0;
}

static bool ChildNameMatchesA(const char *child_path)
{
    if (!g_cfg.target_executable[0]) return false;
    return _stricmp(ChildBaseNameA(child_path),
                    ChildBaseNameA(g_cfg.target_executable)) == 0;
}

// PIDs already claimed for injection, so two launcher threads racing to spawn
// the same target cannot double-inject it.
static volatile LONG g_follow_claims[16] = {};

static bool ChildFollowClaim(DWORD pid)
{
    for (int i = 0; i < 16; ++i)
        if (g_follow_claims[i] == (LONG)pid) return false;
    for (int i = 0; i < 16; ++i)
        if (InterlockedCompareExchange(&g_follow_claims[i], (LONG)pid, 0) == 0) return true;
    return false;
}

// Remote LoadLibraryW address inside the child, mirroring the session
// launcher: kernel32 offset + child kernel32 base from the toolhelp snapshot,
// falling back to the local address (valid pre-entry on Windows and Wine).
static LPTHREAD_START_ROUTINE ChildRemoteLoadLibraryW(DWORD pid)
{
    HMODULE local_kernel = GetModuleHandleW(L"kernel32.dll");
    auto local_load = local_kernel ? GetProcAddress(local_kernel, "LoadLibraryW") : nullptr;
    if (!local_load) return nullptr;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(local_load) -
        reinterpret_cast<uintptr_t>(local_kernel);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return reinterpret_cast<LPTHREAD_START_ROUTINE>(local_load);
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    LPTHREAD_START_ROUTINE remote = nullptr;
    if (Module32FirstW(snapshot, &entry))
        do
        {
            if (_wcsicmp(entry.szModule, L"kernel32.dll") == 0)
            {
                remote = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                    reinterpret_cast<uintptr_t>(entry.modBaseAddr) + offset);
                break;
            }
        }
        while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return remote ? remote : reinterpret_cast<LPTHREAD_START_ROUTINE>(local_load);
}

static bool ChildRemoteInitialize(HANDLE process, DWORD pid, const wchar_t *dll)
{
    wchar_t expected[MAX_PATH] = {};
    if (!GetFullPathNameW(dll, MAX_PATH, expected, nullptr)) return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uintptr_t remote_base = 0;
    if (Module32FirstW(snapshot, &entry))
        do {
            wchar_t actual[MAX_PATH] = {};
            if (GetFullPathNameW(entry.szExePath, MAX_PATH, actual, nullptr) &&
                _wcsicmp(actual, expected) == 0) {
                remote_base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (!remote_base) return false;
    HMODULE local = LoadLibraryExW(dll, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return false;
    FARPROC local_init = GetProcAddress(local, "DLSSBridgeInitialize");
    if (!local_init) { FreeLibrary(local); return false; }
    const uintptr_t offset = reinterpret_cast<uintptr_t>(local_init) -
        reinterpret_cast<uintptr_t>(local);
    FreeLibrary(local);
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_base + offset), nullptr, 0, nullptr);
    if (!thread) return false;
    DWORD result = 0;
    const DWORD wait = WaitForSingleObject(thread, 15000);
    const bool ok = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result) && result == 1;
    CloseHandle(thread);
    return ok;
}

// Inject the session hook DLL into a suspended matching child and run its
// explicit initializer. Never suspends, never terminates, never hangs the
// child: every failure path logs and lets the child run unbridged.
static void FollowInjectChild(PROCESS_INFORMATION *pi)
{
    if (!pi || !pi->hProcess) return;
    if (!ChildFollowClaim(pi->dwProcessId)) return; // raced; the other thread owns it
    wchar_t dll[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_self, dll, MAX_PATH))
    {
        Log("[follow] could not locate session hook DLL; child %lu runs unbridged",
            (unsigned long)pi->dwProcessId);
        return;
    }
    const SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(pi->hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote)
    {
        Log("[follow] VirtualAllocEx failed in child %lu (Win32 %lu); runs unbridged",
            (unsigned long)pi->dwProcessId, GetLastError());
        return;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(pi->hProcess, remote, dll, bytes, &written) || written != bytes)
    {
        Log("[follow] WriteProcessMemory failed in child %lu (Win32 %lu); runs unbridged",
            (unsigned long)pi->dwProcessId, GetLastError());
        VirtualFreeEx(pi->hProcess, remote, 0, MEM_RELEASE);
        return;
    }
    auto load_library = ChildRemoteLoadLibraryW(pi->dwProcessId);
    if (!load_library)
    {
        Log("[follow] could not locate LoadLibraryW in child %lu; runs unbridged",
            (unsigned long)pi->dwProcessId);
        VirtualFreeEx(pi->hProcess, remote, 0, MEM_RELEASE);
        return;
    }
    HANDLE thread = CreateRemoteThread(pi->hProcess, nullptr, 0, load_library,
                                       remote, 0, nullptr);
    if (!thread)
    {
        Log("[follow] CreateRemoteThread failed in child %lu (Win32 %lu); runs unbridged",
            (unsigned long)pi->dwProcessId, GetLastError());
        VirtualFreeEx(pi->hProcess, remote, 0, MEM_RELEASE);
        return;
    }
    DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD loaded = 0;
    const bool mapped = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &loaded) && loaded != 0;
    CloseHandle(thread);
    if (mapped)
    {
        // LoadLibraryW has returned: it consumed its argument, so the remote
        // buffer is safe to release.
        VirtualFreeEx(pi->hProcess, remote, 0, MEM_RELEASE);
    }
    else
    {
        // The remote thread may still be inside LoadLibraryW with `remote` as
        // its argument. Freeing it now would race that thread and can crash
        // the game, so the buffer is deliberately retained: the child is kept
        // alive (a failed follow runs unbridged, unlike the session launcher,
        // which may terminate its child), and the OS reclaims the process
        // memory when the child exits.
        Log("[follow] hook mapping timed out in child %lu; runs unbridged "
            "(remote buffer retained until child exit)",
            (unsigned long)pi->dwProcessId);
        return;
    }
    if (!ChildRemoteInitialize(pi->hProcess, pi->dwProcessId, dll))
    {
        Log("[follow] hook initialization failed in child %lu; runs unbridged",
            (unsigned long)pi->dwProcessId);
        return;
    }
    Log("[follow] bridged child %lu (%hs)", (unsigned long)pi->dwProcessId,
        g_cfg.target_executable);
}

static BOOL WINAPI CreateProcessAndFollowW(LPCWSTR app, LPWSTR cmd,
    LPSECURITY_ATTRIBUTES process_attrs, LPSECURITY_ATTRIBUTES thread_attrs,
    BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION info)
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using Fn = BOOL (WINAPI *)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW,
        LPPROCESS_INFORMATION);
    auto real = kernel ? reinterpret_cast<Fn>(GetProcAddress(kernel, "CreateProcessW")) : nullptr;
    if (!real) return FALSE;
    if (!ChildFollowEnabled())
        return real(app, cmd, process_attrs, thread_attrs, inherit, flags,
                    env, dir, startup, info);
    wchar_t token[MAX_PATH] = {};
    const wchar_t *candidate = app && *app ? app : (ChildFirstTokenW(cmd, token, _countof(token)), token);
    // Follow every child, not just the target: an intermediate that is not
    // bridged could not intercept the CreateProcess that spawns the renderer.
    // The target match only labels the log; bridging itself is driven by the
    // NGX exports each process actually carries.
    const bool matched = ChildNameMatchesW(candidate);
    const bool added_suspend = !(flags & CREATE_SUSPENDED);
    BOOL ok = real(app, cmd, process_attrs, thread_attrs, inherit,
                   flags | CREATE_SUSPENDED, env, dir, startup, info);
    if (!ok || !info || !info->hProcess) return ok;
    Log("[follow] %s child %ls (pid %lu); injecting session hook",
        matched ? "matched target" : "following intermediate",
        ChildBaseNameW(candidate), (unsigned long)info->dwProcessId);
    FollowInjectChild(info);
    // Never hold the child suspended on the bridge's account: a failed follow
    // still starts the game, only unbridged.
    if (added_suspend && ResumeThread(info->hThread) == static_cast<DWORD>(-1))
        Log("[follow] ResumeThread failed for child %lu (Win32 %lu)",
            (unsigned long)info->dwProcessId, GetLastError());
    return ok;
}

static BOOL WINAPI CreateProcessAndFollowA(LPCSTR app, LPSTR cmd,
    LPSECURITY_ATTRIBUTES process_attrs, LPSECURITY_ATTRIBUTES thread_attrs,
    BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir,
    LPSTARTUPINFOA startup, LPPROCESS_INFORMATION info)
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using Fn = BOOL (WINAPI *)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA,
        LPPROCESS_INFORMATION);
    auto real = kernel ? reinterpret_cast<Fn>(GetProcAddress(kernel, "CreateProcessA")) : nullptr;
    if (!real) return FALSE;
    if (!ChildFollowEnabled())
        return real(app, cmd, process_attrs, thread_attrs, inherit, flags,
                    env, dir, startup, info);
    char token[MAX_PATH] = {};
    const char *candidate = app && *app ? app : (ChildFirstTokenA(cmd, token, sizeof(token)), token);
    // Follow every child, not just the target (see the wide variant above).
    const bool matched = ChildNameMatchesA(candidate);
    const bool added_suspend = !(flags & CREATE_SUSPENDED);
    BOOL ok = real(app, cmd, process_attrs, thread_attrs, inherit,
                   flags | CREATE_SUSPENDED, env, dir, startup, info);
    if (!ok || !info || !info->hProcess) return ok;
    Log("[follow] %s ANSI child (pid %lu); injecting session hook",
        matched ? "matched target" : "following intermediate",
        (unsigned long)info->dwProcessId);
    FollowInjectChild(info);
    if (added_suspend && ResumeThread(info->hThread) == static_cast<DWORD>(-1))
        Log("[follow] ResumeThread failed for child %lu (Win32 %lu)",
            (unsigned long)info->dwProcessId, GetLastError());
    return ok;
}
