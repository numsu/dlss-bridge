# Portable architecture

DLSS Bridge attaches to a 64-bit Windows Vulkan or Direct3D 11 process,
captures its NVIDIA NGX temporal-upscaling contract, executes the
corresponding private D3D12 NGX operation through the configured neural host,
and reinserts the result before the game records downstream rendering.

The same package supports native Windows and Windows games launched through
Proton. The launcher creates an isolated user-state directory and injects the
bridge without changing the game directory or Wine prefix.

## Current execution path

```text
game NGX Vulkan/D3D11 create
  -> capture exact feature creation contract (per viewport, keyed by NGX handle)

game NGX Vulkan evaluate
  -> capture color, depth, motion vectors, output, and temporal scalars
  -> copy inputs in the game command buffer
  -> signal the ordered worker (order preserved per viewport)
  -> private D3D12 NGX evaluate through session-local OptiScaler
  -> return the completed output
  -> copy output back in the same game command buffer
  -> game post-processing, UI, and presentation

game NGX D3D11 evaluate (implemented, untested — no hardware-validated run yet)
  -> copy inputs via the immediate context to per-viewport shared textures
  -> private D3D12 NGX evaluate (same per-viewport features as Vulkan)
  -> copy neural output back via the immediate context
```

There is one in-frame execution path. If its synchronization objects cannot be
created, initialization fails. The runtime does not select another NGX
dispatcher or resume the original game-side upscaler after neural-only mode has
latched.

## Components

The runtime composes four statically linked component kinds:

- **Host:** obtains process and graphics lifecycle boundaries
  (`injected-vulkan`, `injected-d3d11`, Vulkan layer, ReShade add-on).
- **Capture:** recognizes NVIDIA NGX feature creation and evaluation
  (`ngx-vulkan`, `d3d11`).
- **Executor:** owns the private D3D12 device, per-viewport NGX sessions, and
  neural calls.
- **Transport:** moves the temporal resources between the game and executor
  (`vulkan-d3d12`, `d3d11-d3d12`; same-adapter NT sharing or directional host
  staging).

The compiled component registry validates that the selected host, capture,
executor, and transport capabilities are compatible. `backend.toml` files are
package inventory. Entries marked `planned` are not executable.

The versioned C ABI describes the intended boundary for future separately
loaded components. Current integrated components remain in one DLL and still
share process-wide graphics state.

## Hosts

The normal host is suspended-process injection. The launcher starts the target,
loads the session-local hook DLL, calls its explicit initializer, and resumes
the target only after initialization succeeds. `DllMain` only records the
module handle and disables thread notifications.

The injected host scopes interception to the application executable, modules
under its directory, and non-system modules causally loaded through those
modules. A Vulkan layer and ReShade add-on build use the same frame runtime but
have received less hardware validation.

## Feature contracts

A successful NGX feature create records the handle and mandatory creation
values: input and output dimensions, quality, and feature flags. Optional
output-subrect policy is preserved when present. Evaluation is accepted only
for a handle with a complete captured contract. The D3D12 executor creates one
private feature per viewport from that viewport's contract and does not invent
quality modes, flags, or retry mutations. Viewports are resolved by NGX handle
identity up to `capture.max_viewports`; beyond that, frames are forwarded
untouched.

Frame Generation and Ray Reconstruction are separate contracts. Frame
Generation creation is rejected while the bridge is attached. Ray
Reconstruction evaluations are not modified.

## GPU placement and transport

`execution.mode` controls placement:

- `same_gpu` uses the adapter that owns the Vulkan device.
- `secondary_gpu` requires another selected adapter.
- `auto` prefers a usable secondary adapter, then the game adapter.

`auto` is capability-based and does not benchmark alternatives. Adapter
indices and LUIDs are process-local selectors. UUID and PCI selector syntax is
reserved until controller-side correlation is implemented.

The same-adapter route shares resources between Vulkan and D3D12. The
secondary-adapter route uses an ordered ring of host-visible allocations. When
Vulkan host-pointer import succeeds, Vulkan writes and reads the directional
staging allocations directly. Otherwise the route uses game-adapter D3D12
helper copies. The active sub-route is reported in the runtime log.

Temporal evaluations remain ordered within each viewport. More ring slots can
absorb scheduling variation, but evaluations for one viewport cannot be
reordered without corrupting its temporal history. Viewports interleave on the
shared worker and transport ring (slots are exclusive while in flight).

## Configuration and profiles

The controller accepts a strict TOML schema. Unknown sections and settings are
errors. The integrated values are:

- `[feature]`: `kind`, `placement`, `working_scale`, `passes`
- `[execution]`: `mode`, `compute_adapter`
- `[transport]`: `output_format`, `neural_queue`, `ring_slots`,
  `neural_pipeline_frames`, `latency_budget_ms`
- `[telemetry]`: `gpu_timestamps`
- `[bridge]`: `verbose`
- `[match]`: `executable`, `graphics_api`
- `[capture]`: `adapter`

Bundled profiles with `[match].executable` are applied automatically. Explicit
`--profile` values are applied afterward. The only integrated feature kind is
`neural_rendering`; integrated capture adapters are `ngx_vulkan` and `d3d11`,
with matching graphics APIs `vulkan` and `d3d11`. Mismatched
adapter/API pairs and `capture.max_viewports` outside 1..4 fail during
resolution. `profiles/bg3.toml` (`bg3.exe`/Vulkan) and
`profiles/bg3-dx11.toml` (`bg3_dx11.exe`/D3D11) both set `max_viewports = 2`
for split-screen co-op.

Feature placement, working scale, and pass count are written to the
session-local OptiScaler configuration. The remaining settings become the
bridge runtime policy. There is no launch-context label in either policy.
The DLL parses that generated policy strictly as well: unknown, malformed, or
inconsistent settings stop initialization instead of being ignored or clamped.

## Session lifecycle

A launch session contains the hook, launcher, neural host, verified neural
runtime, and generated configuration. The target executable in the command is
replaced by the session launcher. Other command arguments remain in order.
The game directory and compatibility prefix are not modified.

At normal exit, text logs are copied to persistent user state and the session
is removed. Abnormal controller termination can leave a stale session; cleanup
and richer crash-artifact collection remain future work.

## Failure behavior

Before neural-only mode latches, the game evaluation is forwarded while the
private resources warm. Every transport slot must contain a completed neural
result before a viewport latches. After latching, that viewport's game-side
evaluation stays suppressed while other viewports warm independently. A busy
ring may repeat a completion-qualified neural result for the same viewport. A
missing contract, failed handoff, NGX exception, or permanent executor failure
is reported and does not silently restore ordinary DLSS. Per-viewport failures
park only that viewport; device-level failures remain fail-closed for the
process.

## Planned extension paths

The repository inventories native Linux Vulkan, Direct3D 12 capture, Streamline
capture, Vulkan execution, peer memory, and final-frame processing as planned
components. They require implementations and validation before their manifests
can be marked integrated. Native Linux execution additionally requires a
native neural executor; the current executor is D3D12.

See [Known limitations](known-limitations.md) for concrete boundaries and test
coverage.
