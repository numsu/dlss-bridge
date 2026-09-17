# Known limitations

This document records current implementation boundaries. Component manifests
marked `planned` describe extension points and are not available execution
paths.

## Game and API coverage

- The integrated capture path is a 64-bit Windows Vulkan process using NVIDIA
  NGX, on native Windows or through Proton. Native Linux Vulkan, Direct3D
  capture, Streamline capture, and final-frame processing are planned.
- The game must expose color, output, depth, motion vectors, temporal scalars,
  and a complete feature-creation contract through NGX.
- One Super Resolution feature history is bridged per process. Multiple active
  features, simultaneous viewports, multiple Vulkan devices, and child
  renderer processes are not supported.
- Frame Generation creation is rejected. Ray Reconstruction evaluations are
  left untouched because neither feature uses the implemented Super Resolution
  contract.
- Exposure textures are not transported. Pre-exposure and exposure-scale
  scalars are copied when available; broader temporal-scalar coverage still
  needs validation across engines.

## Hosting and interception

- The injected host patches the main executable, modules under its directory,
  and non-system modules reached through their `LoadLibrary` calls. A module
  outside those roots or loaded through a lower-level loader API after the
  bounded 60-second scan may not be intercepted.
- Protected processes and anti-cheat systems may reject injection. Only
  officially permitted layer, add-on, or plugin mechanisms are suitable for
  those games.
- NGX interception modifies process-local export prologues and temporarily
  restores them while forwarding. Calls are serialized, but an official NGX
  callback boundary would be safer.
- The hook DLL is pinned for the target process lifetime. Normal library loads
  remain intercepted; the backup scan for lower-level loader calls ends after
  60 seconds.
- The Vulkan layer and ReShade host compile against the same runtime but have
  less end-to-end hardware coverage than suspended-process injection.

## Feature and neural runtime contracts

- The private D3D12 feature is created once from the exact mandatory values
  captured at the corresponding Vulkan feature create. A missing contract is a
  hard error; there are no guessed quality, flag, subrect, or dispatcher
  fallbacks.
- Feature contracts are stored in a fixed process-local table and identified by
  NGX handle identity. Release/recreate and high feature counts need broader
  synthetic coverage.
- The D3D12 NGX interface must come from the exact session-local OptiScaler
  module. A different NGX dispatcher is not accepted because it could bypass
  the neural pass.
- The packaged OptiScaler DLL is pinned and hash-checked. Packaging disables
  its native Vulkan neural pass and redirects the visual-toggle handler to
  `DlssNrApplyModel`; the private D3D12 model stays active. A new release needs
  an explicit code review and patch update. Version 0.8.3 changed the D3D12
  neural path substantially; its CPU-side frame pacing in games that stream
  assets during movement remains unverified.
- The application/project identity and SDK version come from the game's
  successful Vulkan NGX initialization. These are vendor interfaces without a
  stable public cross-API bridge contract.
- The neural runtime is pinned and checksum-verified during setup. Driver,
  model, and GPU-generation combinations beyond the tested hardware are
  capability-gated but not exhaustively validated.

## GPU selection and transport

- `auto` prefers a usable non-game NVIDIA adapter and otherwise uses the game
  adapter. It does not benchmark routes or choose from measured latency.
- `index:N` and LUID are session-local identities. UUID and PCI selection still
  require controller-side correlation and are rejected by the current schema.
- Secondary-GPU execution uses directional host-visible staging, not peer GPU
  memory. It includes CPU `memcpy` operations between mapped Vulkan and D3D12
  allocations and remains sensitive to PCIe topology and concurrent load.
- Vulkan host-pointer import and the D3D12 helper-copy sub-route share one
  transport component. The log identifies the active sub-route, but structured
  telemetry does not yet separate their statistics.
- The worker polls a Vulkan event because Vulkan events do not provide a host
  blocking wait. Input arrival has a fixed three-second safety timeout and
  D3D12 completion has a fixed one-second safety timeout, separate from the
  configured frame latency budget.
- Temporal NGX evaluations are ordered. Ring slots can absorb scheduling
  variation but cannot execute temporal frames out of order.

## Formats and synchronization

- Common color, depth, motion-vector, and output formats have direct mappings.
  Some nonmatching color and motion formats use Vulkan blits. A blit requires a
  graphics-capable queue; a compute-only queue cannot run those conversions.
- Non-float depth uses a compute conversion when the source can be sampled. If
  a valid depth conversion cannot be built, the frame is not bridged.
- Dynamic resolution rebuilds resources and may cause a visible pacing event.
  HDR, unusual subresources, mip levels, array layers, and engine-specific
  depth or motion conventions need broader validation.
- Several image transitions use conservative `ALL_COMMANDS` barriers because
  the NGX parameter contract does not expose the game's exact producer and
  consumer stage/access state.
- Packed output saves bandwidth only when the source format is wider and the
  complete conversion route supports `R11G11B10_FLOAT`.

## Components, configuration, and lifecycle

- Integrated host, capture, executor, and transport components are selected and
  validated at runtime but remain statically linked into one DLL with shared
  process-wide graphics state. The versioned C ABI has no external component
  loader yet.
- Component manifests and the compiled registry are checked independently;
  build-time generation from one source of truth remains future work.
- Bundled executable profile matching is implemented. It matches the final
  `.exe` argument by filename, so a command whose last executable is a helper
  or anti-cheat process may select the wrong profile or target.
- Launch sessions retain text logs on normal exit. Crash dumps, structured
  telemetry, stale-session garbage collection, and recovery after abrupt
  controller termination are not implemented.
- Internal device, queue, hook, module, and feature tables have fixed
  capacities. Several Windows paths use `MAX_PATH`; very long install or game
  paths are not fully supported.
- The controller `probe` command verifies installation/model state and reports
  GPU inventory. Feature creation, transport usability, and visual correctness
  can only be validated inside a game process today.
- `Ctrl+Shift+N` shows or hides only the neural effect while the model,
  private DLSS, and transport path remain active. OptiScaler displays the
  applied visual state. The bridge logs the requested transition, but there is
  no external runtime control or state-query API yet.

## Telemetry and validation

- GPU timestamps can be disabled through `[telemetry].gpu_timestamps`. The
  logs include pipeline and neural-state p50/p95/p99 summaries, fresh-frame
  rate, repeat rate, and maximum output age. Slow-frame logs separately time
  the OptiScaler CPU call, D3D12 submission, and Vulkan handoff; GPU timestamps
  do not cover those CPU waits. Structured per-frame telemetry, export to
  metrics systems, and automatic route selection are not implemented.
- Automated tests cover configuration, session isolation, component policy,
  DLL loading, injection timing, and Vulkan-module discovery. They do not
  replace hardware validation for multi-GPU output correctness, device loss,
  dynamic resolution, or uncommon engine contracts.
- Native Windows installer execution and upgrade behavior still require a
  release-environment smoke test.
