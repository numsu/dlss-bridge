#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p reports runtime/xdg
chmod 700 runtime/xdg
render_adapter="${1:-0}"
neural_adapter="${2:-1}"
width="${3:-320}"
height="${4:-180}"
output_format="${5:-rgba16}"
log="reports/neural-mgpu-${render_adapter}-to-${neural_adapter}-${width}x${height}.log"
docker build -t dlss-probe:local .
docker run --rm --gpus all --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/work" \
    -e WINEARCH=win64 -e WINEPREFIX=/work/runtime/wine-neural \
    -e DXVK_ENABLE_NVAPI=1 -e DXVK_NVAPI_ALLOW_OTHER_DRIVERS=1 \
    -e DXVK_CONFIG_FILE=/work/runtime/dxvk.conf -e DXVK_LOG_PATH=/work/reports \
    -e VKD3D_SHADER_CACHE_PATH=/work/runtime -e XDG_RUNTIME_DIR=/work/runtime/xdg \
    -e 'WINEDLLOVERRIDES=dxgi,d3d11,d3d12,d3d12core,_nvngx,nvngx,nvapi64=n' \
    dlss-probe:local -c "
set -euo pipefail
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -static -Iruntime/headers -Iruntime/ngx/include src/neural-mgpu.cpp -o runtime/neural/neural-mgpu.exe -ld3d12 -ldxgi -ldxguid
install -m 0644 runtime/GE-Proton11-6-x86_64/files/lib/wine/dxvk/x86_64-windows/d3d11.dll runtime/wine-neural/drive_c/windows/system32/d3d11.dll
cd runtime/neural
export LD_LIBRARY_PATH=/work/runtime/GE-Proton11-6-x86_64/files/lib64:/work/runtime/GE-Proton11-6-x86_64/files/lib:\${LD_LIBRARY_PATH:-}
timeout -k 5s 150s xvfb-run -a /work/runtime/GE-Proton11-6-x86_64/files/lib/wine/x86_64-unix/wine neural-mgpu.exe '$render_adapter' '$neural_adapter' '$width' '$height' '$output_format'
" 2>&1 | tee "$log"
