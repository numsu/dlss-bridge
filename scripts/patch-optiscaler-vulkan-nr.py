#!/usr/bin/env python3
"""Apply the pinned OptiScaler integration patches to a staged DLL."""
from __future__ import annotations
import hashlib
import pathlib
import sys

VARIANTS = {
    # wilsjo2 v0.8.3: IFeature_Vk::Evaluate's native DlssNr_Vk creation
    # branch. Taking the existing "disabled" branch leaves NeuralRendering
    # null, so no native pass enters the shader pipeline. The private D3D12
    # implementation is a separate class and remains intact. The neural
    # toggle handler is redirected from DlssNrEnabled (offset 0x13a) to
    # DlssNrApplyModel (offset 0x11b0). This keeps the model and private SR
    # warm while showing or hiding the model's contribution for hitch-free
    # A/B comparison. All offsets are guarded by the complete source hash and
    # exact expected bytes.
    "856f14f111118090ae3eb2a9d11b6f5693e5df8ee99c75404f73d21e5a917a16": {
        0x104A0E: (bytes.fromhex("7463"), bytes.fromhex("eb63")),
        0x1E8AE8: (bytes.fromhex("3b010000"), bytes.fromhex("b1110000")),
        0x1E8AF1: (bytes.fromhex("3a010000"), bytes.fromhex("b0110000")),
        0x1E8AFA: (bytes.fromhex("3c010000"), bytes.fromhex("b2110000")),
        0x1E8B06: (bytes.fromhex("3f010000"), bytes.fromhex("b5110000")),
        0x1E8B1B: (bytes.fromhex("3a010000"), bytes.fromhex("b0110000")),
        0x1E8B22: (bytes.fromhex("3b010000"), bytes.fromhex("b1110000")),
        0x1E8B2B: (bytes.fromhex("3c010000"), bytes.fromhex("b2110000")),
        0x1E8C63: (bytes.fromhex("3b010000"), bytes.fromhex("b1110000")),
        0x1E8C6D: (bytes.fromhex("3a010000"), bytes.fromhex("b0110000")),
        0x1E8C76: (bytes.fromhex("3c010000"), bytes.fromhex("b2110000")),
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
    for offset, (expected, replacement) in patches.items():
        actual = bytes(data[offset:offset + len(expected)])
        if actual != expected:
            raise SystemExit(f"refusing offset {offset:#x}: expected {expected.hex()}, got {actual.hex()}")
        if len(replacement) != len(expected):
            raise SystemExit(f"invalid patch at {offset:#x}: replacement changes file size")
        data[offset:offset + len(expected)] = replacement
    dest.write_bytes(data)
    print(f"staged OptiScaler.dll with private-path and hitch-free A/B patches: {dest}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
