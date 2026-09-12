# Portable DLSS neural execution bridge

The current working implementation intercepts a Vulkan game's NGX temporal-upscaling call, recreates its frame contract on a private D3D12/NGX session, optionally executes that session on a second NVIDIA GPU, and reinserts the neural result before the game runs post-processing, UI, and presentation.

The path is hardware-validated with No Man's Sky under Proton on two RTX 3090s. Sunshine remains an independent launcher and capture server; it receives the game's ordinary presented display.

## Current implementation

- Vulkan NGX capture through layer, ReShade add-on, Vulkan proxy, or proxy-DLL host.
- Same-GPU D3D12 NGX execution.
- Secondary-GPU D3D12 NGX execution through directional host-memory staging.
- In-frame Vulkan reinsertion before downstream rendering.
- Portable C ABI for future host, capture, executor, and transport plugins.
- Portable configuration and frame-scheduler core.
- Exact process-local DXGI LUID selection, with the proven numeric index retained for the current NMS setup and UUID/PCI reserved as persistent controller identities.
- `same_gpu`, `secondary_gpu`, and `auto` execution policies.
- Shared profile resolver and launch entry points for Linux desktop, Steam/Proton, Sunshine, and Windows.
- GPU inventory reports using UUID and PCI identity.

The full target design and migration boundaries are in [docs/portable-architecture.md](docs/portable-architecture.md).

## Neural-only frame policy

The runtime permits the game-side DLSS call during discovery and private-feature bootstrap. After the first private neural frame completes, `require_neural_result=1` latches neural-only operation:

- the game-side DLSS evaluate is no longer executed;
- a late cross-GPU result repeats the last completed neural image;
- a frame that cannot enter the bridge returns NGX failure;
- a permanent bridge failure stays intercepted instead of silently resuming game DLSS.

This removes the old per-frame game-DLSS fallback. It also makes failures visible, which is intentional: the runtime must not claim that an original-DLSS frame received neural rendering.

## Configuration

Portable policy is TOML. Resolve it to the flat in-process format with:

```bash
./controller/dlss_bridge.py resolve \
  --config profiles/default.toml \
  --profile profiles/no-mans-sky-vulkan.toml \
  --output runtime/resolved/nms.cfg
```

Important settings:

```toml
[execution]
mode = "auto"                 # auto | same_gpu | secondary_gpu
compute_adapter = "auto"      # auto | game | index:N | luid:HIGH:LOW
require_neural_result = true
```

GPU series is never used as a compatibility switch. The final capability decision is based on adapter identity, driver/API support, model/runtime compatibility, feature creation/evaluation, resource formats, and the available transport route. This permits RTX 3000, 4000, and 5000 combinations when their actual runtime probes pass.

## Launch frontends

Each frontend resolves the same policy and exports `DLSS_BRIDGE_CONFIG`:

```bash
launchers/linux-desktop/run.sh --config profiles/default.toml -- /path/to/game
launchers/steam-proton/run.sh --config profiles/default.toml -- proton run game.exe
launchers/sunshine/run.sh --config profiles/default.toml -- /path/to/game-wrapper
```

The launchers and controller refuse to write outside this project. A game-local or Wine-prefix bundle is installed or mounted by the containing deployment system.

## Build and validation

```bash
bash scripts/build-vk-bridge.sh
bash scripts/bridge-load-test.sh
```

The Docker build emits every existing PE64 host under `runtime/build-vk-bridge/`. The portable core tests are:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -Icore/include \
  core/src/runtime.cpp tests/unit/runtime_test.cpp -o /tmp/dlss-runtime-test
/tmp/dlss-runtime-test
python3 tests/unit/controller_test.py
gcc -std=c11 -Wall -Wextra -Werror -Iinclude tests/unit/abi_c_test.c -o /tmp/dlss-abi-test
/tmp/dlss-abi-test
```

## Diagnostics

- `scripts/preflight.sh`: host, driver, Docker, and GPU inventory.
- `controller/dlss_bridge.py probe`: portable JSON GPU identity report.
- `scripts/transfer.sh`: bidirectional byte-validated transfer.
- `scripts/neural-probe.sh`: NGX and neural model initialization.
- `scripts/neural-mgpu.sh`: synthetic two-GPU D3D12 route.
- `scripts/build-vk-bridge.sh`: all bridge host builds.

The current cross-adapter transport is functional but still uses a GPU 0 D3D12 helper on the return path. GPU timestamp instrumentation and direct Vulkan import of the directional host ring remain the next performance backend milestone.
