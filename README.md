# Portable DLSS neural execution bridge

DLSS Bridge inserts neural rendering into a supported game's temporal
upscaling pipeline. The neural workload can run on the game GPU or on another
NVIDIA GPU, and the result returns to the game before post-processing, UI, and
presentation.

The validated Linux path uses a Vulkan NGX title through Proton with two RTX
3090 GPUs. Native Windows uses the same launcher and injected host without Wine.

## Supported path

The validated release path is 64-bit Windows Vulkan games that expose NVIDIA
NGX:

- Steam Proton on Linux
- Native Windows
- Same-GPU or secondary-GPU neural execution
- RTX 3000, 4000, and 5000 series cards when runtime capability checks pass

Direct3D 11 capture and multi-viewport split-screen (`capture.max_viewports`
up to 4) are implemented but untested — see `docs/known-limitations.md`.
`profiles/bg3-dx11.toml` is experimental; prefer `profiles/bg3.toml`.

Native Linux games and Direct3D 12-only capture are planned backends. Frame
generation is not supported by the current in-frame execution contract.

## Install

Linux:

```bash
curl -fsSL https://github.com/numsu/dlss-bridge/releases/latest/download/install.sh | sh
```

Windows:

1. Download and run `dlss-bridge-windows-x86_64-setup.exe` from the latest
   release.
2. Restart Steam.

Set the Steam launch option:

```text
dlss-bridge run -- %command%
```

The launcher attaches DLSS Bridge from an isolated user-state session and
removes that session when the game exits. The game directory and Wine prefix
remain untouched. Omit the prefix to launch normally.

You can configure the same option without opening Steam's Properties dialog.
Exit Steam first, then run:

```bash
dlss-bridge steam configure APPID --profile same-gpu
```

Restore the previous setting with `dlss-bridge steam remove APPID`.

Both installers download and checksum-verify the pinned NVIDIA neural runtime;
installation fails if it is unavailable or invalid. The runtime is not bundled
in this project’s release artifacts. Rerun the Linux installer or run a newer
Windows setup EXE to update. Runtime files, settings, and collected logs remain
in user state.

See [Installation and updates](docs/installation.md) for requirements, GPU
selection, conflicts, removal, and troubleshooting.

## Toggle neural rendering in game

Press **Ctrl+Shift+N** to show or hide the neural effect. Capture, transport, and the
private DLSS upscaler and neural model remain active, so this provides an
immediate visual comparison without rebuilding the feature or returning to the
game's original upscaler. OptiScaler displays the applied state on screen. The
session log records each request and reports state-separated GPU timing once it
has enough completed samples.

## GPU selection

Automatic selection prefers another compatible NVIDIA GPU and uses the game GPU
when no secondary adapter is available.

```text
dlss-bridge run --profile same-gpu -- %command%
dlss-bridge run --profile secondary-gpu -- %command%
```

## Runtime behavior

The runtime intercepts the game's Vulkan NGX temporal-upscaling call, recreates
its frame contract in a private D3D12/NGX session, executes neural rendering,
and reinserts the result before downstream rendering.

After the first private neural frame completes, neural-only operation is
latched:

- the game-side DLSS evaluate is not executed;
- a late result repeats the last completed neural image;
- a frame that cannot enter the bridge reports NGX failure;
- a permanent bridge failure remains visible instead of silently resuming the
  game's original DLSS path.

## Configuration

Policy is stored in TOML. The default profile uses automatic GPU placement, a
four-slot ring, a two-frame pipeline, and the unified neural queue.

```toml
[feature]
placement = "before_sr"
working_scale = 1.0
passes = 1

[execution]
mode = "auto"
compute_adapter = "auto"

[transport]
neural_queue = "unified"
ring_slots = 4
neural_pipeline_frames = 2
```

Custom profiles can override these values:

```text
dlss-bridge run --profile /absolute/path/custom.toml -- %command%
```

GPU generations are not used as compatibility switches. Adapter identity,
driver and API support, model compatibility, resource formats, feature
creation, and transport probes determine whether a route is usable.

## Architecture

The implementation provides:

- Vulkan NGX capture through suspended-process injection, a Vulkan layer, or a ReShade add-on
- Same-GPU D3D12 NGX execution
- Secondary-GPU execution through directional host-memory staging
- In-frame Vulkan reinsertion
- A versioned C ABI for capture, executor, transport, and host backends
- Shared configuration, scheduling, adapter identity, and telemetry policy
- Linux/Proton and native Windows launch wrappers

See [Portable architecture](docs/portable-architecture.md) for interfaces,
frame ownership, synchronization, and planned backends. [External
dependencies](docs/dependencies.md) records the version and purpose of every
third-party runtime, build, and test component. See [Known
limitations](docs/known-limitations.md) for current coverage and remaining
extraction work.

## Build and test

Build the Windows bridge DLLs in Docker:

```bash
scripts/build-vk-bridge.sh
```

Build a Linux release archive:

```bash
VERSION=vVERSION packaging/build-linux-package.sh dist
```

Run the core checks:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -Icore/include \
  core/src/runtime.cpp core/src/components.cpp tests/unit/runtime_test.cpp -o /tmp/dlss-runtime-test
/tmp/dlss-runtime-test
PYTHONDONTWRITEBYTECODE=1 python3 tests/unit/controller_test.py
gcc -std=c11 -Wall -Wextra -Werror -Iinclude \
  tests/unit/abi_c_test.c -o /tmp/dlss-abi-test
/tmp/dlss-abi-test
scripts/bridge-load-test.sh
```

Tagged releases build the Linux archive and native Windows setup EXE through
[the release workflow](.github/workflows/release.yml).
