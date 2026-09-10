# ROM workspace

Place each personal disc dump under its PlayStation 2 serial. This directory is
ignored by Git; neither an ISO nor extracted game files are ever committed.

```
games/bt3/roms/
├── SLUS_216.78/                 # USA retail — canonical/default
│   ├── SLUS_216.78              # boot ELF
│   ├── SLUS_216.78.iso          # optional original disc image
│   ├── BIN/
│   ├── DATA/
│   ├── IRX/
│   └── SYSTEM.CNF
├── SLES_549.45/                 # Europe/Australia retail
│   └── ...
└── SLUS_219.78/                 # B14 Rev2 fan-mod variant
    └── ...
```

`SLUS_216.78` remains the source profile for the current playable port. Other
serials must retain their own extracted tree and generated runner: their code
addresses, VU1 program map, overlay map and game-specific overrides cannot be
shared safely with USA.

The versioned descriptors in `../variants/` say which inputs are still needed
before a variant can be built. A ROM being present here does not make it
supported automatically.
