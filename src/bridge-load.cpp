#include <windows.h>
#include <cstdio>

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }
    FARPROC symbol = GetProcAddress(module, argv[2]);
    std::printf("module=%p symbol=%p error=%lu\n", (void *)module, (void *)symbol, GetLastError());
    FreeLibrary(module);
    return symbol ? 0 : 4;
}
