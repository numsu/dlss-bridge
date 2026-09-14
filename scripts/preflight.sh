#!/usr/bin/env bash
# Read-only inventory. Run from any directory; no files are created.
set -euo pipefail
printf 'GPU inventory\n'
nvidia-smi --query-gpu=index,uuid,name,driver_version --format=csv
printf '\nDocker storage (builds and containers write here)\n'
docker info --format '{{.DockerRootDir}}'
printf '\nAvailable build/runtime tools\n'
for tool in docker python3 gcc g++ make cmake clang++ x86_64-w64-mingw32-g++ wine wine64 vulkaninfo; do
    if location=$(command -v "$tool"); then
        printf '%s: %s\n' "$tool" "$location"
    else
        printf '%s: unavailable on host PATH\n' "$tool"
    fi
done
