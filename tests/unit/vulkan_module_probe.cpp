#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

int main()
{
    HMODULE engine = LoadLibraryW(L"vulkan-engine-probe.dll");
    if (!engine)
    {
        std::printf("engine_load_error=%lu\n", GetLastError());
        return 2;
    }
    using Run = int (*)();
    auto run = reinterpret_cast<Run>(GetProcAddress(engine, "RunVulkanModuleProbe"));
    if (!run) return 3;
    // Deliberately call immediately: the launch hook must patch the newly
    // loaded renderer before LoadLibrary returns to this executable.
    const int result = run();
    std::printf("late_module_result=%d\n", result);
    FreeLibrary(engine);
    return result;
}
