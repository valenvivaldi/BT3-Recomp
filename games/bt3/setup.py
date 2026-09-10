#!/usr/bin/env python3
"""Build (+ optional deploy) a registered Dragon Ball Z: Budokai Tenkaichi 3 variant.

    python3 games/bt3/setup.py <iso|elf> [--jobs N] [--deploy OUT] [--skip-setup]

Cross-platform (Linux primary; Windows and macOS experimental). The game's code is generated
locally from YOUR copy of the game — this repository ships no game code or assets.
Steps: extract/verify the game files, build the recompiler, generate the runner
sources, generate the overlay module, apply patches, build the runner.

  --deploy OUT   after a successful build, copy the playable tree (game data,
                 settings, assets, fonts, and the runner) into OUT. On Windows the
                 runtime DLLs are copied too. The Linux self-extracting launcher is
                 created by build_and_deploy.sh, which calls this script with
                 --deploy so the built runner ends up in place.
  --skip-setup   skip steps 1-6 (ISO/ELF extraction, recompile, patches). Rebuild the
                 runner from the already-generated sources and then deploy. Speeds up
                 re-deploys when nothing in the pipeline changed.
"""
import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
# Overridable so a container can generate into a build-local dir (PS2X_BUILD_DIR)
# instead of the host source tree. Defaults to <repo>/build.
BUILD = Path(os.environ.get("PS2X_BUILD_DIR") or str(ROOT / "build"))
WORK = HERE / "work"
VARIANTS_DIR = HERE / "variants"
DEFAULT_VARIANT = "SLUS_216.78"
IS_WINDOWS = os.name == "nt"
IS_MACOS = sys.platform == "darwin"
# Generated TUs are huge; high job counts can exhaust RAM (16 GB: keep <= 3).
DEFAULT_JOBS = "3"


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def load_variants() -> dict[str, dict]:
    """Read committed, content-free variant descriptors keyed by PS2 serial."""
    variants: dict[str, dict] = {}
    for path in sorted(VARIANTS_DIR.glob("*.json")):
        try:
            item = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            die(f"cannot read variant descriptor {path}: {exc}")
        serial = item.get("serial")
        if serial is None:
            # Per-variant auxiliary manifests (for example SLES_549.45.vu1.json)
            # live beside the selectable variant descriptors.
            continue
        if not isinstance(serial, str) or not serial:
            die(f"variant descriptor {path} has no serial")
        if serial in variants:
            die(f"duplicate variant descriptor for {serial}")
        variants[serial] = item
    if DEFAULT_VARIANT not in variants:
        die(f"missing canonical variant descriptor: {DEFAULT_VARIANT}")
    return variants


def variant_input(variant: dict, key: str, default: Path | None) -> Path | None:
    """Resolve a repo-relative input path recorded in a variant descriptor."""
    value = variant.get(key)
    return (ROOT / value) if value else default


def rebase_stubs(variant: dict, serial: str, elf: Path, function_map: Path,
                 work: Path, cfg_text: str) -> str:
    """Re-point config.toml.in's stub bindings from the base variant onto this one.

    [symxfer] The recompiler substitutes its own HLE for a stubbed function, and
    the binding is `name@0xADDRESS`. Those addresses are the canonical variant's,
    so reusing them for another serial does not merely miss: an address that
    happens to start a different function there silently replaces that function
    with an unrelated stub. Worse, the ones that DO miss leave the game's real
    library code in place -- and that code talks to an IOP which does not exist,
    which is why an un-rebased variant hangs in sceSifInitRpc polling for an IOP
    acknowledgement that never arrives.
    """
    base = variant.get("base_serial")
    if not base:
        print(f"== {serial} has no base_serial; leaving the stub list untouched")
        return cfg_text
    base_variant = load_variants().get(base)
    base_elf = WORK / base
    base_map = variant_input(base_variant or {}, "function_map", None)
    if not base_elf.is_file() or not base_map or not base_map.is_file():
        die(f"{serial}: rebasing the stub list needs the base variant {base}'s ELF "
            f"and function map ({base_elf}, {base_map}). Build {base} first, or "
            f"record a variant stub list of your own.")
    stubs_out = work / "stubs_rebased.txt"
    run([sys.executable, HERE / "transfer_symbols.py",
         "--from-elf", base_elf, "--from-map", base_map,
         "--to-elf", elf, "--to-map", function_map,
         "--output", work / "functions_named.csv",
         "--rebase-stubs", HERE / "config.toml.in", "--stubs-out", stubs_out])
    head, sep, rest = cfg_text.partition("stubs = [")
    if not sep:
        die("config.toml.in has no 'stubs = [' list to rebase")
    _, closer, tail = rest.partition("]")
    if not closer:
        die("config.toml.in's stub list is not closed")
    return head + "stubs = [\n" + stubs_out.read_text() + "]" + tail


