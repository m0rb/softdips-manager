# SoftDips Manager

![Screenshot](/screenshot.png?raw=true "Screenshot of SoftDipsManager")

A cross-platform tool for viewing, editing, and cloning settings
contained in **BackBit Platinum Cartridge** .softdips files.

It reads the soft DIP tables directly from a game's program ROM, lets you edit
every setting, and can **clone** (bulk-apply) settings across many titles at
once — matching equivalent settings even when games have named or encoded them
differently.

## Features

- Reads soft DIP tables straight from the P-ROM (`.p1`, `.pd`, `.ep1`,
  `*-p1.bin`, etc.) — no guesswork.
- Full support for list, packed (`CREDIT/LEVEL`), count (lives/continues,
  `WITHOUT`/`1-99`/`INFINITE`), and time (`MM:SS`) settings.
- **Clone** settings across titles with semantic matching, confirm/skip prompts.
- **Audit** existing `.softdips` against their P-ROM and regenerate stale ones.
- **Reset to Defaults** from the P-ROM.

## Web app

A browser version runs the same core compiled to WebAssembly — **everything is
client-side; no files are uploaded.** Hosted at:

**https://m0rb.github.io/softdips-manager/**

On a Chrome-based browser (Chrome / Edge / Brave) it uses the File System
Access API to edit `.softdips` **in place** — point it at your SD card or
cartridge `NeoGeo` folder and it provides full-collection management (edit, 
clone, generate, audit). Firefox, its derivatives, and other browsers 
without FS Access API can open a single file and download edited copies, 
but not write in place or browse folders.

### Building the web app

Requires the [Emscripten SDK](https://emscripten.org/) (`emcmake` on `PATH`):

```sh
emcmake cmake -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web        # emits web/softdips.js + web/softdips.wasm
python3 -m http.server -d web  # then open http://localhost:8000
```

GitHub Pages is built and deployed automatically from `main` by
[`.github/workflows/pages.yml`](.github/workflows/pages.yml).

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

## Credits

Thank you so much to evie at BackBit for an amazing product, being there for 
everyone, and all the hard work!

Thanks to HornHeaDD, NeoGeo81, and lithy on The BackBit Forum™ for all the 
inspiration that lead to this program.

## Disclaimer

**THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED. USE IT AT YOUR OWN RISK.** The authors are **not responsible for any
damage, data loss, or hardware issues** (including to flash cartridges, save
data, or ROMs) arising from its use. Always keep backups of your `.softdips`
files and ROMs. This project is not affiliated with or endorsed by SNK,
BackBit, or any rights holder; no copyrighted ROM or BIOS material is included.