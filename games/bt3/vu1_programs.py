#!/usr/bin/env python3
"""vu1_programs.py -- [vu1manifest] rebuild the static VU1 recompiler's input from the user's own ELF.

The game's VU1 microprograms are game code, so nothing derived from them is committed. vu1_programs.json
lists, per program, the ELF file offsets/lengths of its bytes (a program stored as VIF MPG packets is several
segments, the 8-byte MPG headers between 2 KB chunks are skipped), the upload extent, and the FNV-1a hash the
runtime matches an uploaded program with. This script cuts the bodies out of SLUS_216.78, verifies them, pads
each to a 16 KB microcode image and runs ps2xRuntime/tools/gen_vu1.py, which writes
ps2xRuntime/src/lib/vu1_jit_gen.inc (git-ignored). Without that file the runtime builds with an empty program
table and runs the VU1 interpreter instead.

Usage: vu1_programs.py --elf SLUS_216.78 [--runtime <repo>/ps2xRuntime] [--work <dir>] [--manifest vu1_programs.json]
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def fnv1a64(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def generate(elf: Path, runtime: Path, work: Path,
             manifest: Path = HERE / "vu1_programs.json",
             output: Path | None = None) -> Path:
    """Cut the programs out of `elf`, verify, write images under `work`, run the generator. Returns the .inc path."""
    spec = json.loads(manifest.read_text())
    image_size = int(spec["image_size"], 16)
    data = elf.read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != spec["elf_sha256"]:
        print(f"WARNING: {elf} sha256 {got[:16]}... differs from the manifest's ELF; the VU1 program hashes will tell")
    work.mkdir(parents=True, exist_ok=True)
    args = []
    for p in spec["programs"]:
        extent = int(p["extent"], 16)
        body = b"".join(data[int(s["offset"], 16):int(s["offset"], 16) + int(s["length"], 16)] for s in p["segments"])
        if len(body) != extent:
            raise SystemExit(f"VU1 program {p['id']}: assembled {len(body):#x} bytes, manifest extent {extent:#x}")
        h = fnv1a64(body)
        if h != int(p["fnv1a64"], 16):
            raise SystemExit(f"VU1 program {p['id']}: hash {h:016x} != manifest {p['fnv1a64']} -- wrong ELF?")
        image = body + bytes(image_size - len(body))
        img = work / f"vumicro_{h:016x}.bin"
        img.write_bytes(image)
        args.append(f"{img.as_posix()}:{extent:x}")
    out = output or (runtime / "src" / "lib" / "vu1_jit_gen.inc")
    out.parent.mkdir(parents=True, exist_ok=True)
    gen = runtime / "tools" / "gen_vu1.py"
    subprocess.run([sys.executable, str(gen), str(out)] + args, check=True)
    print(f"== VU1 programs: {len(args)} bodies from {elf.name} -> {out}")
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", required=True, type=Path, help="SLUS_216.78 (USA)")
    ap.add_argument("--runtime", type=Path, default=ROOT / "ps2xRuntime")
    ap.add_argument("--work", type=Path, default=HERE / "work" / "vu1")
    ap.add_argument("--manifest", type=Path, default=HERE / "vu1_programs.json")
    ap.add_argument("--output", type=Path,
                    help="generated include path (defaults to ps2xRuntime/src/lib/vu1_jit_gen.inc)")
    a = ap.parse_args()
    generate(a.elf.resolve(), a.runtime.resolve(), a.work.resolve(), a.manifest.resolve(),
             a.output.resolve() if a.output else None)


if __name__ == "__main__":
    main()
