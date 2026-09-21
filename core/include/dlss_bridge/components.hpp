#pragma once

#include <stdint.h>

namespace dlss_bridge {

enum class ComponentKind : uint8_t { Host, Capture, Executor, Transport };

enum ComponentCapability : uint32_t {
    VulkanFrames       = 1u << 0,
    D3D12Execution     = 1u << 1,
    SameAdapter        = 1u << 2,
    CrossAdapter       = 1u << 3,
    TemporalInputs     = 1u << 4,
    D3D11Frames        = 1u << 5,
    MultiViewport      = 1u << 6,
    D3D12Frames        = 1u << 7,
};

struct ComponentDescriptor {
    ComponentKind kind;
    const char *id;
    uint32_t provides;
    uint32_t required_capabilities;
};

struct RuntimeComposition {
    const ComponentDescriptor *host;
    const ComponentDescriptor *capture;
    const ComponentDescriptor *executor;
    const ComponentDescriptor *transport;

    bool Configure(const char *host_id);
    bool SelectTransport(bool cross_adapter);
    bool Ready() const;
    uint32_t ProvidedCapabilities() const;
};

const ComponentDescriptor *FindIntegratedComponent(ComponentKind kind, const char *id);

} // namespace dlss_bridge
