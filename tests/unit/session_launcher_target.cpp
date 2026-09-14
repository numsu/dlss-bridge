#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
int wmain()
{
    HMODULE hook = GetModuleHandleW(L"dlss5-vk-hook.dll");
    std::printf("hook_loaded=%s\n", hook ? "yes" : "no");
    return hook ? 0 : 9;
}
