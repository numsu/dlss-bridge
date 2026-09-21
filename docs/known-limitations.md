# Known limitations

This document records current implementation boundaries. Component manifests
marked `planned` describe extension points and are not available execution
paths.

## Game and API coverage

- The validated capture path is a 64-bit Windows Vulkan process using NVIDIA
  NGX, on native Windows or through Proton. Direct3D 11 and Direct3D 12
  capture are implemented but untested: they build, link, and pass native
  checks, with no end-to-end hardware validation yet. Native Linux Vulkan and
  final-frame processing are planned. Streamline-native titles need no
  separate backend: sl.* plugins drive the same in-process NGX exports, so
  the NGX-level hooks intercept their forwarded calls (resident sl modules
  are logged); there is deliberately no SL-API capture component.
- Direct3D 11 capture runs synchronously on the game thread (immediate
  context order is the barrier) and rides the shared host-staging ring under
  a secondary GPU. Direct3D 12 capture records into the game's open command
  lists on the adopted game device — a cross-device private queue could never
  observe unsubmitted inputs — and stands aside under a secondary GPU (the
  async submit-boundary handoff is planned). Color/output/motion formats must
   match exactly on both D3D paths (no conversion pass; CopyResource between
   incompatible formats is invalid), as must float depth. Game resources are
    transitioned for copies under the documented NGX input states
    (non-pixel, compute-readable inputs; UAV output) and restored before
    evaluating. Because the D3D12 copies live in the game's own lists,
    retirement waits on actual game-queue completion: the app-IAT
    `D3D12CreateDevice` hook observes the game device at creation, and
    per-object vtable slot patches on its `CreateCommandQueue` and each
    queue's `ExecuteCommandLists` (no system module code modified; the
    patched objects are never restored, because restoring a freed object's
    vptr would itself be a use-after-free) signal a game-device fence after
    every accepted submission. A retired generation or private feature is
    released only after the fence value of the latest accepted submission
    completes; the wait is a non-blocking poll per frame with a bounded
    fallback on rebuild/teardown. Where the `D3D12CreateDevice` import was
    not reachable (engine resolves it through an unpatched path, or a host
    without IAT patching), no submission is observed and the legacy
    immediate release applies — D3D12 same-adapter capture remains
    untested end-to-end either way.
- The game must expose color, output, depth, motion vectors, temporal scalars,
  and a complete feature-creation contract through NGX.
- Up to four concurrent Super Resolution histories are bridged per process,
  keyed by game NGX handle identity. Viewports allocate on demand, so the
  default already covers split-screen; `capture.max_viewports` remains only
  as an optional bound. Each viewport owns a private D3D12 feature, shared
  textures, latch, and telemetry; the D3D12 device, queues, and worker stay
  shared. Multiple graphics devices in one process are not supported.
- Launcher-spawned games are bridged through child-process follow:
  `--follow-children TARGET.exe` (or the equivalent `[launch]` section)
  makes the bridge force-suspend, inject, and resume EVERY child the
  injected process spawns, at any depth (Launcher -> Helper -> Game
  chains work because each injected process re-arms the same rule); the
  target filename only selects the bridge's renderer. The hook itself is
  inert in a process that never creates NGX features, so following
  intermediates is passive. A child of a process the bridge never
  injected cannot be intercepted; a failed follow logs and lets the child
  run unbridged rather than hanging it.
- Split-screen notes: viewports interleave on the shared worker while temporal
  order is preserved within each viewport. Multi-GPU multi-viewport requires
  uniform per-slot geometry (footprints derive from viewport 0).
- Frame Generation coexists: FG features are tracked and forwarded untouched
  (never bridged, never refused), so FG titles keep native frame generation
  while Super Resolution frames bridge. The passthrough precedes the
  fail-closed gate, so FG keeps running even after the SR path has failed;
  release hooks drop handle classifications so recycled addresses cannot
  misclassify, and a full table evicts oldest-first. FG interpolates the
  neural-enhanced frames; the bridge's host-block latency feeds FG cadence,
  which still needs hardware measurement.
- Ray Reconstruction is bridged on the Direct3D 12 path: denoiser-guidance
  textures (diffuse/specular albedo, normals, roughness, specular motion
  vectors, optional hit-distance) are transported in game format and bound on
  the private feature under the game's own key spellings, with view matrices
  forwarded per frame. Creation-time modes (`DLSS.Denoise.Mode` and
  siblings) are mirrored when the game sets them, never invented. Parameter
  blocks are rebuilt from the stored creation contract whenever RR inputs
  change or vanish, so released textures are never evaluated. Separate
  feature-13 handles resolve to their own viewport like everything else.
  Vulkan RR frames stand aside (no known native title); D3D11 has no RR in
  the NGX SDK. RR plus secondary-GPU execution stands aside in v1.
