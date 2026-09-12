#define INITGUID
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr, const char* step) {
    if (FAILED(hr)) { fprintf(stderr, "%s failed: 0x%08lx\n", step, (unsigned long)hr); std::exit(1); }
}
struct Queue {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 serial = 0;
    explicit Queue(ID3D12Device* device) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        check(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        check(list->Close(), "initial Close");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!event) std::exit(1);
    }
    ~Queue() { if (event) CloseHandle(event); }
    void begin() {
        check(allocator->Reset(), "allocator Reset");
        check(list->Reset(allocator.Get(), nullptr), "list Reset");
    }
    void finish() {
        check(list->Close(), "Close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++serial), "Signal");
        check(fence->SetEventOnCompletion(serial, event), "SetEventOnCompletion");
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) {
            fprintf(stderr, "GPU fence timed out\n"); std::exit(1);
        }
    }
};
static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 bytes,
        D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES props{}; props.Type = type;
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    auto desc = buffer_desc(bytes);
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource");
    return r;
}
static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}
int main(int argc, char** argv) {
    const UINT width = argc > 1 ? (UINT)std::strtoul(argv[1], nullptr, 10) : 1920;
    const UINT height = argc > 2 ? (UINT)std::strtoul(argv[2], nullptr, 10) : 1080;
    if (!width || !height || width > 7680 || height > 4320) return 2;
    const UINT64 bytes = UINT64(width) * height * 4;
    const SIZE_T allocation = (SIZE_T)((bytes + 65535) & ~UINT64(65535));
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    std::vector<ComPtr<ID3D12Device3>> devices;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        check(hr, "EnumAdapters1");
        DXGI_ADAPTER_DESC1 desc{};
        check(adapter->GetDesc1(&desc), "GetDesc1");
        if (desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        ComPtr<ID3D12Device3> device;
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        const LUID actual = device->GetAdapterLuid();
        if (actual.HighPart != desc.AdapterLuid.HighPart || actual.LowPart != desc.AdapterLuid.LowPart) {
            fprintf(stderr, "D3D12 device does not match requested adapter\n"); return 2;
        }
        if (!devices.empty()) {
            const LUID first = devices[0]->GetAdapterLuid();
            if (actual.HighPart == first.HighPart && actual.LowPart == first.LowPart) {
                fprintf(stderr, "Both devices refer to the same adapter\n"); return 2;
            }
        }
        printf("physical adapter %u LUID=%08lx:%08lx\n", i,
            (unsigned long)desc.AdapterLuid.HighPart, (unsigned long)desc.AdapterLuid.LowPart);
        devices.push_back(device);
    }
    if (devices.size() != 2) { fprintf(stderr, "Expected exactly two NVIDIA adapters\n"); return 2; }
    void* address = VirtualAlloc(nullptr, allocation, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!address) return 1;
    {
        ComPtr<ID3D12Heap> heaps[2];
        ComPtr<ID3D12Resource> shared[2];
        auto desc = buffer_desc(bytes);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        for (int i = 0; i < 2; ++i) {
            check(devices[i]->OpenExistingHeapFromAddress(address, IID_PPV_ARGS(&heaps[i])), "OpenExistingHeapFromAddress");
            check(devices[i]->CreatePlacedResource(heaps[i].Get(), 0, &desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&shared[i])), "CreatePlacedResource");
            printf("host memory imported on GPU %d\n", i);
        }
        for (int direction = 0; direction < 2; ++direction) {
            const int src = direction, dst = 1-direction;
            Queue producer(devices[src].Get()), consumer(devices[dst].Get());
            auto upload = buffer(devices[src].Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            auto source = buffer(devices[src].Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
            auto target = buffer(devices[dst].Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
            auto readback = buffer(devices[dst].Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            std::vector<unsigned char> expected((size_t)bytes);
            double total_ms = 0;
            for (unsigned frame = 0; frame < 16; ++frame) {
                for (size_t i = 0; i < expected.size(); ++i)
                    expected[i] = (unsigned char)((i * 37 + (i >> 8) + frame * 53 + direction * 17) & 255);
                void* mapped = nullptr;
                D3D12_RANGE no_read{0, 0};
                check(upload->Map(0, &no_read, &mapped), "upload Map");
                std::memcpy(mapped, expected.data(), expected.size());
                upload->Unmap(0, nullptr);
                producer.begin();
                producer.list->CopyBufferRegion(source.Get(), 0, upload.Get(), 0, bytes);
                transition(producer.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                producer.finish();
                auto start = std::chrono::steady_clock::now();
                producer.begin();
                transition(producer.list.Get(), shared[src].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                producer.list->CopyBufferRegion(shared[src].Get(), 0, source.Get(), 0, bytes);
                transition(producer.list.Get(), shared[src].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                producer.finish();
                consumer.begin();
                transition(consumer.list.Get(), shared[dst].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                consumer.list->CopyBufferRegion(target.Get(), 0, shared[dst].Get(), 0, bytes);
                transition(consumer.list.Get(), shared[dst].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                consumer.finish();
                total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
                consumer.begin();
                transition(consumer.list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                consumer.list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, bytes);
                transition(consumer.list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                consumer.finish();
                D3D12_RANGE read_range{0, (SIZE_T)bytes};
                check(readback->Map(0, &read_range, &mapped), "readback Map");
                bool correct = !std::memcmp(mapped, expected.data(), expected.size());
                readback->Unmap(0, &no_read);
                if (!correct) { fprintf(stderr, "DATA MISMATCH direction=%d frame=%u\n", direction, frame); return 3; }
                producer.begin();
                transition(producer.list.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                producer.finish();
            }
            printf("PASS GPU%d->GPU%d %ux%u RGBA-equivalent bytes=%llu frames=16 byte-exact transfer_mean_ms=%.3f\n",
                src, dst, width, height, (unsigned long long)bytes, total_ms/16);
        }
    }
    VirtualFree(address, 0, MEM_RELEASE);
    printf("Transport only: CPU waits on GPU fences; no CPU copy between GPUs; no neural rendering tested.\n");
    return 0;
}
