#define INITGUID
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
using Microsoft::WRL::ComPtr;

static bool ok(HRESULT hr, const char *step)
{
    std::printf("%s=0x%08lx\n", step, (unsigned long)hr);
    return SUCCEEDED(hr);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const UINT adapter_index = argc > 1 ? (UINT)std::strtoul(argv[1], nullptr, 10) : 1;
    const UINT width = argc > 2 ? (UINT)std::strtoul(argv[2], nullptr, 10) : 1920;
    const UINT height = argc > 3 ? (UINT)std::strtoul(argv[3], nullptr, 10) : 1080;

    ComPtr<IDXGIFactory4> factory;
    if (!ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return 2;
    ComPtr<IDXGIAdapter1> adapter;
    if (!ok(factory->EnumAdapters1(adapter_index, &adapter), "EnumAdapters1")) return 2;
    DXGI_ADAPTER_DESC1 ad = {};
    adapter->GetDesc1(&ad);
    std::printf("adapter=%u vendor=0x%04x device=0x%04x\n", adapter_index, ad.VendorId, ad.DeviceId);

    ComPtr<ID3D12Device3> dev;
    if (!ok(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                              IID_PPV_ARGS(&dev)), "D3D12CreateDevice")) return 2;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    HRESULT feature_hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                                   &options, sizeof(options));
    std::printf("CrossAdapterRowMajorTextureSupported_hr=0x%08lx value=%u\n",
                (unsigned long)feature_hr,
                SUCCEEDED(feature_hr) && options.CrossAdapterRowMajorTextureSupported ? 1u : 0u);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    td.Flags = (D3D12_RESOURCE_FLAGS)(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                                      D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);

    D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &td);
    std::printf("allocation_size=%llu alignment=%llu\n",
                (unsigned long long)ai.SizeInBytes, (unsigned long long)ai.Alignment);
    if (ai.SizeInBytes == 0 || ai.SizeInBytes == UINT64_MAX) return 3;
    const SIZE_T bytes = (SIZE_T)((ai.SizeInBytes + 65535ull) & ~65535ull);
    void *host = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!host) { std::printf("VirtualAlloc_failed=%lu\n", GetLastError()); return 3; }
    std::memset(host, 0, bytes);

    ComPtr<ID3D12Heap> heap;
    HRESULT hr = dev->OpenExistingHeapFromAddress(host, IID_PPV_ARGS(&heap));
    if (!ok(hr, "OpenExistingHeapFromAddress")) { VirtualFree(host, 0, MEM_RELEASE); return 4; }

    ComPtr<ID3D12Resource> output;
    hr = dev->CreatePlacedResource(heap.Get(), 0, &td, D3D12_RESOURCE_STATE_COMMON,
                                   nullptr, IID_PPV_ARGS(&output));
    if (!ok(hr, "CreatePlacedResource_row_major_uav"))
    {
        VirtualFree(host, 0, MEM_RELEASE);
        return 5;
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    if (!ok(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&descriptors)),
            "CreateDescriptorHeap")) return 6;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = td.Format;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = descriptors->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = descriptors->GetGPUDescriptorHandleForHeapStart();
    dev->CreateUnorderedAccessView(output.Get(), nullptr, &ud, cpu);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    if (!ok(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue") ||
        !ok(dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc)), "CreateCommandAllocator") ||
        !ok(dev->CreateCommandList(0, qd.Type, alloc.Get(), nullptr,
                                   IID_PPV_ARGS(&list)), "CreateCommandList") ||
        !ok(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                             IID_PPV_ARGS(&fence)), "CreateFence")) return 6;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 6;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    ID3D12DescriptorHeap *active[] = { descriptors.Get() };
    list->SetDescriptorHeaps(1, active);
    const float clear[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    list->ClearUnorderedAccessViewFloat(gpu, cpu, output.Get(), clear, 0, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1, &barrier);
    if (!ok(list->Close(), "CommandList_Close")) return 7;
    ID3D12CommandList *lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    if (!ok(queue->Signal(fence.Get(), 1), "Queue_Signal") ||
        !ok(fence->SetEventOnCompletion(1, event), "Fence_SetEvent")) return 7;
    DWORD wait = WaitForSingleObject(event, 10000);
    std::printf("wait=%lu completed=%llu device=0x%08lx\n", (unsigned long)wait,
                (unsigned long long)fence->GetCompletedValue(),
                (unsigned long)dev->GetDeviceRemovedReason());
    const unsigned short *v = static_cast<const unsigned short *>(host);
    std::printf("first_pixel_half=%04x,%04x,%04x,%04x\n", v[0], v[1], v[2], v[3]);
    const bool pass = wait == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1 &&
                      SUCCEEDED(dev->GetDeviceRemovedReason()) &&
                      v[0] == 0x3c00 && v[1] == 0x3800 &&
                      v[2] == 0x3400 && v[3] == 0x3c00;
    std::printf("RESULT=%s\n", pass ? "PASS" : "FAIL");
    CloseHandle(event);
    output.Reset();
    heap.Reset();
    VirtualFree(host, 0, MEM_RELEASE);
    return pass ? 0 : 8;
}
