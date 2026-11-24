# Project Americas

Runs the Xbox Series X|S build of Grand Theft Auto VI natively on Windows 11, from game files
the user supplies. The game's x86-64 code runs directly; this project supplies the console
libraries and GPU layer underneath it. See [docs/PLAN.md](docs/PLAN.md).

**Status:** milestone M1 done; M2 under way, its memory step done. The loader runs the game's
executable through its memory set-up; it stops at the next console service not implemented yet
(XGameRuntime). See
[docs/PLAN.md](docs/PLAN.md), section 11. Continuing the work? Start with
[docs/HANDOFF.md](docs/HANDOFF.md).

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
| `am-cpuscan [<exe\|dll\|folder>] [--sites] [--json <file>]` | Read-only census of the CPU instruction sets the code uses, by confidence. `--sites` lists where it uses those this CPU lacks. `--boundaries <va>,<va>` prints a range's decoded instructions (a check against another disassembler). |
| `am-shaders index [<shaders.rpf>]` / `am-shaders dump <name>` | Extracts the game's shader corpus (every record inflated, de-duplicated) into `C:\Project Americas\cache\shaders\pack` with an index; `dump` shows one container's structure. The pack is only read. |
| `americas [--title <exe>] [--unimplemented zero\|trap] [--console auto\|series_s\|series_x] [--memory-limit <MiB>] [--timeout <s>] [-- <args>]` | Runs the title. Without `--title`, it runs the executable `MicrosoftGame.Config` names in `game_dir`. `--console` (or `console` in `americas.ini`) picks the console the game believes it runs on, which sets its memory budget. Logs, `report.json` and `crash.txt` go to `C:\Project Americas\logs`. |
