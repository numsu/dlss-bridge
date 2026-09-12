# dlss5-vk-bridge

A **Vulkan port** of [NIGos' DLSS 5 DX11 Bridge](https://github.com/NIGos/dlss5-dx11-bridge).

DLSS 5 (Neural Rendering) add-ons — such as the RenoDX ReShade add-on that
inserts the neural pass — only hook the **D3D12** NGX evaluate. A Vulkan game
never makes that call, so the neural pass never runs for it. This bridge closes
that gap for Vulkan titles the same way the original does for D3D11: it
intercepts the game's NGX Vulkan evaluate, mirrors the DLSS contract onto a
private **D3D12** device, and runs a second NGX evaluate there — the call the
add-on detours — then copies the neural result back into the game's own image.

> **Status: working.** Verified end to end on hardware (RTX 5090, driver
> 610.74, Baldur's Gate 3 Vulkan): a DLSS 5 add-on attaches to the bridge's
> private D3D12 evaluate and its result is visible in-game. Still a young
> project — bug reports and logs from other titles are very welcome.

The bridge comes as two builds of the same core, and you install **one**:

| build | file | how it gets into the game | line |
|-------|------|---------------------------|------|
| **ReShade add-on** | `dlss5-vk-bridge.addon64` | drop it next to the DLSS 5 add-on; ReShade loads it | v0.2 (prerelease — field reports wanted) |
| **Vulkan layer** | `dlss5-vk-bridge.dll` + `.json` | registry entry + environment variable | v0.1 (field-verified) |

---

## Is this for you?

DLSS 5 neural rendering reaches a game through an add-on (RenoDX's
renodx-dlss, or Deep Fried Chicken) that hooks the game's **own** DLSS call --
and it can only hook the DirectX 12 flavour of that call. So the tool you need
depends on what your game already does:

| Your game | What you need |
|-----------|---------------|
| Runs on **Vulkan** and has a **DLSS option in its own graphics settings** -- Baldur's Gate 3 in Vulkan mode, X4: Foundations, DOOM Eternal, Indiana Jones and the Great Circle, No Man's Sky ... | **This bridge**, plus the DLSS 5 add-on |
| Runs on **Vulkan** and has **no DLSS option at all** -- including old games that DXVK pushes onto Vulkan | [**DLSS5-Feeder**](https://github.com/jlrouzies-fr/DLSS5-Feeder), plus the DLSS 5 add-on. Not this bridge: the game never makes a DLSS call, so there is nothing for it to catch |
| Runs on **DirectX 12** (or DirectX 11) and has DLSS | The DLSS 5 add-on on its own. Nothing to bridge. (For DirectX 11 there is also [NIGos' DX11 bridge](https://github.com/NIGos/dlss5-dx11-bridge), which this project is a port of) |
| Runs on **DirectX 9, OpenGL**, or is **32-bit**, and has no DLSS | DLSS5-Feeder |

**The quick check:** start the game in Vulkan mode and look for *DLSS* (or
*NVIDIA DLSS Super Resolution*) in its graphics settings. If it is there, this
bridge is for you. If the game lets you choose between DirectX 12 and Vulkan,
the simplest route is DirectX 12 with the add-on alone; the bridge is for when
you want or need the Vulkan renderer.

**What you get:** the game's own DLSS pass, unchanged -- its real motion
vectors, depth and jitter, and whatever Quality / Balanced / Performance
scaling you picked -- with the neural pass inserted into it. DLSS5-Feeder
cannot offer that on its games, because they make no DLSS call: it has to build
a request out of the finished frame (native-resolution DLAA, estimated motion,
HUD included). That is the right tool where the game has no DLSS, and the
wrong one where it does.

**Never install this bridge and DLSS5-Feeder into the same game.** The Feeder
does not know the frame was already bridged and would run the neural pass a
second time over it, with estimated motion vectors and the HUD baked in. And
within the bridge, install one host only: the add-on or the layer.

---

## How it works

The bridge is two cooperating pieces in one module: a host that gives it the
game's Vulkan device, queues and submits, and the bridge core that does the
work. The Vulkan-layer host is described first; the ReShade add-on host that
replaces it in v0.2 is at the end of this section.

1. **A Vulkan layer** (`VK_LAYER_dlss5_vk_bridge`). Interop across the API
   boundary needs device extensions the game never enables
   (`VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32`, timeline
   semaphores). The layer intercepts `vkCreateInstance` / `vkCreateDevice` and
   adds them transparently, and it exposes the one place a Vulkan semaphore can
   be signalled — `vkQueueSubmit` — which is where the cross-API handoff is
   driven.

2. **An NGX Vulkan evaluate hook.** Every module exporting
   `NVSDK_NGX_VULKAN_EvaluateFeature` is patched with a 14-byte jump so the
   game's DLSS call can be forwarded untouched and then mirrored.

Per frame (`sync = 1`, the default) everything below is recorded **into the
game's own command buffer** at the evaluate hook — between the game's DLSS call
and the post-processing that consumes its output:

```
game cmd buffer:  game evaluate │ input copies  Color/Depth/MV ─▶ shared textures
                  │ SetEvent(ev_inputs) │ WaitEvents(ev_output) ◀── GPU parks here
                  │ copy-back  shared Output ─▶ game Output │ SetEvent(ev_done)
                  │ game post-fx ...

worker thread:    sees ev_inputs ─▶ signals fence_in ─▶ private D3D12 evaluate
                      └─ a DLSS 5 add-on inserts its neural pass here
                  waits fence_out ─▶ sets ev_output ─▶ the game's frame resumes
```

The park is what makes the result reachable at all: games read their DLSS
output in the same command buffer, right after the evaluate, so a copy-back
submitted any later (kept as `sync = 0`, diagnostic only) lands on pixels
nobody reads. After the first completed private neural frame, strict mode skips the game-side evaluate. The private result is authoritative for the rest of the process.

The worker is armed the moment the sandwich is recorded. Which of the game's
queue submissions later carries that command buffer is the game's business:
the layer's `vkQueueSubmit`/`vkQueueSubmit2` hooks match every submit against
the command buffers the bridge has work in (following `vkCmdExecuteCommands`
when the pass lives in a secondary), and only a match gets the bridge's
fence queued behind it. Engines that record and submit their DLSS pass from
another thread, on another queue family, with unrelated submits in between
(idTech 8 does all three, on its async-compute family) therefore work the same
as single-threaded ones. The game's thread never waits on the bridge: a frame
slot whose sandwich has not retired is not reused, and that frame is simply
forwarded untouched — the bridge can degrade, never wedge the GPU.

The shared textures live on the D3D12 side (`HEAP_FLAG_SHARED` +
`ALLOW_SIMULTANEOUS_ACCESS`) and are imported into Vulkan as aliased `VkImage`s.
Two shared D3D12 fences plus three `VkEvent`s per frame slot carry the
synchronisation: the worker releases the D3D12 queue the moment the inputs are
on the GPU and unparks the game's queue the moment the result is written;
`ev_done` tells the bridge when the slot has fully retired, whether or not its
submit was ever seen. (The same fences are also imported as Vulkan timeline
semaphores for the `sync = 0` path.) The private D3D12 device is chosen by **LUID** so it sits on the same
physical GPU as the game's Vulkan device — cross-API shared handles require it.

This mirrors the D3D11 original almost exactly on the D3D12/NGX side; the new
work is all at the Vulkan edge (extension injection, memory/semaphore import,
and the in-frame event sandwich, because Vulkan has no implicit
immediate-context queue to signal inline the way D3D11 does).

### The ReShade add-on host (v0.2)

The add-on the bridge exists for is a ReShade add-on already, so from v0.2 the
bridge can be one too: `dlss5-vk-bridge.addon64` sits next to it and ReShade
loads both. Nothing is registered machine-wide and no other Vulkan process ever
sees the bridge. ReShade's events supply what the layer used to intercept —
the `VkDevice`, every queue with its capabilities, and every command buffer as
it begins, executes secondaries and is submitted — and ReShade creates the
device with `VK_KHR_external_memory_win32`, which is all the shared-texture
import needs. Two gaps are closed differently from the layer:

* An add-on has no instance or physical device it may use, so the one
  physical-device query the core needs (memory properties) is answered through
  a private, one-call `VkInstance` created with every implicit layer disabled.
  The shared-texture imports additionally ask the driver which memory types the
  handle itself allows (`vkGetMemoryWin32HandlePropertiesKHR`).
* ReShade reports a submit *before* it goes down, so nothing can be queued
  behind the game's work: a frame slot retires through the event at the tail
  of its sandwich instead of a fence, and only the in-frame handoff
  (`sync = 1`, the mode that produces a visible result) is available.

The NGX hooks, the private D3D12 session and the per-frame sandwich are the
same code in both builds.

---

## Requirements

* An **NVIDIA RTX** GPU with a DLSS 5 / Neural Rendering capable driver.
* A **DLSS 5 Neural Rendering add-on** loaded in the process (e.g. the RenoDX
  ReShade add-on) plus the neural model DLL (`nvngx_dlssnr.dll`). The add-on is
  what actually inserts the neural pass into the D3D12 evaluate the bridge runs;
  without it the bridge just reproduces ordinary DLSS.
* **ReShade with add-on support**, installed for the game as a **Vulkan** layer,
  so the add-on is in-process and hooks the D3D12 NGX calls the bridge makes.
  The add-on build of the bridge needs ReShade **6.6 or newer** (add-on API 18).
* A game that already uses **DLSS Super Resolution through Vulkan/NGX**.

The bridge does nothing on its own — it exists to let a D3D12-only neural
add-on reach a Vulkan game. If anything fails it disables itself and the game
renders normally.

---

## Install

Pick one host. Do not install both: the add-on stands down (with a log line)
when it finds the layer loaded in the same process.

### ReShade add-on (v0.2, prerelease)

1. Install ReShade (Vulkan) with add-on support into the game, plus the DLSS 5
   Neural Rendering add-on and `nvngx_dlssnr.dll` — the usual RenoDX setup.
2. Grab `dlss5-vk-bridge.addon64` from the release and drop it into the same
   folder as the DLSS 5 add-on (ReShade loads every `.addon64` from its own
   folder, or from the `AddonPath` set in `ReShade.ini`). Nothing to register.
3. Launch the game and turn DLSS on. ReShade's add-on list (the overlay's
   *Add-ons* tab) shows **dlss5-vk-bridge**, and `dlss5-vk-bridge.log` appears
   next to the `.addon64`: the add-on registers, ReShade reports the device,
   the NGX hook lands, the D3D12 session opens, and frames flow.

To disable, delete or rename the `.addon64` (or tick it off in ReShade's
add-on list). If you previously used the layer, unregister it first (below).

### Vulkan layer (v0.1 line)

1. Build (see below) or grab `dlss5-vk-bridge.dll` and `dlss5-vk-bridge.json`
   from the latest release. Keep the two files together, in a folder you can
   write to (the log and config are created next to the DLL -- avoid
   `Program Files`).
2. Register the layer and turn it on (paste into PowerShell or cmd, with your
   own path):

   ```bat
   reg add "HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "C:\path\to\dlss5-vk-bridge.json" /t REG_DWORD /d 0 /f
   setx ENABLE_DLSS5_VK_BRIDGE 1
   ```

   Then **restart Steam completely** (tray icon → Exit) so it picks up the
   variable. The layer is gated behind `ENABLE_DLSS5_VK_BRIDGE`, so registering
   it does nothing until the variable is set.

   > Why not `VK_ADD_LAYER_PATH`? The loader treats layers found through it as
   > explicit and ignores their `enable_environment`, and per-session variables
   > rarely survive a Steam launch chain anyway. The registry route works from
   > any launcher. (`VK_ADD_IMPLICIT_LAYER_PATH` + the enable variable also
   > works on loaders ≥ 1.3.234, **if** you launch the game exe directly from
   > that same shell.)

   **If the game runs as administrator, `HKCU` is not enough.** The Vulkan
   loader deliberately ignores `HKCU\...\ImplicitLayers` (and `VK_LAYER_PATH`
   & co.) in a process whose token is high-integrity, so that an elevated
   process never loads a library that did not need administrator rights to
   install. A game whose launcher or anti-cheat elevates inherits that token,
   so a user-level registration is never seen and the log file is never
   created (Arknights: Endfield, issue #4). Register the layer machine-wide
   instead, from an elevated prompt:

   ```bat
   reg add "HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "C:\path\to\dlss5-vk-bridge.json" /t REG_DWORD /d 0 /f
   ```

   `ENABLE_DLSS5_VK_BRIDGE` still gates the layer either way; an elevated
   process of the same account inherits your user variables.
3. Install ReShade (Vulkan) with add-on support into the game, plus the DLSS 5
   Neural Rendering add-on and `nvngx_dlssnr.dll`.
4. Launch the game and turn DLSS on. Check `dlss5-vk-bridge.log` next to the
   DLL: you should see the layer load, the NGX hook land, the D3D12 session
   open, and frames flow. If the log never appears, the loader did not load
   the layer at all: re-check the registration (and the administrator note in
   step 2).

To disable temporarily: `setx ENABLE_DLSS5_VK_BRIDGE 0` (and restart Steam).
To remove, delete the folder and the registry value (whichever hive you used;
the `HKLM` one needs an elevated prompt):

```bat
reg delete "HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "C:\path\to\dlss5-vk-bridge.json" /f
reg delete "HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers" /v "C:\path\to\dlss5-vk-bridge.json" /f
```

---

## Configuration

The bridge reads `DLSS_BRIDGE_CONFIG` when set, otherwise it reads
`dlss5-vk-bridge.cfg` beside the module. The portable controller resolves TOML
profiles into this flat runtime format.

```ini
mode=0
flags=-1
subrects=1
verbose=0
sync=1
execution_mode=secondary_gpu
compute_adapter=luid:00000000:12345678
require_neural_result=1
ring_slots=3
latency_budget_ms=16
```

| key | default | meaning |
| --- | --- | --- |
| `execution_mode` | `auto` | `same_gpu`, `secondary_gpu`, or `auto` execution policy |
| `compute_adapter` | `auto` | `auto`, `game`, `index:N`, or session-local `luid:HIGH:LOW`; persistent UUID/PCI selectors require platform-controller correlation |
| `require_neural_result` | `1` | after the first completed private result, never execute game DLSS as a frame fallback |
| `ring_slots` | `3` | requested scheduler ring size; the current Vulkan backend provides three |
| `latency_budget_ms` | `16` | frame budget; secondary-GPU chain deadline is twice this value, clamped to 32-250 ms |
| `mode` | `0` | `0` normal, `1` omit private evaluate for diagnostics, `2` magenta copy-back test |
| `flags` | `-1` | copy the game's DLSS create flags |
| `subrects` | `1` | enable output subrects on the private feature |
| `verbose` | `0` | extra frame logging |
| `sync` | `1` | required in-frame copy-back mode |

`neural_adapter=N` and `game_eval=0` remain accepted as legacy inputs. A numeric adapter index and a Wine/DXGI LUID are not durable identities. Persist UUID or PCI identity once the platform correlation backend is available; the current NMS profile retains its proven launch-time index. Strict neural-only policy overrides `game_eval`. Startup still
forwards observation/bootstrap evaluates until the private feature completes its
first frame. It then latches: a late neural result or a temporarily unavailable slot repeats
the last completed neural image rather than silently executing the original game
upscaler.

---

## Known limitations (v0.3 portable runtime)

* **The add-on host (v0.2) is a prerelease** that has passed the compiler and
  the code reading, not a game yet: the same core as v0.1.14, driven by
  ReShade's events instead of the loader chain, and shipped so that people
  who already run the RenoDX setup can try it without touching the registry.
  It supports only the in-frame handoff (`sync = 1`), reads memory properties
  through a private throwaway `VkInstance` (visible in a loader log as a
  second instance created from the game's render thread, once), and needs
  ReShade 6.6 or newer with add-on support. If it fails during bootstrap it disables itself with a reason in the log. After
  neural-only mode latches it fails closed and does not restore game DLSS; attach
  the log to an issue.
* **Hardware-verified end to end** (RTX 5090, driver 610.74, Baldur's Gate 3
  Vulkan; RTX 5080, driver 616.56, Indiana Jones and the Great Circle): layer
  load, private NGX D3D12 session, feature creation, in-frame per-frame
  bridging, an add-on detoured on the private evaluate, and the bridged result
  visible in-game. Other titles will surface new cases — logs welcome.
* **Multi-queue engines** (idTech 8 — Indiana Jones and the Great Circle)
  submit their DLSS pass on an async-compute family, which cannot run
  `vkCmdBlitImage`. The bridge therefore learns that family from the first
  evaluate's own submit before it records anything (during the start-up
  grace, at no cost), aliases the game's motion-vector format directly
  (RG16F, RG32F, RGBA16F, RGBA32F) so the inputs are copied rather than
  blitted, and forwards untouched — with a log line — any frame whose formats
  would still need a blit on such a family. Built for issue #3 (v0.1.13's
  submit matching, v0.1.14's no-blit-before-the-family-is-known rule) and
  confirmed in the field with v0.1.14: the family is learned before the first
  frame is touched, and the add-on's pass runs on every bridged frame.
* **Frame cost.** With `sync = 1` every frame waits for the private D3D12
  evaluate before the game's post-processing runs -- that wait is what makes the
  result visible at all (games consume their DLSS output inside the same
  command buffer). Strict neural-only mode recovers the cost of the game-side
  evaluate after bootstrap. In secondary-GPU mode, an early Vulkan host enables
  `VK_EXT_external_memory_host` and imports both directional host buffers, so
  Vulkan performs the game-adapter input and reinsertion copies without GPU 0
  D3D12 helper queues. A late GPU 1 chain is capped at twice
  `latency_budget_ms` (32-250 ms) and repeats the last neural image.
* **DLSS Frame Generation is not bridged around — the bridge stands aside.**
  Streamline's FG (`sl.dlss_g`) pairs every present with that frame's DLSS
  evaluate and interpolates at present time; the bridge's in-frame stall and
  private second evaluate break that pairing inside FG's own bookkeeping (two
  X4: Foundations minidumps show the identical crash in `sl.dlss_g.dll`, one
  frame after the first bridged evaluate). The bridge watches NGX feature
  creation and latches the moment a FrameGeneration feature is created — and
  because games bring DLSS features up in a burst (SuperSampling first,
  evaluates flowing at once, FG a few milliseconds later), it also waits
  3 seconds after the first bridgeable evaluate before touching any frame, so
  an FG create in the same burst always wins. **Disable Frame Generation in
  the game's settings (and restart it) to use the bridge.** Enabling FG from
  the menu of a session that has already been bridging is REFUSED instead of
  forwarded: FG's bookkeeping cannot absorb a frame history the bridge has
  touched (that collision crashed X4 twice). Field data shows X4's own FG
  enable path may crash on ANY create failure, refusal included — so treat
  Frame Generation as a launch-time choice, never a mid-game one, with or
  without this bridge in the picture. Pick one per launch; FG coexistence is
  future work.
* **DLSS Super Resolution only.** Ray Reconstruction (DLSSD) evaluates travel
  through the same NGX entry point and hand over the same colour, depth and
  motion-vector resources, but their result is *denoised* — mirroring one onto
  the bridge's super-sampling feature would upscale the still-noisy input and
  overwrite the denoised frame with it. Since v0.1.7 the bridge recognises
  those frames by their denoiser inputs and stands aside, so Ray Reconstruction
  renders exactly as it does without the bridge (it just is not bridged). The
  log names the key that identified them.
* **One DLSS evaluate per frame** is bridged; extra features in the same frame
  are forwarded but not mirrored.
* **Exposure texture** inputs are not bridged; auto/neutral exposure is used.
* **Secondary GPU is hardware-verified on two RTX 3090 cards under
  Proton/NMS.** GPU 0 renders and presents while a private D3D12 NGX session on
  GPU 1 evaluates. The tested PCIe topology has no supported peer access, so
  the transport uses directional host allocations imported by Vulkan and
  opened by D3D12. Other adapter pairs remain capability-probed rather than
  enabled by a GPU-series allowlist.

The layer build is registered implicitly, so it is loaded into *every* Vulkan
process on the machine — most of which will never touch DLSS. It is written so that it
can never be the reason one of them fails: everything it adds to an instance or
device is added only if the driver advertises it, a create call that fails with
those additions is retried with the application's own arguments untouched, and
a handle the layer is not tracking is forwarded straight down the chain instead
of being refused. If a game still will not start, `setx ENABLE_DLSS5_VK_BRIDGE 0`
(and restart Steam) turns the layer off without uninstalling anything — please
open an issue with the `dlss5-vk-bridge.log` if that is what it takes.

---

## Build

Windows + MSVC. Vulkan headers are fetched automatically; the ReShade add-on
API headers (v6.7.0, BSD 3-clause) are vendored under `third_party/reshade`.
Neither build links a Vulkan or D3D12 import library — everything is resolved
at runtime.

```bat
cmake -B build -S . -A x64
cmake --build build --config Release
```

Output: `build\Release\dlss5-vk-bridge.dll` (layer) and
`build\Release\dlss5-vk-bridge.addon64` (ReShade add-on), both from
`src/dlss5-vk-bridge.cpp` — the `DLSS5VK_ADDON_HOST` define selects
`src/addon_host.inc` over `src/vk_layer.inc`. CI
(`.github/workflows/build.yml`) builds the same way on `windows-latest` and
uploads both as one artifact.

---

## Credit & license

This is a port. All of the hard-won D3D12/NGX behaviour — the parameter
forwarding rules, the version negotiation, the crash and robustness handling —
comes from **NIGos' DLSS 5 DX11 Bridge**, and this project would not exist
without it:

> <https://github.com/NIGos/dlss5-dx11-bridge>

Licensed **MIT** (see [LICENSE](LICENSE)), preserving the original copyright.

Not affiliated with or endorsed by NVIDIA. DLSS is a trademark of NVIDIA
Corporation. "NGX" symbol names are used only for interoperability.
