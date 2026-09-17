#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:?usage: packaging/build-runtime.sh OUTPUT_DIRECTORY}
archive=${OPTISCALER_ARCHIVE:-"$root/runtime/downloads/OptiScaler-NR-v0.8.3.zip"}
expected=3f2d26fb136d964a394bf50896d082156173153a2a55b88e1995277b4dabe3c8
if [[ ! -f "$archive" ]]; then
 mkdir -p "$(dirname "$archive")"
 curl -fL --retry 3 https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/releases/download/v0.8.3/OptiScaler-NR-v0.8.3.zip -o "$archive"
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
if [[ ${DLSS_BRIDGE_SKIP_BUILD:-0} != 1 ]]; then
  [[ -f "$root/runtime/headers/d3d12.h" ]] || "$root/scripts/fetch-runtimes.sh"
  "$root/scripts/build-vk-bridge.sh"
fi
for built in dlss-bridge-launcher.exe dlss5-vk-hook.dll; do
  [[ -f "$root/runtime/build-vk-bridge/$built" ]] || { echo "missing built bridge: $built" >&2; exit 1; }
done
rm -rf "$output"; mkdir -p "$output"/payload/{bridge,optiscaler} "$output"/{controller,profiles,licenses,docs}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
unzip -q "$archive" -d "$tmp"
source_dir=$(find "$tmp" -type f -name OptiScaler.dll -printf '%h\n' | head -1)
[[ -n "$source_dir" ]] || { echo "OptiScaler.dll missing from archive" >&2; exit 1; }
cp -a "$source_dir/." "$output/payload/optiscaler/"
python3 "$root/scripts/patch-optiscaler-vulkan-nr.py" "$output/payload/optiscaler/OptiScaler.dll" "$output/payload/optiscaler/OptiScaler.dll.patched"
mv "$output/payload/optiscaler/OptiScaler.dll.patched" "$output/payload/optiscaler/OptiScaler.dll"
python3 "$root/packaging/configure_optiscaler.py" "$output/payload/optiscaler/OptiScaler.ini"
[[ ! -e "$output/payload/optiscaler/nvngx.dll_dlssnr.dll" ]] || {
  echo "obsolete OptiScaler neural-runtime helper was packaged" >&2
  exit 1
}
rm -rf "$output/payload/optiscaler/docs"
rm -f "$output/payload/optiscaler/setup_linux.sh" \
      "$output/payload/optiscaler/setup_windows.bat" \
      "$output/payload/optiscaler/get_streamline.ps1" \
      "$output/payload/optiscaler/!! EXTRACT ALL FILES TO GAME FOLDER !!" \
      "$output/payload/optiscaler/INSTALL-DLSSNR.md" \
      "$output/payload/optiscaler/README.md" \
      "$output/payload/optiscaler/SHA256SUMS.txt"
install -m0644 "$root/runtime/build-vk-bridge/dlss-bridge-launcher.exe" "$output/payload/bridge/"
install -m0644 "$root/runtime/build-vk-bridge/dlss5-vk-hook.dll" "$output/payload/bridge/"
install -m0644 "$root/controller/dlss_bridge.py" "$root/controller/runtime_setup.py" "$root/controller/session.py" "$root/controller/steam_config.py" "$output/controller/"
install -m0644 "$root/runtime-sources.json" "$output/runtime-sources.json"
cp -a "$root/profiles/." "$output/profiles/"
for group in hosts capture executors transports platform; do
  mkdir -p "$output/$group"
  find "$root/$group" -name backend.toml -print0 | while IFS= read -r -d '' manifest; do
    destination="$output/$group/${manifest#"$root/$group/"}"
    mkdir -p "$(dirname "$destination")"
    cp "$manifest" "$destination"
  done
done
install -m0644 "$root/bridge/dlss5-vk-bridge/LICENSE" "$output/licenses/bridge-GPL-3.0.txt"
install -m0644 "$output/payload/optiscaler/LICENSE" "$output/licenses/OptiScaler-GPL-3.0.txt"
install -m0644 "$root/scripts/patch-optiscaler-vulkan-nr.py" "$output/licenses/OptiScaler-binary-patch.py"
install -m0644 "$root/docs/installation.md" "$output/docs/installation.md"
install -m0644 "$root/docs/dependencies.md" "$output/docs/dependencies.md"
install -m0644 "$root/docs/known-limitations.md" "$output/docs/known-limitations.md"
install -m0644 "$root/docs/diagnostics.md" "$output/docs/diagnostics.md"
install -m0644 "$root/docs/portable-architecture.md" "$output/docs/portable-architecture.md"
cat >"$output/RUNTIME-SOURCE.txt" <<'EOF'
The NVIDIA nvngx_dlssnr.dll runtime is not redistributed in this package.
The installer downloads it from the pinned upstream release recorded in
runtime-sources.json, verifies both archive and DLL SHA-256 checksums, and
stores it in per-user state. Reinstall DLSS Bridge if this file is missing.
EOF
cat >"$output/THIRD-PARTY-NOTICES.md" <<'EOF'
# Third-party notices
This package contains a modified OptiScaler DLSS-NR v0.8.3 binary from
https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass. The exact,
hash-checked modification is included as licenses/OptiScaler-binary-patch.py.
OptiScaler and the bridge are GPL-3.0. Their source and build scripts are
available from the repository that publishes this package. Other notices remain
under payload/optiscaler/Licenses. The proprietary NVIDIA runtime is downloaded from the source recorded in
runtime-sources.json and is absent from this package.
EOF
find "$output" -type f ! -name PAYLOAD-SHA256SUMS -print0 | sort -z | xargs -0 sha256sum >"$output/PAYLOAD-SHA256SUMS"
