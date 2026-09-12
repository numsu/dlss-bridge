#!/bin/bash
set -euo pipefail
docker rm -f dlss-nms-interactive >/dev/null 2>&1 || true
echo "Stopped the isolated DLSS NMS test session."
