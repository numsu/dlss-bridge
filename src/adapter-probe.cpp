#define INITGUID
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <cstdio>
#include <vector>

int main() {
    IDXGIFactory4 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { printf("factory failed: 0x%08lx\n", (unsigned long)hr); return 1; }
    std::vector<ID3D12Device*> devices;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
        printf("adapter=%u name=%s vendor=%04x luid=%08lx:%08lx software=%u\n", i, name, desc.VendorId,
            (unsigned long)desc.AdapterLuid.HighPart, (unsigned long)desc.AdapterLuid.LowPart,
            !!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
        if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            ID3D12Device *device = nullptr;
            hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            printf("D3D12CreateDevice=0x%08lx\n", (unsigned long)hr);
            if (SUCCEEDED(hr)) devices.push_back(device);
        }
        adapter->Release();
    }
    int result = 2;
    if (devices.size() == 2) {
        D3D12_HEAP_DESC desc{};
        desc.SizeInBytes = 65536;
        desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
        ID3D12Heap *heap = nullptr, *opened = nullptr;
        HANDLE handle = nullptr;
        hr = devices[0]->CreateHeap(&desc, IID_PPV_ARGS(&heap));
        printf("CreateHeap=0x%08lx\n", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            hr = devices[0]->CreateSharedHandle(heap, nullptr, GENERIC_ALL, nullptr, &handle);
            printf("CreateSharedHandle=0x%08lx\n", (unsigned long)hr);
        }
        if (SUCCEEDED(hr)) {
            hr = devices[1]->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
            printf("OpenSharedHandle=0x%08lx\n", (unsigned long)hr);
        }
        if (opened) opened->Release();
        if (handle) CloseHandle(handle);
        if (heap) heap->Release();
        result = SUCCEEDED(hr) ? 0 : 3;
    }
    printf("nvidia_d3d12_devices=%zu result=%d (resource creation only; no frame transfer or neural execution tested)\n", devices.size(), result);
    for (auto device : devices) device->Release();
    factory->Release();
    return result;
}