def print_variants(variants: dict[str, dict]) -> None:
    for serial, item in variants.items():
        base = item.get("base_serial")
        suffix = f" (base: {base})" if base else ""
        print(f"{serial}: {item.get('title', 'unnamed')} — {item.get('build_status', 'unknown')}{suffix}")


def run(cmd, **kw) -> None:
    print("+ " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def find_extractor():
    # bsdtar reads ISO9660 directly; Windows 10+ ships it as tar.exe.
    for name in ("bsdtar", "tar"):
        exe = shutil.which(name)
        if exe:
            return ("tar", exe)
    for name in ("7z", "7za"):
        exe = shutil.which(name)
        if exe:
            return ("7z", exe)
    die("need bsdtar/tar (Windows 10+ ships tar.exe) or 7z to extract the ISO")


def make_writable(root: Path) -> None:
    # ISO9660 files extract read-only; the game opens some (e.g. BIN/DBZP.BIN) read-write.
    for p in root.rglob("*"):
        try:
            p.chmod(p.stat().st_mode | stat.S_IWRITE)
        except OSError:
            pass


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sync_tree(src: Path, dst: Path, exclude=()) -> None:
    """rsync -a --checksum --delete equivalent: copy changed files, remove extras."""
    dst.mkdir(parents=True, exist_ok=True)
    src_names = {p.name for p in src.iterdir() if p.is_file() and p.name not in exclude}
    for p in sorted(dst.iterdir()):
        if p.is_file() and p.name not in src_names:
            p.unlink()
    copied = 0
    for name in sorted(src_names):
        s, d = src / name, dst / name
        if not d.exists() or s.stat().st_size != d.stat().st_size or \
           sha256_of(s) != sha256_of(d):
            shutil.copyfile(s, d)
            copied += 1
    print(f"synced {src} -> {dst} ({copied} updated, {len(src_names)} total)")


def find_binary(name: str) -> Path:
    exe = name + (".exe" if IS_WINDOWS else "")
    hits = sorted(BUILD.rglob(exe))
    if not hits:
        die(f"{exe} not found under {BUILD} after build")
    return hits[0]


# Windows builds use the Clang toolset of the Visual Studio Build Tools ("C++ Clang Compiler for
# Windows" + "MSBuild support for LLVM (clang-cl) toolset" in the installer): the static VU1
# recompiler is computed-goto code and the runtime uses GCC/Clang builtins, which MSVC cannot
# compile. PS2X_SETUP_TOOLSET overrides (e.g. "v143" to try plain MSVC).
def cmake_configure_extra() -> list:
    """-T only makes sense for the Visual Studio generator; a Ninja build directory (clang-cl via
    -DCMAKE_CXX_COMPILER=clang-cl) must not get it, or the reconfigure fails."""
    extra = ["-DCMAKE_BUILD_TYPE=Release"]   # explicit: a Windows Ninja/clang-cl configure came up Debug (/Od /RTC1 -MDd)
    extra.append("-DPS2X_RUNNER_VARIANT=" + os.environ.get("PS2X_RUNNER_VARIANT", "default"))
    # ps2xStudio (the editor tool) fetches four git branches at configure time; a network hiccup there
    # aborted a user's whole game build (imgui_colortextedit populate failed, 2026-09-08). The game does
    # not need it: off unless PS2X_SETUP_STUDIO=1.
    extra.append("-DPS2X_BUILD_STUDIO=" + ("ON" if os.environ.get("PS2X_SETUP_STUDIO") == "1" else "OFF"))
    if IS_MACOS:
        extra += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
        # [pgs] paraLLEl-GS does not build on macOS: its Granite dependency's
        # sleep_until_nsecs() falls back to clock_nanosleep/TIMER_ABSTIME for
        # everything that is not _WIN32, and macOS has neither. CMake enables the
        # backend whenever the submodule checkout exists, and setup.py fetches
        # submodules, so without this a fresh macOS configure breaks the build.
        # PS2X_SETUP_PGS=1 opts back in (needs MoltenVK and a patched Granite).
        if os.environ.get("PS2X_SETUP_PGS") != "1":
            extra.append("-DPS2X_DISABLE_PGS=ON")
        if os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
            extra.append("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + os.environ["MACOSX_DEPLOYMENT_TARGET"])
        if not (BUILD / "CMakeCache.txt").exists() and shutil.which("ninja"):
            extra += ["-G", "Ninja"]
    if not IS_WINDOWS:
        return extra
    cache = BUILD / "CMakeCache.txt"
    if cache.exists():
        gen = ""
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_GENERATOR:"):
                gen = line.split("=", 1)[1]
                break
        if "Visual Studio" not in gen:
            return extra
    # Fresh configure. Prefer Ninja + clang-cl when this is a Visual Studio developer prompt (ninja and
    # clang-cl on PATH, VC environment loaded): Ninja compiles every file in parallel, whereas the default
    # Visual Studio generator hands the runner project's ~1000 unity units to MSBuild, which without
    # multi-processor compilation builds them one at a time (a 45-minute scratch build, 2026-09-08).
    # PS2X_SETUP_GENERATOR=vs forces the Visual Studio generator.
    dev_prompt = bool(os.environ.get("VCToolsInstallDir") or os.environ.get("INCLUDE"))
    if (os.environ.get("PS2X_SETUP_GENERATOR", "ninja").lower() != "vs" and dev_prompt
            and shutil.which("ninja") and shutil.which("clang-cl")):
        return extra + ["-G", "Ninja", "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    return extra + ["-T", os.environ.get("PS2X_SETUP_TOOLSET", "ClangCL")]


def configured() -> bool:
    """True when the build dir holds a COMPLETED configure: CMakeCache.txt alone is not enough --
    a configure that failed half-way (a FetchContent download error) leaves the cache behind with no
    project files, and `cmake --build` then dies with "MSB1009: Project file does not exist"."""
    if not (BUILD / "CMakeCache.txt").exists():
        return False
    desired_variant = os.environ.get("PS2X_RUNNER_VARIANT", "default")
    cache_text = (BUILD / "CMakeCache.txt").read_text(errors="replace")
    if not any(line.startswith("PS2X_RUNNER_VARIANT:") and line.endswith("=" + desired_variant)
               for line in cache_text.splitlines()):
        return False
    if IS_MACOS and os.environ.get("MACOSX_DEPLOYMENT_TARGET"):
        cache = (BUILD / "CMakeCache.txt").read_text(errors="replace")
        desired = os.environ["MACOSX_DEPLOYMENT_TARGET"]
        if not any(line.startswith("CMAKE_OSX_DEPLOYMENT_TARGET:") and line.endswith("=" + desired)
                   for line in cache.splitlines()):
            return False
    if IS_MACOS:
        # [pgs] A cache configured before the parallel-gs submodule was fetched
        # has PS2X_DISABLE_PGS=OFF and no PGS targets; reusing it silently keeps
        # the backend enabled on the next build and fails to compile Granite.
        desired_pgs = "OFF" if os.environ.get("PS2X_SETUP_PGS") == "1" else "ON"
        if not any(line.startswith("PS2X_DISABLE_PGS:") and line.endswith("=" + desired_pgs)
                   for line in cache_text.splitlines()):
            return False
    if any((BUILD / f).exists() for f in ("build.ninja", "Makefile", "ALL_BUILD.vcxproj")):
        return True
    return any(BUILD.glob("*.sln"))


def cmake_build(target: str, jobs: str) -> None:
    cmd = ["cmake", "--build", BUILD, "--target", target, "-j", jobs]
    if IS_WINDOWS:
        cmd += ["--config", "Release"]  # multi-config generators (Visual Studio)
    run(cmd)


def copytree_overlay(src: Path, dst: Path) -> None:
    """copy_tree-like that overwrites instead of failing on existing dirs."""
    if src.is_dir():
        dst.mkdir(parents=True, exist_ok=True)
        for child in src.iterdir():
            copytree_overlay(child, dst / child.name)
    elif src.is_file():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def deploy_tree(runner: Path, out: Path) -> None:
    """Assemble the portable playable tree in OUT.

    layout: OUT/data/ (game data extracted from the ISO), OUT/savedata/
    (bt3_settings.ini; existing user saves are preserved), OUT/assets/ (fonts).

    Windows additionally copies the runtime DLLs next to the runner. The Linux
    build_and_deploy.sh replaces `runner` with the self-extracting payload ELF.
    """
    print(f"== assembling deploy tree in {out}")
    work = HERE / "work"
    out.mkdir(parents=True, exist_ok=True)

    # game data: BIN/ DATA/ IRX/ SYSTEM.CNF + the boot ELF
    data_dst = out / "data"
    for name in ("BIN", "DATA", "IRX", "SYSTEM.CNF"):
        src = work / name
        if src.exists():
            copytree_overlay(src, data_dst / name)
    boot = data_dst / "SLUS_216.78"
    if work.joinpath("SLUS_216.78").exists():
        boot.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(work / "SLUS_216.78", boot)

    # settings: default bt3_settings.ini only if none deployed yet (user keeps their saves)
    save_dst = out / "savedata"
    save_dst.mkdir(parents=True, exist_ok=True)
    cfg_src = runner.parent / "bt3_settings.ini"
    cfg_dst = save_dst / "bt3_settings.ini"
    if cfg_src.exists() and not cfg_dst.exists():
        shutil.copy2(cfg_src, cfg_dst)
        print(f"  copied default settings -> {cfg_dst}")

    # fonts + overlay assets
    for a in ("assets",):
        src = runner.parent / a
        if src.exists():
            copytree_overlay(src, out / a)

    # runtime DLLs (Windows). The runner itself gets copied by build_and_deploy.sh
    # on Linux; here we place it next to the data so the tree is self-contained.
    if IS_WINDOWS:
        for p in runner.parent.glob("*.dll"):
            shutil.copy2(p, out / p.name)
    else:
        shutil.copy2(runner, out / runner.name)
    shutil.copymode(runner, out / runner.name)
    print(f"  runner -> {out / runner.name}")
    print(f"Deploy tree ready: {out}")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Build (and optionally deploy) Dragon Ball Z: Budokai Tenkaichi 3.")
    ap.add_argument("--variant", default=DEFAULT_VARIANT, metavar="SERIAL",
                    help=f"registered game serial to build (default {DEFAULT_VARIANT})")
    ap.add_argument("--list-variants", action="store_true",
                    help="list registered serials and their build status")
    ap.add_argument("src", nargs="?", metavar="<iso|elf>",
                    help="ISO or bare boot ELF for the selected variant (not needed with --skip-setup)")
    ap.add_argument("--jobs", default=DEFAULT_JOBS, metavar="N",
                    help=f"parallel jobs for the ps2EntryRunner build (default {DEFAULT_JOBS})")
    ap.add_argument("--deploy", metavar="OUT",
                    help="after the build, copy the playable tree into OUT")
    ap.add_argument("--skip-setup", action="store_true",
                    help="skip ISO/recompile/patches; only rebuild the runner (+deploy)")
    ap.add_argument("--gen-only", action="store_true",
                    help="stop after recompile/generation/patches (steps 1-6); skip building the runner")
    args = ap.parse_args()
    variants = load_variants()
    if args.list_variants:
        print_variants(variants)
        return
    variant = variants.get(args.variant)
    if not variant:
        die(f"unknown variant {args.variant}; use --list-variants")
    experimental = variant.get("build_status") != "playable"
    if experimental:
        # A registered-but-unported variant is a probe, not a release target. It
        # used to be refused outright, which also made the whole variant code
        # path unreachable -- and hid the fact that its generation steps still
        # need per-variant inputs. Building one is allowed, loudly and opt-in.
        needs = variant.get("required_inputs", [])
        detail = "".join(f"\n  - {n}" for n in needs)
        print(f"WARNING: {args.variant} is a work-in-progress port target. It still needs:{detail}")
        print(f"         USA {DEFAULT_VARIANT} remains the playable default.")
        if os.environ.get("PS2X_SETUP_EXPERIMENTAL") != "1":
            die(f"refusing to build {args.variant} without PS2X_SETUP_EXPERIMENTAL=1")
    if args.skip_setup and args.gen_only:
        die("--skip-setup and --gen-only are mutually exclusive")

    jobs = str(args.jobs)
    canonical = args.variant == DEFAULT_VARIANT
    os.environ["PS2X_RUNNER_VARIANT"] = "default" if canonical else args.variant
    rt = ROOT / "ps2xRuntime"
    if canonical:
        runner_src_dir = rt / "src" / "runner"
        overlay_src_dir = rt / "src" / "runner_overlay"
        variant_include_dir = rt / "include"
        vu_include = rt / "src" / "lib" / "vu1_jit_gen.inc"
    else:
        runner_src_dir = rt / "src" / "runner" / "variants" / args.variant
        overlay_src_dir = rt / "src" / "runner_overlay" / "variants" / args.variant
        variant_include_dir = rt / "include" / "variants" / args.variant
        vu_include = variant_include_dir / "vu1_jit_gen.inc"

    # The canonical target keeps the historical flat work/ layout (extracted disc
    # tree + generated output). A variant gets work/<SERIAL>/ so its disc tree,
    # its derived function map and its generated output cannot collide with USA's
    # -- work/<SERIAL> used to be the boot ELF *file*, which left nowhere for the
    # per-variant map splat_function_map.py is documented to write.
    work = WORK if canonical else WORK / args.variant

    # Per-variant recompiler inputs. These used to be hardcoded to the USA files,
    # so a variant build would have recompiled its own ELF against USA addresses.
    function_map = variant_input(variant, "function_map", HERE / "functions.csv" if canonical else None)
    vu1_manifest = variant_input(variant, "vu1_manifest", HERE / "vu1_programs.json" if canonical else None)
    overlay_map = variant_input(variant, "overlay_map", HERE / "dbzp_funcs.csv" if canonical else None)
    # The overlay is the game code (BIN/DBZP.BIN), so a missing variant map would
    # silently mean "generate this ELF against USA overlay addresses".
    for key, path, hint in (
        ("function_map", function_map, "splat_function_map.py against the variant's ELF"),
        ("vu1_manifest", vu1_manifest, "vu1_manifest.py against the variant's ELF"),
        ("overlay_map", overlay_map, "splat_function_map.py against the variant's BIN/DBZP.BIN"),
    ):
        if path is None:
            die(f"{args.variant}: no \"{key}\" recorded in variants/{args.variant}.json.\n"
                f"A variant must not reuse the USA inputs. Generate one with {hint}\n"
                f"and record its repo-relative path in the descriptor.")
        if not path.is_file():
            die(f"{args.variant}: \"{key}\" points at {path}, which does not exist.\n"
                f"Generate it with {hint}.")

    if args.skip_setup:
        if not (work / args.variant).is_file():
            die(f"--skip-setup requires an existing {work}/ (no {args.variant} found)")
        src = None
        elf = work / args.variant
    else:
        if not args.src:
            ap.print_usage(sys.stderr)
            die(f"missing the ISO or bare {args.variant} ELF path")
        src = Path(args.src).resolve()
        if not src.exists():
            die(f"{src} does not exist")
        work.mkdir(parents=True, exist_ok=True)
        elf = work / args.variant

    # 1. Obtain the game files. The runtime reads loose files (BIN/DBZP.BIN, IRX/,
    #    DATA/) from the directory the ELF lives in, so extract the WHOLE ISO tree.
    if not args.skip_setup and src.suffix.lower() == ".iso":
        kind, exe = find_extractor()
        print(f"== extracting ISO contents (~4 GB) with {exe}")
        if kind == "tar":
            run([exe, "-xf", src, "-C", work])
        else:
            run([exe, "x", "-y", f"-o{work}", src], stdout=subprocess.DEVNULL)
        if not elf.is_file():
            die(f"{args.variant} not found in ISO (is this the selected release?)")
        make_writable(work)
    elif not args.skip_setup:
        # An earlier ISO extraction leaves the whole tree read-only (ISO9660), so
        # copying a bare ELF over a previous run's copy fails on the open. Make
        # the tree writable first -- the game itself opens BIN/DBZP.BIN
        # read-write, so this is needed for anything the user dropped in too.
        if work.exists():
            make_writable(work)
        shutil.copyfile(src, elf)
        make_writable(work)
        print("NOTE: you passed a bare ELF. The game also needs the ISO's BIN/, IRX/")
        print(f"      and DATA/ directories next to it in {work}.")

    if args.skip_setup:
        print(f"--skip-setup: reusing existing {work}/ and generated sources")
    else:
        # 2. Verify the exact boot ELF for this registered variant.
        got = sha256_of(elf)
        expected_sha256 = variant.get("boot_elf_sha256")
        if not expected_sha256:
            die(f"{args.variant} has no registered boot ELF hash")
        if got != expected_sha256:
            print(f"ERROR: ELF sha256 mismatch.\n  expected: {expected_sha256}\n  got:      {got}")
            print(f"Set PS2X_SETUP_FORCE=1 to continue with an unverified {args.variant} ELF.")
            if os.environ.get("PS2X_SETUP_FORCE") != "1":
                sys.exit(1)

    # 2b. [vu1manifest] the static VU1 recompiler's input: the game's VU1 microprograms, cut out of the ELF by
    #     games/bt3/vu1_programs.json (offsets + hashes only) and translated by ps2xRuntime/tools/gen_vu1.py into
    #     ps2xRuntime/src/lib/vu1_jit_gen.inc (git-ignored, like the EE runner sources). Runs with --skip-setup
    #     too: it takes seconds, and a tree without the file still builds (VU1 interpreter only).
    print("== generating VU1 programs from the ELF")
    sys.path.insert(0, str(HERE))
    from vu1_programs import generate as generate_vu1
    generate_vu1(elf, rt, work / "vu1", manifest=vu1_manifest, output=vu_include)

    # 3. Configure + build the recompiler. Configure only once: the globs use
    #    CONFIGURE_DEPENDS, so later builds re-run cmake by themselves when the
    #    source set changes — and an unnecessary reconfigure rewrites the MSVC
    #    project files, which makes MSBuild rebuild everything from scratch.
    if not args.skip_setup:
        # [pgs] the paraLLEl-GS backend lives in a git submodule (ps2xRuntime/third_party/parallel-gs, with its own
        # Granite submodule); CMake builds it in only when the checkout is present, so fetch it here. Harmless when
        # the tree is not a git checkout or the submodule is already there. PS2X_SETUP_NO_SUBMODULES=1 skips it.
        if not os.environ.get("PS2X_SETUP_NO_SUBMODULES") and (ROOT / ".gitmodules").exists() and shutil.which("git"):
            try:
                run(["git", "-C", ROOT, "submodule", "update", "--init", "--recursive"])
            except Exception as e:   # noqa: BLE001
                print(f"== submodule fetch failed ({e}); building without the paraLLEl-GS backend")
        print("== building recompiler")
        if not configured():
            run(["cmake", "-S", ROOT, "-B", BUILD] + cmake_configure_extra())
        cmake_build("ps2_recomp", str(os.cpu_count() or 4))
        recomp = find_binary("ps2_recomp")

        # 4. Generate the runner sources. The function map first gets its oversized
        #    Ghidra-truncation rows deduplicated and split into compiler-friendly
        #    chunks (see split_functions.py) — without this, single generated
        #    functions reach ~100K lines and exhaust MSVC's heap.
        print("== generating runner sources")
        sys.path.insert(0, str(HERE))
        from split_functions import split_csv
        split = work / "functions_split.csv"
        split_csv(elf, function_map, split)
        out = work / "output"
        if out.exists():
            shutil.rmtree(out)
        cfg_text = (HERE / "config.toml.in").read_text()
        if not canonical:
            cfg_text = rebase_stubs(variant, args.variant, elf, function_map, work, cfg_text)
        cfg_text = (cfg_text.replace("@ELF@", elf.as_posix())
                            .replace("@CSV@", split.as_posix())
                            .replace("@OUT@", out.as_posix() + "/"))
        (work / "config.toml").write_text(cfg_text)
        run([recomp, work / "config.toml"])

        # 5. Post-generation patches + the overlay module from DBZP.BIN.
        #    apply_patches.py and apply_overlay_patches.py are anchored to USA
        #    generated output and exit loudly when an anchor is missing, so they
        #    only run for the canonical target.
        if args.variant == DEFAULT_VARIANT:
            run([sys.executable, HERE / "apply_patches.py", out])
        else:
            print(f"== skipping apply_patches.py ({args.variant} has no ported source patches)")
        print("== generating overlay sources from BIN/DBZP.BIN")
        overlay_cmd = [sys.executable, HERE / "gen_overlay.py",
                       "--recomp", recomp, "--dbzp", work / "BIN" / "DBZP.BIN",
                       "--work", work / "overlay", "--runtime", rt,
                       "--output-dir", overlay_src_dir, "--header-dir", variant_include_dir,
                       "--main-map", overlay_map]
        if args.variant == DEFAULT_VARIANT:
            run(overlay_cmd)
            run([sys.executable, HERE / "apply_overlay_patches.py", rt])
        else:
            # The re-entry-label and gap-stitch tables in gen_overlay.py are USA
            # addresses; --simple emits just the main map for the variant.
            run(overlay_cmd + ["--simple"])

        # 6. Install into the runtime tree.
        print("== installing runner sources")
        sync_tree(out, runner_src_dir,
                  exclude=("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"))
        for h in ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"):
            variant_include_dir.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(out / h, variant_include_dir / h)

    if args.gen_only:
        print("--gen-only: runner + overlay sources generated (skipping runner build)")
        return

    # 7. Build the game. CONFIGURE_DEPENDS re-globs on Makefile generators, but the
    #    Visual Studio generator does not reliably pick up a changed source SET within
    #    the same build invocation (fresh Windows builds linked without main/the
    #    function tables). Reconfigure explicitly when the runner/overlay file set
    #    changed since the last configure; content-only changes still skip it.
    rt = ROOT / "ps2xRuntime"
    cache = BUILD / "CMakeCache.txt"
    need_cfg = not configured()
    if not need_cfg:
        ct = cache.stat().st_mtime
        for d in (runner_src_dir, overlay_src_dir):
            if d.exists() and d.stat().st_mtime > ct:
                need_cfg = True
                break
    if need_cfg:
        run(["cmake", "-S", ROOT, "-B", BUILD] + cmake_configure_extra())
    print(f"== building ps2EntryRunner (-j{jobs}, this takes a while)")
    cmake_build("ps2EntryRunner", jobs)
    runner = find_binary("ps2EntryRunner")

    if args.deploy:
        deploy_tree(runner, OUT := Path(args.deploy).resolve())
        return

    env_line = ("set PS2X_CD_IMAGE=<path to your BT3 ISO>& " if IS_WINDOWS else
                'env PS2X_CD_IMAGE="<path to your BT3 ISO>" ')
    print(f"""
Done. Run with:

  cd {runner.parent}
  {env_line}\\
      {runner} {elf}
""" if not IS_WINDOWS else f"""
Done. Run with (cmd.exe):

  cd {runner.parent}
  set PS2X_CD_IMAGE=<path to your BT3 ISO>
  {runner} {elf}
""")


if __name__ == "__main__":
    main()
