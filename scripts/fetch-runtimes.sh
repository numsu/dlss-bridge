#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p runtime
fetch() {
    local url="$1" archive="$2" checksum="$3"
    if [[ ! -f "runtime/$archive" ]]; then
        curl --fail --location "$url" --output "runtime/$archive"
    fi
    printf '%s  %s\n' "$checksum" "runtime/$archive" | sha256sum --check
}
fetch https://github.com/doitsujin/dxvk/releases/download/v2.4.1/dxvk-2.4.1.tar.gz dxvk-2.4.1.tar.gz 7b23db4e1386b5d9a3ec0d83daa8b06096b758639185c11a673373a5ae478d54
fetch https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v3.0.1/vkd3d-proton-3.0.1.tar.zst vkd3d-proton-3.0.1.tar.zst 3cf2315522af5e43605ef6d3c41dad91387040bf97199934f3f7ab76caaa2f0c
tar -xf runtime/dxvk-2.4.1.tar.gz -C runtime
tar --zstd -xf runtime/vkd3d-proton-3.0.1.tar.zst -C runtime
cp runtime/dxvk-2.4.1/x64/dxgi.dll runtime/dxgi.dll
cp runtime/vkd3d-proton-3.0.1/x64/d3d12.dll runtime/d3d12.dll
cp runtime/vkd3d-proton-3.0.1/x64/d3d12core.dll runtime/d3d12core.dll
printf 'dxgi.hideNvidiaGpu = False\n' > runtime/dxvk.conf
# Ubuntu's MinGW headers lack ID3D12Device3. Use the exact upstream declaration.
mkdir -p runtime/headers
if [[ ! -f runtime/headers/d3d12.h ]]; then
    curl --fail --location https://raw.githubusercontent.com/mingw-w64/mingw-w64/0a663a666a03eec5a470fc02e43c9425b55a573c/mingw-w64-headers/include/d3d12.h --output runtime/headers/d3d12.h
fi
printf '%s  %s\n' 23935395fe8939ac0ca71c5b34cd7b21a689ff5e9641846563613f24bc0dd85e runtime/headers/d3d12.h | sha256sum --check
