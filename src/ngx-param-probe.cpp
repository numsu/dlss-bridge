#include <windows.h>
#include <cstdio>

struct ID3D11Resource;
struct ID3D12Resource;
typedef int NVSDK_NGX_Result;
static constexpr NVSDK_NGX_Result NGX_SUCCESS = 1;

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char *, unsigned long long) = 0;
    virtual void Set(const char *, float) = 0;
    virtual void Set(const char *, double) = 0;
    virtual void Set(const char *, unsigned int) = 0;
    virtual void Set(const char *, int) = 0;
    virtual void Set(const char *, ID3D11Resource *) = 0;
    virtual void Set(const char *, ID3D12Resource *) = 0;
    virtual void Set(const char *, void *) = 0;
    virtual NVSDK_NGX_Result Get(const char *, unsigned long long *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, float *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, double *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, unsigned int *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, int *) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, ID3D11Resource **) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, ID3D12Resource **) const = 0;
    virtual NVSDK_NGX_Result Get(const char *, void **) const = 0;
    virtual void Reset() = 0;
};

using Allocate = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter **);
using Destroy = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter *);

int main()
{
    HMODULE module = LoadLibraryW(L"OptiScaler.dll");
    if (!module) { std::printf("LoadLibrary failed %lu\n", GetLastError()); return 2; }
    auto allocate = reinterpret_cast<Allocate>(GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
    auto destroy = reinterpret_cast<Destroy>(GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (!allocate) { std::puts("AllocateParameters export missing"); return 3; }
    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Result result = allocate(&params);
    if (result != NGX_SUCCESS || !params) { std::printf("allocate failed %08x\n", (unsigned)result); return 4; }

    void *expected = reinterpret_cast<void *>(0x12345678ull);
    params->Set("Bridge.ResourceProbe", expected);
    void *actual = nullptr;
    result = params->Get("Bridge.ResourceProbe", &actual);
    std::printf("void resource round-trip: result=%08x expected=%p actual=%p\n",
                (unsigned)result, expected, actual);
    const bool ok = result == NGX_SUCCESS && actual == expected;
    if (destroy) destroy(params);
    FreeLibrary(module);
    return ok ? 0 : 5;
}
