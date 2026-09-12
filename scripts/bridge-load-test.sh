#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p reports runtime/xdg
chmod 700 runtime/xdg
bash scripts/build-vk-bridge.sh
docker run --rm --gpus all --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/work" \
    -e WINEARCH=win64 -e WINEPREFIX=/work/runtime/wine-neural \
    -e XDG_RUNTIME_DIR=/work/runtime/xdg \
    --entrypoint /bin/bash dlss-probe:local -lc '
set -euo pipefail
x86_64-w64-mingw32-g++ -std=c++17 -O2 -static src/bridge-load.cpp -o runtime/build-vk-bridge/bridge-load.exe
cd runtime/build-vk-bridge
export LD_LIBRARY_PATH=/work/runtime/GE-Proton11-6-x86_64/files/lib64:/work/runtime/GE-Proton11-6-x86_64/files/lib:${LD_LIBRARY_PATH:-}
timeout -k 5s 30s xvfb-run -a /work/runtime/GE-Proton11-6-x86_64/files/lib/wine/x86_64-unix/wine bridge-load.exe dlss5-vk-bridge.addon64 AddonInit
timeout -k 5s 30s xvfb-run -a /work/runtime/GE-Proton11-6-x86_64/files/lib/wine/x86_64-unix/wine bridge-load.exe dlss5-vk-bridge.dll vkNegotiateLoaderLayerInterfaceVersion
' 2>&1 | tee reports/bridge-load-test.log
