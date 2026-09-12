#define INITGUID
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <nvsdk_ngx_params.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
using Microsoft::WRL::ComPtr;

static void check(HRESULT hr, const char* step) {
    if (FAILED(hr)) { std::fprintf(stderr, "%s failed: 0x%08lx\n", step, (unsigned long)hr); std::exit(1); }
}
struct Queue {
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr; UINT64 serial = 0;
    explicit Queue(ID3D12Device* d) {
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(d->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        check(d->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        check(d->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        check(list->Close(), "initial Close");
        check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event) std::exit(1);
    }
    ~Queue() { if (event) CloseHandle(event); }
    void begin() { check(allocator->Reset(), "Reset allocator"); check(list->Reset(allocator.Get(), nullptr), "Reset list"); }
    void finish() {
        check(list->Close(), "Close list"); ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists); check(queue->Signal(fence.Get(), ++serial), "Signal");
        check(fence->SetEventOnCompletion(serial, event), "Fence event");
        if (WaitForSingleObject(event, 60000) != WAIT_OBJECT_0) { std::fprintf(stderr, "GPU timeout\n"); std::exit(1); }
    }
};
static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; l->ResourceBarrier(1, &b);
}
static ComPtr<ID3D12Resource> texture(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT fmt,
                                      D3D12_RESOURCE_STATES state, bool uav = false) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r; check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                               IID_PPV_ARGS(&r)), "Create texture"); return r;
}
static D3D12_RESOURCE_DESC bufferDesc(UINT64 bytes) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER; return d;
}
struct Plane { UINT64 offset; UINT rowPitch; UINT width; UINT height; DXGI_FORMAT format; };
static Plane plane(UINT64& cursor, UINT width, UINT height, DXGI_FORMAT format, UINT bpp) {
    cursor = (cursor + 511) & ~UINT64(511); Plane p{cursor, (width * bpp + 255u) & ~255u, width, height, format};
    cursor += UINT64(p.rowPitch) * height; return p;
}
static void textureToBuffer(ID3D12GraphicsCommandList* l, ID3D12Resource* tex, ID3D12Resource* buf, const Plane& p) {
    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = tex; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = buf; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Offset = p.offset; d.PlacedFootprint.Footprint.Format = p.format;
    d.PlacedFootprint.Footprint.Width = p.width; d.PlacedFootprint.Footprint.Height = p.height;
    d.PlacedFootprint.Footprint.Depth = 1; d.PlacedFootprint.Footprint.RowPitch = p.rowPitch;
    l->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
}
static void bufferToTexture(ID3D12GraphicsCommandList* l, ID3D12Resource* buf, ID3D12Resource* tex, const Plane& p) {
    D3D12_TEXTURE_COPY_LOCATION s{}; s.pResource = buf; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    s.PlacedFootprint.Offset = p.offset; s.PlacedFootprint.Footprint.Format = p.format;
    s.PlacedFootprint.Footprint.Width = p.width; s.PlacedFootprint.Footprint.Height = p.height;
    s.PlacedFootprint.Footprint.Depth = 1; s.PlacedFootprint.Footprint.RowPitch = p.rowPitch;
    D3D12_TEXTURE_COPY_LOCATION d{}; d.pResource = tex; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
}

using NgxInit = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version);
using NgxCaps = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using NrCreate = void* (*)(const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                           unsigned, unsigned, int, float, int, float, float, float, int, int);
using NrEvaluate = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                           ID3D12Resource*, ID3D12Resource*, unsigned, unsigned, unsigned, unsigned,
                           int, int, float, int, float, float, float, int, float, float);
