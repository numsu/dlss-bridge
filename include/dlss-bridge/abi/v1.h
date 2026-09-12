#ifndef DLSS_BRIDGE_ABI_V1_H
#define DLSS_BRIDGE_ABI_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DLSS_BRIDGE_ABI_V1 1u

typedef enum DlssBridgeApiV1 {
    DLSS_BRIDGE_API_UNKNOWN = 0,
    DLSS_BRIDGE_API_VULKAN = 1,
    DLSS_BRIDGE_API_D3D11 = 2,
    DLSS_BRIDGE_API_D3D12 = 3
} DlssBridgeApiV1;

typedef enum DlssBridgeFeatureV1 {
    DLSS_BRIDGE_FEATURE_SUPER_RESOLUTION = 1,
    DLSS_BRIDGE_FEATURE_NEURAL_RENDERING = 2,
    DLSS_BRIDGE_FEATURE_RAY_RECONSTRUCTION = 3,
    DLSS_BRIDGE_FEATURE_FRAME_GENERATION = 4,
    DLSS_BRIDGE_FEATURE_FINAL_FRAME = 100
} DlssBridgeFeatureV1;

typedef enum DlssBridgeResourceSemanticV1 {
    DLSS_BRIDGE_RESOURCE_COLOR = 1,
    DLSS_BRIDGE_RESOURCE_OUTPUT = 2,
    DLSS_BRIDGE_RESOURCE_DEPTH = 3,
    DLSS_BRIDGE_RESOURCE_MOTION_VECTORS = 4,
    DLSS_BRIDGE_RESOURCE_EXPOSURE = 5,
    DLSS_BRIDGE_RESOURCE_REACTIVE_MASK = 6,
    DLSS_BRIDGE_RESOURCE_TRANSPARENCY_MASK = 7,
    DLSS_BRIDGE_RESOURCE_NORMALS = 8,
    DLSS_BRIDGE_RESOURCE_ROUGHNESS = 9,
    DLSS_BRIDGE_RESOURCE_SPECULAR_ALBEDO = 10,
    DLSS_BRIDGE_RESOURCE_SPECULAR_MOTION_VECTORS = 11
} DlssBridgeResourceSemanticV1;

typedef struct DlssBridgeAdapterIdV1 {
    uint32_t struct_size;
    uint32_t valid_fields;
    uint8_t gpu_uuid[16];
    uint8_t dxgi_luid[8];
    uint32_t pci_domain;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
} DlssBridgeAdapterIdV1;

typedef struct DlssBridgeResourceV1 {
    uint32_t struct_size;
    uint32_t semantic;
    uint32_t api;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t mip;
    uint32_t layer;
    uint64_t native_handle;
    uint64_t state_or_layout;
    DlssBridgeAdapterIdV1 owner;
} DlssBridgeResourceV1;

typedef struct DlssBridgeFrameContractV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t frame_id;
    uint64_t viewport_id;
    uint64_t session_generation;
    uint32_t feature_kind;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
    float jitter_x;
    float jitter_y;
    float motion_scale_x;
    float motion_scale_y;
    float delta_time_ms;
    uint32_t reset_history;
    uint32_t resource_count;
    const DlssBridgeResourceV1 *resources;
    const void *extension_chain;
} DlssBridgeFrameContractV1;

typedef struct DlssBridgeStatusV1 {
    uint32_t struct_size;
    int32_t code;
    uint32_t recoverable;
    const char *message;
} DlssBridgeStatusV1;

typedef struct DlssBridgeRouteV1 {
    uint32_t struct_size;
    uint32_t route_kind;
    DlssBridgeAdapterIdV1 render_adapter;
    DlssBridgeAdapterIdV1 compute_adapter;
    uint64_t capability_bits;
    uint64_t required_alignment;
    uint64_t measured_p95_ns;
} DlssBridgeRouteV1;

typedef struct DlssBridgeHostServicesV1 {
    uint32_t struct_size;
    void *context;
    uint64_t (*monotonic_ns)(void *context);
    void (*log)(void *context, uint32_t level, const char *message);
    DlssBridgeStatusV1 (*adapter_id)(void *context, uint64_t native_device,
                                     DlssBridgeAdapterIdV1 *identity);
} DlssBridgeHostServicesV1;

typedef struct DlssBridgeCaptureAdapterV1 {
    uint32_t struct_size;
    void *context;
    DlssBridgeStatusV1 (*probe)(void *context, const DlssBridgeHostServicesV1 *host);
    DlssBridgeStatusV1 (*capture)(void *context, uint64_t native_parameters,
                                  DlssBridgeFrameContractV1 *frame);
    DlssBridgeStatusV1 (*record_reinsertion)(void *context,
                                             const DlssBridgeFrameContractV1 *frame,
                                             uint64_t output_handle);
    void (*destroy)(void *context);
} DlssBridgeCaptureAdapterV1;

typedef struct DlssBridgeExecutorV1 {
    uint32_t struct_size;
    void *context;
    DlssBridgeStatusV1 (*probe)(void *context, const DlssBridgeAdapterIdV1 *adapter,
                                uint32_t feature_kind);
    DlssBridgeStatusV1 (*create_session)(void *context,
                                         const DlssBridgeFrameContractV1 *prototype,
                                         uint64_t *session);
    DlssBridgeStatusV1 (*submit)(void *context, uint64_t session,
                                 const DlssBridgeFrameContractV1 *frame,
                                 uint64_t *completion);
    DlssBridgeStatusV1 (*poll)(void *context, uint64_t completion, uint32_t *complete);
    void (*destroy_session)(void *context, uint64_t session);
    void (*destroy)(void *context);
} DlssBridgeExecutorV1;

typedef struct DlssBridgeTransportV1 {
    uint32_t struct_size;
    void *context;
    DlssBridgeStatusV1 (*probe)(void *context,
                                const DlssBridgeAdapterIdV1 *render_adapter,
                                const DlssBridgeAdapterIdV1 *compute_adapter,
                                DlssBridgeRouteV1 *route);
    DlssBridgeStatusV1 (*allocate_ring)(void *context, const DlssBridgeRouteV1 *route,
                                        uint32_t slots, uint64_t *ring);
    DlssBridgeStatusV1 (*upload)(void *context, uint64_t ring, uint32_t slot,
                                 const DlssBridgeFrameContractV1 *frame, uint64_t *signal);
    DlssBridgeStatusV1 (*download)(void *context, uint64_t ring, uint32_t slot,
                                   uint64_t executor_completion, uint64_t *signal);
    DlssBridgeStatusV1 (*poll)(void *context, uint64_t signal, uint32_t *complete);
    void (*destroy_ring)(void *context, uint64_t ring);
    void (*destroy)(void *context);
} DlssBridgeTransportV1;

typedef struct DlssBridgePluginV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *name;
    uint64_t capability_bits;
    void *context;
    void (*destroy)(void *context);
} DlssBridgePluginV1;

typedef int32_t (*DlssBridgeGetPluginV1)(uint32_t requested_abi,
                                         DlssBridgePluginV1 *plugin);

#ifdef __cplusplus
}
#endif
#endif
