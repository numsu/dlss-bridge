# Portable architecture

DLSS Bridge attaches to a 64-bit Windows Vulkan, Direct3D 11, or Direct3D 12
process, captures its NVIDIA NGX temporal-upscaling contract, executes the
corresponding private D3D12 NGX operation through the configured neural host,
and reinserts the result before the game records downstream rendering. Only
the Vulkan path is hardware-validated; the D3D11 and D3D12 paths are
implemented but untested.

The same package supports native Windows and Windows games launched through
Proton. The launcher creates an isolated user-state directory and injects the
bridge without changing the game directory or Wine prefix.

## Current execution path

```text
game NGX Vulkan/D3D11/D3D12 create
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

game NGX D3D12 evaluate (implemented, untested; same-adapter only)
  -> transition game resources for copies (documented NGX input states),
     restore before evaluating
  -> record copy-in into the game's open command list
  -> private neural evaluate recorded inline on the same list (the session
     shares the game device, so list order keeps inputs current and the
     output complete before downstream passes)
  -> record copy-back into the still-open list
    -> same-API reentrancy fenced by skipping the session-local OptiScaler
       module plus a thread-local in-bridge guard honored at every private
       create/evaluate entry point (create, synchronous, pipelined) and in
       both the evaluate and create detours
 
 A rebuild or teardown retires the shared-texture generation rather than
 freeing it: completion is tracked on the game's actual queue. A fence on the
 game device is signalled after every submission the game accepts (observed
 through a per-object `ExecuteCommandLists` vtable patch, itself discovered
 via a device `CreateCommandQueue` patch installed from the app-IAT
 `D3D12CreateDevice` hook; no system module code is modified). A retired
 generation carries the fence value of the latest accepted submission --
 which orders behind its last referencing list -- and is released only after
 that value completes (non-blocking poll per frame; bounded wait on
 rebuild/teardown). If no submission was observed yet, nothing of the
 bridge's work is in flight on the game's queue, so the legacy immediate
 release applies. The private feature is released the same way (it is
 consumed by the game's queue, not the bridge's). The depth store view
 releases immediately (bridge timeline).
```

There is one in-frame execution path. If its synchronization objects cannot be
created, initialization fails. The runtime does not select another NGX
dispatcher or resume the original game-side upscaler after neural-only mode has
latched.

## Components

The runtime composes four statically linked component kinds:

- **Host:** obtains process and graphics lifecycle boundaries
  (`injected-vulkan`, `injected-d3d11`, `injected-d3d12`, Vulkan layer,
  ReShade add-on).
- **Capture:** recognizes NVIDIA NGX feature creation and evaluation
  (`ngx-vulkan`, `d3d11`, `ngx-d3d12`).
- **Executor:** owns the private D3D12 device (or the adopted game device for
  same-adapter D3D12 capture), per-viewport NGX sessions, and neural calls.
- **Transport:** moves the temporal resources between the game and executor
  (`vulkan-d3d12`, `d3d11-d3d12`; same-adapter NT sharing or directional host
  staging). D3D12 same-adapter capture needs no cross-device transport: the
  session shares the game device and records into the game's lists.

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

Frame Generation and Ray Reconstruction are separate contracts tracked by
handle identity alongside Super Resolution. Frame Generation features are
forwarded untouched (never bridged, never refused): FG titles keep native
frame generation while their Super Resolution frames bridge. Ray
Reconstruction on the D3D12 path is bridged: denoiser-guidance textures and
view matrices ride the same per-viewport private feature under the game's own
key spellings, and dedicated feature-13 handles resolve to their own
viewport. Vulkan RR frames stand aside; D3D11 has no RR in the NGX SDK.

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
- `[capture]`: `adapter`, `max_viewports`
- `[launch]`: `follow_children`, `target_executable`

There are no per-game profiles, and none are needed. The graphics API is not
configured: the hook observes the game's own NGX calls and the runtime
selects the matching capture (`ngx_vulkan`, `d3d11`, `ngx_d3d12`) itself, so
an adapter/API mismatch can only come from an explicit custom profile, which
fails resolution. Viewports likewise need no per-game setting: they allocate
on demand up to the static capacity of four, so single-feature and
split-screen titles run on defaults (`capture.max_viewports` remains as an
optional bound against pathological multi-feature processes). The only
bundled profiles are GPU-placement policies (`same-gpu`, `secondary-gpu`).
The D3D11 capture path runs synchronously on the game thread (immediate
context order is the barrier) and rides the shared host-staging ring when a
secondary GPU is configured. The D3D12 capture path records into the game's
open lists on the adopted game device and stands aside under a secondary GPU
(a cross-device D3D12 capture needs an async submit-boundary handoff, which
is planned); Ray Reconstruction plus secondary-GPU execution stands aside
(no neural-side RR textures yet).

