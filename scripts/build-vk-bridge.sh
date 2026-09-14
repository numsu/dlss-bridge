#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

image="${DLSS_DOCKER_IMAGE:-dlss-probe:local}"
out="runtime/build-vk-bridge"

docker build -t "$image" .
mkdir -p "$out"

docker run --rm --entrypoint /bin/bash \
    --user "$(id -u):$(id -g)" \
    -e DLSS_BUILD_VERSION="${VERSION:-dev}" \
    --mount "type=bind,src=$PWD,dst=/work" \
    -w /work "$image" -lc '
set -euo pipefail
rm -rf runtime/build-vk-bridge
mkdir -p runtime/build-vk-bridge/vulkan-headers
cp -a /usr/include/vulkan /usr/include/vk_video runtime/build-vk-bridge/vulkan-headers/
printf "#include <windows.h>\n" > runtime/build-vk-bridge/Windows.h
printf "#define DLSS_BRIDGE_VERSION \"%s\"\n" "$DLSS_BUILD_VERSION" > runtime/build-vk-bridge/build_version.hpp
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared \
    -static -lwinpthread \
    -DDLSS5VK_ADDON_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Ibridge/dlss5-vk-bridge/third_party/reshade/include \
    -Iruntime/build-vk-bridge/vulkan-headers \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    core/src/components.cpp \
    -o runtime/build-vk-bridge/dlss5-vk-bridge.addon64
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Iruntime/build-vk-bridge/vulkan-headers \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    core/src/components.cpp \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.def \
    -o runtime/build-vk-bridge/dlss5-vk-bridge.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread -municode \
    bridge/dlss5-vk-bridge/src/session_launcher.cpp \
    -o runtime/build-vk-bridge/dlss-bridge-launcher.exe
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread -municode \
    tests/unit/session_launcher_target.cpp \
    -o runtime/build-vk-bridge/session-launcher-target.exe
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    tests/unit/optiscaler_stub.cpp \
    -o runtime/build-vk-bridge/OptiScaler.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -DDLSS5VK_HOOK_HOST=1 -DUNICODE -D_UNICODE \
    -Iruntime/build-vk-bridge \
    -Iruntime/headers \
    -Ibridge/dlss5-vk-bridge/src \
    -Icore/include \
    -Iruntime/build-vk-bridge/vulkan-headers \
    bridge/dlss5-vk-bridge/src/dlss5-vk-bridge.cpp \
    core/src/runtime.cpp \
    core/src/components.cpp \
    -o runtime/build-vk-bridge/dlss5-vk-hook.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread \
    -Iruntime/build-vk-bridge/vulkan-headers \
    src/vk-layer-probe.cpp -o runtime/build-vk-bridge/vk-layer-probe.exe
x86_64-w64-mingw32-dlltool \
    -d bridge/dlss5-vk-bridge/src/vulkan-import.def \
    -l runtime/build-vk-bridge/libvulkan-1.dll.a
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -shared -static -lwinpthread \
    -Iruntime/build-vk-bridge/vulkan-headers \
    tests/unit/vulkan_engine_module.cpp runtime/build-vk-bridge/libvulkan-1.dll.a \
    -o runtime/build-vk-bridge/vulkan-engine-probe.dll
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread \
    tests/unit/vulkan_module_probe.cpp \
    -o runtime/build-vk-bridge/vulkan-module-probe.exe
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread \
    -Iruntime/build-vk-bridge/vulkan-headers \
    tests/unit/vulkan_dynamic_probe.cpp \
    -o runtime/build-vk-bridge/vulkan-dynamic-probe.exe
clang++ --target=x86_64-w64-windows-gnu -fuse-ld=lld \
    -fms-extensions -std=c++17 -O2 -static -lwinpthread \
    -Iruntime/build-vk-bridge/vulkan-headers \
    src/vk-iat-probe.cpp runtime/build-vk-bridge/libvulkan-1.dll.a \
    -o runtime/build-vk-bridge/vk-iat-probe.exe
cp bridge/dlss5-vk-bridge/dlss5-vk-bridge.json runtime/build-vk-bridge/
'

file "$out/dlss-bridge-launcher.exe" "$out/dlss5-vk-bridge.addon64" "$out/dlss5-vk-bridge.dll" "$out/dlss5-vk-hook.dll"
sha256sum "$out/dlss-bridge-launcher.exe" "$out/dlss5-vk-bridge.addon64" "$out/dlss5-vk-bridge.dll" "$out/dlss5-vk-hook.dll"
