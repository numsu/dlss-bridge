#!/bin/bash
set -u

REPORT=/work/reports/nms-hook-diagnostic.log
CONNECTION=/home/gamer/.steam/debian-installation/logs/connection_log.txt
STEAM=/home/gamer/.steam/debian-installation/steam.sh
RUN=/work/runtime/nms-run
BRIDGE_LOG=$RUN/Binaries/dlss5-vk-bridge.log

exec > >(tee "$REPORT") 2>&1
echo "diagnostic_start=$(date -u +%FT%TZ)"

mkdir -p "$RUN/Binaries"
for entry in /games/nms/* /games/nms/.[!.]*; do
    [ -e "$entry" ] || continue
    [ "$(basename "$entry")" = Binaries ] && continue
    ln -sfn "$entry" "$RUN/$(basename "$entry")"
done
for entry in /games/nms/Binaries/*; do
    [ -e "$entry" ] || continue
    [ "$(basename "$entry")" = NMS.exe ] && continue
    [ "$(basename "$entry")" = SETTINGS ] && continue
    ln -sfn "$entry" "$RUN/Binaries/$(basename "$entry")"
done
if [ -L "$RUN/Binaries/SETTINGS" ]; then unlink "$RUN/Binaries/SETTINGS"; fi
mkdir -p "$RUN/Binaries/SETTINGS"
for entry in /games/nms/Binaries/SETTINGS/*; do
    [ -e "$entry" ] || continue
    [ "$(basename "$entry")" = TKGRAPHICSSETTINGS.MXML ] && continue
    ln -sfn "$entry" "$RUN/Binaries/SETTINGS/$(basename "$entry")"
done
install -m 0644 /games/nms/Binaries/SETTINGS/TKGRAPHICSSETTINGS.MXML "$RUN/Binaries/SETTINGS/TKGRAPHICSSETTINGS.MXML"
sed -i 's/name="AntiAliasing" value="[^"]*"/name="AntiAliasing" value="DLSS"/' "$RUN/Binaries/SETTINGS/TKGRAPHICSSETTINGS.MXML"
install -m 0644 /work/runtime/nms-test/NMS.exe "$RUN/Binaries/NMS.exe"
rm -f "$RUN/Binaries/winhttp.dll" "$RUN/Binaries/XINPUT9_1_0.dll"
install -m 0644 /work/runtime/nms-test/winmm.dll "$RUN/Binaries/winmm.dll"
install -m 0644 /work/runtime/nms-test/vulkan-1.dll "$RUN/Binaries/vulkan-1.dll"
install -m 0644 /work/runtime/GE-Proton11-6-x86_64/files/lib/wine/x86_64-windows/vulkan-1.dll "$RUN/Binaries/vulkan-1-real.dll"
install -m 0644 /work/runtime/nms-test/dlss5-vk-bridge.cfg "$RUN/Binaries/dlss5-vk-bridge.cfg"
bridge_latency_budget_ms="${NMS_BRIDGE_LATENCY_BUDGET_MS:-16}"
case "$bridge_latency_budget_ms" in
    ''|*[!0-9]*) echo "Invalid NMS_BRIDGE_LATENCY_BUDGET_MS=$bridge_latency_budget_ms"; exit 2 ;;
esac
if [ "$bridge_latency_budget_ms" -lt 8 ] || [ "$bridge_latency_budget_ms" -gt 125 ]; then
    echo "NMS_BRIDGE_LATENCY_BUDGET_MS must be between 8 and 125"
    exit 2
fi
sed -i "s/^latency_budget_ms=.*/latency_budget_ms=$bridge_latency_budget_ms/" "$RUN/Binaries/dlss5-vk-bridge.cfg"
echo "bridge_latency_budget_ms=$bridge_latency_budget_ms"
python3 /work/scripts/patch-optiscaler-vulkan-nr.py \
    /work/runtime/optiscaler-full/OptiScaler.dll "$RUN/Binaries/OptiScaler.dll"
