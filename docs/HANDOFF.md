# Handoff: continuing Project Americas

You are picking up a C++ project that runs the Xbox Series X|S build of Grand Theft Auto VI
natively on Windows. The game's x86-64 code runs directly in our process: no CPU emulation,
no translation of its code. We supply the console libraries underneath it and a GPU layer.
The full design is [PLAN.md](PLAN.md); progress is its section 11. This file is what you need
to start working: rules, setup, state, lessons learned, next steps.

State as of 2026-11-24: **M0 and M1 are done; the M3 groundwork (the shader corpus) is done.**
The loader runs the real game executable into its own initialization and stops at the first
console service we do not provide yet (memory).

## 1. Rules from the user (follow them exactly)

1. **Read your memory notes first** (`MEMORY.md` in your project memory). They hold standing
   rules not repeated here.
2. **The game folder is read-only:** `E:\Movies\GTA6.Xbox.v1.0.2.1`. Open its files for reading
   only. Never write, rename or delete anything there. Patches happen in memory, never on disk.
3. **Everything generated goes under `C:\Project Americas`:** build trees, caches, logs, tools,
   save data. Nothing from the game, and nothing derived from it, goes into the repository.
   The pre-commit hook (`scripts/check_assets.py`) refuses binaries and pasted data.
4. **Commit finished, tested work straight to `main`, without asking.** No feature branches.
   **Never push**; the user pushes.
5. **C++** (C++20, MSVC). Write our own implementations. Third-party code only when its licence
   fits and the user agreed to the download.
6. **A program for everyone's PC.** Detect hardware features at run time and take the fast path
   when one exists (Resizable BAR, 64-lane waves, large pages, the console's Zen 2
   instructions), with a working fallback when it does not. The user's PC is one test machine
   (PLAN.md section 4).
7. **Ask before downloading anything**, with file name, source and size. Ask before anything
   that changes the system outside `C:\Project Americas` and the repository.
8. **Native performance is the goal.** The user asked to be sure nothing is emulated. Be
   precise about the exceptions: see section 6 of this file.
9. **Network stays off** in game runs. The game's crash reporter would upload dumps.

## 2. Environment

* **The machine (Dev PC 1):**
  * Windows 11 Pro 26100;
  * Intel i7-7700K, 4 cores / 8 threads;
  * 16 GB RAM, page file 16 GB. Other programs often hold most of the commit.
  * AMD RX 9060 XT 16 GB (RDNA4): SM 6.8, waves 32–64, mesh shaders, DXR 1.1;
  * **no Resizable BAR**;
  * the CPU lacks SSE4a, SHA, CLWB, CLZERO, RDPID, RDPRU and MONITORX/MWAITX.

  `am-probe` prints all of this.
* **Toolchain:**
  * VS 2022 Community: MSVC 14.44, CMake 3.31, Ninja;
  * Windows SDK 10.0.26100, with DXC 1.9 in `Windows Kits\10\bin\10.0.26100.0\x64`;
  * Python 3.13 (`python -I`).
* **Repository:** `C:\Users\Admin\Documents\GitHub\Project Americas`, remote `origin` on
  GitHub. A stale local branch `m0-foundations` exists; leave it unless the user says
  otherwise.
* **Settings:** `C:\Project Americas\americas.ini` (`game_dir = E:\Movies\GTA6.Xbox.v1.0.2.1`).
  `%AMERICAS_ROOT%` overrides the root, and the tests use that to log elsewhere.

### Build and test

Each tool call runs a fresh PowerShell process, so enter the developer shell in the same
command:

```powershell
Set-Location "C:\Users\Admin\Documents\GitHub\Project Americas"; . .\scripts\env.ps1; cmake --preset release; cmake --build --preset release; ctest --preset release
```

* Binaries land in `C:\Project Americas\build\release\bin`.
* `ctest` runs 5 entries: 37 unit test cases, the probe, the inspector, and the loader running
  the fixture title at its own base and relocated.
