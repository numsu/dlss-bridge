#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p reports runtime/xdg
chmod 700 runtime/xdg
scripts/build-vk-bridge.sh

docker run --rm --user "$(id -u):$(id -g)" \
  --mount "type=bind,src=$PWD,dst=/work" \
  -e WINEARCH=win64 -e WINEPREFIX=/work/runtime/wine-neural \
  -e XDG_RUNTIME_DIR=/work/runtime/xdg \
  --entrypoint /bin/bash dlss-probe:local -lc '
set -euo pipefail
cd /work/runtime/build-vk-bridge
export LD_LIBRARY_PATH=/work/runtime/GE-Proton11-6-x86_64/files/lib64:/work/runtime/GE-Proton11-6-x86_64/files/lib:${LD_LIBRARY_PATH:-}
probe_output=$(timeout -k 5s 30s xvfb-run -a \
  /work/runtime/GE-Proton11-6-x86_64/files/lib/wine/x86_64-unix/wine \
  dlss-bridge-launcher.exe \
  --hook Z:\\work\\runtime\\build-vk-bridge\\dlss5-vk-hook.dll \
  -- Z:\\work\\runtime\\build-vk-bridge\\session-launcher-target.exe 2>&1)
printf "%s\n" "$probe_output"
grep -q "hook_loaded=yes" <<<"$probe_output"
' 2>&1 | tee reports/injected-host-test.log
