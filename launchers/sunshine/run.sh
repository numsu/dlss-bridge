#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
exec "$root/controller/dlss_bridge.py" launch --frontend sunshine "$@"
