#!/usr/bin/env python3
"""Create a PS2Recomp function CSV from splat's ``asm/text.s`` output.

The output contains only function boundaries and names derived from the user's
ELF.  It is an input to the static recompiler, not game code, and should stay
in the ignored per-serial work directory.

Example:
  python3 games/bt3/splat_function_map.py \
      /path/to/asm/text.s games/bt3/work/SLES_549.45/functions.csv \
      --text-end 0x002c0680
"""
import argparse
import csv
from pathlib import Path


def function_starts(asm: Path) -> list[int]:
    starts: set[int] = set()
    for line in asm.read_text(errors="replace").splitlines():
        fields = line.split()
        if len(fields) != 2 or fields[0] != "glabel" or not fields[1].startswith("func_"):
            continue
        address = fields[1][len("func_"):]
        try:
            starts.add(int(address, 16))
        except ValueError:
            continue
    return sorted(starts)


def write_map(asm: Path, output: Path, text_end: int) -> int:
    starts = function_starts(asm)
    if not starts:
        raise ValueError(f"no 'glabel func_<address>' entries found in {asm}")
    starts = [start for start in starts if start < text_end]
    if not starts:
        raise ValueError(f"no function starts before text end {text_end:#x}")

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(("Name", "Start", "End", "Size"))
        for index, start in enumerate(starts):
            end = starts[index + 1] if index + 1 < len(starts) else text_end
            if end > start:
                writer.writerow((f"FUN_{start:08x}", f"0x{start:08X}",
                                 f"0x{end:08X}", end - start))
    return len(starts)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asm", type=Path, help="splat-generated asm/text.s")
    parser.add_argument("output", type=Path, help="PS2Recomp CSV to create")
    parser.add_argument("--text-end", required=True, type=lambda value: int(value, 0),
                        help="exclusive virtual end of the EE .text section")
    args = parser.parse_args()
    try:
        count = write_map(args.asm, args.output, args.text_end)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"wrote {count} function boundaries to {args.output}")


if __name__ == "__main__":
    main()
