#!/usr/bin/env python3
"""Packs PNGs from an .iconset (scripts/make-icon.swift) into a Windows .ico.

    python3 scripts/make-ico.py <AppIcon.iconset> <out.ico>
"""
import struct
import sys
from pathlib import Path

iconset, out = Path(sys.argv[1]), Path(sys.argv[2])
sources = [(16, "icon_16x16.png"), (32, "icon_32x32.png"), (64, "icon_32x32@2x.png"),
           (128, "icon_128x128.png"), (256, "icon_256x256.png")]
images = [(size, (iconset / name).read_bytes()) for size, name in sources]

header = struct.pack("<HHH", 0, 1, len(images))
offset = len(header) + 16 * len(images)
entries, blobs = b"", b""
for size, png in images:
    dim = 0 if size >= 256 else size  # 0 means 256 in ICONDIRENTRY
    entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset)
    blobs += png
    offset += len(png)
out.write_bytes(header + entries + blobs)
print(f"Wrote {out} ({len(images)} sizes)")
