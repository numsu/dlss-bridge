# External dependencies

This page lists the external projects used by DLSS Bridge, why each one exists,
the version currently used, and its upstream project. Build scripts remain the
source of truth for changing versions.

## Runtime components

| Name | Why it exists | Version | Upstream |
| --- | --- | --- | --- |
| OptiScaler DLSS-NR PreSR Multipass | Hosts the private D3D12 NGX call and invokes the neural-rendering add-on. Packaging applies the repository's Vulkan-call patch so the bridge owns the Vulkan execution point. | 0.7.7 | [wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/releases/tag/v0.7.7) |
| NVIDIA DLSS Neural Rendering runtime | Provides `nvngx_dlssnr.dll`, the neural model runtime loaded in each isolated session. Setup downloads it separately because it is governed by NVIDIA's license. | DLSS 5 Visual Enhancer 7.0 bundle | [Merserk/dlss5-visual-enhancer](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v7.0) |

## Source and build dependencies

| Name | Why it exists | Version | Upstream |
| --- | --- | --- | --- |
| Vulkan Headers | Defines the Vulkan API and loader-layer interfaces used by the bridge and probes. The Docker build obtains these from Ubuntu's `libvulkan-dev` package. | 1.3.275 (`libvulkan-dev` on Ubuntu 24.04) | [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) |
| ReShade add-on API | Compiles the optional ReShade host. Its unmodified public headers and license are vendored under `bridge/dlss5-vk-bridge/third_party/reshade`. | ReShade 6.7.0, API 18 | [crosire/reshade](https://github.com/crosire/reshade/tree/v6.7.0) |
| MinGW-w64 D3D12 header | Supplies the `ID3D12Device3` declarations missing from Ubuntu's packaged MinGW headers. | mingw-w64 commit `0a663a6` | [mingw-w64/mingw-w64](https://github.com/mingw-w64/mingw-w64/commit/0a663a666a03eec5a470fc02e43c9425b55a573c) |
| Ubuntu build image | Provides the reproducible Linux cross-compilation and Wine test environment. | Ubuntu 24.04 | [Ubuntu container image](https://hub.docker.com/_/ubuntu) |
| PyInstaller | Builds the native Windows controller executable. | 6.16.0 | [pyinstaller/pyinstaller](https://github.com/pyinstaller/pyinstaller/releases/tag/v6.16.0) |
| Inno Setup | Builds the per-user Windows setup executable. | 6 | [Inno Setup](https://jrsoftware.org/isinfo.php) |

## Test-only dependencies

| Name | Why it exists | Version | Upstream |
| --- | --- | --- | --- |
| DXVK | Supplies a known DXGI runtime for low-level compatibility probes. It is not shipped in DLSS Bridge releases. | 2.4.1 | [doitsujin/dxvk](https://github.com/doitsujin/dxvk/releases/tag/v2.4.1) |
| vkd3d-proton | Supplies known D3D12 runtime binaries for multi-GPU and interop probes. It is not shipped in DLSS Bridge releases. | 3.0.1 | [HansKristian-Work/vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton/releases/tag/v3.0.1) |
| GE-Proton | Runs Windows bridge tests on Linux. Developers provide this under the ignored `runtime` directory; it is not shipped. | GE-Proton11-6 | [GloriousEggroll/proton-ge-custom](https://github.com/GloriousEggroll/proton-ge-custom/releases/tag/GE-Proton11-6) |

## Release automation

| Name | Why it exists | Version | Upstream |
| --- | --- | --- | --- |
| GitHub checkout action | Checks out release source. | 4 | [actions/checkout](https://github.com/actions/checkout) |
| GitHub artifact actions | Move the shared payload between Linux and Windows release jobs. | 4 | [actions/upload-artifact](https://github.com/actions/upload-artifact), [actions/download-artifact](https://github.com/actions/download-artifact) |
| GitHub release action | Publishes installers, archives, and checksums for a version tag. | 2 | [softprops/action-gh-release](https://github.com/softprops/action-gh-release) |

## Upgrading a dependency

- Update the neural runtime in `runtime-sources.json`.
- Update the packaged OptiScaler release in `packaging/build-runtime.sh`, then
  adapt and verify `scripts/patch-optiscaler-vulkan-nr.py` against the new DLL.
- Replace the vendored ReShade headers and update their local README when its
  add-on API changes.
- Update DXVK, vkd3d-proton, or the MinGW header in
  `scripts/fetch-runtimes.sh`.
- Update the Docker image or its packages in `Dockerfile`, Windows packaging
  tools under `packaging/windows`, and release actions in
  `.github/workflows/release.yml`.

After an upgrade, run the unit tests, injected-host test, bridge load test, and
release-package build documented in the main README. Hardware-facing runtime
changes also require an in-game same-GPU and secondary-GPU trial.

The repository does not require source submodules. OptiScaler is consumed as a
versioned release artifact, and ReShade's small public header interface is
vendored directly with its provenance and license.
