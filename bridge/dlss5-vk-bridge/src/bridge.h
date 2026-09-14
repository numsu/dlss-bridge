// Vulkan -> D3D12 NGX bridge state.
//
// The game drives DLSS through NVSDK_NGX_VULKAN_EvaluateFeature. That call is
// intercepted and forwarded untouched, then this bridge reproduces the same
// DLSS contract on a second NGX session running on its own D3D12 device -- the
// call a DLSS 5 Neural Rendering add-on detours and inserts its pass into.
//
// Per frame the pixels have to cross from the game's Vulkan device to the
// bridge's D3D12 device and back. That crossing is the whole reason this is
// harder than the D3D11 original: D3D11 has an implicit immediate-context queue
// so the copy / signal / evaluate / copy-back run inline; Vulkan can only
// signal a semaphore at submit granularity, so the handoff is split across the
// evaluate hook and the following vkQueueSubmit. See vk_interop.inc.
//
//   evaluate hook  : forward the game's evaluate during bootstrap, then stand
//                    in for it), then record INTO the game's own command buffer
//                    -- the one handed to EvaluateFeature -- the input copies
//                    (game images -> shared images), a SetEvent(ev_inputs), a
//                    WaitEvents(ev_output) park, and the copy-back (shared
//                    Output -> game Output): all of it placed before the game's
//                    post-processing reads the output.
//   vkQueueSubmit  : hand the submitted frame to a worker thread that releases
//                    the private D3D12 evaluate once ev_inputs fires on the GPU
//                    and host-sets ev_output once the result is written back.
//
// The shared textures are created on the D3D12 side (HEAP_FLAG_SHARED +
// ALLOW_SIMULTANEOUS_ACCESS) and imported into Vulkan as VkImages backed by
// imported VkDeviceMemory (VK_KHR_external_memory_win32). Two shared D3D12
// fences, imported into Vulkan as timeline semaphores (VK_KHR_external_semaphore
// _win32, D3D12_FENCE handle type), order the two queues.

#pragma once

enum { SLOT_COLOR = 0, SLOT_OUTPUT, SLOT_DEPTH, SLOT_MV, SLOT_COUNT };
enum { CROSS_UPLOAD = 0, CROSS_DOWNLOAD, CROSS_COUNT };
enum { MAX_FRAME_SLOTS = 8 };

static const char *kSlotKey[SLOT_COUNT]  = { "Color", "Output", "Depth", "MotionVectors" };
static const char *kSlotName[SLOT_COUNT] = { "Color", "Output", "Depth", "MV" };

// One imported shared texture: the D3D12 resource, its shared NT handle, and
// the Vulkan objects that alias the same memory on the game's device.
struct SharedTex
{
    ID3D12Resource *tex12;
    HANDLE          nt_handle;      // from ID3D12Device::CreateSharedHandle
    VkImage         image;          // imported alias on the game's VkDevice
    VkDeviceMemory  memory;         // imported memory backing `image`
    VkImageLayout   layout;         // current Vulkan layout of the alias
    UINT            w, h;
    DXGI_FORMAT     fmt12;          // format on the D3D12 side
    VkFormat        fmtvk;          // format of the Vulkan alias
};

// The per-frame NGX scalars the game supplies with its evaluate, captured when
// the frame is recorded so that the D3D12 evaluate that consumes them -- run
// later, on the worker, possibly with the next frame already recorded --
// applies that frame's own jitter and not the newest one.
struct FrameScalars
{
    float    jitter_x, jitter_y, sharpness;
    bool     has_jitter_x, has_jitter_y, has_sharpness;
    float    mv_scale_x, mv_scale_y, pre_exposure, exposure_scale;
    unsigned bases[8];
    unsigned reset;
    float    frame_dt;
    bool     has_frame_dt;
};

struct CrossPlane
{
    UINT64 offset;
    UINT   row_pitch;
    UINT   width, height;
    DXGI_FORMAT format;
};

