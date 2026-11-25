# Handoff: continuing Project Americas

You are picking up a C++ project that runs the Xbox Series X|S build of Grand Theft Auto VI
natively on Windows. The game's x86-64 code runs directly in our process: no CPU emulation,
no translation of its code. We supply the console libraries underneath it and a GPU layer.
The full design is [PLAN.md](PLAN.md); progress is its section 11. This file is what you need
to start working: rules, setup, state, lessons learned, next steps.

State as of 2026-11-24: **M0 and M1 are done, the M3 groundwork (the shader corpus) is done,
and M2's memory and XGameRuntime core steps are done.** The loader runs the real game
executable through its memory set-up and runtime initialization (task queues, user,
networking, GameInput). It stops at the next console service we do not provide yet: the
console DirectStorage factory. File access is still refused.

**Files and DirectStorage (2026-11-24, night):** console paths work (the title's folder
read-only, `S:\`/`T:\` in `userdata`, the rest refused; `runtime/files`, `hle/files.cpp`), and
the console DirectStorage (`runtime/storage/dstorage.cpp`) opens files and runs queued reads
(zlib, memory or physical-page destinations; BCPack and other transforms refused). The game
opens all its RPF archives (PLAN.md section 11, "M2.3"). It also asks for XPackage,
XPersistentLocalStorage and XDisplay HDR, not implemented yet (`gdk/catalog.cpp`).

**The D3D12.X device (2026-11-24, evening; PLAN.md section 11, "M2.4", has the slot table):**
`D3D12XboxCreateDevice` returns our device (`runtime/d3d12x`). The game creates through it:
* 1 direct queue and 4 console compute queues;
* 5 fences;
* command allocators and command lists;
* the device's DXGI view (adapter, output);
* 5 descriptor heaps, the big one in its own memory;
* null views.

**Resources, root signatures, pipelines (2026-11-24, late):** committed resources (slot 127)
take their memory through the game's `XMemAlloc` hooks with the graphics allocator id, so its
back-buffer budget check passes. The ray-tracing BVH builder (`XboxBvhBuilder.dll`) then
serializes its root signatures through `d3d12_xs!D3D12SerializeVersionedRootSignature` (ours)
and creates its 40 compute pipelines (slot 62). Physical pages now commit on first touch: the
game maps 6.1 GiB of them at once, which eager commit could not hold in a 16 GB PC.

**The frame loop (2026-11-24, late night):** with the console texture layouts (`xg_xs.dll`,
`d3d12x/layout.h`), resources placed at the title's own addresses (slot 133), pipelines
(slots 55, 57, 67, 86), the console's queue slot order (Signal at 16), fence value addresses
and frame pacing (slots 78-80), **the game runs its frame loop at its own 30 fps** for the
whole run: it records command lists and presents once a frame, with the null back end. No
crash in 120 s. **PresentX shows its back buffer in a window** (`runtime/present`: a window
thread and a swap chain of the host's D3D12); the title bar reads about "30 fps, frame N (no
GPU rendering yet)". The picture is black until the GPU back end exists. Closing the window
ends the run.

**Command lists (2026-11-25):** every command list call is recorded with its data, and the log
describes frames: the game sits on a boot screen of three full-screen draws. Most pipelines
come from GPU programs in memory (slot 57), all of them in the corpus; every shader the game
hands over is saved under `cache\shaders\runtime`.

**M3:** the RDNA 2 decoder is done and decodes all 26,084 console GPU programs of the corpus,
checked against LLVM (100 % on 2.8 M instructions). **Translator version 1 runs** (PLAN.md,
section 11, "M3.2"): compute programs become HLSL, one thread per console lane, and are checked
on the host GPU against the ISA. With images (a descriptor table the host keeps, SM 6.6 heaps)
**2,486 of 2,682 compute programs (92.7 %) translate**. Control flow is rebuilt as nested ifs
and loops (2,058 of them; the irreducible rest keep a dispatcher), so DXC compiles the largest
in 38 s and the median in 5 s.

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
7. **Downloads need no asking** (the user's standing OK, 2026-11-24): official sources only,
   everything under `C:\Project Americas\tools`, each tool recorded in PLAN.md's "Local tools".
   Still ask before installing anything system-wide, outside `C:\Project Americas` and the
   repository.
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
* `ctest` runs 5 entries: 52 unit test cases, the probe, the inspector, and the loader running
  the fixture title (41 checks) at its own base and relocated.
* `C:\Project Americas\build\release\test-root\logs\run.log` is the last fixture run's log:
  the relocated one.
* `am_tests.exe <fragment>` runs a subset.
* `/W4 /WX` is on: fix warnings, never silence them broadly.

### Run the game (safe, unattended)

```powershell
& "C:\Project Americas\build\release\bin\americas.exe" --memory-limit 8192 --timeout 120
```

* Without `--title`, it runs the executable `MicrosoftGame.Config` names.
* `--memory-limit` caps private commit (a job object) and, separately, the physical page pool's
  commit, which a job does not count. The game commits about 4.4 GiB at start-up (no RAM until
  touched), so 6144 is now too tight.
* `--console auto|series_s|series_x` overrides `americas.ini`'s `console`. Auto gives Series S
  on this 16 GB PC.
* Logs go to `C:\Project Americas\logs`:
  * `run.log`;
  * `report.json`: modules, stubs with call counts, CPU patches, emulation hits;
  * `crash.txt`: the faulting `module+offset`, registers, and an unwound stack;
  * `hang.txt` (when `--timeout` ends the run): every thread's name and stack. Name our own
    frames with `python -I scripts\symbolize.py "C:\Project Americas\build\release\bin\americas_rt.map" <report>`.
* `--unimplemented trap` makes stubs raise an exception naming the import, instead of
  returning 0.

## 3. Repository map

| Path | What |
|---|---|
| `src/core` | read-only file mapping, path containment, SHA-256, JSON writer, settings |
| `src/pe` | bounds-checked PE32+ parser: imports with IAT addresses, exports, TLS, CodeView, relocations, function table |
| `src/host` | CPU features vs the console's, memory and topology, GPUs and D3D12 features, target-GPU choice, DXC discovery, `dxc_compile` (in-process DXC) |
| `src/x86` | our x86-64 decoder (length, operands, flow, required feature) and the two-pass code census |
| `src/loader` | the import routing policy: ours / wrap / host / title / unresolved |
| `src/formats` | DEFLATE (raw and zlib) |
| `src/shader` | shader pack reader (records found by their framing), DXBC container parser, `console_program` (XDAT / XBCH / FSSM: the native programs and their records), `rdna2` (the decoder; `rdna2_tables.inl` generated from AMD's XML), `translate` (console program → HLSL), `image_bindings` (the T# / S# table) |
| `src/runtime` | the runtime DLL, below |
| `src/runtime/memory` | title memory: console flag translation, physical pages, the XMemAlloc default allocator |
| `src/runtime/gdk` | XGameRuntime: `runtime.*` (exports, QueryApiImpl, area objects, report), `catalog.cpp` (the areas not done yet, by slot), `task_queue*` (XTaskQueue / XAsync), `offline.cpp` (XUser, XSystem, XGame, XNetworking, features, events) |
| `src/runtime/input` | GameInput (no devices yet) |
| `src/launcher` | `americas.exe`: TLS slot 0 for the title, a TLS callback, nothing else |
| `src/tls_carrier` | `am_tls_1..4.dll`: one TLS slot each for title DLLs |
| `src/tools/*` | `am-probe`, `am-inspect`, `am-cpuscan`, `am-shaders` |
| `tests` | unit tests (own tiny runner, `tests/test.h`), the fixture title in `tests/fixtures/title` |
| `scripts` | `env.ps1` (developer shell), `setup.ps1` (hooks, ini), `check_assets.py` (asset policy); read-only analysis: `import_calls.py` (call sites of a DLL's imports), `vtable_calls.py` (slots called through an interface getter), `symbolize.py` (names our frames in hang and crash reports), `dxc_corpus.py` (compiles translated HLSL in parallel, with progress) |

`src/runtime` in more detail:

| File | Role |
|---|---|
| `process.*` | the title process: maps modules, binds imports by the policy, maps title DLLs on demand, initializes them in dependency order, forwards thread start/exit, runs the entry point, crash filter, `report.json` |
| `console.*` | the console profile (Series S / X): title memory budget, chosen from the host's RAM or the settings |
| `title_config.*` | what `MicrosoftGame.Config` says: executable, title id, identity, local storage size |
| `memory/console_flags.*` | the console's allocation, protection and XMem flag bits, and the host request each becomes (pure, unit-tested) |
| `memory/title_memory.*` | allocations with their console attributes; the physical page pool (one section); views over placeholders; release, query, commit cap |
| `memory/default_alloc.*` | `XMemAllocDefault`: size classes in 2 MiB arenas, large blocks on their own, all below 2^40 |
| `image.*` | maps a PE, relocations, export lookup, protection, `RtlAddFunctionTable` |
| `tls.*` | carrier slots: the title's template is copied over the carrier's, existing threads' blocks are filled, the title's TLS index is set |
| `stubs.*` | reporting stubs for unimplemented imports |
| `cpu_compat.*` | census cache (`cache\cpu`), same-length patches, the illegal-instruction emulator |
| `host_patch.*` | redirects `RtlPcToFileHeader` in host DLLs' import tables, so C++ exceptions work in title code |
| `hle/` | our implementations: `identity.cpp`, `system.cpp`, `network.cpp`, `xmem.cpp` (xmem.dll and the console `VirtualAlloc` family), `d3d12x.cpp` (`D3D12ConfigureVirtualMemoryX` so far); `registry.cpp` lists them |

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
  5. memory: `D3D12ConfigureVirtualMemoryX`, two 24 GiB reservations of 64 KiB pages, four
     2 MiB-page heaps (4.4 GiB) and a 20 MiB commit (PLAN.md, section 11, "M2.1");
  6. `GetFileAttributesW` is refused (files are not done);
  7. XGameRuntime: `InitializeApiImplEx2`, task queues, XUser, XNetworking (PLAN.md,
     section 11, "M2.2");
  8. GameInput: its gamepad device callback (no devices yet);
  9. files and DirectStorage: it opens all its RPF archives (M2.3);
  10. D3D12.X: device, queues, fences, command lists, DXGI view, descriptor heaps and views,
      back buffers (committed resources), the BVH builder's root signatures and compute
      pipelines, 6.1 GiB of physical pages mapped (M2.4);
  11. texture layouts (XG), placed resources, graphics pipelines, frame pacing;
  12. **its frame loop, at 30 fps, in a window**: command lists, `PresentX` once a frame.
* **Memory facts** (PLAN.md, section 11, "M2.1", has the full table):
  * XMem's reference pages on learn.microsoft.com are NDA-only: read the game's code instead;
  * allocation type bit 29 means 2 MiB pages, bits 29 + 22 mean 64 KiB pages;
  * protection bits `0x800`-`0x40000` are the GPU's access;
  * the low two XMem flag bits are 1 for CPU memory and 3 for GPU-mapped memory; bit 2 asks for
    an address below 4 GiB;
  * `XMemAlloc` goes to the executable's export `__XMemAllocationHooks__` (allocate, free);
  * physical page numbers must be multiples of 16 below 2^25;
  * title memory, and title images, stay in [4 GiB, 2^40).
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

### M2: from memory to a frame loop

Memory is done (PLAN.md, section 11, "M2.1"). The method that worked:
* find each console function's call sites in the executable: a small read-only Python scan for
  `call`/`jmp` to its import thunk;
* read the argument set-up with `dumpbin /disasm:nobytes /range:<va>,<va>`, output kept in the
  scratchpad;
* log the arguments at run time;
* rerun the game after each step and follow `report.json`'s called stubs.

XGameRuntime's core is done too (PLAN.md, section 11, "M2.2"). Its remaining areas
(`gdk/catalog.cpp`: XPackage, XGameSave, XStore, XPersistentLocalStorage, ...) get implemented
as the game reaches them; `report.json`'s `runtime` section and the log name each missing
method the game calls.

DirectStorage and files are done (PLAN.md section 11, "M2.3").

1. **Draw the boot screen (the next step).** Command lists now record every command
   (`d3d12x/commands.h`; slot table in PLAN.md 11, M2.4) and the log describes frames. The boot
   screen is a black clear and three full-screen triangles, the same every frame:
   * pipeline 1, a D3D12 stream: containers `7f26a694cd60...` (VS) and `3dc6e3523c81...` (PS),
     saved in `cache\shaders\runtime` (not in `shaders.rpf`);
   * pipeline 2, slot 55: shaders not found yet (its description is fixed-function state);
   * pipeline 3, slot 57: GPU programs from the corpus, markers in the frame log.
   To draw it: host resources for render targets and the textures it samples (PLAN.md 5.2,
   class A and B), descriptor translation for its two tables and root SRV, the VS/PS
   translation of those programs (the shader track), host PSOs, then running the recorded
   commands at `ExecuteCommandListsX` and presenting the host back buffer. Clears alone show
   nothing new: the only one is the back buffer to black.
   * Most game pipelines come from slot 57 (programs already in GPU memory); all 911 of their
     programs are in the corpus, found by the 24-byte marker. The translator's output should be
     looked up by that marker at run time.
   Still probes each frame:
   * device 70 (writes 32-byte descriptors into the big heap:
     probably the console's own view creation from a GPU address) and 81, queue 35 (twice a
     frame), and the command list's slots (9, 12, 20-25, 32-36, 44-52, 70-75, 126, 139-146).
     Each probe line names its call site now.
   * Texture layouts: our model pads mips to whole blocks and keeps texels linear. The
     console's real addressing (RDNA swizzle equations, mip tails) is needed once textures
     come from disk pre-tiled; the texture headers carry the mode the data uses.
2. **The rest of D3D12.X with the null back end**, slot by slot as the game reaches them.
   * `report.json`'s `graphics` section and the `d3d12x: #N ... not implemented` log lines
     name each one with its arguments; D3D12's slot order is the first hypothesis.
   * The command list's own fields: the game's code writes records into the list object.
     Watch for crashes at small offsets from a list pointer.
   * Then `PresentX`, frame pacing, a window.
3. **Threads and topology.** Console affinity masks mapped onto the host's processors;
   decide what topology the game sees (PLAN.md section 5.1).
4. **GameInput devices:** pads through XInput (or the host's GameInput), readings and device
   callbacks. The interface exists already, with no devices (`runtime/input`).

### M3: shader recompiler

Steps 1 and 2 are done (PLAN.md, section 11, "M3.1"): the console blocks are mapped, and the
RDNA 2 decoder decodes all 26,084 programs by control flow, checked against LLVM.
* `am-shaders survey` (3 s) and `am-shaders disasm <hash prefix> [--raw <dir>]` (a listing).
* `python -I scripts\rdna2_oracle.py <am-shaders.exe> <llvm-mc.exe> <pack dir> [<sample> [<seed>]]`
  checks the decoder against `C:\Project Americas\tools\llvm-amdgpu-23.1.6\bin\llvm-mc.exe`.
* Regenerate the tables with `python -I scripts\gen_rdna2_tables.py <rdna2.xml> <rdna1.xml>`.

3. **Translator.** Version 1 (a faithful wave, PLAN.md 5.6 step 3) runs for compute:
   * `am-shaders translate <hash prefix>` (HLSL and DXC for one program) and
     `am-shaders tsurvey [<n>]` (the whole corpus in 8 s; `cache\shaders\translation.json`).
   * GPU tests in `tests/translate_tests.cpp`: assemble with llvm-mc
     (`-triple=amdgcn -mcpu=gfx1030 -mattr=+wavefrontsize64 --show-encoding`), put the words
     in, check registers and memory.
   * Done since: typed buffers, SDWA/DPP, `v_fma_mix`, permutes, cube ops, atomics, images
     (`shader/image_bindings`: the host maps T# / S# words to heap indices; misses are queued).
   * `am-shaders images` lists every image form; `tsurvey <n> --raw <dir>` writes the HLSL
     of `n` translated programs instead of compiling them.
   * **Open job: compile every translated program once** (checks that all compile, and gives
     the corpus's DXC time; translator 2, about 3 to 4 hours on 4 cores). Started and stopped
     on 2026-11-25 so the user can watch it. To run it (resumable; results append to
     `C:\Project Americas\cache\shaders\dxil\results.tsv`):

     ```powershell
     & "C:\Project Americas\build\release\bin\am-shaders.exe" tsurvey 3000 --raw "C:\Project Americas\cache\shaders\hlsl\cs"
     python -I scripts\dxc_corpus.py "C:\Project Americas\cache\shaders\hlsl\cs" --out "C:\Project Americas\cache\shaders\dxil"
     ```

     When it is done: put the total time, the failures and the slowest programs in PLAN.md 11
     (M3.2, "Compile time"), which still says the run is under way. The run covers the 2,070
     programs translated then; rerunning both commands adds the 416 the LDS, D16 and
     16-bit work brought in (and recompiles those whose f16 code changed) (`--raw` leaves unchanged files alone, so the rest are skipped).
   * Structured control flow is done (`Structurer` in `translate.cpp`; the dispatcher stays
     for irreducible CFGs).
   * Next: the other stages (PS/VS/mesh I/O from the FSSM records), the indirect jumps, and
     the runtime binding model (M4). Then registers as SSA values (IR) for faster compiles
     and faster code.
   * Then the speed work (IR, structuring, uniformity) and `am-shaders precompile`.
   * It must be resumable, run below normal priority, and be content-addressed (console bytes
     + translator version).
   * Started as soon as it works for a share of the corpus, so the PC grinds in the
     background (PLAN.md section 7a).

### Also open

* **Make the 29 emulated CPU sites native.** INSERTQ (6–7 bytes) becomes a jump to a per-site
  stub. CLZERO (3 bytes) moves into a stub together with the instruction after it, taking
  care with branch targets. The exception handler stays only as the safety net for
  unconfirmed sites.
* **Refused stubs return 0, and 0 is success for some functions** (`S_OK`, a `NULL` handle that
  is not `INVALID_HANDLE_VALUE`, "attributes 0"). `QueryApiImpl`, `GameInputCreate` and
  `DStorageGetFactory` each stopped the game this way. A per-function failure value for refused
  wrappers would make runs more predictable.
* **Decisions the user still owns** (PLAN.md section 10):
  * graphics back end (D3D12 recommended);
  * default console profile (implemented as recommended: chosen from the host's RAM, with an
    override);
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
* **Placeholders** (`VirtualAlloc2`, `MapViewOfFile3`, `UnmapViewOfFile2`) are only in the
  umbrella import libraries (`onecore.lib`, `mincore.lib`), which must not be mixed with
  `kernel32.lib`. We resolve them from `kernelbase.dll` at run time. Their declarations need
  `NTDDI_VERSION` of Windows 10 1803 or later; the build sets 2004.
* **The executable's base can be taken.** A random allocation sometimes sat in
  `0x140000000`+176 MiB. The launcher now holds that window from its first instruction. When an
  image must move anyway, it goes into title memory, never above 2^40.
* **The game creates its own window** ("Americas", class `sgaWindowClass`, 3840x2160) through
  `CreateWindowExA`/`ShowWindow`. Our presenter's window shows the frames, so the title's is
  created but kept hidden (`hle/identity.cpp`); otherwise two windows open.
* **Thread start and exit run under the loader lock.** Code on that path (the launcher's TLS
  callback, `TitleProcess::on_thread`) must never wait on a lock of ours: a thread holding it
  may be loading a DLL. It deadlocked once.
* **The console GPU is RDNA 2 plus RDNA 1's MAD family** (`v_mac_f32`, `v_mad_f32`,
  `v_madak_f32`), and MIMG / MUBUF have opcode bits the spec's layouts leave out (bit 0, bit 25).
  LLVM's gfx1030 calls VOP2 31 `v_dot2c_f32_f16`: confirm `v_mac_f32` with the translator's
  first GPU tests.
* **Every console program starts with a 24-byte hash marker** (two VOP3 `v_nop`s with literals),
  and some carry data after `s_endpgm`: decode by control flow, never linearly.
* **llvm-mc as an oracle:** one instruction per line, a numbered `s_nop` with bit 15 set as the
  delimiter (the code's own `s_nop`s never set it).
* **Runtime thunks** are easy to list: calls of the GDK helper at `0x1455DE048`, with the
  class / interface GUIDs in `rcx` / `rdx` and the slot in the following `mov rax,[rax+slot*8]`.
* **Commit in `--memory-limit` runs.** A job's memory limit counts private commit only, not
  commit of page-file-backed sections. That is why the physical pool checks the cap itself.
* **Console graphics objects must never have C++ virtual functions.** A C++ vtable pointer takes
  offset 0, where the title reads the console vtable. It happened once: the game jumped into
  the text "AES-NI". `make_object` in `d3d12x/object.h` keeps our state in a header in front of
  the object.
* **`small` is a macro in the Windows headers** (`#define small char`); don't use it as a name.
* **Learn a console call's shape from its call site.** Slot 127 looked like
  `CreateCommittedResource1` in the probe's log (7 arguments shown) but takes ten, like
  `CreateCommittedResource3`: the extra ones are on the stack. Log `_ReturnAddress()` and a few
  stack arguments once, then read the call site with `llvm-objdump`.
* **Never commit memory the title only maps.** The game maps far more physical pages than it
  touches early; a fault handler commits each page on first touch (`memory/title_memory.cpp`).
  The kernel cannot fault pages in, so host calls that read or write title buffers in the
  kernel (`ReadFile`, `WriteFile`, later the GPU's heaps) commit them first (`commit_range`).
* **PowerShell after `. .\scripts\env.ps1`:** the script sets `$ErrorActionPreference = 'Stop'`
  for the session, so a native program writing to stderr ends the command. Run programs that
  log to stderr (the tests, `americas.exe`) through `cmd /c "... 2>nul"` or with redirects.