- Exposure textures are not transported. Pre-exposure and exposure-scale
  scalars are copied when available; broader temporal-scalar coverage still
  needs validation across engines.

## Hosting and interception

- The injected host patches the main executable, modules under its directory,
  and non-system modules reached through their `LoadLibrary` calls. A module
  outside those roots or loaded through a lower-level loader API after the
  bounded 60-second scan may not be intercepted.
- Protected processes and anti-cheat systems may reject injection, and the
  bridge never attempts to circumvent them: BattlEye-protected Online modes
  (GTA Online, Red Dead Online) are unsupported. Story modes with the
  anti-cheat disabled at the launcher (GTA V `-nobattleye`) are the supported
  shape. Only officially permitted layer, add-on, or plugin mechanisms are
  suitable for protected games.
- NGX interception modifies process-local export prologues and temporarily
  restores them while forwarding. Calls are serialized, but an official NGX
  callback boundary would be safer.
- The hook DLL is pinned for the target process lifetime. Normal library loads
  remain intercepted; the backup scan for lower-level loader calls ends after
  60 seconds.
- The Vulkan layer and ReShade host compile against the same runtime but have
  less end-to-end hardware coverage than suspended-process injection.

## Feature and neural runtime contracts

- The private D3D12 feature is created once per viewport from the exact
  mandatory values captured at the corresponding game feature create
  (SuperSampling or RayReconstruction; anything else is refused, not
  guessed). A missing contract is a hard error; there are no guessed quality,
  flag, subrect, or dispatcher fallbacks.
- Feature contracts are stored in a fixed process-local table and identified by
  NGX handle identity. Release/recreate is handled: the release detour resets
  the viewport's bridge state so a recreated feature at the same address
  rebuilds clean, and a recreated feature at the same geometry is re-created
  when its captured creation contract (quality, flags, SR/RR kind) drifts.
  High feature counts and contract variations beyond the known titles still
  need broader coverage.
- The D3D12 NGX interface must come from the exact session-local OptiScaler
  module. A different NGX dispatcher is not accepted because it could bypass
  the neural pass.
- The packaged OptiScaler DLL is pinned and hash-checked. Packaging disables
  its native Vulkan neural pass and redirects the visual-toggle handler to
  `DlssNrApplyModel`; the private D3D12 model stays active. It also advances
  the model past OptiScaler 0.8.3's creation guard when a Vulkan host drives
  the exported D3D12 path without a DXGI frame counter. A new release needs an
  explicit code review and patch update. Version 0.8.3 changed the D3D12 neural
  path substantially; its CPU-side frame pacing in games that stream assets
  during movement remains unverified.
- The application/project identity and SDK version come from the game's
  successful NGX initialization (Vulkan or D3D12 Init variants). These are
  vendor interfaces without a stable public cross-API bridge contract.
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
  or anti-cheat process may select the wrong profile or target. Only the
  GPU-placement bundles use matching; there are no per-game profiles to
  mismatch.
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
- `heroic configure` manages `PROTON_ENABLE_NVAPI=1` and
  `PROTON_HIDE_NVIDIA_GPU=0` alongside the wrapper so DX11/DX12 NGX titles
  expose DLSS. This matches current GE-Proton practice and is safe for
  DLSS-capable titles, but NVAPI exposure historically changes GPU-identity
  code paths: a non-NGX or fragile title that regresses should have the two
  keys edited manually after configure, and `heroic remove` restores their
  prior values.
- The DLSS-off idle notice is activity-gated (512 Vulkan presents or 512
  D3D12 accepted submissions) with zero successful game creates, fires once
  per process, and never appears before frames flow. D3D12 counting needs
  the injected host's `D3D12CreateDevice` hook; layer/add-on hosts without
  IAT patching observe no submissions there. D3D11 has no present or
  submission hook yet, so the notice is Vulkan + D3D12 only in v1. Long
  loading screens present rendering activity and can trip the hint before
  the user reaches the settings menu; enabling DLSS afterwards activates
  the bridge normally with no further action.

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
  dynamic resolution, or uncommon engine contracts. The D3D11 capture/host
  path and the multi-viewport (split-screen) executor have no hardware
  validation yet: treat D3D11/D3D12 capture, multi-viewport split-screen,
  FG coexistence, RR bridging, and child-process follow as experimental until
  passing game logs exist. Same-API reentrancy guards, per-viewport
  isolation, and fail-closed behavior still need a live D3D12 NGX title to
  prove out.
- Native Windows installer execution and upgrade behavior still require a
  release-environment smoke test.
