#!/bin/bash
set -u

REPORT=/work/reports/steam-login-isolated.log
READINESS=/work/reports/steam-readiness.log
CONNECTION=/home/gamer/.steam/debian-installation/logs/connection_log.txt
STEAM=/home/gamer/.steam/debian-installation/steam.sh

exec > >(tee "$REPORT") 2>&1
echo "diagnostic_start=$(date -u +%FT%TZ)"

sed -i 's/| head -n1/| sed -n "1p"/' /usr/local/bin/generate-xorg-config
install -d -o gamer -g gamer -m 0700 /run/user/1000
generate-xorg-config
Xorg :0 -noreset -nolisten tcp -ac >/work/reports/steam-login-xorg.log 2>&1 &
xorg_pid=$!
trap 'kill "$steam_pid" "$xorg_pid" 2>/dev/null || true' EXIT
steam_pid=""

for _ in $(seq 1 100); do
    DISPLAY=:0 xrandr >/dev/null 2>&1 && break
    kill -0 "$xorg_pid" 2>/dev/null || {
        tail -100 /work/reports/steam-login-xorg.log
        exit 1
    }
    sleep 0.1
done
DISPLAY=:0 xrandr >/dev/null 2>&1 || exit 1

before=$(grep -c '\[Logged On\]' "$CONNECTION" 2>/dev/null || true)
su - gamer -c 'exec env DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000 STEAM_RUNTIME=0 STEAM_RUNTIME_STEAMRT=/work/runtime/steam-launch/direct-runtime /home/gamer/.steam/debian-installation/steam.sh -silent -cef-disable-gpu -cef-disable-gpu-compositing' &
steam_pid=$!

logged_on=0
for second in $(seq 1 120); do
    now=$(grep -c '\[Logged On\]' "$CONNECTION" 2>/dev/null || true)
    if [ "$now" -gt "$before" ]; then
        logged_on=1
        echo "logged_on_after_seconds=$second"
        break
    fi
    kill -0 "$steam_pid" 2>/dev/null || {
        echo "steam_process_exited_after_seconds=$second"
        break
    }
    sleep 1
done

{
    echo "logged_on=$logged_on"
    echo "steam_alive=$(kill -0 "$steam_pid" 2>/dev/null && echo 1 || echo 0)"
    echo "webhelper_count=$(pgrep -u 1000 -c steamwebhelper 2>/dev/null || true)"
} | tee "$READINESS"

echo "recent_connection_events:"
tail -80 "$CONNECTION" 2>/dev/null | sed -n '/LogOn\|Logged On\|Connect\|websocket\|WebSocket/p' | tail -30 || true
echo "recent_webhelper_events:"
tail -100 /home/gamer/.steam/debian-installation/logs/webhelper.txt 2>/dev/null | sed -n '/BrowserReady\|websocket\|WebSocket\|Singleton\|lock/p' | tail -30 || true

exit $((1 - logged_on))
