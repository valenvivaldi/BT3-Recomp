# Deploy — self-extracting single-ELF runtime

On Linux, BT3-Recomp ships as a **self-extracting ELF**: one executable that contains the
compiled runner, the shared libraries it needs, and the game data layout, and
unpacks itself to a per-machine cache on first run. No installation step.

```
┌───────────────────────────  one executable  ───────────────────────────┐
│ [ static ELF stub ] [ zstd-compressed payload (tar) ] [ 32B footer ]   │
└─────────────────────────────────────────────────────────────────────────┘
                    │ first run                        │ later runs
                    ▼                                  ▼
        ~/.cache/bt3-recomp/<seed>/           cache hit → skip extraction
        ├── ps2EntryRunner
        └── lib/  (shared libraries)
```

The payload is `ps2EntryRunner` plus the shared-library closure it needs at
runtime. The footer stores the payload offset/size and a cache seed derived
from the payload hash, so every rebuilt binary gets its own cache slot and the
cache is invalidated automatically when the payload changes.

## One executable, two scripts

The platform packaging scripts share the Python build pipeline.

| Script | Platform | Role |
|---|---|---|
| `build_and_deploy.sh` | Linux | Interactive build + deploy. Asks for the ISO and output dir, runs the full `setup.py` pipeline, then assembles the self-extracting ELF and lays out the deploy tree. |
| `build_and_deploy_macos.sh` | macOS (experimental) | Native `.app`, dependency deployment and ad-hoc signing. |
| `games/bt3/setup.py` | Linux, Windows and macOS | The build pipeline: extract/verify ISO, build the recompiler, generate ~7,800 runner sources, apply patches, build `ps2EntryRunner`. Also has `--deploy` to assemble the playable tree. |

## Build + deploy (Linux)

```sh
./build_and_deploy.sh                          # prompts for ISO + output dir
./build_and_deploy.sh --iso /path/game.iso --output /path/deploy
./build_and_deploy.sh --skip-setup --output /path/deploy   # reuse existing work/, rebuild runner only
```

`--skip-setup` skips ISO extraction and source generation, rebuilding only the
runner from the already-generated sources (fast; needs ccache/sccache warm).
`--jobs N` (env `BT3_JOBS`) sets the runner build parallelism
(default: `nproc`).

`build_and_deploy.sh`:

1. calls `setup.py` with `--deploy OUT`,
2. compiles the static stub once (`gcc -static` + `libzstd.a`) if missing,
3. collects the runner's shared-library closure with `ldd` into the payload,
4. tars + zstd-compresses the payload,
5. appends the 32-byte footer,
6. writes the deploy tree under `OUT/` (see below).

## Build + deploy (Windows, experimental)

```sh
python games\bt3\setup.py C:\path\game.iso --deploy C:\path\deploy
```

The same pipeline runs; on Windows the runner and its DLLs are copied to the
output dir instead of a self-extracting ELF.

## macOS .app (experimental)

Use a Mac with Xcode Command Line Tools and `brew install cmake ninja pkg-config ffmpeg qt`.
The Linux self-extracting ELF script is not used on macOS.

```sh
./build_and_deploy_macos.sh --iso /path/game.iso --jobs 3
# Reuse the generated sources and rebuild into a new output path:
./build_and_deploy_macos.sh --skip-setup --output /path/BT3-Recomp-new.app
# Package existing runner + Launcher.app without rebuilding:
./build_and_deploy_macos.sh --skip-build --output /path/BT3-Recomp-test.app
```

Output defaults to `build/macos-dist/BT3-Recomp.app`. An existing destination is
rejected so a failed build cannot overwrite a working app. `PS2X_BUILD_DIR` selects
an alternate build directory. Build each CPU architecture separately; Universal 2
is not supported by the shared SIMD configuration.

The script stages the launcher and `bt3-runner` in `Contents/MacOS`, assets in
`Contents/Resources`, and uses `macdeployqt` to collect dylibs, Qt frameworks and
plugins (including Cocoa). It checks architectures, rejects external absolute
library paths, checks the minimum OS versions in Mach-O load commands, then signs
inside out with an ad-hoc identity and verifies the bundle. The destination appears
only after these steps succeed. No game files are embedded in the app.

