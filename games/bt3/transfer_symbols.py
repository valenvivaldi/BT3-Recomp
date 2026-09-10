#!/usr/bin/env python3
"""Copy library symbol names from one variant's function map onto another's.

[symxfer] The recompiler substitutes its own HLE implementations for known
library functions *by name*: a map row called ``sceSifInitRpc`` becomes a call to
``ps2_stubs::sceSifInitRpc`` instead of the game's real code. That matters
because the real code talks to an IOP that does not exist here -- USA's
``sceSifInitRpc`` polls an IOP acknowledgement word forever if it actually runs.

The USA map is Ghidra-derived and carries those SDK symbol names. A splat-derived
map carries none, so a variant recompiles the real library bodies and hangs. The
bodies themselves are the same Sony SDK code built by the same toolchain, so the
names can be transferred by matching the instruction bytes.

Matching is done on a normalised copy of each function: the immediate field of
every instruction that encodes an address or a link-time constant is masked out
-- j/jal targets, lui, the addiu/ori completing a lui pair, and load/store
displacements -- because those differ between regions even when the code is
identical. Opcodes and register fields still have to agree exactly, and a name is
only transferred when exactly one candidate function in the target matches; an
ambiguous or missing match is reported, never guessed.

    transfer_symbols.py --from-elf USA_ELF --from-map USA_CSV \\
                        --to-elf PAL_ELF  --to-map PAL_CSV --output OUT_CSV
"""
import argparse
import csv
import re
import struct
import sys
from pathlib import Path

# Instructions whose immediate is an address or a link-time constant.
OP_J = 0x02
OP_JAL = 0x03
OP_LUI = 0x0F
OP_ADDIU = 0x09
OP_ORI = 0x0D
ADDRESS_OPS = {OP_J, OP_JAL, OP_LUI, OP_ADDIU, OP_ORI}

# Loads and stores: the 16-bit displacement is a link-time data offset, and the
# data segment does not sit at the same place in two regional builds. Both
# registers and the opcode still have to agree, so this loses little: it turns a
# would-be mismatch into a candidate that the uniqueness and size/prefix checks
# still have to clear. sceSifAllocIopHeap differs from its USA twin in exactly
# three such displacements and nothing else.
MEMORY_OPS = {
    0x1E, 0x1F,                                      # lq, sq (PS2)
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,  # lb..lwu
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,        # sb..swr
    0x31, 0x35, 0x36, 0x37,                          # lwc1, ldc1, lqc2, ld
    0x39, 0x3D, 0x3E, 0x3F,                          # swc1, sdc1, sqc2, sd
}

GENERIC_PREFIXES = ("FUN_", "sub_", "func_", "thunk_")


def load_segments(elf: Path):
    data = elf.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError(f"{elf} is not an ELF file")
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize = struct.unpack_from("<H", data, 42)[0]
    phnum = struct.unpack_from("<H", data, 44)[0]
    segments = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from(
            "<IIIIIIII", data, phoff + i * phentsize)
        if p_type == 1 and p_filesz:
            segments.append((p_vaddr, p_offset, p_filesz))
    return data, segments


def read_range(data: bytes, segments, vaddr: int, size: int):
    for seg_vaddr, seg_off, seg_size in segments:
        if seg_vaddr <= vaddr and vaddr + size <= seg_vaddr + seg_size:
            start = seg_off + (vaddr - seg_vaddr)
            return data[start:start + size]
    return None


def normalise(body: bytes) -> bytes | None:
    """Mask only the immediates that genuinely differ between regions.

    j/jal targets and lui immediates are absolute addresses. An addiu/ori is
    masked ONLY when it completes a lui pair -- masking every addiu would erase
    the syscall number in ``addiu $v1, $zero, N; syscall``, which is the one
    field telling ~160 otherwise identical syscall wrappers apart.
    """
    if not body or len(body) % 4:
        return None
    out = bytearray()
    lui_regs = set()
    for offset in range(0, len(body), 4):
        word = struct.unpack_from("<I", body, offset)[0]
        op = word >> 26
        rs = (word >> 21) & 0x1F
        rt = (word >> 16) & 0x1F
        if op in (OP_J, OP_JAL):
            word &= 0xFC000000
        elif op == OP_LUI:
            word &= 0xFFFF0000
            lui_regs.add(rt)
        elif op in (OP_ADDIU, OP_ORI) and rs in lui_regs:
            word &= 0xFFFF0000
            lui_regs.add(rt)
        elif op in MEMORY_OPS:
            word &= 0xFFFF0000
        out += struct.pack("<I", word)
    return bytes(out)



