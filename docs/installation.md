# Installation and updates

DLSS Bridge attaches only when its command prefixes a game launch. It does not
run as a background service, modify the game directory or Wine prefix, or
register a system-wide Vulkan layer.

## Compatibility

The validated release path is 64-bit Windows Vulkan games that expose NVIDIA
NGX Super Resolution. Direct3D 11 and Direct3D 12 capture, multi-viewport
(`capture.max_viewports` up to 4), Frame Generation coexistence, Ray
Reconstruction on D3D12, and child-process follow are implemented but
untested — no hardware-validated game run exists yet. D3D11/D3D12 capture
additionally requires float depth in v1 (no conversion pass); secondary-GPU
execution rides the shared host-staging ring on all three APIs.

| Host | Game runtime | Attachment method |
| --- | --- | --- |
| Linux | Steam Proton | Suspended-process injection inside Proton |
| Windows | Native Windows | Suspended-process injection |

The launcher injects the bridge before the game starts and patches Vulkan
imports in the executable and loaded engine modules. The neural workload can run
on the game's NVIDIA GPU or another NVIDIA GPU. RTX 3000, 4000, and 5000 series
cards are selected through runtime capability checks rather than a generation
allowlist.

Native Linux games and system-wide automatic injection are not supported in
this release. Streamline-native titles work through the NGX-level hooks with
no extra setup (Cyberpunk 2077's sl.* chain is intercepted transparently).
No per-game configuration exists: the graphics API is auto-detected from the
game's own NGX calls at runtime, and viewports allocate on demand up to the
static capacity of four, so single-feature and split-screen titles alike run
on defaults. The only bundled profiles are GPU-placement policies
(`same-gpu`, `secondary-gpu`).

Titles exercised or queued during development (all untested unless noted):
Vulkan — No Man's Sky (works), DOOM Eternal, Wolfenstein Youngblood, Red
Dead Redemption 2 (follow), Baldur's Gate 3 (`--skip-launcher` so the bridge
sees `bg3.exe`/`bg3_dx11.exe` directly); Direct3D 11 — God of War (DLSS 2);
Direct3D 12 — Alan Wake 2, Metro Exodus Enhanced, Cyberpunk 2077, The
Witcher 3 (DX12 mode), GTA V Enhanced (follow). Avoid injection in protected
multiplayer games unless the game and its anti-cheat system explicitly permit
it — BattlEye-protected Online modes are never injected; GTA V Enhanced and
RDR2 story modes require the anti-cheat disabled (`-nobattleye`) plus
`--follow-children`.

## Requirements

### Linux

- An x86-64 Linux distribution
- Python 3.11 or newer
- `curl`, `tar`, and `sha256sum`
- Steam with a 64-bit Proton version
- A current NVIDIA display driver
- Internet access during installation

### Windows

- 64-bit Windows
- A current NVIDIA display driver
- The Vulkan runtime installed by the display driver
- Internet access during installation

The Windows package contains a native executable and does not install or use
Wine.

## Install on Linux

Run the installer from the latest release:

```bash
curl -fsSL https://github.com/numsu/dlss-bridge/releases/latest/download/install.sh | sh
```

The default installation paths are:

- Application: `~/.local/libexec/dlss-bridge`
- Command: `~/.local/bin/dlss-bridge`
- User state: `${XDG_STATE_HOME:-~/.local/state}/dlss-bridge`

Add `~/.local/bin` to `PATH` if the installer reports that it is missing.

During setup, the installer downloads the pinned DLSS Neural Rendering runtime
from the upstream DLSS 5 Visual Enhancer release. It verifies the complete ZIP
and extracted DLL with SHA-256 before installing the DLL into user state. Setup
fails if the download, extraction, or verification does not complete. The
source URL, version, hashes, and NVIDIA license URL are recorded in
`runtime-sources.json`.

For automation, `dlss-bridge acquire-runtime --json` returns the structured
runtime acquisition result.

## Install on Windows

Download `dlss-bridge-windows-x86_64-setup.exe` from the latest release and run
it. The installer adds `dlss-bridge.exe` to the user `PATH`. Restart Steam and
any open terminals after installation so they receive the updated environment.

Setup downloads and verifies the pinned DLSS Neural Rendering runtime before it
finishes. If runtime acquisition fails, setup reports failure instead of leaving
a nominally complete installation. Runtime data and logs are stored under
`%LOCALAPPDATA%\dlss-bridge`.

