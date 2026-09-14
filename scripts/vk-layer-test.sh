#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p reports runtime/xdg
chmod 700 runtime/xdg
bash scripts/build-vk-bridge.sh
set +e
docker run --rm --gpus all --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/work" \
    -e WINEARCH=win64 -e WINEPREFIX=/work/runtime/wine-neural \
    -e XDG_RUNTIME_DIR=/work/runtime/xdg \
    -e ENABLE_DLSS5_VK_BRIDGE=1 \
    -e 'VK_LAYER_PATH=Z:\work\runtime\build-vk-bridge' \
    --entrypoint /bin/bash dlss-probe:local -lc '
set -euo pipefail
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static \
  -Iruntime/build-vk-bridge/vulkan-headers \
  src/vk-layer-probe.cpp -o runtime/build-vk-bridge/vk-layer-probe.exe
cd runtime/build-vk-bridge
export LD_LIBRARY_PATH=/work/runtime/GE-Proton11-6-x86_64/files/lib64:/work/runtime/GE-Proton11-6-x86_64/files/lib:${LD_LIBRARY_PATH:-}
probe_output=$(timeout -k 5s 45s xvfb-run -a /work/runtime/GE-Proton11-6-x86_64/files/lib/wine/x86_64-unix/wine vk-layer-probe.exe 2>&1 || true)
printf "%s\n" "$probe_output"
if ! grep -q "^layer=VK_LAYER_dlss5_vk_bridge$" <<<"$probe_output"; then
  printf "Vulkan layer enumeration failed\n" >&2
  exit 4
fi
' 2>&1 | tee reports/vk-layer-test.log
container_status=${PIPESTATUS[0]}
set -e
if grep -q '^Vulkan layer enumeration failed$' reports/vk-layer-test.log; then
    exit 4
fi
exit "$container_status"
