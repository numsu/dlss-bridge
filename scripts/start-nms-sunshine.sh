#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")/.."
project_dir=$PWD
container_name=dlss-nms-interactive

steam_root=/home/metsamir/.steam
steam_share=/home/metsamir/.local/share/Steam
library_root=$(sed -n 's|^[[:space:]]*-[[:space:]]*\([^:]*SteamLibrary\):.*|\1|p' /home/metsamir/sunshine/docker-compose.yml | head -n1)
nms_root="$library_root/steamapps/common/No Man's Sky"
shader_cache="$library_root/steamapps/shadercache/275850"

test -d "$nms_root/Binaries"
test -d "$shader_cache"
mkdir -p runtime/steam-profile/htmlcache runtime/nms-run/Binaries runtime/sunshine-interactive runtime/resolved reports
chmod 0775 runtime/steam-profile runtime/steam-profile/htmlcache runtime/nms-run runtime/nms-run/Binaries

test -s runtime/sunshine-interactive/sunshine_state.json

python3 controller/dlss_bridge.py resolve \
    --config profiles/default.toml \
    --profile profiles/no-mans-sky-vulkan.toml \
    --output runtime/resolved/nms-sunshine.cfg >/dev/null

host_ip=$(hostname -I | awk '{print $1}')
python3 - "$project_dir/runtime/sunshine-interactive/sunshine.conf" "$host_ip" <<'PY'
import pathlib, re, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text() if path.exists() else ""
updates = {
    "port": "47989",
    "bind_address": sys.argv[2],
    "address_family": "ipv4",
    "sunshine_name": "DLSS NMS Test",
    "upnp": "disabled",
    "global_prep_cmd": '[{"do":"env SUNSHINE_CLIENT_WIDTH=1920 SUNSHINE_CLIENT_HEIGHT=1080 SUNSHINE_CLIENT_FPS=60 /usr/local/bin/sunshine-resolution-do","undo":"/usr/local/bin/sunshine-resolution-undo"}]',
}
for key, value in updates.items():
    pattern = re.compile(rf"(?m)^\s*{re.escape(key)}\s*=.*$")
    replacement = f"{key} = {value}"
    if pattern.search(text):
        text = pattern.sub(replacement, text, count=1)
    else:
        if text and not text.endswith("\n"):
            text += "\n"
        text += replacement + "\n"
path.write_text(text)
PY
chown -R 1000:1000 runtime/sunshine-interactive
rm -f reports/nms-interactive-ready

docker rm -f "$container_name" >/dev/null 2>&1 || true
docker run -d --name "$container_name" \
    --gpus all \
    --network host \
    --ipc host \
    --shm-size 4g \
    --cap-add SYS_NICE \
    --ulimit nice=40:40 \
    --ulimit rtprio=99:99 \
    --security-opt seccomp=unconfined \
    --security-opt apparmor=unconfined \
    --device /dev/uinput \
    --device /dev/dri \
    --device-cgroup-rule 'c 13:* rwm' \
    --device-cgroup-rule 'c 226:* rwm' \
    --entrypoint /bin/bash \
    -e NVIDIA_DRIVER_CAPABILITIES=all \
    -e NMS_INTERACTIVE=1 \
    -e NMS_INTERACTIVE_SECONDS="${NMS_INTERACTIVE_SECONDS:-1800}" \
    -e NMS_BRIDGE_CONFIG_SOURCE=/work/runtime/resolved/nms-sunshine.cfg \
    -e NMS_DLSSNR_WORKING_SCALE="${NMS_DLSSNR_WORKING_SCALE:-}" \
    -e NMS_BRIDGE_LATENCY_BUDGET_MS="${NMS_BRIDGE_LATENCY_BUDGET_MS:-}" \
    -e NMS_NEURAL_PLACEMENT="${NMS_NEURAL_PLACEMENT:-}" \
    -e NMS_NEURAL_PASSES="${NMS_NEURAL_PASSES:-}" \
    -e NMS_NEURAL_RUNTIME_VARIANT="${NMS_NEURAL_RUNTIME_VARIANT:-}" \
    -e SUNSHINE_CORS_ORIGIN="https://127.0.0.1:47990" \
    -v "$project_dir:/work" \
    -v "$steam_root:/home/gamer/.steam" \
    -v "$steam_share:/home/gamer/.local/share/Steam" \
    -v "$nms_root:/games/nms:ro" \
    -v "$shader_cache:$shader_cache" \
    -v "$project_dir/runtime/sunshine-interactive:/home/gamer/.config/sunshine" \
    -v "$project_dir/runtime/steam-profile/htmlcache:/home/gamer/.steam/debian-installation/config/htmlcache" \
    -v "$project_dir/runtime/steam-launch/steam-runtime-check-requirements:/home/gamer/.steam/debian-installation/ubuntu12_32/steam-runtime/amd64/usr/bin/steam-runtime-check-requirements:ro" \
    -v /run/udev:/run/udev:ro \
    -v /dev/input:/dev/input \
    dlss-nms:local /work/scripts/nms-hook-diagnostic-inner.sh >/dev/null

for _ in $(seq 1 300); do
    if [ -e reports/nms-interactive-ready ]; then
        host_ip=$(hostname -I | awk '{print $1}')
        echo "ready=yes"
        echo "moonlight_host=$host_ip"
        echo "container=$container_name"
        exit 0
    fi
    if [ "$(docker inspect -f '{{.State.Running}}' "$container_name" 2>/dev/null || true)" != true ]; then
        docker logs --tail 160 "$container_name"
        exit 1
    fi
    sleep 1
done

echo "Timed out waiting for the interactive NMS session."
docker logs --tail 160 "$container_name"
exit 1
