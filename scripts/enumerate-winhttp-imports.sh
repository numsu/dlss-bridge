#!/bin/bash
set -euo pipefail
while IFS= read -r -d '' binary; do
    imports=$(x86_64-w64-mingw32-objdump -p "$binary" 2>/dev/null | awk '
        BEGIN { hit=0 }
        /DLL Name: [Ww][Ii][Nn][Hh][Tt][Tt][Pp]/ { hit=1; next }
        /DLL Name:/ { hit=0 }
        hit && /^[[:space:]]*[0-9a-fA-F]+[[:space:]]/ { print $3 }
    ')
    if [ -n "$imports" ]; then
        echo "FILE $(basename "$binary")"
        echo "$imports"
    fi
done < <(find /game/Binaries -maxdepth 1 -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0)
