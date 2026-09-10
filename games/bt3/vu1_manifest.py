#!/usr/bin/env python3
"""Extract a VU1 manifest from the VIF MPG packets embedded in a boot ELF.

The manifest records offsets, sizes and hashes only. The program bytes remain
in the user's local ELF and are read later by ``vu1_programs.py``.

[vu1manifest] The microprograms live in the ELF as a VIF packet stream, not as
readable sections. Each upload chunk is::

    00 00 00 00        4-byte pad (the packet is qword-aligned)
    4A NUM IMM IMM     VIFcode: CMD=0x4A (MPG), NUM x 64-bit units, IMM = dest
    <NUM * 8 bytes>    microcode

A VIF MPG transfer carries at most 256 instruction pairs, so a program longer
than 0x800 bytes arrives as several chunks: the first with ``IMM == 0`` and each
continuation with ``IMM`` equal to the number of 64-bit units already uploaded.
The 8-byte packet headers are *not* part of the program, and the runtime hashes
the reassembled microcode (``ps2_vu1.cpp``, ``VU1Interpreter``'s JIT lookup), so
the manifest has to describe a program as the list of its chunk byte ranges.

The ``.DVP.overlay.*`` ELF sections describe the same programs and are handy as
a cross-check on the chunk-size profile, but their *bytes differ* from the
uploaded microcode (they are the pre-link form), so they cannot be hashed for
the runtime's benefit. This module verifies the scan against them and refuses to
emit a manifest whose chunk sizes disagree.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

# VIFcode CMD for "transfer microprogram". A VIF MPG NUM field counts 64-bit
# units and 0 means the maximum, 256 (= 0x800 bytes = the chunk limit).
VIF_CMD_MPG = 0x4A
MPG_UNIT = 8
MPG_MAX_UNITS = 256
# Each chunk is preceded by a 4-byte pad plus the 4-byte VIFcode.
MPG_HEADER = 8


def fnv1a64(data: bytes) -> int:
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def sections(elf: Path):
    """Yield (name, offset, size, body) for every non-empty .DVP.overlay section."""
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


def mpg_packets(data: bytes) -> list[tuple[int, int, int]]:
    """Find every plausible MPG packet. Returns (code_offset, length, dest_units).

    A candidate is a 4-byte-aligned VIFcode with CMD 0x4A whose 4 preceding bytes
    are the zero pad, and whose payload fits in the file. Microcode can contain
    the same byte pattern by chance, so the chaining in group_programs() -- which
    demands that a continuation chunk start exactly where the previous one ended
    and carry the matching destination address -- is what rejects false hits.
    """
    found = []
    for offset in range(4, len(data) - 4, 4):
        word = struct.unpack_from("<I", data, offset)[0]
        if (word >> 24) != VIF_CMD_MPG:
            continue
        if struct.unpack_from("<I", data, offset - 4)[0] != 0:
            continue
        units = (word >> 16) & 0xFF or MPG_MAX_UNITS
        length = units * MPG_UNIT
        code = offset + 4
        if code + length > len(data):
            continue
        found.append((code, length, word & 0xFFFF))
    return found


def group_programs(packets: list[tuple[int, int, int]]) -> list[list[tuple[int, int]]]:
    """Chain MPG chunks into programs. Returns a list of [(offset, length), ...].

    ``dest == 0`` opens a program. A chunk continues the current one when its
    packet header sits exactly at the end of the previous chunk and its
    destination equals the number of 64-bit units uploaded so far.
    """
    programs: list[list[tuple[int, int]]] = []
    current: list[tuple[int, int]] | None = None
    units = 0
    next_code = -1
    for code, length, dest in packets:
        if current is not None and code == next_code and dest == units:
            current.append((code, length))
            units += length // MPG_UNIT
            next_code = code + length + MPG_HEADER
            continue
        if current is not None:
            programs.append(current)
            current = None
        if dest != 0:
            # A stray 0x4A pattern inside microcode, or a chunk whose program
            # start was not recognised. Either way it cannot open a program.
            continue
        current = [(code, length)]
        units = length // MPG_UNIT
        next_code = code + length + MPG_HEADER
    if current is not None:
        programs.append(current)
    return programs


def cross_check(elf: Path, programs: list[list[tuple[int, int]]]) -> None:
    """Compare the scanned chunk sizes with the .DVP.overlay section sizes.

    The section bytes are the pre-link form and must not be hashed, but their
    sizes are the same chunk profile, so a mismatch means the scan went wrong.
    """
    try:
        expected = [size for _, _, size, _ in sections(elf)]
    except (OSError, ValueError):
        return  # no usable section table; the scan is all we have
    if not expected:
        return
    got = [length for program in programs for _, length in program]
    if got != expected:
        raise ValueError(
            "MPG scan disagrees with the .DVP.overlay section sizes.\n"
            f"  sections: {[hex(s) for s in expected]}\n"
            f"  scanned:  {[hex(s) for s in got]}")


def make_manifest(elf: Path) -> dict:
    data = elf.read_bytes()
    programs = group_programs(mpg_packets(data))
    if not programs:
        raise ValueError(f"{elf} has no recognisable VIF MPG packets")
    cross_check(elf, programs)
    entries = []
    for index, segments in enumerate(programs):
        body = b"".join(data[offset:offset + length] for offset, length in segments)
        digest = fnv1a64(body)
        entries.append({
            "id": f"{index:02d}-{digest:016x}",
            "extent": f"0x{len(body):x}",
            "fnv1a64": f"0x{digest:016x}",
            "segments": [{"offset": f"0x{offset:x}", "length": f"0x{length:x}"}
                         for offset, length in segments],
        })
    return {
        "_comment": "Generated from the user's ELF; offsets and hashes only.",
        "elf_sha256": hashlib.sha256(data).hexdigest(),
        "image_size": "0x4000",
        "programs": entries,
    }


def restrict_to_reference(manifest: dict, reference: Path) -> dict:
    """Keep only the programs whose extent appears in a reference manifest.

    The USA manifest is the validated JIT set: it covers 7 of the 9 programs in
    the stream and lets the interpreter handle the rest. Restricting a variant to
    the same extents keeps a bring-up comparable to USA instead of introducing
    JIT coverage USA never ran.
    """
    wanted = {p["extent"] for p in json.loads(reference.read_text())["programs"]}
    kept = [p for p in manifest["programs"] if p["extent"] in wanted]
    if not kept:
        raise ValueError(f"no scanned program matches an extent in {reference}")
    manifest = dict(manifest)
    manifest["programs"] = kept
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("elf", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--reference", type=Path,
                        help="restrict output to the program extents present in this "
                             "manifest (use games/bt3/vu1_programs.json to mirror the "
                             "validated USA JIT set)")
    args = parser.parse_args()
    try:
        manifest = make_manifest(args.elf)
        if args.reference:
            manifest = restrict_to_reference(manifest, args.reference)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {len(manifest['programs'])} VU1 programs to {args.output}")
    for program in manifest["programs"]:
        chunks = len(program["segments"])
        print(f"  {program['id']} extent={program['extent']} chunks={chunks}")


if __name__ == "__main__":
    main()
