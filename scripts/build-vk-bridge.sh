#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

image="${DLSS_DOCKER_IMAGE:-dlss-probe:local}"
out="runtime/build-vk-bridge"

docker build -t "$image" .
mkdir -p "$out"

docker run --rm --entrypoint /bin/bash \
    --user "$(id -u):$(id -g)" \
    --mount "type=bind,src=$PWD,dst=/work" \
    -w /work "$image" -lc '
set -euo pipefail
mkdir -p runtime/build-vk-bridge
printf "#include <windows.h>\n" > runtime/build-vk-bridge/Windows.h
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared \
    -static -lwinpthread \
    -DDLSS5VK_ADDON_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ibridge/dlss5-vk-bridge/third_party/reshade/include \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    -o runtime/build-vk-bridge/dlss5-vk-bridge.addon64
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.def \
    -o runtime/build-vk-bridge/dlss5-vk-bridge.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DDLSS5VK_HOOK_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    -o runtime/build-vk-bridge/dlss5-vk-hook.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DDLSS5VK_HOOK_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    bridge/dlss5-vk-bridge/src/winhttp_proxy.cpp \
    bridge/dlss5-vk-bridge/src/winhttp-proxy.def \
    -o runtime/build-vk-bridge/winhttp.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DDLSS5VK_HOOK_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    bridge/dlss5-vk-bridge/src/xinput_proxy.cpp \
    bridge/dlss5-vk-bridge/src/xinput-proxy.def \
    -o runtime/build-vk-bridge/XINPUT9_1_0.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DDLSS5VK_HOOK_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    bridge/dlss5-vk-bridge/src/winmm_proxy.cpp \
    bridge/dlss5-vk-bridge/src/winmm-proxy.def \
    -o runtime/build-vk-bridge/winmm.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DUNICODE -D_UNICODE \
    bridge/dlss5-vk-bridge/src/winmm_proxy.cpp \
    bridge/dlss5-vk-bridge/src/winmm-proxy.def \
    -o runtime/build-vk-bridge/winmm-loader.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DDLSS5VK_PROXY_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    bridge/dlss5-vk-bridge/src/vulkan-proxy.def \
    -o runtime/build-vk-bridge/vulkan-1.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    src/vk-layer-probe.cpp -o runtime/build-vk-bridge/vk-layer-probe.exe
x86_64-w64-mingw32-dlltool \
    -d bridge/dlss5-vk-bridge/src/vulkan-import.def \
    -l runtime/build-vk-bridge/libvulkan-1.dll.a
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread \
    -Ivendor/OptiScaler_DLSSNR/external/vulkan/include \
    src/vk-iat-probe.cpp runtime/build-vk-bridge/libvulkan-1.dll.a \
    -o runtime/build-vk-bridge/vk-iat-probe.exe
cp bridge/dlss5-vk-bridge/dlss5-vk-bridge.json runtime/build-vk-bridge/
'

file "$out/dlss5-vk-bridge.addon64" "$out/dlss5-vk-bridge.dll" "$out/dlss5-vk-hook.dll" "$out/winhttp.dll" "$out/XINPUT9_1_0.dll" "$out/winmm.dll" "$out/winmm-loader.dll" "$out/vulkan-1.dll"
sha256sum "$out/dlss5-vk-bridge.addon64" "$out/dlss5-vk-bridge.dll" "$out/dlss5-vk-hook.dll" "$out/winhttp.dll" "$out/XINPUT9_1_0.dll" "$out/winmm.dll" "$out/winmm-loader.dll" "$out/vulkan-1.dll"