* `am_tests.exe <fragment>` runs a subset.
* `/W4 /WX` is on: fix warnings, never silence them broadly.

### Run the game (safe, unattended)

```powershell
& "C:\Project Americas\build\release\bin\americas.exe" --memory-limit 6144 --timeout 120
```

* Without `--title`, it runs the executable `MicrosoftGame.Config` names.
* Logs go to `C:\Project Americas\logs`:
  * `run.log`;
  * `report.json`: modules, stubs with call counts, CPU patches, emulation hits;
  * `crash.txt`: the faulting `module+offset`, registers, and an unwound stack.
* `--unimplemented trap` makes stubs raise an exception naming the import, instead of
  returning 0.

## 3. Repository map

| Path | What |
|---|---|
| `src/core` | read-only file mapping, path containment, SHA-256, JSON writer, settings |
| `src/pe` | bounds-checked PE32+ parser: imports with IAT addresses, exports, TLS, CodeView, relocations, function table |
| `src/host` | CPU features vs the console's, memory and topology, GPUs and D3D12 features, target-GPU choice, DXC discovery |
| `src/x86` | our x86-64 decoder (length, operands, flow, required feature) and the two-pass code census |
| `src/loader` | the import routing policy: ours / wrap / host / title / unresolved |
| `src/formats` | DEFLATE (raw and zlib) |
| `src/shader` | shader pack reader (records found by their framing), DXBC container parser |
| `src/runtime` | the runtime DLL, below |
| `src/launcher` | `americas.exe`: TLS slot 0 for the title, a TLS callback, nothing else |
| `src/tls_carrier` | `am_tls_1..4.dll`: one TLS slot each for title DLLs |
| `src/tools/*` | `am-probe`, `am-inspect`, `am-cpuscan`, `am-shaders` |
| `tests` | unit tests (own tiny runner, `tests/test.h`), the fixture title in `tests/fixtures/title` |
| `scripts` | `env.ps1` (developer shell), `setup.ps1` (hooks, ini), `check_assets.py` (asset policy) |

`src/runtime` in more detail:

| File | Role |
|---|---|
| `process.*` | the title process: maps modules, binds imports by the policy, maps title DLLs on demand, initializes them in dependency order, forwards thread start/exit, runs the entry point, crash filter, `report.json` |
| `image.*` | maps a PE, relocations, export lookup, protection, `RtlAddFunctionTable` |
| `tls.*` | carrier slots: the title's template is copied over the carrier's, existing threads' blocks are filled, the title's TLS index is set |
| `stubs.*` | reporting stubs for unimplemented imports |
| `cpu_compat.*` | census cache (`cache\cpu`), same-length patches, the illegal-instruction emulator |
| `host_patch.*` | redirects `RtlPcToFileHeader` in host DLLs' import tables, so C++ exceptions work in title code |
| `hle/` | our implementations: `identity.cpp`, `system.cpp`, `network.cpp`; `registry.cpp` lists them |

## 4. What exists and what it showed

* **Import census** (`am-inspect`): the executable and 16 DLLs make 1,924 imports.
  * Routes: 46 ours, 177 wrap, 1,234 host, 467 title, 0 unresolved.
  * Every host function the game imports exists on Windows.
  * Console-only work, by module: `xmem` 13, `acphal` 9, `chad` 5, `xapu` 5, `d3d12_xs` 4,
    `xg_xs` 2, `xcrdapi` 1, `gameinput` 1.
* **CPU census** (`am-cpuscan`): 23.5 M instructions in the executable, 96 % confirmed.
  * It is heavy on AVX, FMA3, AVX2, BMI1/2, F16C, ADX, AES, PCLMULQDQ and MOVBE. Those are hard
    requirements: roughly Broadwell / Zen or newer.
  * The Zen 2-only instructions appear at only **41 sites**, all in the executable: INSERTQ
    (immediate form) ×19, CLZERO ×10 (counted zeroing loops), CLWB ×8, MONITORX/MWAITX ×4.
  * Checked against `dumpbin` on 1.06 M instructions.