// One transport generation. A frame never aliases another frame's upload or
// download allocation. output_valid becomes true only after the D3D12 download
// fence completes, so a missed evaluation can safely reuse this slot's prior
// completed neural image.
struct CrossFrameTransport
{
    ID3D12Heap *game_heap[CROSS_COUNT];
    ID3D12Heap *neural_heap[CROSS_COUNT];
    ID3D12Resource *game_buffer[CROSS_COUNT];
    ID3D12Resource *neural_buffer[CROSS_COUNT];
    // Ordinary neural-device staging resources used by the direct Vulkan
    // route. Keeping D3D12 away from host-backed cross-adapter heaps avoids
    // opaque residency work in the neural queue's critical path.
    ID3D12Resource *neural_staging[CROSS_COUNT];
    void *neural_staging_mapped[CROSS_COUNT];
    void *host_memory[CROSS_COUNT];
    void *vulkan_input_memory;  // coherent Vulkan producer, never opened by D3D12
    void *vulkan_output_memory; // immutable Vulkan consumer, never opened by D3D12
    VkBuffer cross_vk[CROSS_COUNT];
    VkDeviceMemory cross_vk_memory[CROSS_COUNT];
    UINT64 generation;
    UINT64 upload_fence;
    UINT64 output_fence;
    UINT64 output_timeline;
    bool output_valid;
    // Ordered NGX producer state. The readback remains immutable until it has
    // been presented into a later Vulkan consumer allocation.
    UINT64 producer_fence;
    UINT64 producer_timeline;
    UINT64 producer_sequence;
    int producer_timing_slot;
    bool producer_valid;
    bool producer_presented;
};

// The Vulkan-importable textures and fences must stay on the game adapter.
// When multi-GPU mode is enabled, NGX and its GPU-local textures live on a
// second adapter and this state owns the ordered cross-adapter transport ring.
struct MultiGpuState
{
    bool active;
    UINT game_index, neural_index;
    ID3D12Device3 *game_dev;
    ID3D12CommandQueue *game_queue;
    ID3D12CommandAllocator *game_alloc[MAX_FRAME_SLOTS];
    ID3D12GraphicsCommandList *game_list[MAX_FRAME_SLOTS];
    ID3D12Fence *game_fence;
    HANDLE game_event;
    UINT64 game_value;

    // Cross-adapter transfers run on the neural adapter's copy engine. NGX
    // evaluation remains on Bridge::queue, which must be a DIRECT queue.
    ID3D12CommandQueue *neural_copy_queue;
    ID3D12CommandAllocator *neural_copy_alloc[MAX_FRAME_SLOTS];
    ID3D12GraphicsCommandList *neural_copy_list[MAX_FRAME_SLOTS];
    ID3D12CommandAllocator *neural_download_alloc[MAX_FRAME_SLOTS];
    ID3D12GraphicsCommandList *neural_download_list[MAX_FRAME_SLOTS];
    ID3D12GraphicsCommandList *pipeline_list[MAX_FRAME_SLOTS];

    ID3D12Resource *neural_tex[SLOT_COUNT];
    SIZE_T host_bytes;
    CrossPlane plane[SLOT_COUNT];
    CrossFrameTransport frame[MAX_FRAME_SLOTS];
    UINT64 transport_generation;
    UINT64 producer_sequence;
    UINT64 next_present_sequence;

    // Optional zero-queue-hop paths. Vulkan imports every frame allocation:
    // it writes game inputs inline and reads only completion-qualified output.
    bool direct_vulkan_upload;
    bool direct_vulkan_download;
};

struct Bridge
{
    bool disabled;          // set after a hard failure; never retried
    bool session_ready;     // D3D12 device, queue, fences, NGX session
    bool frame_ready;       // shared textures and NGX feature match the game
    int  consecutive_fails;

    // ---- D3D12 side (mirrors the D3D11 original's private session) --------
    ID3D12Device              *dev12;
    IDXGIAdapter3             *adapter3;
    ID3D12CommandQueue        *queue;
    ID3D12GraphicsCommandList *list;

    static const int           kMaxFrames = MAX_FRAME_SLOTS;
    ID3D12CommandAllocator    *alloc[kMaxFrames];
    UINT64                     alloc_fence[kMaxFrames];
    int                        frame_slot;

    HANDLE                     fence_event;

    // fence_in : signalled by the Vulkan queue once the input copies are done,
    //            waited on by the D3D12 queue before the evaluate.
    // fence_out: signalled by the D3D12 queue after the evaluate, waited on by
    //            the Vulkan queue before the copy-back.
    // Both are D3D12 fences (SHARED) aliased into Vulkan as timeline semaphores.
    ID3D12Fence               *fence_in;
    ID3D12Fence               *fence_out;
    ID3D12Fence               *fence_gpu;    // D3D12-internal retire fence
    UINT64                     gpu_value;

    // Four direct-queue timestamps per allocator slot: before upload, after
    // upload, after NGX, and after download. Used only for bounded diagnostics.
    ID3D12QueryHeap           *timing_heap;
    ID3D12Resource            *timing_readback;
    UINT64                     timing_frequency;

