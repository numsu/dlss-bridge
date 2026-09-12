#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <detours.h>
extern "C" __declspec(dllexport) LONG smoke(void **p, void *d) {
    LONG r = DetourTransactionBegin();
    if (r == NO_ERROR) r = DetourUpdateThread(GetCurrentThread());
    if (r == NO_ERROR) r = DetourAttach(p, d);
    if (r == NO_ERROR) r = DetourTransactionAbort();
    return r;
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, void*) { return TRUE; }
