#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bridge_log="$root_dir/runtime/nms-run/Binaries/dlss5-vk-bridge.log"
report="$root_dir/reports/gpu-neural-window.txt"
mkdir -p "$root_dir/reports"

deadline=$((SECONDS + 600))
while (( SECONDS < deadline )); do
  if [[ -f "$bridge_log" ]] && grep -qE 'first frame bridged|frame timing upload=' "$bridge_log"; then
    {
      date --iso-8601=seconds
      nvidia-smi --query-gpu=index,pci.bus_id,name,utilization.gpu,utilization.memory,memory.used,power.draw,clocks.sm,clocks.mem,temperature.gpu,fan.speed --format=csv
      nvidia-smi dmon -i 0,1 -s pucvmet -d 1 -c 45
    } >"$report" 2>&1
    exit 0
  fi
  sleep 1
done

printf 'Timed out waiting for neural bridge activation\n' >"$report"
exit 1