using NrRelease = void (*)(void*);

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    UINT renderIndex = argc > 1 ? (UINT)std::strtoul(argv[1], nullptr, 10) : 0;
    UINT neuralIndex = argc > 2 ? (UINT)std::strtoul(argv[2], nullptr, 10) : 1;
    UINT width = argc > 3 ? (UINT)std::strtoul(argv[3], nullptr, 10) : 320;
    UINT height = argc > 4 ? (UINT)std::strtoul(argv[4], nullptr, 10) : 180;
    const bool packedOutput = argc > 5 && std::strcmp(argv[5], "r11") == 0;
    if (renderIndex == neuralIndex || !width || !height) return 2;

    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create factory");
    ComPtr<ID3D12Device3> dev[2];
    UINT indices[2] = {renderIndex, neuralIndex};
    for (int n = 0; n < 2; ++n) {
        ComPtr<IDXGIAdapter1> a; check(factory->EnumAdapters1(indices[n], &a), "Enum adapter");
        DXGI_ADAPTER_DESC1 ad{}; check(a->GetDesc1(&ad), "Adapter desc");
        check(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev[n])), "Create device");
        LUID l = dev[n]->GetAdapterLuid();
        std::printf("%s adapter=%u vendor=%04x LUID=%08lx:%08lx\n", n ? "neural" : "render", indices[n],
                    ad.VendorId, (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    }
    LUID a = dev[0]->GetAdapterLuid(), b = dev[1]->GetAdapterLuid();
    if (a.HighPart == b.HighPart && a.LowPart == b.LowPart) return 2;
    Queue render(dev[0].Get()), neural(dev[1].Get());

    UINT64 cursor = 0;
    Plane colorP = plane(cursor, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8);
    Plane depthP = plane(cursor, width, height, DXGI_FORMAT_R32_FLOAT, 4);
    Plane motionP = plane(cursor, width, height, DXGI_FORMAT_R16G16_FLOAT, 4);
    Plane resultP = plane(cursor, width, height,
                          packedOutput ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT,
                          packedOutput ? 4 : 8);
    std::printf("output_format=%s bytes_per_pixel=%u\n", packedOutput ? "R11G11B10_FLOAT" : "R16G16B16A16_FLOAT", packedOutput ? 4u : 8u);
    SIZE_T allocation = (SIZE_T)((cursor + 65535) & ~UINT64(65535));
    void* address = VirtualAlloc(nullptr, allocation, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!address) return 3;
    ComPtr<ID3D12Heap> heaps[2]; ComPtr<ID3D12Resource> bridge[2]; auto bd = bufferDesc(allocation);
    for (int n = 0; n < 2; ++n) {
        check(dev[n]->OpenExistingHeapFromAddress(address, IID_PPV_ARGS(&heaps[n])), "Open host heap");
        check(dev[n]->CreatePlacedResource(heaps[n].Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&bridge[n])), "Create bridge buffer");
    }

    auto gameColor = texture(dev[0].Get(), width, height, colorP.format, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto gameDepth = texture(dev[0].Get(), width, height, depthP.format, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto gameMotion = texture(dev[0].Get(), width, height, motionP.format, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto gameResult = texture(dev[0].Get(), width, height, resultP.format, D3D12_RESOURCE_STATE_COPY_DEST);
    auto nrColor = texture(dev[1].Get(), width, height, colorP.format, D3D12_RESOURCE_STATE_COPY_DEST);
    auto nrDepth = texture(dev[1].Get(), width, height, depthP.format, D3D12_RESOURCE_STATE_COPY_DEST);
    auto nrMotion = texture(dev[1].Get(), width, height, motionP.format, D3D12_RESOURCE_STATE_COPY_DEST);
    auto nrOutput = texture(dev[1].Get(), width, height, resultP.format, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);

    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    auto query = nvapi ? reinterpret_cast<void* (*)(unsigned)>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
    auto nvinit = query ? reinterpret_cast<int (*)()>(query(0x0150e828)) : nullptr;
    std::printf("NvAPI_Initialize=%d\n", nvinit ? nvinit() : -1);
    HMODULE core = LoadLibraryW(L"_nvngx.dll");
    auto init = core ? reinterpret_cast<NgxInit>(GetProcAddress(core, "NVSDK_NGX_D3D12_Init")) : nullptr;
    auto caps = core ? reinterpret_cast<NgxCaps>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters")) : nullptr;
    if (!init || !caps || init(0x24480451ull, L"Z:\\work\\runtime\\neural", dev[1].Get(), NVSDK_NGX_Version_API) != 1) return 4;
    NVSDK_NGX_Parameter* params = nullptr; if (caps(&params) != 1 || !params) return 4;
    HMODULE fwd = LoadLibraryW(L"Z:\\work\\runtime\\optiscaler\\nvngx.dll_dlssnr.dll");
    auto create = fwd ? reinterpret_cast<NrCreate>(GetProcAddress(fwd, "dlssnr_call_create")) : nullptr;
    auto evaluate = fwd ? reinterpret_cast<NrEvaluate>(GetProcAddress(fwd, "dlssnr_call_evaluate")) : nullptr;
    auto release = fwd ? reinterpret_cast<NrRelease>(GetProcAddress(fwd, "dlssnr_call_release")) : nullptr;
    if (!create || !evaluate || !release) return 4;

    neural.begin();
    void* feature = create(L"Z:\\work\\runtime\\neural\\nvngx_dlssnr.dll", L"Z:\\work\\runtime\\neural",
                           dev[1].Get(), neural.list.Get(), params, width, height, 0, 1.0f, 0,
                           1.0f, 1.0f, -1.0f, 1, 1);
    if (!feature) { std::printf("feature create failed\n"); return 5; }
    neural.finish();

    auto started = std::chrono::steady_clock::now();
    render.begin();
    barrier(render.list.Get(), bridge[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    textureToBuffer(render.list.Get(), gameColor.Get(), bridge[0].Get(), colorP);
    textureToBuffer(render.list.Get(), gameDepth.Get(), bridge[0].Get(), depthP);
    textureToBuffer(render.list.Get(), gameMotion.Get(), bridge[0].Get(), motionP);
    barrier(render.list.Get(), bridge[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    render.finish();
    auto t_upload0 = std::chrono::steady_clock::now();

    neural.begin();
    barrier(neural.list.Get(), bridge[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bufferToTexture(neural.list.Get(), bridge[1].Get(), nrColor.Get(), colorP);
    bufferToTexture(neural.list.Get(), bridge[1].Get(), nrDepth.Get(), depthP);
    bufferToTexture(neural.list.Get(), bridge[1].Get(), nrMotion.Get(), motionP);
    barrier(neural.list.Get(), bridge[1].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(neural.list.Get(), nrColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(neural.list.Get(), nrDepth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(neural.list.Get(), nrMotion.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    neural.finish();
    auto t_upload1 = std::chrono::steady_clock::now();

    neural.begin();
    int eval = evaluate(neural.list.Get(), feature, params, nrColor.Get(), nrDepth.Get(), nrMotion.Get(),
                        nrOutput.Get(), width, height, width, height, 0, 1, 1.0f, 0,
                        1.0f, 1.0f, -1.0f, 1, 1.0f, 1.0f);
    barrier(neural.list.Get(), nrOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    neural.finish();
    auto t_eval = std::chrono::steady_clock::now();

    neural.begin();
    barrier(neural.list.Get(), bridge[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    textureToBuffer(neural.list.Get(), nrOutput.Get(), bridge[1].Get(), resultP);
    barrier(neural.list.Get(), bridge[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    neural.finish();
    auto t_download = std::chrono::steady_clock::now();
    render.begin();
    barrier(render.list.Get(), bridge[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bufferToTexture(render.list.Get(), bridge[0].Get(), gameResult.Get(), resultP);
    barrier(render.list.Get(), bridge[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    render.finish();
    auto finished = std::chrono::steady_clock::now();
    auto elapsed = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    double ms = elapsed(started, finished);
    std::printf("phase_ms game_to_host=%.3f host_to_neural=%.3f evaluate=%.3f neural_to_host=%.3f host_to_game=%.3f\n",
                elapsed(started, t_upload0), elapsed(t_upload0, t_upload1), elapsed(t_upload1, t_eval),
                elapsed(t_eval, t_download), elapsed(t_download, finished));
    HRESULT r0 = dev[0]->GetDeviceRemovedReason(), r1 = dev[1]->GetDeviceRemovedReason();
    std::printf("evaluate=0x%08x render_device=0x%08lx neural_device=0x%08lx roundtrip_ms=%.3f\n",
                (unsigned)eval, (unsigned long)r0, (unsigned long)r1, ms);
    release(feature); VirtualFree(address, 0, MEM_RELEASE);
    if (eval != 1 || FAILED(r0) || FAILED(r1)) return 6;
    std::printf("PASS game textures GPU%u -> DLSS Neural Rendering GPU%u -> game output GPU%u\n",
                renderIndex, neuralIndex, renderIndex);
    return 0;
}