chmod 0644 "$RUN/Binaries/OptiScaler.dll"
install -m 0644 /work/runtime/optiscaler-full/OptiScaler.ini "$RUN/Binaries/OptiScaler.ini"
cp -a /work/runtime/optiscaler-full/OptiScaler "$RUN/Binaries/"
install -m 0644 /work/runtime/optiscaler-full/nvngx.dll_dlssnr.dll "$RUN/Binaries/nvngx.dll_dlssnr.dll"
install -m 0644 /work/runtime/neural/nvngx_dlssnr.dll "$RUN/Binaries/nvngx_dlssnr.dll"
# OptiScaler drives the neural snippet through NVIDIA's driver NGX core. Keep
# both core filenames beside the isolated executable so Wine does not depend on
# a system32 layout that varies between Proton prefixes.
install -m 0644 /work/runtime/neural/_nvngx.dll "$RUN/Binaries/_nvngx.dll"
install -m 0644 /work/runtime/neural/nvngx.dll "$RUN/Binaries/nvngx.dll"
sed -i 's/^VulkanUpscaler=.*/VulkanUpscaler=dlss/' "$RUN/Binaries/OptiScaler.ini"
sed -i '/^\[Log\]/,/^\[/ s/^LogToFile=.*/LogToFile=false/' "$RUN/Binaries/OptiScaler.ini"
sed -i '/^\[fakenvapi\]/,/^\[/ s/^ForceReflex=.*/ForceReflex=1/' "$RUN/Binaries/OptiScaler.ini"
sed -i '/^\[DlssNr\]/,/^\[/ s/^Enabled=.*/Enabled=true/' "$RUN/Binaries/OptiScaler.ini"
dlssnr_working_scale="${NMS_DLSSNR_WORKING_SCALE:-1.0}"
case "$dlssnr_working_scale" in
    0.25|0.50|0.5|0.75|1.0) ;;
    *) echo "Unsupported NMS_DLSSNR_WORKING_SCALE=$dlssnr_working_scale"; exit 2 ;;
esac
sed -i "/^\[DlssNr\]/,/^\[/ s/^WorkingScale=.*/WorkingScale=$dlssnr_working_scale/" "$RUN/Binaries/OptiScaler.ini"
echo "dlssnr_working_scale=$dlssnr_working_scale"
echo 275850 > "$RUN/Binaries/steam_appid.txt"
chown -R gamer:gamer "$RUN/Binaries"
: > "$BRIDGE_LOG"
chown gamer:gamer "$BRIDGE_LOG"

sed -i 's/| head -n1/| sed -n "1p"/' /usr/local/bin/generate-xorg-config
install -d -o gamer -g gamer -m 0700 /run/user/1000
generate-xorg-config
Xorg :0 -noreset -nolisten tcp -ac >/work/reports/nms-hook-xorg.log 2>&1 &
xorg_pid=$!
steam_pid=""
game_pid=""
session_pid=""
trap 'kill "$game_pid" "$steam_pid" "$session_pid" "$xorg_pid" 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
    DISPLAY=:0 xrandr >/dev/null 2>&1 && break
    kill -0 "$xorg_pid" 2>/dev/null || {
        tail -100 /work/reports/nms-hook-xorg.log
        exit 1
    }
    sleep 0.1
done
DISPLAY=:0 xrandr >/dev/null 2>&1 || exit 1