def rebase_stub_list(src_data, src_segs, src_rows, dst_data, dst_segs, dst_rows,
                     entries, min_size):
    """Re-point "name@0xADDR" stub bindings from the source variant to the target.

    The recompiler substitutes its own implementation for a stubbed function, and
    the binding is an ADDRESS. Reusing the source variant's addresses does not
    merely miss -- an address that happens to start a different function in the
    target silently replaces that function with an unrelated stub.
    """
    src_by_start = {r["_start"]: r for r in src_rows}
    dst_by_start = {r["_start"]: r for r in dst_rows}
    target = {}
    for row in dst_rows:
        body = read_range(dst_data, dst_segs, row["_start"], row["_end"] - row["_start"])
        key = normalise(body) if body else None
        if key:
            target.setdefault(key, []).append(row)

    # Anchors: every source function that matches exactly one target function.
    # The two builds are not a single global shift -- each region moves by its own
    # amount -- so an address is re-based with the offset of the nearest anchor
    # below it, and the candidate is then verified rather than trusted.
    anchors = []
    for row in src_rows:
        size = row["_end"] - row["_start"]
        if size < 32:
            continue
        body = read_range(src_data, src_segs, row["_start"], size)
        key = normalise(body) if body else None
        if not key:
            continue
        hits = target.get(key, [])
        if len(hits) == 1:
            anchors.append((row["_start"], hits[0]["_start"] - row["_start"]))
    anchors.sort()
    anchor_addrs = [a for a, _ in anchors]

    def nearest_offset(addr):
        import bisect
        i = bisect.bisect_right(anchor_addrs, addr) - 1
        return anchors[i][1] if i >= 0 else None

    def prefix_key(data, segs, start, size, instructions=4):
        body = read_range(data, segs, start, min(size, instructions * 4))
        return normalise(body) if body else None

    rebased, dropped = [], []
    for name, addr in entries:
        row = src_by_start.get(addr)
        if row is None:
            dropped.append((name, addr, "not a function start in the source map"))
            continue
        size = row["_end"] - row["_start"]
        if size < min_size:
            dropped.append((name, addr, f"too small to match uniquely ({size} bytes)"))
            continue
        body = read_range(src_data, src_segs, row["_start"], size)
        key = normalise(body) if body else None
        if not key:
            dropped.append((name, addr, "unreadable in the source ELF"))
            continue
        hits = target.get(key, [])
        if len(hits) == 1:
            rebased.append((name, hits[0]["_start"]))
            continue

        # No unique whole-body match: the same function can differ by an
        # instruction between regions. Fall back to the nearest anchor's offset
        # and accept only a candidate that really looks like the same function.
        offset = nearest_offset(addr)
        if offset is None:
            dropped.append((name, addr, f"{len(hits)} body matches, no anchor"))
            continue
        candidate = addr + offset
        row2 = dst_by_start.get(candidate)
        if row2 is None:
            dropped.append((name, addr, f"0x{candidate:08X} is not a function start"))
            continue
        size2 = row2["_end"] - row2["_start"]
        if abs(size2 - size) > 8:
            dropped.append((name, addr,
                            f"0x{candidate:08X} size {size2} vs {size}"))
            continue
        if prefix_key(src_data, src_segs, addr, size) != \
           prefix_key(dst_data, dst_segs, candidate, size2):
            dropped.append((name, addr, f"0x{candidate:08X} prefix differs"))
            continue
        rebased.append((name, candidate))
    return rebased, dropped


