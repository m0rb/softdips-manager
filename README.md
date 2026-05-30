# SoftDips Manager

![Screenshot](/screenshot.png?raw=true "Screenshot of SoftDipsManager")

A cross-platform tool for viewing, editing, and cloning settings
contained in **BackBit Platinum Cartridge** .softdips files.

It reads the soft DIP tables directly from a game's program ROM, lets you edit
every setting, and can **clone** (bulk-apply) settings across many titles at
once — matching equivalent settings even when games have named or encoded them
differently.

## Features

- Reads soft DIP tables straight from the P-ROM (`.p1`, `.pd`, `*-p1.bin`,
  etc.) — no guesswork.
- Full support for list, packed (`CREDIT/LEVEL`), count (lives/continues,
  `WITHOUT`/`1-99`/`INFINITE`), and time (`MM:SS`) settings.
- **Clone** settings across titles with semantic matching, confirm/skip prompts.
- **Audit** existing `.softdips` against their P-ROM and regenerate stale ones.
- **Reset to Defaults** from the P-ROM.

## Building

Requires CMake ≥ 3.16, a C++17 compiler, and **Qt 6** (or Qt 5).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Targets: `softdips_gui` (GUI), `softdips_cli` (CLI), `softdips_tests` (tests).

## License

This software is released under the **MIT License** — see [`LICENSE`](LICENSE).

It links the Qt framework under the **LGPLv3**; see [`NOTICE.md`](NOTICE.md) for
third-party license details.

## Disclaimer

**THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED. USE IT AT YOUR OWN RISK.** The authors are **not responsible for any
damage, data loss, or hardware issues** (including to flash cartridges, save
data, or ROMs) arising from its use. Always keep backups of your `.softdips`
files and ROMs. This project is not affiliated with or endorsed by SNK,
BackBit, or any rights holder; no copyrighted ROM or BIOS material is included.
