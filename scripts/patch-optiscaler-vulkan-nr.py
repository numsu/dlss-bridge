#!/usr/bin/env python3
"""Disable only OptiScaler's two Vulkan DLSS-NR calls in a staged DLL."""
from __future__ import annotations
import hashlib
import pathlib
import sys

VARIANTS = {
    # wilsjo2 v0.7.7: pre- and post-SR Vulkan calls to EvaluateAtSeamVk.
    "5d05d560bd27eee4aba24f8fab5d118f916550d8b65a98b60d4a855275223b3c": {
        0x2E61C5: bytes.fromhex("e8e6b2fbff"),
        0x2E63AA: bytes.fromhex("e801b1fbff"),
    },
}

def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} SOURCE_DLL DEST_DLL")
    source, dest = map(pathlib.Path, sys.argv[1:])
    data = bytearray(source.read_bytes())
    digest = hashlib.sha256(data).hexdigest()
    patches = VARIANTS.get(digest)
    if patches is None:
        raise SystemExit(f"refusing unfamiliar OptiScaler.dll: sha256={digest}")
    for offset, expected in patches.items():
        actual = bytes(data[offset:offset + len(expected)])
        if actual != expected:
            raise SystemExit(f"refusing offset {offset:#x}: expected {expected.hex()}, got {actual.hex()}")
        data[offset:offset + len(expected)] = b"\x90" * len(expected)
    dest.write_bytes(data)
    print(f"staged OptiScaler.dll with Vulkan DLSS-NR disabled: {dest}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
