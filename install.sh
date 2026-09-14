#!/usr/bin/env sh
set -eu
prefix=${DLSS_BRIDGE_PREFIX:-"$HOME/.local"}
state_base=${XDG_STATE_HOME:-"$HOME/.local/state"}
case ${1:-} in
  --uninstall)
    rm -f "$prefix/bin/dlss-bridge"
    rm -rf "$prefix/libexec/dlss-bridge"
    echo "DLSS Bridge was removed. User state remains at $state_base/dlss-bridge"
    exit 0
    ;;
  "")
    ;;
  *)
    echo "Usage: install.sh [--uninstall]" >&2
    exit 2
    ;;
esac
repository=${DLSS_BRIDGE_REPOSITORY:-numsu/dlss-bridge}
version=${DLSS_BRIDGE_VERSION:-latest}
case "$version" in
  latest) release_path=releases/latest/download ;;
  *) release_path=releases/download/$version ;;
esac
for c in curl tar sha256sum python3; do command -v "$c" >/dev/null 2>&1 || { echo "Required command missing: $c" >&2; exit 1; }; done
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT HUP INT TERM
base=${DLSS_BRIDGE_RELEASE_BASE:-"https://github.com/$repository/$release_path"}
artifact=dlss-bridge-linux-x86_64.tar.gz
curl -fsSL --retry 3 "$base/$artifact" -o "$tmp/$artifact"
curl -fsSL --retry 3 "$base/SHA256SUMS" -o "$tmp/SHA256SUMS"
(cd "$tmp" && grep "  $artifact$" SHA256SUMS | sha256sum -c -)
mkdir -p "$tmp/unpacked" "$prefix/bin" "$prefix/libexec"
tar -xzf "$tmp/$artifact" -C "$tmp/unpacked"
# Acquire and validate the required runtime before changing the installed
# application. A failed setup therefore leaves the previous version intact.
echo "Downloading and verifying the neural runtime..."
python3 "$tmp/unpacked/libexec/dlss-bridge/controller/dlss_bridge.py" acquire-runtime
if [ -d "$prefix/libexec/dlss-bridge" ]; then action=Updated; else action=Installed; fi
new="$prefix/libexec/.dlss-bridge.new.$$"
old="$prefix/libexec/.dlss-bridge.old.$$"
rm -rf "$new" "$old"
cp -a "$tmp/unpacked/libexec/dlss-bridge" "$new"
if [ -d "$prefix/libexec/dlss-bridge" ]; then
  mv "$prefix/libexec/dlss-bridge" "$old"
fi
mv "$new" "$prefix/libexec/dlss-bridge"
rm -rf "$old"
install -m 0755 "$tmp/unpacked/bin/dlss-bridge" "$prefix/bin/dlss-bridge"
echo "$action: $prefix/bin/dlss-bridge"
case ":$PATH:" in *":$prefix/bin:"*) ;; *) echo "Add $prefix/bin to PATH";; esac
echo "Run this installer again to update to a newer release."
