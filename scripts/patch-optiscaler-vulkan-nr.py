#!/usr/bin/env python3
"""Disable only OptiScaler's two Vulkan DLSS-NR calls in a staged DLL."""
from __future__ import annotations
import hashlib
import pathlib
import sys

SOURCE_SHA256 = "8f526752d82c9cba99bf747a216a8caccd59a585d9ed0e217a8d47ab3c7c4998"
PATCHES = {
    0x2C063A: bytes.fromhex("e82170fcff"),
    0x2C09BF: bytes.fromhex("e89c6cfcff"),
}

def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} SOURCE_DLL DEST_DLL")
    source, dest = map(pathlib.Path, sys.argv[1:])
    data = bytearray(source.read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    if digest != SOURCE_SHA256:
        raise SystemExit(f"refusing unfamiliar OptiScaler.dll: sha256={digest}")
    for offset, expected in PATCHES.items():
        actual = bytes(data[offset:offset + len(expected)])
        if actual != expected:
            raise SystemExit(f"refusing offset {offset:#x}: expected {expected.hex()}, got {actual.hex()}")
        data[offset:offset + len(expected)] = b"\x90" * len(expected)
    dest.write_bytes(data)
    print(f"staged OptiScaler.dll with Vulkan DLSS-NR disabled: {dest}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