`--deployment-target VERSION` (or `MACOSX_DEPLOYMENT_TARGET`) controls the declared
minimum macOS version. The default is the build Mac's version. Lowering this flag
cannot make Homebrew binaries built for a newer OS compatible: supply dependencies
built for the chosen floor. Developer ID signing, notarization and clean-machine
validation are separate release steps, not performed by this script.

At first launch, select the USA ISO in the install wizard. Mutable files live in
`~/Library/Application Support/BT3-Recomp/`:

- `data/`: verified ELF and files extracted from the disc.
- `savedata/`: memory cards, settings and per-player bindings.
- `textures/`, `mods/`, `logs/`: replacements, mods and diagnostics.

The launcher keeps reading fonts and other bundled assets from Resources. Gamepad
capture/testing in the launcher is unavailable outside Linux; use the in-game
settings overlay. The EE sampling profiler is unavailable on macOS; the phase
profiler (`PS2X_GUESTPROF=1`) uses the native monotonic counter. OpenGL uses the
existing fallbacks for unsupported persistent-buffer and texture-barrier extensions.

## `setup.py` flags

```
python3 games/bt3/setup.py <iso|elf> [--jobs N] [--deploy OUT] [--skip-setup]
```

| Flag | Effect |
|---|---|
| `--jobs N` | runner build parallelism (default 3 — generated TUs are RAM-hungry) |
| `--deploy OUT` | after a successful build, assemble the playable tree in `OUT` |
| `--skip-setup` | reuse `games/bt3/work/` + generated sources; rebuild runner only |

## Deploy tree

```
OUT/
├── Dragon Ball - Budokai Tenkaichi 3   # self-extracting ELF (Linux)
├── ps2EntryRunner                      # plain runner (Windows / before stub swap)
├── data/
│   ├── SLUS_216.78                     # boot ELF (also the CD image's name)
│   ├── BIN/  DATA/  IRX/  SYSTEM.CNF   # game data, extracted from the ISO
├── savedata/
│   ├── bt3_settings.ini                # user settings ([logging], [video], …)
│   └── BASLUS-21678DBZT3/              # memory-card save slot 0
├── assets/fonts/                       # settings-overlay fonts
└── logs/
    └── bt3.log                         # stderr diagnostic log (one per run)
```

Existing `savedata/` is never overwritten — a re-deploy keeps your saves and
settings. `data/` is refreshed from `games/bt3/work/`.

## Settings: `[logging] log_level`

Diagnostics go to `logs/bt3.log` next to the game (fast rotation: the previous
run is kept as `bt3.prev.log`). The verbosity is set in `savedata/bt3_settings.ini`:

```ini
[logging]
log_level=1
```

| level | enabled diagnostics |
|---|---|
| 0 | off (no log file) |
| 1 | `PS2X_PROFILE` (guest branch rate / hot PCs), `PS2X_MCLOG` (memory-card), `PS2X_SCHED_DEBUG` (thread scheduling) |
| 2 | + `PS2X_FTSPIKE` (frame-time spikes), `PS2X_FIGHTPROBE` (fight-state), `PS2X_REVEAL_HIDDEN_MENU_ENTRY` |
| 3 | + `PS2X_FRAMEPROF` (per-frame stats), `PS2X_CAMPROBE` |

`log_level` only *defaults* the matching `PS2X_*` environment variables
(`setenv(..., 0)`): an explicitly exported `PS2X_*` always wins.

## Checksums & reproducibility

The build pipeline does not ship game code. `setup.py` extracts the game from
your own USA ISO and verifies the boot ELF against a sha256 that is pinned in
the script; a different dump or region aborts the build. Generated sources are
never committed.

## Notes / troubleshooting

- The stub is built once and cached in `BT3_DEPLOY_SRC` (default
  `/tmp/opencode/bt3-deploy`): `stub.c` and the `zstd-1.5.7` tree used for
  `libzstd.a`. Recompile it there with
  `gcc -static -O2 -o stub stub.c -I zstd-1.5.7/lib -L zstd-1.5.7/lib -lzstd -lpthread -lm`.
- On first run the executable unpacks to the cache, so the first launch is
  slower than subsequent ones.
- If a run stops dead with a one-line `bt3.log` saying
  `Authorization required, but no authorization protocol specified`, that is an
  X11 auth failure of the launching shell, not a build problem.