Feature placement, working scale, and pass count are written to the
session-local OptiScaler configuration. The remaining settings become the
bridge runtime policy. There is no launch-context label in either policy.
The DLL parses that generated policy strictly as well: unknown, malformed, or
inconsistent settings stop initialization instead of being ignored or clamped.

Heroic launcher state is outside the neural policy: `heroic configure`
prepends the bridge wrapper and ensures `PROTON_ENABLE_NVAPI=1` plus
`PROTON_HIDE_NVIDIA_GPU=0` in `enviromentOptions` so Proton exposes the
NVIDIA GPU and deploys NGX before the game queries DLSS support. Only those
two env keys are managed; removal restores their prior values.

## Session lifecycle

A launch session contains the hook, launcher, neural host, verified neural
runtime, and generated configuration. The target executable in the command is
replaced by the session launcher. Other command arguments remain in order.
The game directory and compatibility prefix are not modified.

With `--follow-children TARGET.exe` (or the equivalent `[launch]` section in
a custom profile), the target is the launcher: it starts suspended and
injected as usual, and its `CreateProcessW/A` imports are redirected so
every child it spawns is force-created suspended, injected with the session
hook, initialized, and resumed iff the bridge added the suspension. The
target basename only selects the bridge's renderer: a matching child is the
game, while intermediate launchers (Launcher -> Helper -> Game chains) are
carried along because each injected process re-arms the same follow rule.
The flag's presence is what enables following; a child of an unmatched,
uninjected process cannot be intercepted. Failed follows run exactly as the
launcher requested. The child inherits the launcher's environment, so no
session surgery is needed per process.

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
process. Frame Generation handles are exempt from fail-closed: they always
forward untouched, even after the SR path has latched or failed.

When the game never creates an NGX feature — DLSS off in its graphics
settings — the bridge stays inert: there is no contract to mirror, so no
neural work runs and nothing fails. After 512 observed rendering-activity
units (Vulkan presents, D3D12 accepted game-queue submissions) with zero
successful game creates, the bridge logs one `[bridge] ... DLSS appears
disabled in-game, so the bridge is idle` hint. Activity-gating keeps the
notice silent during process start and loader stalls; any later successful
create suppresses it and the bridge activates normally.

## Planned extension paths

The repository inventories native Linux Vulkan, Vulkan execution, peer
memory, and final-frame processing as planned components:
- Peer GPU memory stays planned as a pure optimization: directional
  host-staging already serves every secondary-GPU route on all three capture
  APIs, and peer mappings additionally require hardware with P2P support
  plus capability probing that no supported title needs to function.
- Final-frame processing stays planned for lack of a contract: with no NGX
  feature to key off, there is nothing to mirror and any geometry or quality
  would have to be invented, which the failure policy forbids.
- Native Linux Vulkan stays planned behind a native neural executor: the
  neural runtime (OptiScaler plus `nvngx_dlssnr.dll`) is Windows-only, so a
  native host would also need a native neural stack first.
Native Linux execution additionally requires that native neural executor;
the current executor is D3D12. D3D12 capture is implemented but, like D3D11
capture, awaits hardware validation before it can be called validated.

Streamline-native titles (sl.interposer/sl.dlss/sl.dlss_g/sl.reflex/sl.pace)
are covered without a dedicated backend: those plugins call the same
in-process NGX exports, which the existing detours already intercept.
`sl.dlss_g` Frame Generation evaluates from its own present thread and hits
the same handle-tracked passthrough, serialized like everything else; Reflex
and Pace only observe timing, so the bridge's host-block latency shows up in
their measurements without breaking correctness. Dynamic resolution through
`NVSDK_NGX_UpdateFeature` needs no hook: every evaluate re-reads actual
image geometry, so size drift rebuilds regardless of how the game announced
it.

See [Known limitations](known-limitations.md) for concrete boundaries and test
coverage.
