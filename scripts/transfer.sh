#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p runtime reports
bash scripts/fetch-runtimes.sh
docker build -t dlss-probe:local .
docker run --rm --gpus all --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/work" \
    -e DXVK_CONFIG_FILE=/work/runtime/dxvk.conf \
    -e DXVK_LOG_PATH=/work/reports \
    -e VKD3D_SHADER_CACHE_PATH=/work/runtime \
    -e 'WINEDLLOVERRIDES=dxgi,d3d12,d3d12core=n' \
    -e XDG_RUNTIME_DIR=/work/runtime/xdg \
    -e VKD3D_DEBUG=info \
    dlss-probe:local -c '
set -euo pipefail
mkdir -p runtime/xdg
chmod 700 runtime/xdg
x86_64-w64-mingw32-g++ -Iruntime/headers -std=c++17 -O2 -Wall -Wextra -static src/host-transfer.cpp -o runtime/host-transfer.exe -ld3d12 -ldxgi -ldxguid
vulkaninfo --summary
 timeout 90s xvfb-run -a /usr/lib/wine/wine64 runtime/host-transfer.exe 1920 1080
timeout 90s xvfb-run -a /usr/lib/wine/wine64 runtime/host-transfer.exe 3840 2160
' 2>&1 | tee reports/host-transfer.log