def read_map(path: Path):
    rows = list(csv.DictReader(path.open()))
    if not rows:
        raise ValueError(f"{path} is empty")
    for row in rows:
        row["_start"] = int(row["Start"], 16)
        row["_end"] = int(row["End"], 16)
    return rows


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from-elf", required=True, type=Path)
    ap.add_argument("--from-map", required=True, type=Path)
    ap.add_argument("--to-elf", required=True, type=Path)
    ap.add_argument("--to-map", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--rebase-stubs", type=Path, metavar="CONFIG",
                    help="also re-point the stubs = [ \"name@0xADDR\" ] list in this "
                         "recompiler config onto the target, writing --stubs-out")
    ap.add_argument("--stubs-out", type=Path,
                    help="where to write the re-based stub list (one entry per line)")
    ap.add_argument("--min-size", type=int, default=16,
                    help="skip functions smaller than this many bytes: tiny leaves "
                         "collide with each other and would match ambiguously "
                         "(default 16)")
    args = ap.parse_args()

    try:
        src_data, src_segs = load_segments(args.from_elf)
        dst_data, dst_segs = load_segments(args.to_elf)
        src_rows = read_map(args.from_map)
        dst_rows = read_map(args.to_map)
    except (OSError, ValueError) as exc:
        ap.error(str(exc))

    named = [r for r in src_rows
             if not r["Name"].startswith(GENERIC_PREFIXES)
             and (r["_end"] - r["_start"]) >= args.min_size]
    print(f"[symxfer] {len(named)} named source functions >= {args.min_size} bytes")

    # Index the target by normalised body so a lookup is one dict hit.
    target = {}
    for row in dst_rows:
        body = read_range(dst_data, dst_segs, row["_start"], row["_end"] - row["_start"])
        key = normalise(body) if body else None
        if key:
            target.setdefault(key, []).append(row)

    renamed = 0
    ambiguous = 0
    missing = []
    used = set()
    for row in named:
        body = read_range(src_data, src_segs, row["_start"], row["_end"] - row["_start"])
        key = normalise(body) if body else None
        if not key:
            missing.append((row["Name"], "unreadable in source ELF"))
            continue
        hits = target.get(key, [])
        if not hits:
            missing.append((row["Name"], "no byte match in target"))
            continue
        if len(hits) > 1:
            ambiguous += 1
            missing.append((row["Name"], f"{len(hits)} equally good matches"))
            continue
        hit = hits[0]
        if id(hit) in used:
            missing.append((row["Name"], "target already claimed"))
            continue
        used.add(id(hit))
        print(f"  {row['Name']:28} {row['Start']} -> {hit['Start']}")
        hit["Name"] = row["Name"]
        renamed += 1

    with args.output.open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["Name", "Start", "End", "Size"])
        for row in sorted(dst_rows, key=lambda r: r["_start"]):
            writer.writerow([row["Name"], row["Start"], row["End"], row["Size"]])

    if args.rebase_stubs:
        if not args.stubs_out:
            ap.error("--rebase-stubs requires --stubs-out")
        text = args.rebase_stubs.read_text()
        block = text.split("stubs = [", 1)
        if len(block) != 2:
            ap.error(f"no 'stubs = [' list in {args.rebase_stubs}")
        entries = []
        for m in re.finditer(r'"([A-Za-z_][A-Za-z0-9_]*)@0x([0-9A-Fa-f]+)"',
                             block[1].split("]", 1)[0]):
            entries.append((m.group(1), int(m.group(2), 16)))
        rebased, dropped = rebase_stub_list(src_data, src_segs, src_rows,
                                            dst_data, dst_segs, dst_rows,
                                            entries, args.min_size)
        args.stubs_out.write_text(
            "".join(f'  "{n}@0x{a:08X}",\n' for n, a in rebased))
        print(f"[symxfer] stubs: {len(rebased)}/{len(entries)} re-based -> {args.stubs_out}")
        print(f"[symxfer] stubs: {len(dropped)} dropped (a wrong address would "
              f"replace an unrelated function)")
        for n, a, why in dropped[:10]:
            print(f"    {n}@0x{a:08X}: {why}", file=sys.stderr)

    print(f"\n[symxfer] transferred {renamed} names -> {args.output}")
    print(f"[symxfer] {len(missing)} not transferred ({ambiguous} ambiguous)")
    for name, why in missing:
        print(f"    {name:28} {why}", file=sys.stderr)


if __name__ == "__main__":
    main()