* **Loader.** The fixture title passes all 21 checks through it. The real game:
  1. the executable maps at `0x140000000`, and 7 title DLLs are mapped and initialized;
  2. TLS slot 0 holds 5,248 bytes;
  3. 12 CPU sites are patched and 29 left to the emulator;
  4. the C runtime starts and the game's global initializers run;
  5. **it stops at `xmem!XMemVirtualAlloc` (stub returns 0), at the game's own `int3`,
     `game_scarlett_master.exe+0x1e1d01b`.** `d3d12_xs!D3D12ConfigureVirtualMemoryX` was
     called just before.
* **Shader corpus** (`am-shaders index`): in `C:\Project Americas\cache\shaders\pack`.
  * 26,503 records, 25,889 distinct (234 MB), plus `index.json`.
  * Kinds: 12,101 pixel, 9,282 mesh, 2,689 compute, 2,236 vertex, 195 geometry.
  * Every container has `SFI0 ISG1 OSG1 PSV0 HASH DXIL PRIV`. The `DXIL` part is the PC's
    program header (SM 6.6), then the console's `XDAT` block, then `XBCH`, then a dummy 4-byte
    bitcode.
  * `am-shaders dump <name>` shows the structure.
* **Local tools** in `C:\Project Americas\tools`: PLAN.md section 11, "Local tools". In short:
  * the AMD RDNA 2 XML spec (MIT): the decoder's source of truth;
  * the RDNA 2 PDF: reference only; AMD's terms forbid redistributing it;
  * LLVM 23.1.6 `llvm-mc` / `llvm-objdump`: **no AMD GPU target** in the Windows release;
  * zstd 1.5.7.

## 5. Next steps, in order

M2 and M3 go in parallel (PLAN.md, "Order of work"): M3's batch jobs need hours of machine
time, which the PC can spend while M2's code is written.

### M2: from the first memory call to a frame loop

1. **Memory (`xmem`, then the memory wrappers).**
   * Find what the game asks of `XMemVirtualAlloc`: the call site just before
     `+0x1e1d01b` (`dumpbin /disasm:nobytes /range:<va>,<va>` on the executable, output kept
     in the scratchpad), and the arguments at run time (log them in a first implementation).
   * Microsoft's public GDK documentation (learn.microsoft.com) describes the `XMem*` API
     contracts: use it for facts; do not copy headers.
   * Then implement them, as designed in PLAN.md section 5.2:
     * console allocation flags translated;
     * physical pages over page-file-backed sections with placeholder reservations
       (`VirtualAlloc2` + `MapViewOfFile3`);
     * `XMemAlloc` / `XMemFree` through the title's allocator hooks;
     * `D3D12ConfigureVirtualMemoryX`.
   * Replace the pass-through memory wrappers in `hle/system.cpp`. Rerun the game after each
     step and follow `report.json`'s called stubs.
