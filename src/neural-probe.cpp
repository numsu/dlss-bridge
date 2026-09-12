#define INITGUID
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx_params.h>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
using Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version);
using Caps = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using SnippetInit = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned index = argc > 1 ? (unsigned)std::strtoul(argv[1], nullptr, 10) : 0;
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return 1;
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(index, &adapter);
    if (FAILED(hr)) { printf("No adapter %u\n", index); return 1; }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    printf("adapter=%u vendor=%04x LUID=%08lx:%08lx\n",index,desc.VendorId,(unsigned long)desc.AdapterLuid.HighPart,(unsigned long)desc.AdapterLuid.LowPart);
    if (desc.VendorId != 0x10de || desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return 1;
    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    printf("D3D12CreateDevice=0x%08lx\n",(unsigned long)hr);
    if (FAILED(hr)) return 1;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) { printf("NVAPI LoadLibrary failed error=%lu\n",GetLastError()); return 2; }
    auto query = reinterpret_cast<void* (*)(unsigned)>(GetProcAddress(nvapi,"nvapi_QueryInterface"));
    if (!query) return 2;
    auto nvinit = reinterpret_cast<int (*)()>(query(0x0150e828));
    if (!nvinit) return 2;
    printf("NvAPI_Initialize=%d\n",nvinit());
    HMODULE core = LoadLibraryW(L"_nvngx.dll");
    if (!core) { printf("core LoadLibrary failed error=%lu\n",GetLastError()); return 2; }
    auto init = reinterpret_cast<Init>(GetProcAddress(core,"NVSDK_NGX_D3D12_Init"));
    auto caps = reinterpret_cast<Caps>(GetProcAddress(core,"NVSDK_NGX_D3D12_GetCapabilityParameters"));
    if (!init || !caps) { printf("Missing NGX core entry points\n"); return 2; }
    printf("core loaded SDK=0x%08x\n", (unsigned)NVSDK_NGX_Version_API);
    auto result = init(0, L"Z:\\work\\runtime\\neural",device.Get(),NVSDK_NGX_Version_API);
    printf("core Init=0x%08x\n", (unsigned)result);
    if (result != NVSDK_NGX_Result_Success) return 3;
    NVSDK_NGX_Parameter* params = nullptr;
    result = caps(&params);
    printf("GetCapabilityParameters=0x%08x nonnull=%d\n", (unsigned)result, params != nullptr);
    if (result != NVSDK_NGX_Result_Success || !params) return 3;
    HMODULE snippet = LoadLibraryW(L"nvngx_dlssnr.dll");
    if (!snippet) { printf("snippet LoadLibrary failed error=%lu\n",GetLastError()); return 4; }
    auto nr_init = reinterpret_cast<SnippetInit>(GetProcAddress(snippet,"NVSDK_NGX_D3D12_Init_Ext"));
    if (!nr_init) { printf("Missing neural Init_Ext\n"); return 4; }
    result = nr_init(0,L"Z:\\work\\runtime\\neural",device.Get(),NVSDK_NGX_Version_API,params);
    printf("snippet Init_Ext=0x%08x\n",(unsigned)result);
    printf("Initialization diagnostic only. Feature creation and evaluation have NOT been tested.\n");
    return result == NVSDK_NGX_Result_Success ? 0 : 5;
}
