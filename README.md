# Project Americas

Runs the Xbox Series X|S build of Grand Theft Auto VI natively on Windows 11, from game files
the user supplies. The game's x86-64 code runs directly; this project supplies the console
libraries and GPU layer underneath it. See [docs/PLAN.md](docs/PLAN.md).

**Status:** milestone M0, foundations and analysis tools. Nothing runs the game yet.

## Rules

* The game folder is only ever read. Nothing from the game, and nothing derived from it, goes
  into this repository (`scripts/check_assets.py` runs on every commit).
* Everything generated lives under `C:\Project Americas` (or `%AMERICAS_ROOT%`): build trees,
  caches, logs, saves.

## Build

Requirements: Windows 11 x64, Visual Studio 2022 with "Desktop development with C++" (MSVC,
CMake, Ninja, Windows SDK 10.0.26100 with DXC).

```powershell
.\scripts\setup.ps1 -GameDir <unpacked game folder>   # once: git hooks, C:\Project Americas\americas.ini
. .\scripts\env.ps1                                   # each new shell: x64 developer environment
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Binaries land in `C:\Project Americas\build\release\bin`.

## Tools

| Tool | What it does |
|---|---|
| `am-probe [--json <file>]` | This PC's CPU instruction sets (against the console's), memory, GPUs and their D3D12 features, the GPU the runtime would pick, and the shader compiler. |
| `am-inspect [<exe\|dll\|folder>] [--imports] [--exports] [--no-hash] [--json <file>]` | Read-only census of the game's executable and DLLs: build identity, layout, TLS, and the route every import will take (ours, wrapped host, host, title, unresolved). Without a path it reads `game_dir` from `americas.ini`. |
