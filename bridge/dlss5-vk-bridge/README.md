# Vulkan NGX bridge

This DLL bridges a game's Vulkan NGX Super Resolution operation to a private
D3D12 NGX session so the configured neural add-on can execute. It is one
runtime component of the repository and is normally installed and launched by
the controller.

## Requirements

The target must be a 64-bit Windows Vulkan process with NVIDIA NGX. Native
Windows and Proton are supported hosts. The session directory must contain the
bridge DLL, launcher, OptiScaler payload, generated configuration, and verified
neural runtime.

The bridge does not replace `vulkan-1.dll` or write into the game directory.
The suspended-process launcher injects `dlss5-vk-hook.dll`, calls
`DLSSBridgeInitialize`, and resumes the process only after initialization
succeeds.

## Frame path

The NGX Vulkan create hook records the exact creation contract for each
accepted feature handle. During evaluation the bridge reads color, depth,
motion vectors, output, and temporal values from the game parameter block.

```text
game command buffer
  input copies
  SetEvent(inputs ready)
  WaitEvents(output ready)
  output reinsertion
  game post-processing

ordered worker
  wait for inputs-ready event
  execute private D3D12 NGX operation
  wait for completion-qualified output
  SetEvent(output ready)
```

The handoff is always recorded inside the game command buffer. There is no
late copy-back path. Failure to create the Vulkan events or worker is a hard
initialization failure.

Before neural-only mode latches, the original evaluate runs while every
transport slot receives a valid private result. After latching, the original
evaluate is suppressed. The runtime never selects another NGX D3D12 dispatcher
or silently resumes ordinary DLSS.

## Feature creation

The private feature uses the dimensions, quality, feature flags, and optional
output-subrect policy captured from the matching successful Vulkan create.
Single-node D3D12 creation and visibility masks come from the private executor.
Missing mandatory values fail the bridge. There are no guessed defaults or
create retries with mutated quality/subrect values.

The D3D12 entry points must come from the exact session-local OptiScaler module.
This prevents a driver dispatcher from bypassing the neural insertion point.

Frame Generation is not compatible with this Super Resolution frame contract,
so its create is rejected explicitly. Ray Reconstruction evaluation is left
untouched.

## GPU routes

The private D3D12 session can use the Vulkan device's adapter or a selected
secondary adapter. Same-adapter execution uses shared resources. Secondary-GPU
execution uses an ordered directional host-staging ring. Vulkan host-pointer
import is preferred when supported; the D3D12 helper-copy sub-route remains
available for adapter/API combinations that cannot import those allocations.
The log names the selected adapter and active transport sub-route.

Temporal frames execute in order. Completed output is published only after its
D3D12 fence has retired, so Vulkan never reads an allocation that is still
being written.

## Configuration

The controller writes `dlss5-vk-bridge.cfg` into the isolated session. Runtime
keys are:

```ini
verbose=0
execution_mode=auto
compute_adapter=auto
ring_slots=4
neural_pipeline_frames=2
latency_budget_ms=16
output_transport=native
neural_queue_mode=unified
gpu_timestamps=1
```

Neural placement, working scale, and pass count are applied separately to the
session-local `OptiScaler.ini`. Unknown runtime keys are logged and rejected by
the controller before launch.

## Builds

`scripts/build-vk-bridge.sh` builds:

- `dlss5-vk-hook.dll` for suspended-process injection;
- `dlss5-vk-bridge.dll` as a Vulkan layer;
- `dlss5-vk-bridge.addon64` as a ReShade add-on;
- `dlss-bridge-launcher.exe` as the suspended-process launcher.

The injected host is the primary validated route. See
[known limitations](../../docs/known-limitations.md) for current format,
lifecycle, host, and test boundaries.
