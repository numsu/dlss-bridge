# Portable architecture

DLSS Bridge attaches to a 64-bit Windows Vulkan process, captures its NVIDIA
NGX temporal-upscaling contract, executes the corresponding private D3D12 NGX
operation through the configured neural host, and reinserts the result before
the game records downstream rendering.

The same package supports native Windows and Windows games launched through
Proton. The launcher creates an isolated user-state directory and injects the
bridge without changing the game directory or Wine prefix.

## Current execution path

```text
game NGX Vulkan create
  -> capture exact feature creation contract

game NGX Vulkan evaluate
  -> capture color, depth, motion vectors, output, and temporal scalars
  -> copy inputs in the game command buffer
  -> signal the ordered worker
  -> private D3D12 NGX evaluate through session-local OptiScaler
  -> return the completed output
  -> copy output back in the same game command buffer
  -> game post-processing, UI, and presentation
```

There is one in-frame execution path. If its synchronization objects cannot be
created, initialization fails. The runtime does not select another NGX
dispatcher or resume the original game-side upscaler after neural-only mode has
latched.

## Components

The runtime composes four statically linked component kinds:

- **Host:** obtains process and Vulkan lifecycle boundaries.
- **Capture:** recognizes NVIDIA NGX Vulkan feature creation and evaluation.
- **Executor:** owns the private D3D12 device, NGX session, and neural call.
- **Transport:** moves the temporal resources between the game and executor.

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

A successful NGX Vulkan feature create records the handle and mandatory
creation values: input and output dimensions, quality, and feature flags.
Optional output-subrect policy is preserved when present. Evaluation is
accepted only for a handle with a complete captured contract. The D3D12
executor uses that contract once and does not invent quality modes, flags, or
retry mutations.

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

Temporal evaluations remain ordered. More ring slots can absorb scheduling
variation, but evaluations cannot be reordered without corrupting temporal
history.

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
`neural_rendering`, capture adapter is `ngx_vulkan`, and graphics API is
`vulkan`; other values fail during resolution.

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
result before latching. After latching, the game-side evaluation stays
suppressed. A busy ring may repeat a completion-qualified neural result. A
missing contract, failed handoff, NGX exception, or permanent executor failure
is reported and does not silently restore ordinary DLSS.

## Planned extension paths

The repository inventories native Linux Vulkan, Direct3D capture, Streamline
capture, Vulkan execution, peer memory, and final-frame processing as planned
components. They require implementations and validation before their manifests
can be marked integrated. Native Linux execution additionally requires a
native neural executor; the current executor is D3D12.

See [Known limitations](known-limitations.md) for concrete boundaries and test
coverage.
