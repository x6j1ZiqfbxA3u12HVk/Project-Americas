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
2. **The game folder is read-only:** `C:\GTA6.Xbox.v1.0.2.1` (the user's copy on the fast drive; the original
   `E:\Movies\GTA6.Xbox.v1.0.2.1` stays read-only too). Open its files for reading
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
* **Settings:** `C:\Project Americas\americas.ini` (`game_dir = C:\GTA6.Xbox.v1.0.2.1`, a copy on the fast drive since 2026-11-25).
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
& "C:\Project Americas\build\release\bin\americas.exe" --timeout 120
```

* Without `--title`, it runs the executable `MicrosoftGame.Config` names.
* `--memory-limit` caps private commit (a job object) and, separately, the physical page pool's
  commit, which a job does not count. The game commits about 4.4 GiB at start-up (no RAM until
  touched), and once its runtime queries are answered it maps a 6.1 GiB physical page pool: 8192
  is now too tight (the mapping fails, error 1450) and so is 12288: run without the limit.
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
   Done: the host back end without replay (PLAN.md 11, M2.4, "The host back end"): console
   commands go straight to host command lists; render targets and depth live on the GPU; the
   window shows the host back buffer GPU to GPU. **The user's rule: nothing is translated
   while the game runs.** Console calls map to host calls one for one; shaders are compiled
   ahead of time and looked up. The vertex and pixel shaders of two of its three pipelines
   translate now (PLAN.md 11, "M3.3"; `am-shaders translate <path>` takes a saved container).
   The runtime binding's memory part works (PLAN.md 11, "M4.1"): translated programs read and
   write title memory in place through a GPU page table. Still to do for the boot screen: the
   user data layout (root arguments to SGPRs), images and samplers in the runtime binding, the
   textures it samples (class B copies of physical pages), the slot 55 pipeline's shaders, host
   PSOs from precompiled DXIL looked up by program marker, then forwarding draws.
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
   * **Done (2026-11-25): every translated program compiled once** with
     `scripts\dxc_corpus.py` (translator 2, the 2,070 of that day): all compile, none fail,
     2 h 22 min on 4 cores. To compile what translates now (it skips unchanged files):

     ```powershell
     & "C:\Project Americas\build\release\bin\am-shaders.exe" tsurvey 3000 --raw "C:\Project Americas\cache\shaders\hlsl\cs"
     python -I scripts\dxc_corpus.py "C:\Project Americas\cache\shaders\hlsl\cs" --out "C:\Project Americas\cache\shaders\dxil"
     ```
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

## Images in the runtime binding (done, 2026-11-25)

Translated shaders read textures and samplers through the binding table the runtime keeps
(translator 4):

* `shader/translate.cpp`: the runtime prelude is `am_user[5]` (20 user SGPRs) at b0 plus
  `am_bindings` at u0.
* `d3d12x/gpu_memory`: 20 user constants; the binding table is root UAV u0 (compute: param 1;
  graphics: VS b0 = 0, PS b0 = 1, table = 2). `allocate_view_slot` hands out shader heap
  indices for texture views from the top (65535 down) while windows grow from 1.
* `d3d12x/bindings`: the table as a GPU buffer, a 2048-entry sampler heap (S# misses become
  host samplers), host textures from texture views (uploaded from title memory by layout.h's
  linear layout: **no detiling yet**), dirty marks, the misses' readback after the fence.
* `list.cpp`: the first draw of a list flushes the page table and the bindings, sets both
  heaps, the root signature and the table; `submit_lists` reports the fence of lists that
  carry the flush.
* `views.cpp`: texture SRVs write a real T# (`bind_texture_view`); buffers as before.
* Writes the textures see: plain-address DirectStorage reads and XG `CopyIntoSubresource`.
  DirectStorage reads into physical pages are not reported yet (their virtual addresses are
  not known there).
* Precompiled shaders live in `cache\shaders\dxil\rt<translator version>` (both
  `am-shaders precompile` and `host_pipeline.cpp`). For translator 4: 947 compiled in 6 min,
  18 refused for instructions not translated yet (`global_atomic_fmax/fmin`, depth export
  `exp` target 8, `buffer_load_ushort`, `ds_write_b64`, `ds_bpermute_b32`,
  `v_mad_u64_u32`, `v_lshrrev_b64`; `rt4\failures.tsv`).
* **The game run (120 s):** the frame loop runs to the timeout, no crash; 16 host textures
  made from the game's views (a 256x128 and 1x1 ones), no T# reported missing; the stream draw
  (slot 86) runs every frame. The slot 57 and slot 55 draws still wait: host pipelines exist
  only for stream pipelines so far, so the textured UI draw cannot be checked yet.

## The console's own pipelines (slot 57) on the host (done, 2026-11-25)

The boot screen's last draw (slot 57, the UI composite: a full-screen textured rectangle into
the back buffer) now runs on the GPU: 2 of the frame's 3 draws, no debug-layer error.

* **Slot 57's blocks** (`pipeline.cpp`, saved as `cache\shaders\runtime\*.cpipe`): block 0 is
  register state, block 1 the input layout, then each program after its own metadata.
  * Block 0 at 0xD0: CB_BLEND0..7_CONTROL (checked on 8,000 pipelines: blend off in 56 %, then
    alpha, multiply, additive, premultiplied). Mapped to D3D12 blend states.
  * Block 1: at 0x4C the element count, then per element a name length, the name, and the six
    fields of a D3D12_INPUT_ELEMENT_DESC.
  * Programs: their DXIL by marker (rt4); the DXIL program header says vertex or pixel.
  * Host pipelines are made at the first draw, per set of target and depth formats (key in
    `ConsolePipeline::made`). Not read yet: rasterizer (no culling), depth state (off).
* **Topology 17 is the console's rect list** (three corners; the UI draws 3 vertices). The host
  draws triangles and a generated geometry shader adds the fourth corner opposite the
  right-angle vertex (from the vertex shader's OSG1 signature).
* **Vertex and index buffers** bind title memory in place (`host_gpu_address`: a window of its
  own allocation, or pages in a row in the committed pool).
* **Render targets as textures:** slot 18 `CreateShaderResourceView` now writes a T#; for render
  and depth targets the table entry views the GPU's own host texture (no copy).
* **Multisampled targets are single-sampled on the host** for now (shaders sample them as 2D).
* **Depth format:** the bound depth view's format is the pipeline's (D3D12 requires the match).
* **Textures from title memory are checked against it** every 15th flush (a checksum, 16 MiB a
  time) and uploaded again when they changed: CPU writes and DirectStorage page writes too.
* **Diagnostics** (environment variables):
  * `AMERICAS_CAPTURE=300,900`: those frames saved as `logs\frame-<n>.bmp`;
  * `AMERICAS_D3D12_DEBUG=1`: D3D12's debug layer, its warnings and errors in `run.log`;
  * `AMERICAS_DEBUG_PS=1`: slot 57 pixel shaders replaced by solid magenta, clears dark blue
    (`AMERICAS_DEBUG_NOCLEAR` skips clears); `AMERICAS_DEBUG_VS=1` (and `_VS_ID`): a simple
    vertex shader. These split "nothing drawn" into its causes quickly.
  * `am-shaders precompile --raw <dir>` writes the HLSL of every program it would compile.
* **Slot 55 derives a pipeline** from another of ours (its first argument) with some state
  replaced: (base, count, descriptions, riid, out), each description a type and the state; the
  game uses type 2, a D3D12_BLEND_DESC (blend variants of slot 57 pipelines).
  `create_derived_pipeline` copies the base and applies the blend.
* **All three boot-screen draws now run on the GPU** (no debug-layer error), and **the black
  screen is what the game draws**, checked from its own data:
  * the stream draw (slot 86) fills the intermediate target with a full-screen triangle whose
    vertex colour (in its vertex data block) is opaque black;
  * the slot 55 draw is a colour overlay (a fade) whose colour is (0, 0, 0, 0);
  * the slot 57 draw copies the intermediate to the back buffer.
  So the GPU path works end to end; the boot sequence itself does not move on.

After that, from the plan:

* **Runtime areas answered (2026-11-25):** XPackage (a packaged process, fully installed,
  every chunk ready, no other packages, nothing to download), XPersistentLocalStorage (`S:\`,
  4 GiB free), XDisplay HDR (off): `gdk/offline.cpp`. Every area the game asks for is now
  offered but XSpeechSynthesizer. With them **the game maps its 6.1 GiB physical page pool**
  (about 7 GiB of physical pages in all), which `--memory-limit 8192` cannot hold: the mapping
  fails with error 1450 and the game stops on its own `int 3` (game+0x1e6c29f, or +0x19f5291
  when the allocation itself is refused). Even 12288 is too little, since the cap counts the
  pool and private memory together: **run without `--memory-limit`** (or 16384+) from now on.
  * The physical pool's GPU part (committed, 1 GiB) now comes on top of the console budget
    (`title_memory.cpp`), so the 6.1 GiB allocation fits however the small batches land.
  * **Open: system commit leaked across runs.** After about 40 game runs on 2026-11-25 the
    system had 58.4 GiB committed with no process holding it (free RAM 10 GiB), and the
    committed GPU pool could not be created (error 1455). Suspect: host heaps opened over title
    memory (`OpenExistingHeapFromAddress` / `FromFileMapping`) or driver allocations not
    released at exit. Check `\Memory\Committed Bytes` before and after a run after a reboot.
    A run without the committed GPU pool (it could not be made) and without GPU windows did
    not grow the commit (58.42 -> 58.37 GiB): the leak likely comes with the pool's opened
    heap or the windows.
  * It still sits on the black boot screen, in the game's "System Init" state (its own crash
    report, `userdata\temp\*.crash.log`, names the state; it also says "Anaconda", Series X).
* **Leads checked on 2026-11-25 (all negative):**
  * The Series X profile (`--console series_x`, 13.5 GiB pool): the same black boot screen.
  * DirectStorage: 4,226 reads, 275 MB, no failure.
  * game+0x314259 is a `ReadFile` loop that sleeps 1 ms and retries on any error but 6, 38
    and 87; a thread was seen sleeping there. Our `ReadFile` wrapper (`hle/xmem.cpp`) now logs
    its first failures: none in a 30 s run, so that loop is not stuck.
  * No stub is called, and every runtime area the game asks for is offered but
    XSpeechSynthesizer (queried once at start; probably harmless).
* **Next: why the boot sequence does not move on.** The frame loop runs, but the game keeps
  drawing its black boot screen (fill, transparent fade, composite). Find what it waits for:
  `hang.txt` (every thread's stack at the timeout, named with `scripts\symbolize.py`),
  `report.json` (the stubs called and how often), and the services it asks for that we refuse
  (XPackage, XPersistentLocalStorage, XDisplay HDR in `gdk/catalog.cpp`; audio). Capture frames
  (`AMERICAS_CAPTURE`) once it moves.
* Slot 57: the rasterizer and depth registers in block 0. Slot 55 with other description
  types (logged when they appear) and counts above 1.
* VB/IB views from console addresses.
* Slot 55's shaders.
* GFX10 detiling for textures.
* Depth export (exp target 8).
* The missing CS ops.
* Record pypdf (tools/pylib) in PLAN.md "Local tools".

## Past the boot screen (2026-11-25, afternoon)

The boot stall is solved. The game now leaves "System Init" for "Game Init", sets up audio,
loads its texture databases (`data\ttdb\*.ttpf`) and draws its first loading frames: a
dispatch and 18 indexed draws a frame, through a new slot 67 pipeline.

* **The cause: ReadFile was never wrapped.** `hle/xmem.cpp` has wrappers that commit the
  title's pages before the kernel writes them, but `loader/import_policy.cpp` did not list
  ReadFile, ReadFileEx, WriteFile or WriteFileEx. A read into a page that is committed on
  first touch failed inside the kernel, and the game retried it forever (game+0x314259:
  Sleep(1) on any error but 6, 38 and 87). The earlier "no ReadFile failures logged" lead was
  wrong for the same reason: the logger was never bound. **Lesson: `am-inspect` shows each
  import's route; check it shows `wrap` before trusting a wrapper's silence.**
* **Command list slots 70-75** (`commands.h`): depth bounds; bottom-of-pipe memory value writes
  (71: 32 bits, 72: 64 bits; address, value, flush, flags; flags 1/2 write the GPU/system
  timestamp, QPC here); memory value waits (73/74: address, comparison, reference). Writes
  land in order from a thread of ours after the host fence that follows their lists. Waits
  hold the submission for up to 2 s. Slots 75, 126, 131, 132 and 142 are taken quietly.
* **Audio** (`hle/audio.cpp`, `hle/audio_streams.cpp`):
  * AcpHal: the APU heap (32-bit device addresses), the SHAPE contexts (350 XMA), and the
    coprocessor (commands acknowledged at once).
  * xapu (Opus) and chad (DSP): streams and results, with silent output.
  * Still missing: the XMA decoder, an Opus decoder and the DSP transforms.
* **Device slot 81**, GetFrameStatisticsX: count 0.
* **Two GPU use-after-free bugs fixed** (each showed as a driver timeout and the AMD bug
  report box). Bindings uploads and page-table uploads are now freed only after the fence of
  the list that copies from them (per list: another list may be submitted first).
* **`AMERICAS_D3D12_DRED=1`** logs DRED breadcrumbs (draws marked with their pipeline) and
  page faults with allocation names when the device is lost.

**Solved: the system commit leak, and with it the device loss during loading.**
`OpenExistingHeapFromFileMapping` loses the section's whole commit charge for good on this PC's
AMD driver. A standalone test showed it: 256 MiB lost per open, even with heap, view and section
released before exit. `OpenExistingHeapFromAddress` on VirtualAlloc memory loses nothing. The
1 GiB physical-pool window was opened that way, so every game run leaked 1 GiB until the system
ran out of commit. Then buffer creation failed and the device was removed (0x887a0001), about
40 s into loading. The pool window is now off unless `AMERICAS_GPU_POOL_WINDOW=1`, and a run
leaks nothing (checked). Without it, shaders read zeros from pool pages. **A leak-free way for
the GPU to reach pool pages is open.** Ideas: per-view copies into GPU buffers, or pool pages
from VirtualAlloc memory with remapping done another way. **After runs with the window on,
reboot** to get the commit back.

**Queue slot 31, CopyPageMappingsBatchX(batch count, batches, pool, pool pages, flags)**
(`queue.cpp`, `TitleMemory::copy_mappings`): each batch maps pool pages at its destination,
range by range. The kinds are 0 (pages in a row), 1 (one page repeated), 2 (unmap) and 3
(leave). It is done at once on the CPU.

**Earlier notes on the device loss** (now explained by the leak above) (frame ~1200-3000), reason 0x887a0001
(invalid call). There is no debug-layer error and no page fault. It comes right after the
game's `XMemVirtualAlloc(3.4 GiB, type 0x20402000, flags 0x23)` at 0xfd8a00000.

Leads, in order:
1. Queue slot 31, the page mapping batch (`(count?, ranges, ...)`, called from
   game+0xf2cc86), is not implemented. The game maps physical pages into that 3.4 GiB
   reservation through it.
2. Memory under an opened GPU window may be changed (decommitted, remapped, re-protected)
   while the GPU maps it.
3. The opened physical pool (`OpenExistingHeapFromFileMapping`) may conflict with new views
   of the section.

The captured frames (300, 600, 900) are still black. The loading draws are the next thing to
look at once the device survives.

## To the start screen and beyond (2026-11-25, evening)

**State:** the game shows its own screens. The sequence is the start screen ("Account picker /
Screen narration / Player"), A signs in the local user, then the first-time setup (subtitles)
and the Legal Notices screen, in the "Game Running" state. Frames are captured with
`AMERICAS_CAPTURE` and presses scripted with `AMERICAS_PRESS="A:45,A:53,..."` (stand-in pad).

What got it there, in order:

* **Stencil views:** a stencil SRV of an R32G8X24 depth target must use plane 1. Plane 0
  removed the device (0x887a0001) during loading. `host_view_desc` in `bindings.cpp` now
  checks every host texture view (depth twins, stencil plane, shader-readable formats).
  Lesson: the debug layer's log budget was used up by a per-frame warning, so warnings are
  now logged 5 times per id and errors have their own budget.
* **Pool window without the leak:** a mirror view of the pool section, opened by address
  (`open_pool`). It leaks nothing (checked) and puts vertex and index buffers in pool pages
  back in reach. `AMERICAS_NO_GPU_POOL_WINDOW=1` turns it off.
* **GameInput devices** (`input/game_input.cpp`): XInput pads, a keyboard stand-in pad (keys
  listed in the file) and the keyboard itself. Readings are kept as a per-device history;
  device callbacks run with enumeration.
* **XG slot 2, GetResourceLayout** (`xg.cpp`): it was a probe that left the game's layout
  struct as stack garbage. Video planes pointed anywhere, and Bink2Scarlett crashed writing
  to them in about half the runs after the start screen. The struct is 0x1b40 bytes: 4
  planes of 15 mips; offsets in `xg.cpp`.
* **Queue slot 31 maps pages for the GPU only** (`map_gpu_pages`). CPU views stay as they
  are.
* **Title soft asserts** (`int 3` then the code that carries on, e.g. game+0x4ffd879) are
  stepped over in title code, as on a retail console without a debugger.
* **T# misses** (descriptors the title wrote itself) become textures from the descriptor's
  own fields.
* **Compute dispatches run on the host** (`list.cpp` `dispatch`, `host_compute_pipeline`),
  once their programs are precompiled. Rerun `am-shaders precompile` after new runs: it picks
  up the compute shaders the game hands over (`cache\shaders\runtime\*.dxbc`).

**Open, next:**
1. **Text misses its x-height letters** (o r e s a n c m u w; letters with ascenders and
   descenders draw). Look at the font atlas texture(s): their upload, detiling (swizzle mode
   5, 4 KB S, is uploaded linearly now), or a compute pass that fills them (dispatches were
   not run before this commit).
2. **GFX10 detiling** of textures from title memory (all uploads are linear now).
3. XG slot 10 (64 KiB tiling) is still a probe.
4. Audio is silent (no XMA, Opus or DSP decoding).

**Shaders after the evening's runs** (`am-shaders precompile`, translator 4): 5,163 compiled,
about 4,600 refused. **The precompile does not retry programs listed in
`rt4\failures.tsv`**: move the file aside after a translator fix. Refusals by first missing
instruction:
* `v_interp_p1ll_f16`: 3,428 pixel shaders. These are 16-bit attributes, two to a 32-bit
  slot (SRC0 = {attr_word, attr_chan, attr}). The vertex side's packed exports must reach
  the host as two values for this to be right.
* `exp` to target 8 (depth export): 888.
* `image_get_lod` 142, `image_sample(_b)` with A16 addresses 88.
* Compute: `ds_bpermute_b32` (non-uniform lane reads) and `v_mad_u64_u32` (VOP3B, with a
  carry out) are the ones left. `global_atomic_fmin/fmax` (a CAS loop), `ds_write_b64`,
  `v_lshrrev_b64`, `v_bfm_b32` and the sub-dword buffer loads were added this evening.
* The game's compute dispatches wait until their programs compile (frame log: "dispatches N,
  waiting M").
* **Compute pipelines are made on a worker thread** (`create_compute_pipeline`). Six programs
  that compiled once the float atomics translated (the `global_atomic_fmin/fmax` CAS loop)
  stalled the game when made on its own thread: no frame, then the game's watchdog wrote to
  address 0 (game+0x2215c35, its deliberate crash). Made on a worker, the game runs, but none
  of them has finished in 60 s, so their dispatches still wait. Look at the HLSL of one
  (`am-shaders precompile --raw`), e.g. 391b80d5337dff03…, and at whether the AMD driver
  stalls on the CAS loop.
* **The game's dispatches use slot 67 pipelines**, not slot 62. The log says "a dispatch
  waits: pipeline (slot 67) (no host pipeline yet)". Slot 67 is the console's own compute
  pipeline creation (programs given by address, like slot 57 for graphics), and its
  description is not read yet: `create_console_pipeline` only logs its first bytes and looks
  for shader containers. **Next:** decode slot 67 as was done for slot 57 (blocks, the
  program's marker), make a host compute pipeline from the precompiled DXIL on the worker
  thread, then check the font atlas and the loading passes with dispatches running.

## Through the first-run menus (2026-11-25, night)

**Getting through the menus** (from the user; keys: Enter = A, Escape = B):
1. The start screen (account picker): Enter.
2. The setup pages: Enter on each.
3. Legal Notices: Down, wait 5-6 s, Down, Enter.
4. Rockstar Games Legal Terms & Policies: Enter, Down, Enter, Down, Enter (both boxes, then
   Confirm).

Unattended, as one `AMERICAS_PRESS`:
`A:45,A:53,A:61,A:69,A:77,A:85,A:93,DOWN:110,DOWN:117,A:119,A:130,DOWN:136,A:142,DOWN:148,A:154`.

**After Confirm the screen stays empty: the game starts streaming assets and DirectStorage
refuses them.** BCPack-compressed requests (options 0x200002, "BCPack not implemented yet"),
and zlib requests whose stream does not inflate to the destination size (options 0x200001).
**Next:**
* Implement BCPack decoding. This is the console's texture codec in DirectStorage.
* Fix the zlib size mismatch.

Then the larger pieces before the 3D world shows:
* **Mesh shaders:** 9,282 of the game's programs; the translator does not do them yet.
* **Compute correctness:** dispatches run with `AMERICAS_COMPUTE=1` but blank the menus.
* GFX10 detiling, 16-bit interpolation and depth export.

**This session also:**
* Slot 67 (console compute pipelines) is decoded.
* Compute pipelines are made in a two-thread pool, plus a lane for programs over 256 KB.
  The driver takes up to a minute over some.
* Writable textures (UAV views, simultaneous access) are in.
* Slot 19 writes real descriptors.
* The precompile does `wanted-first.txt` first and keeps `cpipe-index.tsv`. A pass now
  starts in seconds.

## Streaming works; compute correctness is next (2026-11-26, after midnight)

* **BCPack** (`formats/bcpack.*`): our decoder (rANS lanes, references and residuals,
  BC1/3/4/5/7; BC6H refused). Its constant tables (78 KB, not ours, licence unclear) stay
  **outside the repository**: `C:\Project Americas\tools\bcpack\tables.bin`
  (`AMERICAS_BCPACK_TABLES` overrides). They cannot be derived from the game's files. The
  user chose to load them from outside the repo.
* **zlib** streams longer than their destination keep what fits, as the hardware does.
* A run past Confirm now streams **24,815 requests, 1.33 GB, 0 failures**. The game then
  sits in its loading phase. Each frame it runs about a dozen slot 67 compute passes, then a
  few UI draws, and the screen stays dark.
* **Compute is off by default** (`AMERICAS_COMPUTE=1` turns it on). The dispatches run, but
  their results are wrong: with them the menus go blank. **Next: make translated compute
  correct.** The plan:
  1. Capture one dispatch's inputs: user data, the memory it reads, its descriptors.
  2. Run the program on a reference. The translator's tests already check programs on the
     host GPU against the ISA (`tests/translate_tests.cpp`, `gpu_runner`).
  3. Compare the outputs, starting with the loading-phase programs in the frame log, e.g.
     the slot 67 blocks `3099c2719d231d0b` and `79f03f52e321819a`.
* Keys for driving the menus yourself: Enter = A, Escape = B, arrows = d-pad. The title bar
  lists the rest. The user drives faster than `AMERICAS_PRESS`.
* The background precompile was restarted. `wanted-first.txt` holds what the game waited
  for.
* **Compute user data: found and fixed the layout (commit after ed5ec0b).** Compute programs
  take user data from **s0** (the "from s8" rule only fits merged stages like the game's
  vertex shaders, and mesh shaders). Each program's metadata block (after the program in a
  slot 67 bundle: u32 0x403, size, code dwords, ...; 32-byte records from +0x18) lists per
  resource a `0x41` record:
  * byte 4: resource type (0 SRV, 1 sampler, 2 CBV, 3 UAV);
  * byte 5: root type (0 constants, 1 root descriptor, 3 table, 5/6 heap addresses);
  * bytes 8/12/16: table offset (dwords), register, view dimension;
  * byte 20: descriptor dwords;
  * byte 21: **the user-data dword** (= sN for compute);
  * byte 22: **the root slot**.

  `compute_user_layout` reads them and `dispatch` fills exactly those registers.
  Example: pipeline bundle `3099c2719d231d0b` puts root slot 1 (a table) in s5, and its code
  starts `s_mov_b32 s8, s5`.
* **With `AMERICAS_COMPUTE=1` the game now crashes about 70 s in, in its own code**
  (game+0xf6349, reading 0xf). Compute results reach the game now and some are still wrong.
  Next:
  * Find which dispatch writes what game+0xf6349 reads, e.g. with a write watch on that
    memory.
  * Check root types 5/6 (heap addresses), which `dispatch` does not fill yet.
  * Check root constants' size against the record's dwords.
  * Graphics may need the same per-program placement (PS from s0?): today's "from s8" works
    for the boot screen's shaders only.
* **Compute is on by default again** (58eb52c; `AMERICAS_NO_COMPUTE=1` turns it off). With
  the user data in the right registers, the menus' compute passes draw their text: Finish
  Setup and Brightness render every letter, bold text and the controller glyphs. A full
  scripted run through Confirm (300 s) did not crash. The game+0xf6349 crash (a lookup by
  hash key that found nothing) came once, with the presses; it did not repeat.
* **Still wrong, all likely one cause, textures the CPU writes and we upload linearly
  (no GFX10 detiling):**
  * the intro video is solid green: its YUV planes reach the GPU as zeros or untiled;
  * the long scrolling legal text on the right still misses letters (the menu items on
    the left are whole);
  * the brightness page's calibration symbol is missing.
  * After Confirm the loading phase stays dark.
* **Next: GFX10 detiling** in `bindings.cpp`'s upload, by swizzle mode. Reference for the
  patterns: AMD's addrlib, MIT (`tools\downloads\addrlib-gfx10`); anything committed from it
  carries AMD's notice.
  * Modes seen: 2 (256B_D), 5 (4KB_S), 21 (4KB_S_X), 24 to 27 (64KB_*_X). The _X modes XOR
    with the pipe and bank configuration of the console's GPU, which must be found (Series
    X/S GB_ADDR_CONFIG).
  * Also re-upload video textures every frame: the checksum check runs every 15th flush,
    at most 16 MiB at a time.
* **SW_4KB_S detiling is in** (`bindings.cpp`, `offset_4k_s`). Mode 5 is the tiling of
  nearly every texture the game makes from title memory (only 1x1 textures use an XOR
  mode). The per-element-size bit patterns come from AMD's addrlib; they are written out in
  the code. No visible change in the menus.
* **The intro video stays solid green even so**, so its frames are not CPU-written mode-5
  textures. Likely Bink decodes on the GPU with compute, whose programs may be refused or
  still wrong. **Next:** find the video's planes (its T#s at the video's draw) and the
  dispatches that write them.
* The menu text arriving letter by letter is settling, not a bug (the user watched it fill
  in).
* **The green intro screen was uncompiled compute programs, not textures.**
  * During the intro, two 4-vertex draws (slot 57 bundles `db5a5d…`, `efeabc…`) and the
    compute programs behind them were waiting. The game had listed them in
    `wanted-first.txt` (`f714…`, `3061…`), and no precompile had run since.
  * Once compiled, the green is gone. Captures at frames 1100 and 1300 show the start
    screen; whether the video itself plays is not confirmed yet.
  * **Habit: after a run that reached new screens, run `am-shaders precompile`.** Round 0
    does what the game waited for, in minutes.
  * Do not run two precompiles at once.
* **The intro video is still solid green** (frames 400 to 800, before the start screen).
  During it the GPU runs one compute pass and two 4-vertex video draws a frame (slot 57
  `db5a5d…` and `efeabc…`), so Bink decodes on the CPU.
  * Checking textures every submission instead of every 15th did not help (reverted), so
    the planes never get data at all.
  * **Next:** log the T#s the video draws read, and whether Bink writes them. Bink plays
    its sound through the host's XAudio2 and may wait on that clock.

## The intro video plays (2026-11-26)

* **Cause of the green:** the video's Y/U/V planes (and the setup pages' art) sit in pages
  the game maps for the GPU only (queue slot 31, `CopyPageMappingsBatchX`). The page table
  followed those mappings, but texture uploads read the CPU address, where nothing is mapped,
  so the planes uploaded as zeros. YUV of zeros through the video's matrix is solid green.
* **Fix (400e29d):** `gpu_memory.cpp` keeps the CPU side of every GPU mapping (the pool's
  mirror view for committed pool pages, else the source view) and `gpu_bytes_at` reads through
  it. `bindings.cpp` uploads and checksums textures that way.
* **Textures that change** (video frames) are checked every flush once a check has found them
  changed; they drop back to the round-robin after 300 quiet checks. The round-robin now checks
  8 MiB a flush (it was 16 MiB every 15th flush, far too slow for 1,300 textures).
* Seen with `AMERICAS_PRESS=A:20`: the artwork intro and the logo animation play in color, and
  the setup pages show their art (the flamingo float behind "Finish Setup?").
* **Note for debugging:** the T# miss log never shows textures made from the title's own views
  past the first 200 ("gpu: texture ..."); 1,300 images exist by the video. A lookup that "hits"
  may hit a texture that was uploaded once while still empty.
* **Next:**
  * Pages in the lazy pool (committed on touch) still get no page-table entry for the GPU's
    own mappings (shaders read zeros there); uploads read them through the source view.
  * The setup text still misses letters ("The d f lt t l"): check whether those glyph
    atlases are written by compute into memory the uploads do not see again.
  * The loading phase after Confirm, compute correctness, XOR-mode detiling.

## Video timing, write watch, two PC crashes (2026-11-26, early morning)

* **Texture freshness:** changed textures are uploaded in a list of their own ahead of the
  title's lists in each ExecuteCommandLists (4f645d0). Live textures (new ones, ones that
  changed lately, ones whose pages a slot-31 mapping just placed) are hashed at every
  submission; the rest by a watcher thread (48 MiB a round). Textures over private memory are
  watched for writes (`memory/write_watch`, 7459cf2): read-only after upload, the first write
  faults and marks them. The intro video starts with no green frame and plays through (the
  user confirmed it clean).
* **Do not watch physical-page views:** Bink writes the video planes through another view of
  the same pages, so a watch there never fires (the video froze on its stars).
* **The PC bugchecked twice** in game runs about 20 s in (frame ~600): 0xC2 BAD_POOL_CALLER
  (0x61) at 02:58, then 0x133 DPC_WATCHDOG_VIOLATION at 03:02 (with the write watch off). Both
  came after c3b98c6 (geometry shaders between mismatched stages; XG slot 10
  GetResourceTiling, after which the game maps its UI textures' tiles); that commit is
  reverted (0235919). Earlier the same day dozens of runs did not crash. Minidumps are in
  `C:\WINDOWS\Minidump` (administrator only). **Before more runs:** decide with the user
  whether to analyse them (needs the Windows debugging tools, a system-wide install) and test
  the two parts of c3b98c6 one at a time.
* **What c3b98c6 found (worth bringing back carefully):**
  * Two pipelines were refused every frame on the setup and legal pages: a slot-57 one whose
    pixel program reads fewer of the vertex outputs (D3D12 "signatures between stages are
    incompatible": COLOR0 in another register), and a slot-86 stream pipeline with the
    console's rect-list topology type 19.
  * XG texture computer slot 10 is GetResourceTiling, D3D12's parameter layout without the
    resource (from the call at game+0x344283: tiles, 12-byte packed-mip info, 12-byte tile
    shape, count in/out = 16, first 0, 12-byte tilings). The stub left the UI streamer with
    zero tiles.
* **Missing letters on the legal pages (not solved).** The body text on the right panel is
  drawn by slot-57 pipeline VS 03fe0bb222c474c3 / PS 03ff45e826ecca2a, ~14 indexed draws of
  ~4,000 indices per frame. Each vertex picks placement rows 6n..6n+5 (n = COLOR1.z, UNORM8)
  from a buffer at the VS table + 32; the PS multiplies alpha by a mask (the 3840x2160 A8
  render target at 0xdd7c90000, a host texture, cleared then one quad). Missing are exactly
  the glyphs no taller than the x-height (a c e m n o r s u v w x z . , :); the left-hand
  text of the same size is whole. **Next probe:** that row buffer's V# (records, OOB mode)
  against the highest n of each draw. A collapsed transform (rows read as zero) would draw a
  dot, as seen where "Brightness" loses its n.

* **The two crashes, analysed** (dumps copied to `C:\Project Americas\crash`, with their
  `!analyze` output beside them): **not the GPU driver, and not our code's GPU paths.** Both
  are in Windows' storage stack while it pages:
  * 02:58 `0xC2/0x61` in `EhStorClass.sys`: storport could not free contiguous memory while
    building a TRIM (DataSetManagement) request.
  * 03:02 `0x133` in `fvevol.sys` (BitLocker) completing a write, spinning on the kernel pool
    lock; in `americas.exe`'s context.
  * The PC has 16 GiB of RAM (DDR4 at 2800). With the precompile running (3.5 GiB) 17.6 GiB is
    committed before the game starts; a run adds the 6.1 GiB pool and the game's own memory,
    so Windows pages hard to the encrypted disk. Two different pool failures minutes apart
    can also mean unstable memory (a Windows Memory Diagnostic pass would tell).
  * So c3b98c6 is probably not the cause; it made the UI streamer load more, which adds to
    the pressure. **Do not run the game while the precompile runs**, and bring c3b98c6 back
    when the user agrees.
* **Update (03:40): the tiling answer was the trouble.** With XG slot 10 answered, the UI
  streamer asks for 64 KiB tiles (BCPack and zlib requests with 64 KiB destinations) that we
  refuse as corrupt; runs then lost the device (0x887a0007, a GPU hang), aborted in the host C
  runtime (ucrtbase abort, 0xC0000409), and the PC bugchecked twice. Slot 10 is a probe again
  (1d72dc7); the linkage geometry shader and rect-list streams are back (6ba904e) and a run
  through the setup pages is clean. **Before answering slot 10 again:** decode the tile
  requests (what layout a 64 KiB tile's stream holds, mode 10 textures) and keep the device
  alive when a texture's data is refused.
* **abort() is logged now**: crash.txt gets the escaped exception's message and the stack.
* **Missing letters, probes so far:** the panel's row buffer (VS table + 32: stride 16,
  144 records, OOB mode 0) covers every placement (n up to 23); the rows hold sane colours, a
  mask mapping and 2x3 position transforms (same em-box height for every glyph). So the rows
  are not it. Next: the vertex data per placement (do the vanished glyphs' vertices carry
  alpha > 0, and what positions), dumped once from a panel draw.
* Running the game while `am-shaders precompile` runs is fine (the user's call, see below).

## The missing letters: fixed (2026-11-26, morning)

* **Cause:** the UI draws small glyphs (anything no taller than the x-height, and punctuation)
  from a CPU-rasterized A8 atlas (2048x2048 at 0xde2f60000, mode 0) with slot-57 pipeline
  VS 03ff6febefaa9317 / PS 03ff910a25acddcd; larger glyphs are triangle meshes (VS
  03fe0bb222c474c3 / PS 03ff45e826ecca2a). The small-glyph pipeline was refused (stage
  signatures do not link) until the linkage geometry shader; that shader then paired stages by
  semantic name, and the PS's colour input (named COLOR0) got the VS's never-written COLOR0
  output instead of its second output (TEXCOORD1). **The hardware pairs by position**; the
  geometry shader now does too (bf15cb8). The legal pages' body text is whole.
* How it was found: rasterizing the mesh pipeline's own vertex data offline gave the same
  missing letters, so they were never in that pipeline; the frame's full draw list (logged
  per draw) showed the refused pipeline next to it.
* **Small leftover:** a few letters near the panel's diagonal edge on the right have gaps
  (e.g. "peopl", "sp... hip" on the Copyright page). Probably the panel's clip mask (the
  3840x2160 A8 target at 0xdd7c90000) along that edge.
* **Running alongside the precompile is fine** (the user's call): several runs with both today
  were clean once XG slot 10 stayed a probe. To reach the legal pages, press Enter every 2 s
  from 20 s (`AMERICAS_PRESS=A:20,A:22,...,A:90`).

## The text gaps: fixed; the settings preview is aliasing (2026-11-26, morning)

* **Glitchy patches in the panels' text (fixed, 32581bb):** host render targets stayed in
  RENDER_TARGET state until their list closed. The UI draws a clip mask into an A8 target and
  samples it with the very next draws (the small glyphs), so they read it mid-write. Now, when
  the title sets new targets, every target still being written goes back to COMMON first.
  Found by elimination: an offline render of the dumped vertices and atlas was whole; waiting
  for the GPU after each submission, re-uploading the atlas every time, fine derivatives in
  the pixel program and refusing the rect-list pipelines did not help.
* **Settings > Control Scheme preview (Tab on the blue setup page) shows static (open):** the
  game renders the controller preview into R32_UINT targets (1280x720 at 0xddc1c0000 and
  0xddc544000, 2560x1440 ones) and samples them through a second resource over the same
  memory as R9G9B9E5_SHAREDEXP (format 67, modes 2 and 25). Our bindings make that second one
  from title memory, which the GPU never writes. **Next:** give it a host texture of its own
  format and copy the render target into it (D3D12 allows R32 <-> R9G9B9E5 copies) before
  the reads; do not view the target in the other format (that removed the device once).
* Driving the menus by script: Enter at 20 s, then after the intro 4 Enters (with margin,
  e.g. A:36,A:39,A:42,A:45,A:48), then MENU (Tab) for the settings.
* **The brightness page's symbol (the Rockstar logo) is gone again** since XG slot 10 went
  back to a probe (1d72dc7). With the tiling answer in, the UI streamer loaded its tiled
  textures (512x512, 512x256, 1024x512, 512x1024; BC3/BC7, mode 10) and the user saw the
  logo; without it the streamer asks three times after opening `ttdb_ui.ttpf` and loads
  nothing (the page's centre is a flat 63,63,63; the write watch is not involved). **Next,
  carefully:** answer slot 10 again only together with decoding the 64 KiB tile requests
  (BCPack/zlib streams with 64 KiB destinations that we refused as corrupt), and keep a
  refused request from leaving a texture's memory half-written.

## Menu scripting; UI tile streaming findings (2026-11-26, morning)

* **AMERICAS_SCRIPT** (eb95cd6, `runtime/script.h`): steps by events, not the clock. To the
  settings: `A until draw 03fe8796; draw 03fe9527; wait 1; A; wait 3; A; wait 3; A; wait 3;
  A; wait 3; MENU` (03fe8796 is the intro video's pixel program, 03fe9527 the first setup
  page's). `trace` logs each pipeline the first time it draws, to find a page's own.
  The user's notes: after the intro, 4 Enters reach the blue page (Tab: settings); on the
  legal notices, Down, wait about 6 s, Down, then Enter.
* **Shaders (rt4 precompile):** 6,030 compiled, about 12,600 refused. The refusals: 9,293
  pixel shaders on `v_interp_p1ll_f16` (16-bit attributes), 2,522 on `exp` targets (depth
  export), 385 `image_get_lod`, 215 `image_sample_b`, 111 BVH. `v_interp_p1ll_f16` alone would
  about double the pixel shaders.
* **UI tile streaming (AMERICAS_XG_TILING=1, opt-in since this commit):**
  * The game asks XG slot 10 for its UI textures in `ttdb_ui.ttpf`/`ttdb_graphics.ttpf`:
    swizzle mode 10 (64KB_D, no XOR), BC1/BC3/BC7, 1 mip. Our answer's tile shapes are D3D12's.
  * With it, per-tile requests (zlib, ~21 KB -> 64 KiB, page destinations) decode fine, but
    requests for whole textures (512 KiB) fail: their offsets and sizes are not stream
    starts. Example: the request said offset 0x1240500, 234,501 bytes -> 524,288; the file
    has a zlib stream at 0x123a000 (65,025 bytes -> exactly 524,288). The pack's table
    (`RDS5`, 1550 entries of 8 bytes) holds that entry as `?? ?? ?? ??  a0 23 01 60`
    (offset >> 8 in the low 24 bits, a flag byte 0x60). The game retries refused requests
    and finally aborts (ucrtbase abort, its own handler; ours is not called).
  * **Next:** disassemble the streamer's request builder (the caller of the XG computer,
    game+0x3441f7, and where it turns tile info and table entries into offsets) and find
    where our tiling answer differs from the console's. Mode 10 textures also need 64KB_D
    detiling on upload once their data arrives.

## START HERE (handed over 2026-11-26, morning, for an overnight run)

State of main at d0ad54c. Nothing is pushed (never push). Everything below is committed.

**What works now:** the intro video plays with no green frame; the setup, legal and settings
menus draw their text whole (small glyphs fixed by pairing relinked stages by position,
bf15cb8; the glitchy patches fixed by flushing render targets when the title switches them,
32581bb); event-driven menu scripting (`AMERICAS_SCRIPT`, eb95cd6).

**The task in hand: 16-bit attribute interpolation (translator version 5, d0ad54c).**
1. Run the version-5 precompile (it compiles into `cache\shaders\dxil\rt5`, from scratch;
   rt4 is obsolete). In the background, log to a file:
   `"C:\Project Americas\build\release\bin\am-shaders.exe" precompile > "C:\Project Americas\logs\precompile15.txt" 2>&1`
   It reads ~48,000 pipeline bundles first (minutes, nothing compiled yet), then compiles
   ~18,700 programs; version 4 compiled 6,030 and refused ~12,600 (9,293 of them pixel
   shaders on `v_interp_p1ll_f16`). Expect thousands more compiled now. It held ~4 GiB.
2. Check `rt5\failures.tsv` (marker, stage, reason): if pixel shaders now fail with DXC errors
   about `GetAttributeAtVertex`/`SV_Barycentrics`, fix the generated HLSL in
   `finish_graphics` (translate.cpp). The design: an attribute read by the 16-bit forms is
   declared `nointerpolation`; `main()` copies its three vertices' bits into
   `static uint4 am_vtx<n>[3]` and `SV_Barycentrics` into `am_bary`; p1ll/p1lv give the
   whole interpolation of the chosen half, p2 passes its SRC2 on as f16; `AM_IN<n>` is
   `am_in.i<n>` or the same interpolation for 32-bit reads.
3. Then run the game through the menus with a script (below) and compare captures with the
   rt4-era ones: more UI should draw (backgrounds, icons, the 3D world later).
4. Next refusal groups after this: `exp` targets (2,522, mostly depth export),
   `image_get_lod` (385), `image_sample_b` (215), BVH (111).

**Running the game** (`C:\Project Americas\build\release\bin\americas.exe --timeout N`):
* Running it while the precompile runs is fine (the user's call), but the PC has 16 GiB of
  RAM: keep to one game run at a time.
* To the settings with no timing:
  `AMERICAS_SCRIPT=A until draw 03fe8796; draw 03fe9527; wait 1; A; wait 3; A; wait 3; A; wait 3; A; wait 3; MENU; wait 4; capture; quit`
  (`capture` saves `logs\script-<n>.bmp`; `trace` logs each new pipeline per page). The legal
  notices need Down, ~6 s, Down, Enter. Ask the user not to type into the game window
  during a run (their keys reach it).
* Captures: `scratchpad` scripts are gone with the session; a 3840x2160 BMP is 24 MB.

**Open items, most useful first:**
1. Verify translator 5 (above).
2. UI tile streaming (`AMERICAS_XG_TILING=1`, opt-in): the brightness page's logo and other
   UI textures. Whole-texture requests get wrong offsets (see the section "Menu scripting; UI
   tile streaming findings"). Needs the streamer's request builder disassembled
   (game+0x3441f7 area; capstone is in `C:\Project Americas\tools\py`), and 64KB_D (mode
   10) detiling. Do not leave tiling on by default: refused requests make the game retry and
   abort.
3. Settings preview static: R32_UINT targets read as R9G9B9E5 through a second resource
   (see "The text gaps: fixed; the settings preview is aliasing"). Copy, do not alias views.
4. XOR detiling (modes 21, 24-27) and the loading phase after Confirm.

**Crashes:** the two PC bugchecks of the night were Windows' storage drivers under memory
pressure (analysis in `C:\Project Americas\crash`, WinDbg's cdb in the WinDbg package's
`amd64` folder). With tiling off, runs have been clean since.

## Overnight, 2026-11-26 (the user is asleep)

* **Standing instruction tonight:** keep working with all the usage there is. When the
  5-hour window reaches 95%, shut the PC down, or schedule the shutdown for when the
  background jobs end: `shutdown /s /t 60` after `am-shaders` exits.
* **Shader cache, the answer to "why do we keep compiling":**
  * Compiled programs were never recompiled. New ones kept arriving as the game reached new
    screens (the compute set, new menus).
  * The real gap was the other way: translator fixes did not reach programs compiled
    before them.
  * Now there is `dxil\by-source` (content keys) and `precompile refresh`, which recompiles
    only programs whose translated source changed.
  * Tonight follows START HERE instead: the translator-5 precompile into `rt5`
    (`logs\precompile15.txt`). `precompile all` can come after it.
* **Safety-net shutdown:** a detached watcher (`tools\overnight-shutdown.ps1`, its pid in
  `tools\overnight-shutdown.pid`) shuts the PC down 45 minutes after `am-shaders` exits,
  with a 5-minute warning (`shutdown /a` cancels). Kill it to keep working longer.
* Menu path for the Legal Terms page (the user again): Enter, Down, Enter, Down, Enter.

## Past Confirm: what happens, and what kills it (2026-11-26, night)

* **Translator 5 checked:** the rt5 precompile compiled the 16-bit interpolation programs
  with no DXC errors (no `GetAttributeAtVertex`/`SV_Barycentrics` failures in
  `rt5\failures.tsv`). To have something to draw while it runs, rt5 was seeded with rt4's
  6,084 DXIL files, each with a `.src` sidecar reading `rt4`: a later `precompile refresh`
  recompiles them (their content keys differ) and nothing else.
* **The bundle index is big:** the runtime folder holds ~315,000 `.cpipe` bundles (not
  48,000); a fresh `rt<N>` spends ~40 minutes indexing them before compiling anything.
  Copying the previous version's `cpipe-index.tsv` first would skip that (bundles do not
  depend on the translator).
* **Translator additions tonight (no version bump; refused programs simply have no DXIL, so
  a plain `precompile` picks them up; every earlier program's source is byte-identical, so
  its content key holds):** depth export `exp mrtz` -> `SV_Depth` (2,536 refused before);
  A16 sampling (f16 coordinates, gradients, lod; f16 bias in its own dword; 1D/3D gradient
  padding); `image_get_lod` (helpers emitted only where used); f16 compares;
  `v_cvt_f16_u16/i16`; `v_movrels/movreld/movrelsd_b32`. Tests for each.
* **Menu script to Confirm and on:** `C:\Project Americas\scripts-run\to-load.txt`
  (`AMERICAS_SCRIPT=@<that path>`). Timed `AMERICAS_PRESS` drifts with the frame rate; the
  script anchors on the setup page and reaches Confirm every time.
* **After Confirm** the game streams the world (ttdb props, interiors, maps; `levels.rpf`,
  `navmeshes.rpf`) behind a dark loading screen (RGB ~34,34,44), then black.
* **The crashes after Confirm differ every run** (a pointer of garbage at game+0x52ed0a7, a
  null getter at +0xf6349, a write through a field of 2 at +0x3768dc6, `bad_alloc` in
  DirectStorage and in texture uploads). The cause found: **the process runs out of
  commit.** With the game at the loading screen it had 25.6 GB private commit (system 39.8
  of 42 GB, 0.5 GB RAM free); 1,087 private allocations of ~16 MiB made 16.6 GiB of it.
* **Ruled out:** the page-destination texture streams. Their zlib streams are longer than
  the destination (e.g. 32 KiB stream, 4 KiB destination at page offset 0x6000); the
  sources are real stream starts and `page_offset + size` ends on a 32/64 KiB boundary,
  which fits packs storing the smallest mips first, so keeping the stream's prefix is right.
  `AMERICAS_SKIP_PAGE_WRITES=1` (diagnosis only) skips those writes.
* **The commit hog, found and fixed (c44c25e):** thread stacks taken mid-growth (end a run
  with `--timeout 20` and read `logs\hang.txt`) showed the AMD shader compiler
  (`amdxc64.dll`) building compute pipelines from `make_host_compute`. The runtime made all
  ~2,700 compute pipelines the title creates at boot, and the driver's compiler kept ~15 GB
  in its own heap afterwards. Compute pipelines are now made when a dispatch first needs
  one (`AMERICAS_EAGER_COMPUTE=1` for the old way): 10.2 GB private at the same point
  instead of 25.5 GB. (`AMERICAS_NO_COMPUTE` only skips dispatches; pipelines were still
  made.) Tools used: `AMERICAS_TRACE_MALLOC=1` traces the title's CRT heap by call stack
  (small: the title's heap use is not the problem).
* **Overnight chain:** `tools\overnight-chain.ps1` (started through WMI so it outlives the
  session; pid in `tools\overnight-chain.pid`, progress in `logs\overnight-chain.txt`).
  After the first rt5 precompile ends it runs `precompile` with the new translator (the
  refused programs now translated: `logs\precompile16.txt`), then `precompile refresh`
  (the rt4-seeded programs: `logs\precompile17.txt`), then shuts down with a 5-minute
  warning. The new `am-shaders.exe` was built beside the running one (renamed
  `am-shaders-pass1.exe`). The earlier watcher (pid 10216) had died: processes started
  from a tool call are killed with it unless started through WMI.
* **With memory fixed, loading still stops (the next task):** after Confirm, DirectStorage
  serves ~25,000 requests (1.25 GB: shaders, audio, bvh4, data, packs, levels, anim,
  story000, script) and then none at all, by about frame 4,500, while the dark loading
  screen keeps drawing. Most runs then crash in the title's code; the crash differs:
  * `+0x52ed0a7` (seen 3 times, same object every time): the caller (`+0x3aea37`) walks up
    to 4 entries at `container+0x150` (count at `+0x170`); each entry `+0x20` is passed to
    `+0x52ed040`, which reads 4 pointers at `+0x30`. Entry `0x5f17245c0` holds
    `0x90b7d61d_00000004` there (another run: `0x6a6e6e7d_00694c4c`): a hash and a small
    number, not a pointer. The object is inside a resource DirectStorage loaded intact
    (`AMERICAS_WATCH_ADDR=5f1724610`: `story000.rpf` offset `0x356ad05c0`, zlib 1,168 ->
    37,328 bytes at `0x5f1720000`, putting a 0 at that field). Something writes the
    field afterwards: looks like an unresolved by-hash reference.
  * `+0xf6349`: `object->vfunc[0x3f8](type hash 0x33f70dfaf2c50989, key 0x0f0c08ae)`
    returns null, then the same with type `0x4eaa16dd829995bc` also returns null, and the
    code assumes it cannot. A by-hash lookup of something never registered.
  * Also seen once each: `+0x3768dc6` (a write through a field holding 2), `+0xf2cb40`.
  * **Ruled out:** memory (fixed), page-destination texture writes
    (`AMERICAS_SKIP_PAGE_WRITES=1` still crashes), refused file paths (`D:\decorations\*`
    gets PATH_NOT_FOUND, which is right), DirectStorage failures (0 in every run).
  * **Next:** catch the CPU write to `0x5f1724610` after the load (a hardware data
    breakpoint, DR0-DR3 on every thread, or a guard page on that page with single-step
    re-arm), and look at what the title does with those 37,328 bytes (decompress
    `story000.rpf` at `0x356ad05c0` offline: what is at `+0x4610`?). Crash reports now name
    the classes of objects registers point to (RTTI), when they are objects.
* **Tonight's run recipe:** `AMERICAS_SCRIPT=@C:\Project Americas\scripts-run\to-stall.txt`
  reaches Confirm and waits 90 s (about 6 minutes in all); `to-load-long.txt` waits ~8
  minutes after it.