2. **Files.** Console paths (the game root, `S:\` persistent storage, `T:\` temp) mapped to
   the read-only dump and `C:\Project Americas\userdata`. Deny by default, and refuse any
   write that resolves into the dump: check the final path of the opened handle too.
   * The functions are listed under "console paths" in `src/loader/import_policy.cpp`.
   * Until then they stay refused stubs, which is what keeps runs safe.
3. **Threads and topology.** Console affinity masks mapped onto the host's processors;
   decide what topology the game sees (PLAN.md section 5.1).
4. **XGameRuntime.** The game gets it through `QueryApiImpl` from `xgameruntime.dll`, which
   `LoadLibrary` returns as a pseudo handle. Offline user, package, system, task queue /
   async.
5. **D3D12.X device** with a null back end (commands accepted and dropped), `PresentX`, frame
   pacing, a window. GameInput.

### M3: shader recompiler

1. **Map the console blocks** (`XDAT`, `XBCH`, `PRIV`) to find the RDNA2 code, resource and
   user-data metadata, and per-stage I/O. Use `am-shaders dump` on small shaders first.
2. **RDNA2 decoder** with its tables generated from `amdgpu_isa_rdna2.xml`, at build time or
   committed with AMD's MIT notice.
   * It needs an oracle for tests; ask the user which of the candidates in PLAN.md
     ("Local tools") to download.
   * Then `am-shaders survey`: decode every program, histogram the instructions, report
     failures.
3. **Translator** (IR, structuring, EXEC removal, uniformity, resource tracing; HLSL → DXC),
   then `am-shaders precompile`.
   * It must be resumable, run below normal priority, and be content-addressed (console bytes
     + translator version).
   * Started as soon as it works for a share of the corpus, so the PC grinds in the
     background (PLAN.md section 7a).

### Also open

* **Make the 29 emulated CPU sites native.** INSERTQ (6–7 bytes) becomes a jump to a per-site
  stub. CLZERO (3 bytes) moves into a stub together with the instruction after it, taking
  care with branch targets. The exception handler stays only as the safety net for
  unconfirmed sites.
* **Decisions the user still owns** (PLAN.md section 10):
  * graphics back end (D3D12 recommended);
  * default console profile;
  * licence;
  * page file size;
  * a source for the BCPack tables (PLAN.md section 9).

## 6. Native versus emulated: say it precisely

* **CPU code:** native. The only exceptions are the 41 Zen 2-only sites, and only on CPUs that
  lack those instructions:
  * 12 are patched to equivalent native instructions;
  * 29 currently go through the illegal-instruction handler, a few µs per execution.

  Making those 29 native is open work (section 5). On a Zen 2 or newer CPU nothing is patched
  or emulated.
* **GPU code:** recompiled once to native PC GPU code (DXIL), cached. Not emulated.
* **Console-only hardware** must be done in software, because the PC has no such chip:
  * the audio processor (AcpHal / XApu / XDsp);
  * the BCPack texture decompressor (CPU or GPU compute).

## 7. Lessons learned (save yourself the time)

* **PowerShell 5.1:**
  * `2>&1` on a native program turns its stderr into red "errors": read the log files instead.
  * `Start-Process -NoNewWindow` once returned before the program ran; `cmd /c "... > out 2> err"`
    is reliable.
* **The Bash tool's working directory resets.** Python run inside a Bash heredoc interprets
  escapes such as `\b` in normal strings: use raw strings or the Edit tool for code.
* **MSVC:**
  * `ammintrin.h` must come after `immintrin.h`;
  * `dxcapi.h` needs `unknwn.h` / `oleauto.h` under `WIN32_LEAN_AND_MEAN`;
  * winsock's deprecated functions need `_WINSOCK_DEPRECATED_NO_WARNINGS` where we wrap them on
    purpose.
* **The launcher must never use `thread_local`:** its whole TLS block is overwritten with the
  title's template. Runtime code lives in `americas_rt.dll` and may use TLS freely.
* **C++ exceptions in manually mapped code** need `RtlPcToFileHeader` to answer for the image
  (`_CxxThrowException` uses it). Our wrapper does, and host DLLs' import tables are redirected
  to it.
* **Unimplemented wrapped functions are refused** (a stub returns 0), never passed to the host.
  This is what makes game runs safe; keep it.
* **The census cache** (`cache\cpu`) is keyed by build identity and `kCacheVersion` in
  `cpu_compat.cpp`: bump it when the decoder or the scan changes.
* **`dumpbin` as an x86 oracle:**
  * `/disasm:nobytes /range:a,b`;
  * it cuts the instruction that crosses the range end;
  * it cannot decode EVEX.
* **Downloads:**
  * Windows' `tar` cannot unpack LLVM's `.tar.zst` (zstd window too large): use
    `tools\zstd-1.5.7\zstd.exe -d --long=31 -c <file> | tar -xf - ...`.
  * GitHub's API redirects `facebook/zstd` to `meta/zstd` (`curl -L`).
* **`MicrosoftGame.Config`** has `Name=` inside other attribute names (`OverrideDisplayName`):
  match `" Name=\""` with the space.
