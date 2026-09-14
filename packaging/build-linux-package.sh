#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); version=${VERSION:-dev}; dist=${1:-"$root/dist"}
stage="$dist/stage-linux"; runtime="$stage/libexec/dlss-bridge"
rm -rf "$stage"; mkdir -p "$runtime" "$stage/bin" "$dist"
"$root/packaging/build-runtime.sh" "$runtime"
cat >"$stage/bin/dlss-bridge" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")/../libexec/dlss-bridge" && pwd)
python3 -c 'import sys; assert sys.version_info >= (3,11)' 2>/dev/null || { echo "Python 3.11 or newer is required" >&2; exit 1; }
exec python3 "$here/controller/dlss_bridge.py" "$@"
EOF
chmod 0755 "$stage/bin/dlss-bridge"; printf '%s\n' "$version" >"$runtime/VERSION"
tar -C "$stage" -czf "$dist/dlss-bridge-linux-x86_64.tar.gz" .
(cd "$dist" && sha256sum dlss-bridge-linux-x86_64.tar.gz >SHA256SUMS)