if [ "${NMS_INTERACTIVE:-0}" = 1 ]; then
    for device in /dev/uinput /dev/input /dev/dri /dev/dri/card* /dev/dri/renderD*; do
        [ -e "$device" ] || continue
        gid=$(stat -c '%g' "$device")
        group=$(getent group "$gid" | cut -d: -f1 || true)
        if [ -z "$group" ]; then
            group="nms-device-$gid"
            groupadd --gid "$gid" "$group"
        fi
        usermod -aG "$group" gamer
    done

    : > /home/gamer/.config/sunshine/sunshine.log
    chown gamer:gamer /home/gamer/.config/sunshine/sunshine.log
    capsh --keep=1 --user=gamer --inh=cap_sys_nice --addamb=cap_sys_nice -- \
        -c 'exec env HOME=/home/gamer USER=gamer LOGNAME=gamer dbus-run-session -- /usr/local/bin/gaming-session' \
        >/work/reports/nms-interactive-sunshine.log 2>&1 &
    session_pid=$!

    for _ in $(seq 1 150); do
        if grep -Eq 'Web UI.*47990|Listening.*47989' /home/gamer/.config/sunshine/sunshine.log 2>/dev/null; then
            break
        fi
        kill -0 "$session_pid" 2>/dev/null || {
            echo "Sunshine session exited unexpectedly"
            tail -120 /work/reports/nms-interactive-sunshine.log || true
            exit 3
        }
        sleep 0.1
    done
fi

su - gamer -c 'exec env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 STEAM_RUNTIME=0 STEAM_RUNTIME_STEAMRT=/work/runtime/steam-launch/direct-runtime /home/gamer/.steam/debian-installation/steam.sh -silent -cef-disable-gpu -cef-disable-gpu-compositing' &
steam_pid=$!
sleep 15
kill -0 "$steam_pid" 2>/dev/null || {
    echo "Steam exited before game launch"
    exit 2
}
echo "steam_login_marker=$(grep -F "[Logged On]" "$CONNECTION" 2>/dev/null | tail -1 | tr -d "\r")"

su - gamer -c "cd /work/runtime/nms-run/Binaries && exec env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 STEAM_COMPAT_CLIENT_INSTALL_PATH=/home/gamer/.steam/debian-installation STEAM_COMPAT_DATA_PATH=/work/runtime/proton-nms-test STEAM_COMPAT_INSTALL_PATH=/work/runtime/nms-run STEAM_COMPAT_APP_ID=275850 SteamAppId=275850 SteamGameId=275850 WINEDLLOVERRIDES='winhttp=b;winmm=n,b;vulkan-1=n,b;xinput9_1_0=b' WINEDEBUG=-all PROTON_LOG=0 /work/runtime/GE-Proton11-6-x86_64/proton run /work/runtime/nms-run/Binaries/NMS.exe" &
game_pid=$!
echo "game_launch_pid=$game_pid"
if [ "${NMS_INTERACTIVE:-0}" != 1 ]; then
    (
        sleep 10
        DISPLAY=:0 scrot /work/reports/nms-before-input.png || true
        /work/runtime/uinput-hold-e || true
        sleep 10
        DISPLAY=:0 scrot /work/reports/nms-after-play-key.png || true
    ) &
fi

max_seconds=120
if [ "${NMS_INTERACTIVE:-0}" = 1 ]; then
    max_seconds="${NMS_INTERACTIVE_SECONDS:-1800}"
    touch /work/reports/nms-interactive-ready
fi
for second in $(seq 1 "$max_seconds"); do
    if [ "${NMS_INTERACTIVE:-0}" != 1 ] && grep -Fq 'first D3D12 evaluate completed' "$BRIDGE_LOG" 2>/dev/null; then
        echo "ngx_observed_after_seconds=$second"
        break
    fi
    kill -0 "$game_pid" 2>/dev/null || {
        echo "game_process_exited_after_seconds=$second"
        break
    }
    sleep 1
done

echo "optiscaler_log:"
tail -240 "$RUN/Binaries/OptiScaler.log" 2>/dev/null || true
echo "bridge_log:"
cat "$BRIDGE_LOG" 2>/dev/null || true
echo "proton_processes:"
pgrep -a -u 1000 'NMS.exe|wineserver|wine64' || true

test -s "$BRIDGE_LOG"
