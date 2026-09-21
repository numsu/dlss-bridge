#include "dlss_bridge/components.hpp"

#include <string.h>

namespace dlss_bridge {

static const ComponentDescriptor kComponents[] = {
    { ComponentKind::Host,      "injected-vulkan",            VulkanFrames, 0 },
    { ComponentKind::Host,      "injected-d3d11",             D3D11Frames, 0 },
    { ComponentKind::Host,      "injected-d3d12",             D3D12Frames, 0 },
    { ComponentKind::Host,      "vulkan-windows",             VulkanFrames, 0 },
    { ComponentKind::Host,      "reshade-addon",              VulkanFrames, 0 },
    { ComponentKind::Capture,   "ngx-vulkan",                 (TemporalInputs | MultiViewport), VulkanFrames },
    { ComponentKind::Capture,   "d3d11",                      (TemporalInputs | MultiViewport), D3D11Frames },
    { ComponentKind::Capture,   "ngx-d3d12",                  (TemporalInputs | MultiViewport), D3D12Frames },
    { ComponentKind::Executor,  "ngx-d3d12",                  (D3D12Execution | MultiViewport), TemporalInputs },
    { ComponentKind::Transport, "same-adapter-external-image", (SameAdapter | MultiViewport), TemporalInputs | D3D12Execution },
    { ComponentKind::Transport, "directional-host-staging",    (CrossAdapter | MultiViewport), TemporalInputs | D3D12Execution },
};

const ComponentDescriptor *FindIntegratedComponent(ComponentKind kind, const char *id)
{
    if (!id) return nullptr;
    for (const ComponentDescriptor &component : kComponents)
        if (component.kind == kind && strcmp(component.id, id) == 0) return &component;
    return nullptr;
}

uint32_t RuntimeComposition::ProvidedCapabilities() const
{
    uint32_t result = 0;
    if (host) result |= host->provides;
    if (capture) result |= capture->provides;
    if (executor) result |= executor->provides;
    if (transport) result |= transport->provides;
    return result;
}

static bool RequirementsMet(const ComponentDescriptor *component, uint32_t provided)
{
    return component && (component->required_capabilities & provided) ==
        component->required_capabilities;
}

bool RuntimeComposition::Configure(const char *host_id)
{
    *this = {};
    host = FindIntegratedComponent(ComponentKind::Host, host_id);
    if (!RequirementsMet(host, 0)) return false;
    const char *capture_id = "ngx-vulkan";
    if (host->provides & D3D11Frames) capture_id = "d3d11";
    else if (host->provides & D3D12Frames) capture_id = "ngx-d3d12";
    capture = FindIntegratedComponent(ComponentKind::Capture, capture_id);
    if (!capture) return false;
    executor = FindIntegratedComponent(ComponentKind::Executor, "ngx-d3d12");
    if (!RequirementsMet(capture, host->provides)) return false;
    return RequirementsMet(executor, host->provides | capture->provides);
}

bool RuntimeComposition::SelectTransport(bool cross_adapter)
{
    transport = FindIntegratedComponent(ComponentKind::Transport,
        cross_adapter ? "directional-host-staging" : "same-adapter-external-image");
    return RequirementsMet(transport, ProvidedCapabilities() & ~(
        static_cast<uint32_t>(SameAdapter) | static_cast<uint32_t>(CrossAdapter)));
}

bool RuntimeComposition::Ready() const
{
    return host && capture && executor && transport &&
        RequirementsMet(capture, ProvidedCapabilities()) &&
        RequirementsMet(executor, ProvidedCapabilities()) &&
        RequirementsMet(transport, ProvidedCapabilities());
}

} // namespace dlss_bridge
