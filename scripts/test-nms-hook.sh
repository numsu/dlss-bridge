#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
project_dir=$PWD
steam_root=/home/metsamir/.steam
steam_share=/home/metsamir/.local/share/Steam
library_root=$(sed -n 's|^[[:space:]]*-[[:space:]]*\([^:]*SteamLibrary\):.*|\1|p' /home/metsamir/sunshine/docker-compose.yml | head -n1)
nms_root="$library_root/steamapps/common/No Man's Sky"
shader_cache="$library_root/steamapps/shadercache/275850"

test -d "$nms_root/Binaries"
test -d "$shader_cache"
mkdir -p runtime/steam-profile/htmlcache runtime/nms-run/Binaries reports
chmod 0775 runtime/steam-profile runtime/steam-profile/htmlcache runtime/nms-run runtime/nms-run/Binaries

docker rm -f dlss-nms-hook-test >/dev/null 2>&1 || true
docker run --rm --name dlss-nms-hook-test \
    --gpus all \
    --ipc=host \
    --cap-add SYS_NICE \
    --ulimit nice=40:40 \
    --ulimit rtprio=99:99 \
    --device /dev/uinput \
    --entrypoint /bin/bash \
    -e NVIDIA_DRIVER_CAPABILITIES=all \
    -v "$project_dir:/work" \
    -v "$steam_root:/home/gamer/.steam" \
    -v "$steam_share:/home/gamer/.local/share/Steam" \
    -v "$nms_root:/games/nms:ro" \
    -v "$shader_cache:$shader_cache" \
    -v "$project_dir/runtime/steam-profile/htmlcache:/home/gamer/.steam/debian-installation/config/htmlcache" \
    -v "$project_dir/runtime/steam-launch/steam-runtime-check-requirements:/home/gamer/.steam/debian-installation/ubuntu12_32/steam-runtime/amd64/usr/bin/steam-runtime-check-requirements:ro" \
    dlss-nms:local /work/scripts/nms-hook-diagnostic-inner.sh
