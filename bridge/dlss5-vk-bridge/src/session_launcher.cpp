#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

static std::wstring Quote(const wchar_t *value)
{
    std::wstring input(value), output;
    if (!input.empty() && input.find_first_of(L" \t\"") == std::wstring::npos) return input;
    output.push_back(L'\"');
    size_t slashes = 0;
    for (wchar_t ch : input)
    {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"')
        {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(L'\"');
            slashes = 0;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0;
        output.push_back(ch);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

static void PrintError(const wchar_t *operation, DWORD error = GetLastError())
{
    wchar_t *message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    std::fwprintf(stderr, L"dlss-bridge-launcher: %ls failed (%lu)%ls%ls\n",
                  operation, error, message ? L": " : L"", message ? message : L"");
    if (message) LocalFree(message);
}

static LPTHREAD_START_ROUTINE RemoteLoadLibraryW(DWORD process_id)
{
    HMODULE local_kernel = GetModuleHandleW(L"kernel32.dll");
    auto local_load = local_kernel ? GetProcAddress(local_kernel, "LoadLibraryW") : nullptr;
    if (!local_load) return nullptr;
    const uintptr_t offset = reinterpret_cast<uintptr_t>(local_load) -
        reinterpret_cast<uintptr_t>(local_kernel);

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
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
    // Wine does not expose kernel32 in the snapshot until the suspended
    // process begins loader initialization. Windows and Wine map this system
    // module at a process-independent address, so the local function address
    // is the required pre-entry form when enumeration is not ready yet.
    return remote ? remote : reinterpret_cast<LPTHREAD_START_ROUTINE>(local_load);
}

static uintptr_t RemoteModuleBase(DWORD process_id, const std::wstring &dll)
{
    wchar_t expected[32768] = {};
    if (!GetFullPathNameW(dll.c_str(), 32768, expected, nullptr)) return 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uintptr_t base = 0;
    if (Module32FirstW(snapshot, &entry))
        do {
            wchar_t actual[32768] = {};
            if (GetFullPathNameW(entry.szExePath, 32768, actual, nullptr) &&
                _wcsicmp(actual, expected) == 0) {
                base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return base;
}

static bool InitializeRemoteHook(HANDLE process, DWORD process_id, const std::wstring &dll)
{
    const uintptr_t remote_base = RemoteModuleBase(process_id, dll);
    if (!remote_base) {
        PrintError(L"locate injected hook module", ERROR_MOD_NOT_FOUND);
        return false;
    }
    HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) { PrintError(L"map hook for export lookup"); return false; }
    FARPROC local_init = GetProcAddress(local, "DLSSBridgeInitialize");
    if (!local_init) {
        PrintError(L"locate DLSSBridgeInitialize", ERROR_PROC_NOT_FOUND);
        FreeLibrary(local);
        return false;
    }
    const uintptr_t offset = reinterpret_cast<uintptr_t>(local_init) -
        reinterpret_cast<uintptr_t>(local);
    FreeLibrary(local);
    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_base + offset), nullptr, 0, nullptr);
    if (!thread) { PrintError(L"start hook initialization"); return false; }
    DWORD result = 0;
    const DWORD wait = WaitForSingleObject(thread, 30000);
    const bool ok = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result) && result == 1;
    if (!ok) {
        if (wait == WAIT_TIMEOUT) std::fwprintf(stderr, L"dlss-bridge-launcher: hook initialization timed out\n");
        else PrintError(L"DLSSBridgeInitialize in game process");
    }
    CloseHandle(thread);
    return ok;
}

static bool Inject(HANDLE process, DWORD process_id, const std::wstring &dll)
{
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) { PrintError(L"VirtualAllocEx"); return false; }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dll.c_str(), bytes, &written) || written != bytes)
    {
        PrintError(L"WriteProcessMemory");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    auto load_library = RemoteLoadLibraryW(process_id);
    if (!load_library)
    {
        PrintError(L"locate LoadLibraryW in game process");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    HANDLE thread = CreateRemoteThread(
        process, nullptr, 0, load_library,
        remote, 0, nullptr);
    if (!thread)
    {
        PrintError(L"CreateRemoteThread");
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    DWORD wait = WaitForSingleObject(thread, 30000);
    DWORD loaded = 0;
    bool ok = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &loaded) && loaded != 0;
    if (!ok)
    {
        if (wait == WAIT_TIMEOUT)
            std::fwprintf(stderr, L"dlss-bridge-launcher: hook injection timed out\n");
        else
            PrintError(L"LoadLibraryW in game process");
    }
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok && InitializeRemoteHook(process, process_id, dll);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 5 || std::wcscmp(argv[1], L"--hook") != 0 ||
        std::wcscmp(argv[3], L"--") != 0)
    {
        std::fwprintf(stderr,
            L"usage: dlss-bridge-launcher.exe --hook ABSOLUTE_DLL -- GAME.exe [arguments...]\n");
        return 2;
    }

    wchar_t hook_path[32768];
    DWORD hook_length = GetFullPathNameW(argv[2], 32768, hook_path, nullptr);
    if (hook_length == 0 || hook_length >= 32768)
    {
        PrintError(L"GetFullPathNameW(hook)");
        return 2;
    }
    DWORD attributes = GetFileAttributesW(hook_path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        PrintError(L"hook lookup", ERROR_FILE_NOT_FOUND);
        return 2;
    }

    std::wstring command_line;
    for (int i = 4; i < argc; ++i)
    {
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line += Quote(argv[i]);
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(argv[4], mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED, nullptr, nullptr, &startup, &child))
    {
        PrintError(L"CreateProcessW(game)");
        return 3;
    }

    if (!Inject(child.hProcess, child.dwProcessId, hook_path))
    {
        TerminateProcess(child.hProcess, 4);
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return 4;
    }
    if (ResumeThread(child.hThread) == static_cast<DWORD>(-1))
    {
        PrintError(L"ResumeThread");
        TerminateProcess(child.hProcess, 5);
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return 5;
    }
    CloseHandle(child.hThread);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD exit_code = 1;
    if (!GetExitCodeProcess(child.hProcess, &exit_code)) PrintError(L"GetExitCodeProcess");
    CloseHandle(child.hProcess);
    return static_cast<int>(exit_code);
}