## Neural runtime source and license

The installer obtains `nvngx_dlssnr.dll` from
[DLSS 5 Visual Enhancer v7.0](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v7.0).
That project's MIT license covers its original code and does not relicense the
NVIDIA runtime. Use of the runtime is governed by the
[NVIDIA RTX SDK License](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt).
The DLL is downloaded during setup and is not included in DLSS Bridge release
artifacts.

## Configure Steam from the command line

Exit Steam completely before changing launch options. Configure a game by its
Steam AppID:

```bash
dlss-bridge steam configure APPID --profile same-gpu
```

Omit `--profile` to use the default automatic GPU policy. For a
launcher-spawned game, name the real game executable as well:

```bash
dlss-bridge steam configure APPID --follow-children GTA5_Enhanced.exe
```

Find numeric AppIDs with `dlss-bridge steam list` (installed games across
all libraries, sorted by name; `--steam-root PATH` for unusual installs).
Output is a uniform `ID<TAB>NAME` table with an `ID  NAME` header.

The command locates
the current Steam user, creates a one-time backup of `localconfig.vdf`, and
remembers the previous launch option. If more than one Steam account is present,
select one with `--user STEAMID`. Unusual installations can be selected with
`--steam-root PATH`.

To restore exactly the launch option that existed before configuration, exit
Steam and run:

```bash
dlss-bridge steam remove APPID
```

Removal refuses to overwrite the setting if another program or the user changed
it after DLSS Bridge configured it.

## Configure Heroic from the command line

Exit Heroic completely before changing anything: it keeps settings in memory
and would silently drop external edits. Configure a game by its Heroic app
name (the `GamesConfig/<AppName>.json` basename — the first column of
`dlss-bridge heroic list`, which prints a uniform `ID<TAB>NAME` table with an
`ID  NAME` header, titles resolved from Heroic's library caches and sorted
by name):

```bash
dlss-bridge heroic configure GAME --profile same-gpu
```

