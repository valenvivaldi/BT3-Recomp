#!/usr/bin/env python3
"""Extract a VU1 manifest from ELF DVP overlay sections.

The manifest records offsets, sizes and hashes only. The program bytes remain
in the user's local ELF and are read later by ``vu1_programs.py``.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def fnv1a64(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def sections(elf: Path):
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ValueError(f"{elf} is not a little-endian ELF32 file")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    shoff, shentsize, shnum, shstrndx = header[6], header[11], header[12], header[13]
    raw = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize)
           for i in range(shnum)]
    _, _, _, _, names_off, names_size, *_ = raw[shstrndx]
    names = data[names_off:names_off + names_size]
    for name_off, _, _, _, offset, size, *_ in raw:
        end = names.find(b"\x00", name_off)
        name = names[name_off:end].decode("ascii", errors="replace")
        if name.startswith(".DVP.overlay.") and size:
            yield name, offset, size, data[offset:offset + size]


def make_manifest(elf: Path) -> dict:
    data = elf.read_bytes()
    programs = []
    for index, (name, offset, size, body) in enumerate(sections(elf)):
        digest = fnv1a64(body)
        programs.append({
            "id": f"{index:02d}-{digest:016x}",
            "section": name,
            "extent": f"0x{size:x}",
            "fnv1a64": f"0x{digest:016x}",
            "segments": [{"offset": f"0x{offset:x}", "length": f"0x{size:x}"}],
        })
    if not programs:
        raise ValueError(f"{elf} has no non-empty .DVP.overlay sections")
    return {
        "_comment": "Generated from the user's ELF; offsets and hashes only.",
        "elf_sha256": hashlib.sha256(data).hexdigest(),
        "image_size": "0x4000",
        "programs": programs,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        manifest = make_manifest(args.elf)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {len(manifest['programs'])} VU1 overlays to {args.output}")


if __name__ == "__main__":
    main()
