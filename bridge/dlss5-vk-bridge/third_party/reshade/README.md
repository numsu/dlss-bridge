# ReShade add-on API headers

`include/` is the public add-on API of [ReShade](https://github.com/crosire/reshade)
as shipped in tag **v6.7.0** (`RESHADE_API_VERSION 18`), unmodified. The
add-on host (`src/addon_host.inc`) compiles against it; ReShade accepts any
add-on whose API version is not newer than its own, so the resulting
`dlss5-vk-bridge.addon64` loads into ReShade 6.6 and later.

ReShade is licensed under the BSD 3-clause license, reproduced in
`LICENSE.md`.