This prepends a `{exe, args}` wrapper entry (`dlss-bridge run ... --`) to the
game's `wrapperOptions`, which Heroic places ahead of the game command —
the equivalent of Steam's `%command%` prefix. It also ensures
`enviromentOptions` (Heroic's spelling) contains `PROTON_ENABLE_NVAPI=1` and
`PROTON_HIDE_NVIDIA_GPU=0` so DX11/DX12 NGX titles detect the NVIDIA GPU and
expose their DLSS option instead of hiding it behind an AMD-spoofed path.
Only our own wrapper entry and those two env keys are ever added, moved
first, or removed; every other wrapper, env entry, and setting in Heroic's
shared per-game file is preserved byte-for-byte. A missing settings file is
created holding just our entries, which Heroic merges over its defaults.

```bash
dlss-bridge heroic list
dlss-bridge heroic configure GAME --follow-children GTA5_Enhanced.exe
dlss-bridge heroic remove GAME
```

`--heroic-root PATH` selects the configuration when several exist (custom
`$XDG_CONFIG_HOME`, Flatpak). Removal drops only our wrapper entry and
restores the two managed env keys to their prior values (deleting keys we
added); a settings file we created is deleted only when nothing else has
been added to it.

Inside the headless-sunshine-steam container, run these commands as the gamer
user (`docker exec --user gamer ...`); plain `docker exec` runs as root with
`HOME=/root` and finds neither Heroic nor Steam data.

## Enable a Steam game

Open the game's **Properties**, find **Launch Options**, and enter:

```text
dlss-bridge run -- %command%
```

On Windows, `dlss-bridge.exe` can be used in place of `dlss-bridge`.

For each launch, the controller:

1. Finds the Windows game executable in Steam's command.
2. Creates a disposable session under user state.
3. Places the launcher, hook, configuration, OptiScaler payload, and verified
   neural runtime in that session.
4. Replaces only the in-memory command argument with the session launcher.
5. Starts the game suspended, injects the hook, then resumes it.
6. Preserves session logs and removes the disposable session after exit.

No step writes to the game directory or Wine prefix. Omit the command prefix to
launch without DLSS Bridge; no disable flag is needed.

## Launcher-spawned games (child-process follow)

Some games start through a launcher that spawns the real game as a child
process (Rockstar Games Launcher, Warframe launcher). Prefixing the launcher
command alone would inject the launcher, not the game. Name the real game
executable explicitly instead of writing a profile file for it:

```text
dlss-bridge run --follow-children GTA5_Enhanced.exe -- %command%
```

The injected launcher redirects its own `CreateProcessW/A` imports: every
child it spawns is force-created suspended, injected with the session hook,
initialized, and resumed. The name given in the flag only selects which
process is the game's renderer; intermediate launchers in a
Launcher -> Helper -> Game chain are carried along automatically because
each injected process re-arms the same follow rule, and the hook is inert in
a process that never creates NGX features. Failed follows run exactly as the
launcher requested. The session log records each followed child and
injection result. The session launcher
retains the session directory until every followed child has exited (it polls
the process list by executable name after the launcher itself exits), so
cleanup never deletes DLLs, configuration, or the model from under a running
game; a child that never appears releases the session after a bounded grace
window.

## Toggle neural rendering while playing

Press **Ctrl+Shift+N** to show or hide the neural effect. The binding works
in native Windows and in Windows games running through Proton. DLSS Bridge
continues its private DLSS, neural model, frame capture, transport, and
reinsertion path in both modes; only composition of the neural result changes.
This provides an immediate image-quality comparison without cold-starting the
model or rebuilding its feature.

OptiScaler displays the applied state on screen. The bridge records the requested
transition in the session log. It reports private-evaluate p50, p95, and p99
GPU time every 64 successful evaluations, and also reports a state window when
the toggle changes after at least 16 samples. Once both states have been measured,
it reports the visible-minus-hidden p50 difference. The model and Super
Resolution remain active in both measurements, so this difference should be
close to zero and verifies that the A/B toggle did not disturb the hot path.

## Select a GPU

The default `auto` policy uses another compatible NVIDIA GPU when one is
available. It uses the game GPU on a single-GPU system.

Force the game GPU:

```text
dlss-bridge run --profile same-gpu -- %command%
```

Require a secondary GPU:

```text
dlss-bridge run --profile secondary-gpu -- %command%
```

A custom TOML profile can select a specific adapter by index or DXGI LUID:

```text
dlss-bridge run --profile /absolute/path/custom.toml -- %command%
```

UUID and PCI correlation is planned and those selectors are not executable yet.

## Update

Exit every game launched through DLSS Bridge before updating.

On Linux, rerun the installation command:

```bash
curl -fsSL https://github.com/numsu/dlss-bridge/releases/latest/download/install.sh | sh
```

The installer verifies the release checksum and replaces the application
directory atomically. The verified runtime, settings, and collected logs remain
in user state. When a release pins a newer runtime, setup updates a runtime it
previously managed. A runtime supplied manually by the user is preserved.

To install a particular Linux release, set its tag:

```bash
curl -fsSL https://github.com/numsu/dlss-bridge/releases/download/vVERSION/install.sh |
  DLSS_BRIDGE_VERSION=vVERSION sh
```

On Windows, download the newer setup EXE and run it. The fixed application ID
updates the existing installation. User state remains under `%LOCALAPPDATA%`.

## Uninstall

On Linux:

```bash
curl -fsSL https://github.com/numsu/dlss-bridge/releases/latest/download/install.sh |
  sh -s -- --uninstall
```

This removes the application and command but retains user state. Remove user
state separately only when models, settings, and logs are no longer needed:

```bash
rm -rf "${XDG_STATE_HOME:-$HOME/.local/state}/dlss-bridge"
```

On Windows, remove **DLSS Bridge** from **Installed apps**. The uninstaller
retains `%LOCALAPPDATA%\dlss-bridge`; delete that directory separately to remove
the downloaded runtime, settings, and logs.

## Diagnostics

List detected GPUs:

```bash
dlss-bridge probe
```

List installed and planned backends:

```bash
dlss-bridge components
```

Useful errors:

- **could not find a Windows game executable**: place the command directly
  around Steam's `%command%`.
- **missing runtime files**: rerun the installer. Game launch never downloads
  runtime components.
- **hook injection failed**: check that the target is a 64-bit Windows program
  and that anti-cheat or endpoint security is not blocking process injection.
- **bridge is idle, DLSS appears disabled**: the session log contains
  `DLSS appears disabled in-game, so the bridge is idle` after ~512 presents
  (Vulkan) or submissions (D3D12) with no NGX feature created. Enable DLSS
  in the game's graphics settings and the bridge activates without
  reinstalling anything.