    UINT64                     timeline;     // per-frame value used on both fences

    PFN_D3D12CreateFeature   create_feature;
    PFN_D3D12EvaluateFeature eval_feature;
    PFN_D3D12ReleaseFeature  release_feature;
    PFN_AllocateParameters   alloc_params;

    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    SharedTex tex[SLOT_COUNT];

    // The contract read from the game's own parameter block.
    UINT        width, height;          // Color texture size
    UINT        out_width, out_height;  // Output texture size
    UINT        render_w, render_h;     // rendered area (smaller when upscaling)
    UINT        ngx_out_w, ngx_out_h;
    UINT        feature_flags;
    UINT        slot_w[SLOT_COUNT];
    UINT        slot_h[SLOT_COUNT];
    VkFormat    game_fmt[SLOT_COUNT];   // the game's own Vulkan image formats

    bool   need_reset;
    UINT64 frames_done;

    // ---- Vulkan side ------------------------------------------------------
    // The game's device LUID, matched against DXGI adapters so the D3D12 device
    // lands on the same physical GPU (shared NT handles require it).
    uint8_t  game_luid[VK_LUID_SIZE];
    bool     luid_valid;

    // Captured from the NGX VULKAN Init hook.
    VkInstance         instance;
    VkPhysicalDevice   phys;
    VkDevice           device;

    // One of the game's queues, submitted on only from inside the intercepted
    // vkQueueSubmit (so on the game's own thread, never concurrently), plus a
    // command pool for the bridge's own copy-back / signal command buffers.
    VkQueue        vk_queue;
    uint32_t       vk_queue_family;
    // Per-frame work recorded directly into the game's command buffer.
    struct VkFrame
    {
        VkBuffer        depth_scratch;      // depth-aspect round-trip staging
        VkDeviceMemory  depth_scratch_mem;
        VkDeviceSize    depth_scratch_size;
        VkFence         retire;             // signalled once the game's submit that
                                            //   carried this slot's work
                                            //   has retired
        bool            submitted;          // retire fence has pending work
        VkEvent         ev_inputs;          // device->host: input copies executed
        VkEvent         ev_output;          // host->device: D3D12 result is ready
        VkEvent         ev_done;            // device->host: the whole sandwich, copy-
                                            //   back included, has executed
        VkCommandBuffer cmd;                // the game command buffer the work went into
        VkCommandBuffer primary;            // the primary that executes `cmd` when it
                                            //   is a secondary (vkCmdExecuteCommands)
        UINT64          value;              // this frame's timeline value
        bool            in_flight;          // recorded, not yet seen retired
        bool            matching;           // a submit carrying `cmd` is going down
                                            //   the chain right now
        bool            judged;             // the worker's verdict has been applied
        volatile LONG   worker_done;        // the worker is finished with the slot
        volatile LONG   abandoned;          // the game re-recorded `cmd` before it ran
        LONG            result;             // the worker's verdict (see WorkerRunSlot)
        FrameScalars    scalars;
        int             transport_slot;     // immutable cross-adapter allocation
        VkImageView     game_depth_view;    // bridge-made DEPTH-only view of the
        VkImage         game_depth_image;   //   game's depth image (see interop)
        uint32_t        game_depth_mip, game_depth_layer;
    } vkframe[kMaxFrames];
    int vk_slot;

    // Which queue family runs the DLSS pass is learned from the submit that
    // carries a command buffer the evaluate hook has seen -- before anything
    // only a graphics queue can run (a blit) is recorded into one.
    VkCommandBuffer probe_cmd;       // the latest untouched evaluate's buffer
    VkCommandBuffer probe_primary;   // its primary when it is a secondary
    bool            probe_matching;  // a submit carrying it is going down the chain

    // Multi-queue bookkeeping and diagnostics.
    uint32_t        vk_queue_flags;         // VkQueueFlags of the family the DLSS
                                            //   pass is submitted on (0 = not known)
    volatile LONG64 unmatched_submits;      // submits seen while bridged work still
                                            //   awaited its own
    int             rebuild_busy;           // evaluates a rebuild has waited on
                                            //   in-flight work
    UINT64          no_slot_skips;          // frames forwarded untouched for lack of
                                            //   a free slot

    LONGLONG qpf;
    LONGLONG cpu_ticks;
    LONGLONG span_start;
    UINT64   timed_frames;
    LONGLONG last_entry, iv_min, iv_max;
};

static Bridge g_bridge;
static MultiGpuState g_mgpu;
