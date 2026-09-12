#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
project_dir=$PWD
steam_root=$(docker inspect sunshine-gamestream --format '{{range .Mounts}}{{if eq .Destination "/home/gamer/.steam"}}{{.Source}}{{end}}{{end}}')
steam_share=$(docker inspect sunshine-gamestream --format '{{range .Mounts}}{{if eq .Destination "/home/gamer/.local/share/Steam"}}{{.Source}}{{end}}{{end}}')

test -n "$steam_root"
test -n "$steam_share"
mkdir -p runtime/steam-profile/htmlcache reports
chmod 0775 runtime/steam-profile runtime/steam-profile/htmlcache

docker rm -f dlss-nms-steam-test >/dev/null 2>&1 || true
docker run --rm --name dlss-nms-steam-test \
    --gpus device=0 \
    --ipc=host \
    --entrypoint /bin/bash \
    -e NVIDIA_DRIVER_CAPABILITIES=all \
    -v "$project_dir:/work" \
    -v "$steam_root:/home/gamer/.steam" \
    -v "$steam_share:/home/gamer/.local/share/Steam" \
    -v "$project_dir/runtime/steam-profile/htmlcache:/home/gamer/.steam/debian-installation/config/htmlcache" \
    -v "$project_dir/runtime/steam-launch/steam-runtime-check-requirements:/home/gamer/.steam/debian-installation/ubuntu12_32/steam-runtime/amd64/usr/bin/steam-runtime-check-requirements:ro" \
    dlss-nms:local /work/scripts/steam-login-diagnostic-inner.sh
