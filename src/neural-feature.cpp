#define INITGUID
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx_params.h>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;

using NgxInit = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version);
using NgxCaps = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using NrCreate = void* (*)(const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*,
                           void*, unsigned, unsigned, int, float, int, float, float, float, int, int);
using NrEvaluate = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                           ID3D12Resource*, ID3D12Resource*, unsigned, unsigned, unsigned, unsigned,
                           int, int, float, int, float, float, float, int, float, float);
using NrRelease = void (*)(void*);
using NrProbeFloat = void (*)(void*, const char*, float, int);
using NrSetFloatSlot = void (*)(int);

static bool waitQueue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const UINT64 value = 1;
    HRESULT hr = queue->Signal(fence.Get(), value);
    if (SUCCEEDED(hr) && fence->GetCompletedValue() < value) {
        hr = fence->SetEventOnCompletion(value, event);
        if (SUCCEEDED(hr) && WaitForSingleObject(event, 60000) != WAIT_OBJECT_0) hr = E_FAIL;
    }
    CloseHandle(event);
    return SUCCEEDED(hr);
}

static ComPtr<ID3D12Resource> texture(ID3D12Device* device, unsigned width, unsigned height,
                                      DXGI_FORMAT format, bool output) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> result;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr, IID_PPV_ARGS(&result));
    if (FAILED(hr)) std::printf("texture format=%u output=%d failed=0x%08lx\n", (unsigned)format,
                                output ? 1 : 0, (unsigned long)hr);
    return result;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const unsigned index = argc > 1 ? (unsigned)std::strtoul(argv[1], nullptr, 10) : 0;
    const unsigned width = argc > 2 ? (unsigned)std::strtoul(argv[2], nullptr, 10) : 320;
    const unsigned height = argc > 3 ? (unsigned)std::strtoul(argv[3], nullptr, 10) : 180;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(index, &adapter))) return 1;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::printf("adapter=%u vendor=%04x LUID=%08lx:%08lx model=%ux%u\n", index, desc.VendorId,
                (unsigned long)desc.AdapterLuid.HighPart, (unsigned long)desc.AdapterLuid.LowPart,
                width, height);

    ComPtr<ID3D12Device> device;
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    std::printf("D3D12CreateDevice=0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)))) return 1;

    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) { std::printf("NVAPI LoadLibrary failed error=%lu\n", GetLastError()); return 2; }
    auto query = reinterpret_cast<void* (*)(unsigned)>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    auto nvinit = query ? reinterpret_cast<int (*)()>(query(0x0150e828)) : nullptr;
    std::printf("NvAPI_Initialize=%d\n", nvinit ? nvinit() : -1);

    HMODULE core = LoadLibraryW(L"_nvngx.dll");
    auto ngxInit = core ? reinterpret_cast<NgxInit>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init")) : nullptr;
    auto ngxCaps = core ? reinterpret_cast<NgxCaps>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters")) : nullptr;
    if (!ngxInit || !ngxCaps) return 2;
    NVSDK_NGX_Result result = ngxInit(0x24480451ull, L"Z:\\work\\runtime\\neural", device.Get(), NVSDK_NGX_Version_API);
    std::printf("core Init=0x%08x\n", (unsigned)result);
    if (result != NVSDK_NGX_Result_Success) return 3;
    NVSDK_NGX_Parameter* params = nullptr;
    result = ngxCaps(&params);
    std::printf("GetCapabilityParameters=0x%08x nonnull=%d\n", (unsigned)result, params != nullptr);
    if (result != NVSDK_NGX_Result_Success || !params) return 3;

    HMODULE forwarder = LoadLibraryW(L"Z:\\work\\runtime\\optiscaler\\nvngx.dll_dlssnr.dll");
    if (!forwarder) { std::printf("forwarder load failed error=%lu\n", GetLastError()); return 4; }
    auto create = reinterpret_cast<NrCreate>(GetProcAddress(forwarder, "dlssnr_call_create"));
    auto evaluate = reinterpret_cast<NrEvaluate>(GetProcAddress(forwarder, "dlssnr_call_evaluate"));
    auto release = reinterpret_cast<NrRelease>(GetProcAddress(forwarder, "dlssnr_call_release"));
    auto probeFloat = reinterpret_cast<NrProbeFloat>(GetProcAddress(forwarder, "dlssnr_call_probe_float"));
    auto setFloatSlot = reinterpret_cast<NrSetFloatSlot>(GetProcAddress(forwarder, "dlssnr_call_set_float_slot"));
    auto lastInit = reinterpret_cast<int*>(GetProcAddress(forwarder, "dlssnr_call_last_init"));
    auto lastCreate = reinterpret_cast<int*>(GetProcAddress(forwarder, "dlssnr_call_last_create"));
    if (!create || !evaluate || !release || !probeFloat || !setFloatSlot) return 4;

    const int candidates[] = {1, 2, 5, 6, 7, 4, 3, 0};
    int floatSlot = -1;
    for (int slot : candidates) {
        float readBack = 0.0f;
        probeFloat(params, "DLSSNR.ContainerFloatProbe", 0.375f, slot);
        const auto getResult = params->Get("DLSSNR.ContainerFloatProbe", &readBack);
        std::printf("float probe slot=%d result=0x%08x value=%g\n", slot, (unsigned)getResult, readBack);
        if (getResult == NVSDK_NGX_Result_Success && readBack == 0.375f) {
            floatSlot = slot; setFloatSlot(slot); break;
        }
    }
    std::printf("floatSlot=%d\n", floatSlot);

    void* feature = create(L"Z:\\work\\runtime\\neural\\nvngx_dlssnr.dll",
                           L"Z:\\work\\runtime\\neural", device.Get(), list.Get(), params,
                           width, height, 0, 1.0f, 0, 1.0f, 1.0f, -1.0f, 1, 1);
    std::printf("feature=%p init=0x%08x create=0x%08x\n", feature,
                lastInit ? (unsigned)*lastInit : 0u, lastCreate ? (unsigned)*lastCreate : 0u);
    if (!feature) return 5;

    hr = list->Close();
    if (FAILED(hr)) return 6;
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (!waitQueue(device.Get(), queue.Get())) { std::printf("creation submit timed out\n"); return 6; }
    std::printf("feature creation submitted successfully\n");

    auto color = texture(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    auto depth = texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, false);
    auto motion = texture(device.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, false);
    auto output = texture(device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    if (!color || !depth || !motion || !output) return 7;

    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return 7;
    const int eval = evaluate(list.Get(), feature, params, color.Get(), depth.Get(), motion.Get(),
                              output.Get(), width, height, width, height, 0, 1, 1.0f, 0,
                              1.0f, 1.0f, -1.0f, 1, 1.0f, 1.0f);
    std::printf("evaluate=0x%08x\n", (unsigned)eval);
    if (FAILED(list->Close())) return 8;
    queue->ExecuteCommandLists(1, lists);
    const bool completed = waitQueue(device.Get(), queue.Get());
    const HRESULT removed = device->GetDeviceRemovedReason();
    std::printf("evaluation fence=%s device=0x%08lx\n", completed ? "complete" : "timeout",
                (unsigned long)removed);
    if (!completed || FAILED(removed) || eval != 1) return 8;

    release(feature);
    std::printf("DLSS Neural Rendering feature evaluated successfully\n");
    return 0;
}
