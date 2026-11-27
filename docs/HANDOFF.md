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

## Faster cycles: the shader service, page-driven scripts (2026-11-26, afternoon)

* **`am-shaders serve [jobs]`** replaces the long precompile while working: it maps the
  game's programs by marker (16,890 in ~30 s, no translation up front), compiles what the
  running game waits for first (new lines of `rt5\wanted-first.txt`, read every second),
  then the rest of the game's pipelines' programs smallest first. Programs over 1 MiB of
  HLSL compile with `-O0` (about half the time). Run it in the background with 6 jobs on
  this 8-thread PC while the game runs:
  `"C:\Project Americas\build\release\bin\am-shaders.exe" serve 6 > "C:\Project Americas\logs\serve.txt" 2>&1`.
  The runtime looks for a missing program again every 2 s, so pages fill in while shown.
  `am-shaders.exe` is locked while serve runs: rename it aside (`am-shaders-old16.exe` is
  such a copy) before building.
* **Page-driven script steps:** `A next [seconds]` taps, waits for the page to change (the
  set of draws a frame makes, by pipeline and vertex count) and then to settle (that set
  unchanged and no file or DirectStorage reads for 0.35 s). Menu taps settle in 0.5-1 s.
  `scripts-run\to-load-fast.txt` reaches Confirm and the loading after it in about 2
  minutes; `to-stall-fast.txt` waits there. Don't mix the settings (Tab) into a load
  script: B did not leave the settings, and the taps after it wandered through them.
* **Settings preview:** textures over a render target's memory in another format of the
  same size (R32_UINT drawn, R9G9B9E5 read: ~30 of them, UI and HDR passes) are now copies
  of the target, refreshed when the title sets new targets. The Control Scheme preview is
  still empty (one row of static), so the preview needs more than that.
* **The `+0x3e4551d` crash and the by-hash lookups:** `+0x3e454a0` looks a key up in a
  streaming pool (`+0x3b530`: slot `key & 0x3fffff`, elements at pool+0x38, size at
  +0x40, used bits at +0x60); the element's first qword is the loaded asset, a dictionary
  (hashes at +0x20, count u16 at +0x28, pointers at +0x30). A crash read the hashes through
  `-1` at asset `0x5f1710000`. With `AMERICAS_HW_WATCH=5f1724610,5f1710020` both fields were
  written by our DirectStorage copy (file offsets `0x50000668`, `0x50048e90`) and then
  relocated by the game (`+0x4da62bf`, `+0x4ec4efb`) to proper pointers; no later write in
  that run, and that run did not crash. So the crashes come after a correct load and
  relocation: look for the asset being freed or its memory reused while still registered.
* **Loading still stops** after ~24,000 DirectStorage requests (1.25 GB) with no shader or
  compute pipeline missing (dispatches "waiting 3" is a cumulative count of three early
  ones).
* **Why it stops (found):** one game thread spins forever in the title's page allocator,
  and everything else waits. `hang.txt` now prints the registers of threads running
  outside the system. The loop at `+0x4f8f120` unlinks a page entry from a free list: page
  entries are 8 bytes in an array (`r14 = 0xd84479930` in every run), next index in bits
  41-61 (`0x1fffff` ends a list), list heads per size class at `0x1471bfb08 + 0x74`. The
  entry (`esi = 0x4716e`, another run `0x47166`) is not in the list its header names, the
  walk reaches the end marker, and the loop has no check for it.
  * `AMERICAS_HW_WATCH=d846b24a0` (entry `0x4716e`): 30 writes, **all by the title's own
    allocator** (`+0x52389d4/fb/a2d`, `+0x555b311`, `+0x52387f8`, `+0xe88d7`), none by us.
    The low 16 bits are a count with bit 15 a flag: `0x8000` (0) becomes `0xffff`, `0xfffe`,
    `0xfffd`, `0xfffc`, `0xfffb`: **the page is released more times than it was taken.**
    The releases (`+0xe88d7` <- `+0x4f8f3ed` <- `+0x41dbc7` <- `+0x43c4ede` <- `+0x122d9b`
    <- `+0x4fd23ed` <- `+0x4fd1e88`) run on the streaming thread.
  * Ruled out: DirectStorage writing the metadata (page-destination arrays such as
    `0xd849b0568` hold valid page numbers; `write_pages` refuses anything else and 0
    requests failed); physical-page aliasing (one shared section, views of it).
  * **Next:** find what the title's acquire/release pairs on these pages hang on. Read
    `+0x41dbc7`/`+0x43c4ede` (what decides to release) and `+0x555b311`/`+0x52387f8`
    (what takes a page), and look for an API of ours whose answer makes the title release
    twice: a completion reported twice, a status array or fence value written more than
    once, a residency or tile-mapping call that reports a failure, a physical-page free or
    query answered wrongly. The underflow is deterministic, so the same watch reproduces.

## The loading stall, narrowed to four textures (2026-11-26, evening)

* **The pool.** The title's 64 KiB page allocator manages a 24 GB pool with two views of the
  same physical pages: CPU `[game+0x9e1dac0]` = `0x180090000`, GPU `[game+0x9e1dad0]` =
  `0x780090000` (both from `+0x5f3fcc0` with `0x20402000`; pages mapped into both by
  `+0xf2cbxx`). One 8-byte entry a 64 KiB unit (array at `[game+0x71bfb08]`): size class in
  bits 15-19, reference count in bits 0-14, free-list next index in bits 41-61.
  Allocation: `+0x221b9e0` (size rounded to 64 KiB and a power of two) -> `+0x5238780`.
  Release of a unit: `+0xe8890`. Release of a range: `+0x4f8f330(cpu address, size)`, which
  for a range starting in a class-0 (one unit) block releases every unit of the range one by
  one.
* **The culprits (`AMERICAS_CHECK_PAGE_RELEASE=1`):** the title releases a texture's pages in
  `+0x41db90`: GPU address at +0x18 (64 KiB down), size = u32 +0x8 x (u16 +0xc & 0xfff) (64 KiB
  up), moved to the CPU view, released. The check replaces that function with an identical
  one that logs releases larger than the block they start in. Each run: **exactly four**,
  all objects with vtable `0x1461ecdb0` (a texture class, RTTI stripped; constructors
  `+0x325060`/`+0x325090` for textures placed in a resource, `+0x501f3a0` for others),
  1728 x 1024 (`+0x8` = 0x6c0, `+0xc` = 0x400, `+0xe` = 8), each releasing 27 units from a
  1-unit block, 128 KiB apart (GPU `0xbf2320000`..`0xbf2380000`, CPU `0x5f2320000`..: inside
  `story000.rpf`'s data). Their 27-unit releases run through neighbouring blocks: those
  lose references they never gave, are freed while in use (the corrupt fields of the
  earlier crashes) and the free lists break (the spin).
* So the title believes these four textures own 1.7 MB of pages of their own, while their
  address still points into the resource they were loaded with. On the console a step must
  move them first (texture streaming giving them memory of their own, e.g. for higher mips)
  or keep them from being released this way. Not the cause: XG (only two callers, both
  single-mip textures early on), GetResourceAllocationInfo (48 calls, render targets),
  refused DirectStorage requests (none), our objects (`is_object` false).
* **Next:** follow one of the four from its constructor: which resource holds it (its
  memory sits in `story000.rpf`'s data at `0x5f23xxxxx`), what the streamer's entry for it
  (table `[game+0x9c3e108]`, 16 bytes an index; state in the top 3 bits of the qword the
  release reads at `(rdx)`) says, and which call of ours the streamer waits on before
  giving such a texture memory of its own. `AMERICAS_CLAMP_PAGE_RELEASE=1` refuses releases
  past zero but does not save the run (the over-release takes references that exist).
* Also new: `xg: GetResourceLayout asked from ...` logs each place that asks XG (once).

## The loading stall: fixed by the mip tail (2026-11-26, evening)

* **The cause:** the four bad releases were our own placed resources (`+0x20` of the title's
  texture object): 1024x1024 BC7/BC5, 9 mips, swizzle mode 10. The title released them by the
  size our layout answered, 1,769,472 bytes (27 tiles: every small mip in a 64 KiB tile of
  its own, "no mip tail"), while it had mapped tiles by GetResourceTiling (XG slot 10), which
  we left unanswered. The extra 64 KiB units it released belonged to its neighbours.
* **The fix:** `layout.cpp` now places the mip tail as the GPU's address library does (2D, 4
  KiB and 64 KiB blocks, one sample, several mips): the tail block first, the larger mips after
  it from the smallest up, each tail mip at its slot inside the block (`tail_x`/`tail_y`).
  The same texture is now 1,408 KiB. XG slot 2 reports standard and tail mips, slot 10 is
  answered by default (the tail is the packed mips, in tile 0; `AMERICAS_NO_XG_TILING=1` turns
  it off), the upload and XG's slot 8 place tail mips inside the block. Tests in
  `d3d12x_tests.cpp`.
* **Result:** no bad releases, no crash, no spinning thread. A run reached 84,000 DirectStorage
  requests (5 GB, up from 24,000) and was still loading (dark screen, spinner turning) when
  its timeout ended it. The old worry about slot 10 (device loss, aborts) did not come back.
  Failed requests fell from ~91,000 (slot 10 on our old layout) to 4,099: **BCPack requests
  we refuse** (`dstorage: request refused (BCPack): options 0x20000c`, 1 MiB and 256 KiB
  destinations). Those are next.
* The run's command: `AMERICAS_SCRIPT=@C:\Project Americas\scripts-run\to-past-stall.txt`
  with `--timeout 600` (captures every minute after Confirm).
* Shader service: `am-shaders serve 6` died twice without a message on the 2 MB compute
  programs while a game ran (memory, most likely); it runs with 3 jobs now
  (`logs\serve3.txt`, `serve3-err.txt`).

## Past the stall: BC6H, and the next wall is BVH building (2026-11-26, evening)

* **BCPack BC6H decoded** (`formats/bcpack.cpp`): the table file already held BC6H's
  dictionary; records give the mode (byte 30 + 1), endpoint residuals (interleaved byte
  pairs, zigzag, summed per channel at the mode's precision), index nibbles, the partition;
  blocks are written in the public BC6H layout per mode. The 4,099 refused BCPack requests
  (`options 0x20000c`, BC6H HDR textures) are gone; ~2,000 requests still fail, now logged
  (`dstorage: request failed ...`, first 16) to see why.
* **Where loading goes now (default settings, `to-past-stall.txt`):** 68,000-77,000
  DirectStorage requests (4 GB: terrain, maps, props, interiors), then the title builds
  raytracing structures and `XboxBvhBuilder.dll` throws a C++ exception (`0xe06d7363`; the
  registers hold text ending "...plemente(d)") right after calling command list slots 26
  (from game+0xfe4eeb: `1, ptr, ptr` - D3D12's ResourceBarrier shape) and 148 (from
  XboxBvhBuilder+0x20ed9: `0x280, 0x10, ptr, 0x10, ...`), neither implemented. Before it:
  "gpu memory: cannot open the allocation at 0xdc9600000 (2.1 GiB) for the GPU", repeated.
  **Next:** implement those two slots (or find what the builder asks for), and the 2.1 GiB
  allocation's GPU view.

## BVH building runs; the next stop is a GPU-wait watchdog (2026-11-26, night)

* **Command list slot 148** is "allocate space in the list" (size, alignment) -> pointer;
  XboxBvhBuilder throws E_FAIL on null (the crash before). Now a per-list arena of title memory
  (graphics allocator chunks of 1 MiB), reset with the list.
* **Slot 80** is Dispatch(x, y, z, initiator): the builder writes the dispatch packet itself
  when its list has room, else calls 80 (our list keeps its buffer fields 0, so always 80).
  Recorded as a Dispatch. **Slot 26** (ResourceBarrier) is taken quietly.
* **DirectStorage: 0 failed requests** in the run after these (74,000 requests, 4.1 GB).
* **Where it stops now:** the title's deliberate fatal (`+0x23cf683`, `mov [0], 0x4d8bd421`,
  on a thread of its own) after lists waited for memory values that stayed 0 ("a list waits
  for the value at 0xd8437ff78 (comparison 7, reference 3435, now 0): not met in 2 s, let
  go", 8 times). Unimplemented calls seen in that run: command list 85 (from game+0x4f49be:
  `ptr, 0, 0, 0, ptr`) and 86 (from game+0x4f4b52: `ptr, ptr, 0, 0x1600000000,
  0x1001002100000119, ...`), device 71 (from game+0x511f267). One of them likely writes the
  awaited values. The 2.1 GiB allocation at 0xdc9600000 is fully committed but
  OpenExistingHeapFromAddress still refuses it (size limit? MEM_WRITE_WATCH?).
* **Loading screen draws:** 5 a frame, none skipped (graphics "waiting for shaders 0"): two
  full-screen passes, two 30-vertex indexed draws (the spinner), the final pass. No artwork
  draw is issued at all; the title's loading-screen art may wait on the same stuck work.
* **am-shaders died on the largest programs with a stack overflow (0xC00000FD)**, not memory:
  DXC recurses deeply on 2 MB of HLSL. It now links with a 256 MiB stack reservation.
* **The three calls, read from their call sites (next task: raytracing):**
  * List slot 85 (`vtable+0x2a8`, game+0x4f49b7): called in a loop, once per item, with a
    description pointer, two u32 from `[r15+0x20]`/`[r15+0x24]`, and on the stack 0, a
    pointer, a pointer to a local, 0: the shape of DXR's BuildRaytracingAccelerationStructure.
  * List slot 86 (`+0x2b0`, game+0x4f4b4c): `(dest, source, 0)`: CopyRaytracingAcceleration-
    Structure, mode 0 (clone), by its shape.
  * Device slot 71 (`+0x238`, game+0x511f261): a description (a field `0x20004` at +0x30) and
    two output pointers, the fourth argument `[r12+0x18]`: the prebuild-info query (sizes)
    by its shape; we answer nothing, so the sizes the title uses are whatever was there.
  * The title's lists then wait for values (reference 3435) that the work after these builds
    would write; its watchdog trips on the missing progress. Options: implement DXR (the host
    has it: map the console descriptions to D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC),
    or answer the prebuild query with sizes and make the builds no-ops, so the frame's work runs
    and the waited-for values get written (raytraced effects then show nothing).
* **The awaited values are never written by any list of ours** (the timeout message now says
  so: "never written by a list of ours, N written addresses within its 8 KiB"). Addresses
  0xd8437fe38..0xd84380078, 16 bytes apart, comparison 7, reference = a per-frame counter
  (2719, 2960, 3435 in three runs). So the writer is a GPU write we drop. Suspects, in order:
  the slots we take quietly as "pipeline syncs" (131, 132, 142: one may be an end-of-pipe
  write of a value), a queue-level signal to memory, the raytracing calls (85/86, probes:
  nothing happens). Next: log the arguments of 131/132/142 and of the queue's slots for an
  address in that range; the right one writes `reference` there.
* **Translator additions (night):** `v_swap_b32`, `v_xnor_b32`, `v_ffbh_i32`, `ds_write_b16`
  (atomic half-dword), `ds_cmpst_rtn_b32`, `ds_bpermute_b32` - all from programs the title waits
  for after loading (they were refused). Still refused there: indirect jumps (`s_setpc_b64`,
  2 programs) and `image_bvh_intersect_ray` (raytracing). The GPU-wait watchdog still trips
  (5 waits not met in the last run); one of those programs may be the counters' writer.
  `am-shaders serve` restarted with them (`logs\serve9.txt`). DirectStorage: ~1,600 requests
  fail with E_INVALIDARG, page destinations for `octa_imposters.ttpf` (64 KiB destinations):
  `write_pages` refusing a page number; next to look at.

## The shader service starts itself (2026-11-26, night)

* **Why it was not running:** `am-shaders serve` ran as a background task of the working
  session, and ended with it (no crash: Windows' event log shows none after 18:00). Nothing
  started it again.
* **Now a game run starts it** (`runtime/shader_service.cpp`), when the title creates its D3D12
  device: from a copy in `cache\shaders\service\am-shaders-<size>-<time>.exe` (so builds can
  replace `am-shaders.exe` freely; no more renaming aside), below normal priority, outside the
  run's job (`CREATE_BREAKAWAY_FROM_JOB`; inside it, with a log line saying so, when a job refuses),
  output appended to `logs\serve.txt`. It keeps running after the game ends.
* One service at a time (`Local\AmericasShaderService` mutex); `cache\shaders\service\running.txt`
  names the executable it runs from. When a run finds that build differs from
  `bin\am-shaders.exe` (a new translator), it sets `Local\AmericasShaderServiceStop`; the old
  service ends at once (files are written under temporary names, so a stop mid-write leaves
  nothing half-written) and the new one starts.
* Jobs: one per CPU thread but one (7 here; the largest compiles one per 12 GB of RAM); `americas.ini` `shader_jobs = N`,
  `shader_service = off`, or `--no-shader-service` on the command line.
* Started by hand it still works as before (`am-shaders serve 3`); a second one exits at once.
* **"No compiling going on", the second time:** the service was running but idle. Every
  program left in its queue (211) was refused by the translator within 35 s, and refusals were
  only printed for programs the game had waited on, so `serve.txt` looked dead. It now prints
  every refusal and, when its queue empties, `serve: idle ...` with the refusal reasons ranked.
  When it is idle, the translator is what limits compiling.

## Translator gaps closed; the GPU-wait watchdog is still next (2026-11-26, night)

* **Added** (GPU tests for each in `translate_tests.cpp`):
  * `v_sub_co_ci_u32`, `v_subrev_co_ci_u32`;
  * f64: `v_add/mul/fma/min/max_f64`, `v_rcp_f64`, `v_cvt_f32_f64`, `v_cvt_f64_f32/i32/u32`,
    `v_cvt_i32/u32_f64` (saturating);
  * f16: `v_trunc/ceil/rndne/exp/log/sin/cos_f16`;
  * `buffer_store_byte/short`;
  * LDS: `ds_read_u8/i8/u16/i16` and their d16 / d16_hi forms, `ds_write_b8`,
    `ds_write_b8/b16_d16_hi`, `ds_min/max(_rtn)_i32`;
  * `image_sample_cl/_b_cl/_d_cl`, with D3D's clamp argument;
  * packed f16 ops with inline constants, `v_pack_b32_f16` with op_sel.
* **Packed constants are an assumption:** the ISA guide does not say what VOP3P inline constants
  give the high half. The translator lets both halves read the constant, because LLVM encodes
  `v_pk_mul_f16 v1, v2, 2.0` with op_sel_hi set. The GPU test checks our model, not hardware.
* **Far jumps:** most "indirect jumps" were `s_getpc_b64 sX; s_add_u32 sX, sX, k; s_addc_u32
  sX+1, sX+1, 0; s_setpc_b64 sX`, or backward with `s_sub_u32`/`s_subb_u32`: the long branches
  compilers emit past 16-bit offsets. `rdna2::scan_program` resolves them (`far_jumps`) and the
  translator makes them block jumps. Real indirect jumps (a computed address) are still refused.
* **Stream pipelines (slot 86) retry:** a stream pipeline whose shaders were not compiled when
  it was created never got a host pipeline. Now it keeps its description, lists the missing
  shaders for the service, and draws look again every 2 s. Derived pipelines (slot 55) copy the
  description and get their own blend state; before, they shared the base's host pipeline.
  Compute misses are listed for the service when the pipeline is made, not only once a dispatch
  waits on them.
* `am-shaders disasm <48-digit marker>` lists one program by its marker (the runtime's name for
  it) and decodes the "data" regions too (code reached only by jumps we do not follow).
* **The watchdog still trips** (`+0x23cf683` after 5-8 "a list waits ... not met"). In the
  run, the compute programs missing just before the waits were: `1b16...`
  (`image_bvh_intersect_ray`), `24ea...` and `3db4...` (far jumps, compiled by the build
  from this session), `a5c8...` (never listed, fixed above), and a few with packed constants
  (fixed). The writer of the awaited values is most likely one of these. Next: one more run with
  everything compiled, then look at what is still missing before the waits.
* **Raytracing is the big remaining gap:** 56 programs use `image_bvh_intersect_ray` /
  `image_bvh64_intersect_ray`, and command list slots 85 and 86 and device slot 71 are not
  implemented yet (BVH build, copy, prebuild sizes). The ISA guide (section 8.2.10 and the
  MIMG table) gives the inputs (node pointer, ray extent, origin, direction, inverse
  direction; A16 packs the directions), the outputs (box node: 4 child pointers sorted by hit
  distance; triangle node: t_num, t_denom, triangle id or I/J, hit status) and the T#
  (base >> 8 in bits 0-39, box growing ULPs, sort enable, node count - 1, triangle return
  mode). It does not give the node layouts in memory. XboxBvhBuilder's own compute programs
  write those nodes, so their stores show the layout. Two ways forward:
  * implement the instruction faithfully in HLSL from that layout (real raytracing, in compute);
  * or a stand-in that reports no hit (child pointers all `0xFFFFFFFF`), which lets the frame
    run with raytraced effects missing. That would be a stated exception, not emulation.
* **DirectStorage `octa_imposters.ttpf` failures:** no failures in the last two runs (timing
  dependent). A refused page destination now logs its page numbers and whether each is an
  allocated page.
* Lesson: in the Bash tool, a Python heredoc can lose backslashes (a doubled backslash
  before `a` became a bell character in a document). Write scripts to files in the scratchpad and run them with
  `python -I`.

### Later the same night: where it stopped

* **Shader service hand-over:** a newer build no longer kills the running service. Asked to stop,
  it takes no new work, finishes its compiles and ends; the replacement records itself in
  `running.txt` and waits for it. Compiles over 1 MiB of HLSL (about 3 GB of DXC memory each)
  run one at a time per 12 GB of RAM. **The service running at the end of this session
  (process 4376, started 19:42) is from before that change:** the next game run asks it to
  stop, and it ends at once, dropping what it is compiling.
* **The two far-jump programs (`24ea...`, `3db4...`, about 80,000 instructions each) are too
  deeply nested for DXC** when structured (more than 256 brackets), so they take the dispatcher
  form. DXC had been compiling those for 45 minutes at 9 GB, unfinished, when the session
  ended. If they never finish, the dispatcher is too slow at this size. Next: a structured form
  that nests less, for example regions split into functions or a loop-and-switch per region.
* With everything else compiled, the programs missing before the GPU waits are these two and
  `1b16...` (raytracing). The watchdog still trips (8 waits not met in the last run).
* Also dropped: pixel shader exports to render targets the signature does not declare. Still
  refused (in order of count): `image_bvh(64)_intersect_ray`, `v_interp of attribute N beyond the
  signature` (pixel shaders reading inputs the PC signature does not list), `image_gather4_l`
  (D3D has no gather at a given level), real indirect jumps.

## Loading: what the runs at 21:00-22:15 showed (2026-11-26, late night)

* **The title's watchdog** (+0x2215a80, a thread of its own) ends the run (+0x23cf620 writes to
  address 0) after 30 one-second ticks without a main-loop heartbeat, or when a tracked job runs
  past its limit. The budget is now 600 s, patched in memory (`AMERICAS_TITLE_WATCHDOG_S`).
  With it, the run to `to-past-stall.txt` lasted its full 20 minutes: 110,000 DirectStorage
  requests (7.6 GB, the most so far), no crash.
* **On screen:** the loading screen's background and its spinner (bottom right); no artwork.
  Frames crawl during loading: 20 frames in a minute between captures 3 and 4. At the end
  (hang.txt at the timeout) **no thread was running**: the storage workers were idle, the main
  thread waits on a semaphore (+0x23cf6a0 calls +0x23ceeb0 -> +0x569f0, INFINITE; that is its
  normal state: it hands the frame loop to other threads). So loading stalls; it is not
  CPU-bound.
* **DirectStorage is not the bottleneck:** 7,000, 25,000 and 13,000 requests in the three
  minutes between captures (up to 1.4 GB a minute), then about 0.25 GB a minute, with its
  four workers idle at the end. The title asks at the pace of its loading loop.
* **The ray-tracing stand-in probably hangs the GPU.** DXC refused two raytracing programs with
  "Loop must have break": with the stand-in's constant answers their traversal loop can never
  end. Others compiled, and their loops may only end on data the stand-in never gives. The first
  run after the stand-in lost the device (`0x887a0007`, DXGI_ERROR_DEVICE_RESET), and later runs
  had "lists still running after 2 s". Next: read one traversal program (am-shaders disasm
  <marker>) to see what ends its loop, and give answers that end it; or a real intersection.
* **Pipelines made at draw and dispatch time block the title's recording threads** (the driver
  compiles every new pipeline the first time; hang.txt showed it inside amdxc64 under a draw).
  Dispatches now wait at most 5 s a pipeline; graphics pipelines are still made inline.
* **Service hand-over is too polite:** a replaced service finishing its compiles can take an
  hour on the largest (6 MB of HLSL took 887 s); the replacement waits idle meanwhile. Better:
  the new one starts at once and skips what the old one is compiling.
* Smaller items seen in the console during loading, not done yet: device slot 71 (prebuild
  sizes), command list slots 85/86 (BVH build and copy) and 101, XG texture computer slot 9,
  "a shader used a texture we did not make" (all-zero T#s), the 2.1 GiB allocation at
  0xdc9600000 that cannot be opened for the GPU.

## Batched fixes, and the loading loop at full speed (2026-11-26, near midnight)

The user asked for fixes in batches with one test run each (runs take 10-20 minutes, because
loading does). Work that way.

* **Batch 1** (b43a52d): ray-tracing stand-in answers zero child pointers (the game's
  traversals push children above 7, so 0xFFFFFFFF children were pushed forever); console
  pipelines (slot 57) are made on two threads of their own (draws skip until made);
  `image_gather4_l` (four SampleLevel reads at texel centres); a replacement shader service
  starts at once (`dxil\rt5\compiling\<marker>` holds the compiling process id). Result: the
  loading loop runs at about 27 fps (was 20 frames a minute) and the screen stays on the loading
  background and spinner; later in the run, dispatches of three loop-free compute programs
  (549c..., f714..., 3061...) ran for seconds and the device was lost (`0x887a0007`) again.
* **Batch 2:** pixel inputs skip system values in the record and match two-attribute registers
  by component mask (11 programs); a program refused by the same am-shaders build is not tried
  again (failures.tsv's fourth column); GPU-visible allocations over 1 GiB are made of 1 GiB host
  allocations (D3D12 would not open the 2.1 GiB one; vertex and index buffers in it were bound
  null); dispatches over 2^20 groups are logged with their size and program, and ones over
  D3D12's 65,535 a dimension skipped. A loop-free program running for seconds means a huge grid:
  check those log lines first.
* **Shader numbers (rt5):** 12,358 programs compiled; the game asked for 182 in the runs so far,
  180 compiled (the two far-jump giants are not). Mesh (9,282) and geometry (195) programs are
  not translated at all yet: needed once the game draws its world. Every translator version
  bump recompiles all of rt<N> (2+ hours); avoid bumps for additions that do not change existing
  output.
* **Device slot 71** is not the prebuild query: it fills two descriptions (one with 0x20004 at
  +0x30) and passes a descriptor handle ([r12+0x18]); its result is not read. Probably a view
  creation (ray-tracing acceleration structure view?). Not implemented.

## A GPU hang took the display down; runaway guards (2026-11-27, just after midnight)

* **What happened:** a test run hung the GPU at boot (frame 3) and the display driver did not
  recover: the user had to reset the PC. The only new GPU work in that build was the UAV-clear
  fill program; most likely a descriptor read wrongly gave it a range of billions of dwords.
  Earlier runs had GPU hangs (DXGI_ERROR_DEVICE_HUNG / RESET) that Windows did recover from.
  **Every GPU hang can take the display down: treat them as the most serious bug there is.**
* **Guards now (b126faa):** every structured loop of a translated program ends the program after
  4M passes per thread (AM_SPIN_LIMIT), the dispatcher after 16M (AM_DISPATCH_LIMIT); UAV clears
  only over a range inside the cleared buffer's own memory, at most 256 MiB, with a bounded
  loop. Programs compiled before the guards that can loop (5,979 of 12,393; `am-shaders
  guard-loops` counts them) are compiled again by the service, the game's first, the rest when
  nothing else is left; `dxil\rt5\guarded.txt` lists the guarded ones, and old DXIL serves until
  the new one is written.
* **The hang DRED named** in the runs before: `85d880d595abff03` (one workgroup, after
  `f5cf80d572fdfe03`, 8 groups). Two loops: one over indices in steps of 1,024 up to a loaded
  count, one following a linked list in a buffer until 0. Both run away on wrong data. The
  hangs came when video memory was over budget (14 GiB of 13), before textures were dropped.
  Guarded now; if it hits the guard, its input is wrong (look at what f5cf writes, or what
  clears or copies it before).
* **Textures dropped over reused memory** (d42efb7): 3,900 alive instead of 35,000; video memory
  5.8 GiB at frame 4800 instead of 14.3. Commit still rose to 27 GB after frame 4800 in one run
  (cause not found: the memory line in the frame summary shows each kind of host resource).
* **Compile speed:** the largest programs (1-6 MB of HLSL) take DXC 1-15 minutes each, one thread
  and 3-4 GB each, so memory lets only one or two run at once and the CPU idles behind them.
  The lasting fix is smaller HLSL from the translator (registers as SSA values, fewer copies).
* **Running the game for tests:** start it with `cmd /c start "Project Americas" americas.exe
  --timeout 1200` (a console window the user can watch; the service it starts then gets a window
  of its own) and watch its commit: end it past ~24 GB on this 16 GB PC (one run reached 36 GB
  and the system 63 GB of commit).

## Memory on a 16 GB PC; shaders on demand (2026-11-27, night)

* **How the user wants shaders handled:** translate once, keep forever, compile on demand. The
  runtime lists what a run needs (wanted-first.txt); `am-shaders serve` compiles that, most
  needed first, and nothing else (`serve <jobs> all` restores the old "everything when idle").
  Don't make changes that force recompiling programs the game has not asked for.
* **When stuck, look at how the reference furthest along did it** (it reaches the game world);
  if their method is smart, use the same method in our own code; if not, work it out. It found
  this session: list slots 85/86 are CopyTextureRegionX/CopyResourceX; device slots 70/71 create
  SRV/UAV views of placed memory `(resource desc, view desc, handle)`; BVH node pointers are the
  node's address in 8-byte units with the kind in the low 3 bits (0-3 triangle, 4 box of f16
  bounds, 5 box of f32 bounds; a box answers its hit children nearest first, then -1); and they
  too release write guards before opening memory for the GPU, and keep a pipeline library.
* **GPU windows:** D3D12 refuses OpenExistingHeapFromAddress for an allocation with any
  read-only page (the texture write watch makes some). Watches are released first now; the
  2.1 GiB allocation opens (window 3). OpenExistingHeapFromAddress1 (device 13) can open a
  sub-range instead, if whole allocations ever become a problem.
* **Pipeline library** (`cache\d3d12\host-pipelines.bin`): the second run loaded 45 of 46
  pipelines instead of compiling them.
* **Memory, where it goes** (the memory line, now also from a 2 s timer while commit grows):
  the title's own pages (7 GiB pool, touched as it streams in), our texture copies (now under a
  budget: past 60 % of the GPU's local budget the oldest tenth is dropped), and **the driver's
  pipelines**: during loading, with frames stalled behind 18 dispatches waiting 5 s each for
  pipelines, commit rose about 1 GB every 2 s while textures, video memory and the title's pages
  stayed flat; one 73 KB compute pipeline added 388 MB. Most likely scratch memory: our programs
  keep the console's registers in arrays (v[], s[]), so large ones spill, and the driver reserves
  scratch for every thread the GPU can run. **The fix is the translator's registers as SSA
  values** (no register arrays): programs fit in registers, no scratch, much faster DXC compiles
  too. That is the next big task. Until then runs end around 24 GB on this PC (the watch in the
  test command ends them; one run with no limit reached 36 GB and took the system to 63 GB).

## Shader service idle work; register arrays are not the memory problem (2026-11-27, late)

* **Service:** `am-shaders serve [jobs] [demand]`: the game's requests (wanted-first.txt) first,
  always; when nothing waits, the rest of what its pipelines need plus guard upgrades, on all
  workers but two (kept free for the game's next request). `demand` = requests only. Built and
  running (process started 01:10, `cache\shaders\service\am-shaders-165e00-...exe`).
* **The "registers as plain values" rewrite is not worth doing.** Tested: DXC already turns the
  `s[]`/`v[]` arrays into plain values at -O3. The 73 KB compute pipeline blamed for +388 MB has
  no `alloca` in its DXIL; a 769 KB PS converted to scalar registers compiled 10.9 s → 10.5 s,
  same peak memory (~250 MB), same DXIL size. Not done; no translator output changed.
* **Where stack (scratch) memory does appear:** 153 of 400 sampled DXILs have `alloca`s:
  (a) small `[4 x i32]` helper arrays indexed by a lane-derived value (`w[x >> 5] |= ...`) —
  find that helper and replace the dynamic index with selects; (b) the huge programs compiled
  with `-O0` (2-3 MB DXIL, hundreds of `alloca i32`): the driver must optimize those itself.
* **The +388 MB sample was noisy:** it overlapped opening the 2.1 GiB GPU window, and it is the
  only pipeline logged in that run. Next: run the new test
  `AM_PIPELINE_MEMORY=C:\Project Americas\cache\shaders\dxil\rt5 am_tests d3d12x_driver_pipeline_memory`
  (makes the largest compute programs into pipelines, logs commit/GPU memory around each, never
  dispatches). Its first run was cut off with the session; result unknown — rerun it.
* Still open: loading screen art, device slot 71 (placed UAV), mesh/geometry shaders, real
  raytracing intersection.

## Transient DXC failures were recorded as refusals (2026-11-27, 01:30)

* The service run started 01:10 logged 12 "refused: ... DXC: the compiler failed" in one burst
  (between ordinary compiles), around when the previous session crashed. That text means
  DXC's Compile call itself failed (no compile error; most likely memory), yet it went into
  `failures.tsv` stamped with the build, and the service never tries a program its own build
  refused: momentary failures became permanent until the next build.
* **Now:** `CompileResult::transient` marks a failed Compile call (with its HRESULT in the
  message); the service tries again after 5, 20 and 60 s, and if it still fails, leaves the
  program for the next service run without recording a refusal. The 12 lines were removed from
  `failures.tsv`; the new service (01:33, pid 14908) compiled the first of them at once.
* Cache housekeeping, not done (asks the user first): `cache\shaders\dxil` still holds 2,071 loose
  DXIL files (1 GB, the scheme before rt<N> folders), `rt\` (old) and `rt4\` (1.1 GB, the
  previous translator); the runtime reads only `rt5\` and `by-source\`. Old service copies in
  `cache\shaders\service\` and `bin\am-shaders-old*.exe` / `-pass1.exe` can go too.
* **Progress line:** every compile the service logs ends with `[N left; about Hh MMm to go]`
  (pace of the last 40 compiles in program code, over the code still queued).
* **Visible service window:** `C:\Project Americas\tools\shader-service.ps1` runs the service in
  a console window of its own and appends its output to `logs\serve.txt` (UTF-8). Started from a
  tool call it must go through WMI (Win32_Process Create, ShowWindow 1), or it dies with the call.
* **Far-jump giants no longer refused:** DXC (clang) allows 256 nested brackets and has no
  `-fbracket-depth`. The structurer now has a chain form, used only when the nested form is
  too deep: a node with many merge points emits its region and each merge point's region one
  after another (`am_go<n>` says which runs). `24ea...`: chained structured, nesting 61, 15 MB
  of HLSL. `am-shaders translate <48-digit marker>` translates and compiles one program as the
  service does and prints form, size, nesting and compile time.
* **Device slot 71 = CreateUnorderedAccessViewX** (resource description, UAV description with the
  GPU address at +40, destination descriptor), the twin of slot 70: buffers get a V# over the
  range, textures the view slot 70 would make. The call site shows it: Flags 0x20004 (UAV) in the
  resource description, the view description's +40 the address.

## The commit runaway: unoptimized giants in AMD's driver compiler (2026-11-27, 02:30)

* **Found:** during loading commit rose ~1.7 GB every 2 s with the working set flat. The memory
  timer now walks the address space when commit grows (`memory: commit grew most in ...`): ~100
  new private 15.8 MiB blocks every 2 s. With `AMERICAS_WATCH_COMMITS=1` (imports of
  RtlAllocateHeap and VirtualAlloc in every loaded module watched) the stacks name
  **amdxc64.dll**: AMD's shader compiler, building our pipelines.
* **Why:** programs over 1 MiB of HLSL were compiled with `-O0` (to halve DXC's time); the game
  asks for several (1.4-2.2 MB of DXIL). Measured on one (`ca8f...`) with the pipeline test
  (`AM_PIPELINE_MEMORY=<dir> am_tests d3d12x_driver_pipeline_memory`; `AM_PIPELINE_MEMORY_BELOW=
  <KiB>` limits sizes): **-O0 DXIL (2.27 MB): the driver passed 20 GB and never finished;
  -O3 DXIL (1.2 MB, DXC 55 s instead of 27): 6.8 GB at its peak, 80 s, 87 MB kept.** Game-sized
  pipelines (50-200 KB) cost the driver little (+-90 MB) but 4-8 s each.
* **Fix:** the service compiles everything optimized. Existing DXIL over 1 MiB whose .src key
  lacks "-o3" counts as unoptimized: compiled again, the game's requests first, then before the
  guard upgrades. The runtime builds a pipeline over 1 MiB of DXIL alone (both driver slots).
* **The smarter method, for later:** the reference furthest along does not use DXC for
  translated shaders at all: its own SSA form, an optimizer and an LLVM-bitcode (DXIL) writer
  (~37,000 lines in all). That removes DXC's minutes per program and hands the driver clean
  code. A big task; worth it once the game runs.
* Next: a game run once the game's giants are optimized (watch `waited for:` in the service
  window), to see loading past the old 24 GB wall.
* **Run after the fix (02:32):** commit peaked at 18 GB and fell back to 17 (no runaway; before:
  past 24 GB). Loading then stalls at frame 3,581 / 80,000 requests on the same unmet waits
  ("never written by a list of ours"), with `24ea...` and `3db4...` (far-jump giants, chained
  form) not compiled yet: still the prime suspects for the writer.
* **Giants on demand only:** idle work had taken the one large-compile slot for unrequested 4-5 MB
  programs while those two waited. Now unoptimized DXIL is recompiled only when the game uses
  it (the runtime treats large DXIL without an "-o3" key as missing and asks for it), and a
  large program the game waits for goes first in the large-compile queue.
* **Compile heartbeat:** DXC reports no progress. The service now prints `compiling large ...` when
  a large compile starts and, every 5 minutes, `still compiling ... : N min so far` for compiles
  over 3 minutes, plus `waiting for the large-compile slot` for giants the game waits on. In the
  build after 02:43; the service running then (pid 12512) does not have it and was left alone,
  so the far-jump giants keep the time they already had (34 min at 03:17).

## Unimplemented console calls, batch (2026-11-27, ~03:30)

Named from the furthest reference's method tables (this title uses its "newer" list layout:
list slots from 59 up are two later than in older titles).
* **List 80 DispatchX:** with initiator flag 0x20 the counts are threads, not groups. We took
  them as groups, so the structure builder's dispatches (flags 0x8020) ran up to 64 times their
  groups and wrote past their own structures. Now groups = ceil(threads / numthreads); the group
  size comes from the compiled DXIL (PSV0 runtime info v2: NumThreads at +36).
* **List 28/29/30 Begin/End/ResolveQueryData:** begin/end taken; resolve writes plausible results
  into the title's buffer after the list has run (occlusion 2^20 samples, binary 1, timestamps
  from the GPU clock, statistics zero). **List 101** DriverHintX (0), **129** DispatchMesh (taken
  and counted: mesh shaders are not translated).
* **Device 39** CreateQueryHeap and **41** CreateCommandSignature return objects (were null
  out pointers); signatures keep stride and arguments for ExecuteIndirect, which the host does
  not run yet. **Device 61** CreateSamplerX (27-dword console description) writes a sampler
  record. Device 74/75 register/unregister the page pool (left as probes).
* **XG computer 6** ElementOffset (CPU texel reads; a wrong answer made the title read wild
  memory in the reference) and **9** CopyFromSubresource, both on the same addressing the
  uploads use (`texture_element_offset`). **SetName** (slot 6) on root signatures and pipelines.
* Not done: command allocator slot 9, ExecuteIndirect itself, mesh shaders.
* **Prebuilding giant pipelines** (`AM_PREBUILD=<markers> AM_PREBUILD_CACHE=C:\Project Americas\cache
  am_tests d3d12x_prebuild_compute_pipelines`; the runtime's pipeline description, saved into
  `cache\d3d12\host-pipelines.bin` for game runs to load). Tried on `3db4...` (13 MB of optimized
  DXIL): **the driver passed 26 GB of commit and was stopped unfinished** (commit limit here 34 GB).
  These far-jump giants are too large for AMD's compiler on a 16 GB PC; the way on is smaller
  DXIL (translator output per instruction), or a much larger page file for a one-time build.
* **Again with a 48 GB page file (commit limit 63.9 GB):** the driver building `3db4...` grew
  slowly to 4 GB over 6 minutes, then jumped past 49 GB within two minutes and was stopped. No page
  file size makes this practical. The fix has to be smaller DXIL for the far-jump giants (the
  translator's output per instruction), or keeping them out of the driver some other way.

## Step 3, round 1: lean memory access (2026-11-27, ~05:30)

* **What the giants are made of** (`3db4...` DXIL disassembled): 1.82 M instructions in 316,743
  basic blocks; 17 % branches, 11 % compares, 33,648 raw buffer loads and as many descriptor-handle
  creations. The giant is mostly scalar code (s_or/s_add/s_and/s_mul, 10,183 scalar loads).
* **Cause found:** every title-memory access (`am_load`/`am_store`) had two early-return branches and
  picked its window with `NonUniformResourceIndex`, which makes AMD's driver wrap each access in a
  per-lane loop. The reference furthest along (and its 16 GB fork) does memory access with no
  branches: the page index is clamped, a page without memory reads through a null view.
* **Now:** a null raw UAV at the heap's top slot (`shader::kNullMemoryView` = 65535; texture views
  start below it); `am_entry` clamps instead of returning, a page without a window selects the null
  view; scalar loads (`s_load_dword*`, `s_buffer_load_dword*`) use `am_sload/2/4` with a uniform
  index, 2 or 4 dwords per load within a 64 KiB page. No version bump: existing DXIL keeps working.
* **Measured on `ca8f...`** (1.5 MB HLSL, game-requested): branches 17,632 -> 12,009; DXC 55 s -> 23 s;
  driver pipeline build 80 s -> 50 s, **peak 6.8 GB -> 4.5 GB**; DXIL +5 %.
* Next rounds: the per-instruction `if (AM_ACT)` guards on vector writes, flags (scc) written and
  never read, the chained form's `am_br` unwinding; then the far-jump giants in the driver again.

## Step 3, round 2: vector loads without branches (2026-11-27, ~06:00)

* Vector buffer and global loads (and sub-dword buffer loads) run for every lane; only their
  register writes follow EXEC (`if (AM_ACT != 0u) v[n] = x;`, which DXC turns into a select).
  Stores keep their lane test. Bounds use HLSL 2021's `select()` (its `?:` short-circuits into a
  branch). `am_fmt_load` reads its dwords unconditionally and selects; out of range still gives
  all zeros.
* **`ca8f...` after rounds 1+2:** branches 17,632 -> 9,494; **driver peak 6.8 -> 3.4 GB**, build
  80 -> 37 s; DXC 55 -> 23 s; DXIL size unchanged. Next: the far-jump giant `3db4...` with this.

## Step 3, round 3: helpers without branches (2026-11-27, ~05:20)

* `am_sload2`/`am_sload4` no longer split a group that crosses a 64 KiB page: a window maps a whole
  allocation, so the group reads on in the same window (past the window's end a raw load reads 0).
* The bindings lookup (`am_bind_slot`/`am_bind_find`) reads both probes and picks with `select()`
  and `and()`; the buffer bound (`lim`) uses `select()`. HLSL 2021's `&&`, `||` and `?:` branch.
* **`ca8f...` after rounds 1-3:** branches 9,494 -> 5,925 (17,632 at the start); instructions
  157 K -> 118 K; **DXIL 1,203 -> 853 KiB**; DXC 23 -> 17 s; driver build 37 -> 25 s, peak about
  2.0 GB of private bytes (sampled from the test process every 250 ms).
* Measuring tip: the driver caches pipelines by DXIL, so a second build of the same DXIL takes a
  few ms and shows nothing; compare fresh DXIL only.

## Step 3, round 4: in progress at session end (2026-11-27, ~05:35)

* **Not applied yet** (translate.cpp is unchanged since `fa77a39`). The edit script is
  `C:\Project Americas\tools\step3\round4.py`. Its one bug: the `WaveActiveBallot(cy && AM_ACT
  != 0u)` line with 6-space indent occurs 3 times, not 2. Set that `rep(..., 2)` to 3 and delete
  the next `rep` (the 8-space variant), then run `python -I round4.py`, build, test, re-measure.
* What it does: `Ctx::vwrite` emits `v[n] = select(act, e, v[n])` instead of `if (act) v[n] = e;`
  (`guard=true` keeps the branch for the `ds_read_b32` LDS read). `act()` with DPP uses `and()`.
  Selects replace the guarded writes in the carries, permlane, `v_mad_u64_u32` and buffer/global
  loads. Ballots use `and(cond, act)`. Compares lg/o/u/nlg, the legacy mul/fma, cndmask,
  cvt_i32/u32_f64, ffbh, s_flbit, s_cselect and `scc ? 1u : 0u` drop `&&`, `||` and `?:`.
  The atomic `4u <= lim ? am_atomic(...)` stays a branch (side effect).
* Why: in ca8f after round 3, 1,985 of 3,347 conditional branches are `AM_ACT` lane guards
  (1,318 single guarded writes plus 887 merged runs). The ca8f HLSL still has 2,194 `||` and
  2,540 `?:`. Check whether `merge_lane_guards` tests need updating.
* Tools: `tools\step3\brsrc.py <file.ll>` classifies branch conditions (get the .ll with
  `C:\VulkanSDK\1.4.363.0\Bin\dxc.exe -dumpbin x.dxil`). `tools\step3\peak.ps1 -Dir <dxil dir>`
  runs the driver pipeline test and samples peak private bytes (fresh DXIL only; the driver
  caches).
* **Running at session end:**
  * The 3db4 giant is compiling with round 3 (`cache\shaders\bench\am-shaders-r3.exe translate
    3db4... --raw <scratchpad>\r3-3db4`). The scratchpad may vanish; rerun it into a durable
    folder if needed, then run `peak.ps1` on it with `-StopGB 46`. That tells whether the far-jump
    giants now fit in the driver (before: >49 GB).
  * The old service (pid 12512, build 0243) is still compiling 24ea in the old bloated form. It
    will need recompiling with the lean translator anyway.
* Next: finish round 4 → measure ca8f and 3db4. If the giants fit, restart the shader service
  with the new build and make 3db4/0849/24ea recompile (their old DXIL must be removed or
  rekeyed; the cache cleanup needs the user's OK). Then run the game (black world, stall near
  frame 2,786 with 8 unmet GPU waits).

## Translator 6: lanes as threads, one rebuild of every shader (2026-11-27, ~05:50)

* **The rule (the user's, and it stands):** the translator translates, it does not emulate. A
  console lane is a host thread running the same program. Translator 5 still emulated the wave's
  EXEC: a ballot for every compare and carry, 64-bit mask arithmetic, a lane-bit test around
  every vector write, uniform branches on wave-wide tests. That is what made the giants explode
  in DXC and in AMD's driver.
* **Now (translate.cpp, "lanes as threads"):** a lane mask (EXEC, VCC, compare and carry results,
  saved masks) is the lane's own bit spread over the register (all ones / zero, both dwords).
  The console's bitwise mask instructions keep their formulas; execz / vccz / the SCC of a mask
  operation become the thread's own test; compares write `0u - uint(cond & exec)`. A pass over
  the program's CFG (`mask_kinds`, from the decoder's operand tables) knows which SGPRs hold
  masks: a mask used as a number (s_bcnt1, mbcnt, v_mov to a VGPR, s_cmp...) is made whole with a
  ballot there (`am_whole_*`), a number used as a mask (loaded from memory) gives the lane its
  bit (`am_spread64`, `am_lane_bit`). A program whose register holds a mask on one path and a
  number on another keeps the wave's masks (`Translation::lanes` false; the old code path is
  still there). All 28 translator tests run lanes-as-threads, none fall back; 109 tests pass.
* **Also in this batch:** vector writes are `select()`s (no branch per instruction; the compiler
  drops them where the lane is known on); no `&&`/`||`/`?:` in emitted code or helpers (HLSL 2021
  branches on them); multi-dword loads are one `am_load2/3/4` (one window lookup, not one per
  dword); typed-buffer helpers no longer index a vector by a value (`raw[o >> 5]`, `c[sel - 4]`,
  `data[o >> 5]`: those were the stack arrays (scratch) found in 153 of 400 DXILs); s_bitset_b64
  no longer indexes s[] by a value; implicit derivatives are `ddx_fine`/`ddy_fine` in pixel
  shaders and quad reads in compute (were four cross-lane reads each). Pixel shaders start with
  helper lanes off (`IsHelperLane()`), as the console does. `kTranslatorVersion` 6: everything is
  compiled once more into `dxil\rt6` (the game's lists `cpipe-index.tsv` and `wanted-first.txt`
  were copied over from rt5; they do not depend on the translator).
* **Measured on ca8f (game-requested compute, 1.6 MB HLSL), translator 5 round 3 -> 6:**
  conditional branches 5,925 -> 1,172; DXIL instructions 118 K -> 81 K; DXIL 853 -> 565 KiB;
  ballots and stack arrays 0; **DXC 17 -> 8.1 s; AMD driver build 25 -> 7 s, peak 2.0 -> 0.83 GB**
  (at the start of step 3: 80 s, 6.8 GB; the -O0 form passed 20 GB). The service compiles about
  60 programs a minute at first; all 12,907 of the game's pipelines' programs were estimated at
  4-5 h (translator 5: about 8 h for 1,700).
* **Running at session end (05:50):** the shader service on translator 6 (window "Project
  Americas shader service", `tools\start-shader-service.ps1` starts it from the current build;
  `tools\shader-service.ps1` is what runs in the window). The far-jump giant 3db4 benchmark
  (`tools\step3\bench-3db4-r6.ps1`, log `cache\shaders\bench\r6-3db4\log.txt`): DXC of its 16.6 MB
  of HLSL, then the driver's build with its peak (stops past 40 GB; translator 5 passed 49 GB).
* **Left in the translator (not emulation of EXEC, but run-time decisions worth moving out):**
  images look their T#/S# up in the bindings hash table per block and pick the view type (float,
  uint, sint; array or not; SRV or UAV) at run time, so every image instruction carries up to six
  typed paths; typed buffer loads decode the V#'s format at run time (`am_fmt_*`). Both are tied
  to how the runtime binds resources; the reference furthest along traces descriptors to their
  root parameters at translation time. That is the next large item if image-heavy programs are
  slow. The structurer's chain form (`am_br`, `am_go`) is untouched.
* Cache housekeeping still needs the user's OK: `dxil\rt5` (now unused by the runtime, 3 GB),
  `rt4`, `rt`, loose files, old service copies.

## The giants are typed buffer loads (2026-11-27, ~06:40)

* Only two of the game's programs are giants: **3db4** (16.6 MB HLSL, 3,282 typed buffer loads)
  and **24ea** (15 MB, 1,890). The other entries in `compiling\` were ordinary 170-190 KiB
  programs. With translator 6, 3db4 compiles in DXC in 10 min to 5.4 MB of DXIL (13 MB before),
  but its DXIL is 956 K instructions, of which about 820 K are `am_fmt_load`: each
  `buffer_load_format_*` reads its format from the V# at run time, so every one of the 3,282
  sites carries the whole software format decoder (4 channels x 7 conversions: the DXIL has 200 K
  selects, 39 K int-to-float, 26 K divides, 13 K f16 conversions). That is emulating the texture
  unit, and it is what the driver chokes on: AMD's build of 3db4 swung between 4.6 and 12.6 GB
  of private bytes for 10+ minutes on the 16 GB PC (stopped; total commit had reached 46 GB with
  DXC and the service running too).
* **The fix (the reference's method):** typed loads through typed views, so the GPU's own
  hardware converts the format. In our memory model: when a window opens, the runtime also
  creates typed UAVs of it, one per DXGI format a console buffer format maps to, at fixed heap
  slots (window n, format f -> base + n * formats + f); the translated load finds its window
  through the page table as raw loads do, and reads element (byte offset in window / element
  size) from that view, in the format's class (float, uint, sint; scaled formats convert after).
  32-bit channel formats stay raw dwords (no conversion). Formats without a DXGI twin (10_10_10_2,
  11_11_10) keep a software path. Expected: ~20-30 instructions a site instead of ~250, so the
  giants shrink about 4x, and run faster.
* **The user's question: does it just work with a 48 GB page file?** Measuring: the driver build
  of 3db4 to the end (capped at 45 GB), `tools\step3\driver-3db4-full.ps1`, log
  `cache\shaders\bench\r6-3db4\driver-full.txt` (peak private bytes, system commit at the peak,
  time).

## The giants fit: typed loads lean, V# work done once, wave branches (2026-11-27, ~07:00)

Measured on giant 3db4 (AMD RX 9060 XT, 16 GB PC; `tools\step3\driver-3db4-*.ps1`, logs in
`cache\shaders\bench\r6*-3db4\`):

| translator | DXIL instructions | DXC | driver build |
|---|---|---|---|
| 6 (05:50) | 956 K | 591 s | stopped at 45 GB private (system commit 57 GB), unfinished after 13 min |
| + one channel per output | 523 K | 423 s | stopped at 24 GB |
| + V# parameters where the V# is written | 302 K | 298 s | **done: 263 s, peak 13.7 GB** |
| + uniform window index | 302 K | 256 s | done: 204 s, peak 13.7 GB |
| + wave branches (>32,768 instructions) | (2.4 MB DXIL) | 148 s | done: 317 s, peak 15.7 GB: worse, taken out |
| + one dword per output, 32-bit offsets | **262 K** | 246 s | **done: 167 s, peak 7.6 GB** |

* **Answer to "does a 48 GB page file do it?"** For the 05:50 output, no: past 45 GB and
  still going. Now the giant builds, but 13.7 GB for one pipeline is still heavy for a 16 GB PC.
* **One channel per output:** every one of the giants' typed loads is `buffer_load_format_x`
  (3db4: 3,281; 24ea: 1,890), each read one channel but decoded all four.
* **V# parameters once (`am_tb<r>`, `am_typed`):** all of 3db4's typed loads read V# s[12:15],
  written 7 times; the base, range, format and per-output channel layout are computed after
  each instruction that writes those SGPRs, not at each of the 3,281 loads. A load is now its
  address, one page-table entry, one Load4 and a one-channel conversion (~70 DXIL instructions;
  the software floor). The window index is uniform (one buffer, one allocation): no per-lane
  loop in the driver; this cut the driver's time, not its memory.
* **Wave branches were tried and taken out:** branching on the wave (`WaveActiveAnyTrue` of the
  mask test, the console's exact semantics) for programs over 32,768 instructions made the
  driver worse (15.7 GB, 317 s; larger DXIL). The per-lane branches are not the driver's cost;
  its memory follows the instruction count.
* **One dword per output, 32-bit offsets:** a typed load reads only the dword its channel is in
  (was a 16-byte load and selects) and computes its offset in 32 bits (the console's buffer
  offsets are 32-bit). 3db4: 262 K instructions, 34 non-uniform handles (was ~3,400).
* **Only changed programs are rebuilt:** the typed helpers are included only by programs with
  typed loads, wave branches only in programs over the threshold; every other program's
  source is byte-identical. The service's stale check (06:54): 2,515 of 10,505 compiled
  programs changed (the ones with typed loads, `tbuffer` too), recompiled as idle work after
  the 1,846 not compiled yet.
* **Still to do for speed (would change every program, so not tonight):** raw memory loads
  and stores (`am_load`/`am_store`) index the window with `NonUniformResourceIndex`, which makes
  AMD's driver loop over lanes at every access. Where the address's base is scalar (SGPR base,
  `saddr`, a V#), the window is uniform and the loop can go. Next time translator output may
  change for everything anyway, do this with it.
* **Native typed views** (the hardware converts formats) remain the cleaner design for typed
  loads, but D3D12 caps a typed buffer view at 2^27 elements: large windows need a chain of
  views plus a lookup table. Worth it only if typed-load-heavy programs show up as slow on the
  GPU.
* **Prebuilt:** 3db4's final DXIL is installed in `dxil\rt6` (with its .src key, a by-source
  copy and a guarded.txt line, `tools\step3\install_dxil.py`) and its pipeline built into the
  pipeline library (`AM_PREBUILD=<marker> AM_PREBUILD_CACHE=C:\Project Americas\cache am_tests
  d3d12x_prebuild_compute_pipelines`): a game run loads it instead of building it. (AMD's
  library entry is small: the compiled code lives in the driver's own disk cache.) 24ea: its
  DXC compile of the chained form (17 MB HLSL, nesting 60) takes 7.7 GB and over 15 minutes;
  install and prebuild it the same way once `cache\shaders\bench\r6f-24ea\log.txt` says compiled.

## Next: mesh and geometry shaders (step 4)

The game's pipelines name 18,665 programs; about 5,750 are stages the translator does not
take: mesh (8,765 in the corpus), geometry (195), ES (195). Every pipeline with one of them
cannot be built. What a console mesh program does (from the reference furthest along):
* `s_sendmsg` with message 9 (GS_ALLOC_REQ, counts in M0) asks for its outputs; each lane then
  exports one vertex (`exp pos`, `exp param`) and one primitive (`exp prim`: three 10-bit vertex
  indices, bit 31 culls it). Limits from the program's record: vertices and primitives <= 256,
  threads <= 128, triangles.
* Host form: `ms_6_6`, `[outputtopology("triangle")] [numthreads(n, 1, 1)]`, outputs `out vertices`
  (the record's maxima) and `out indices uint3`; `SetMeshOutputCounts(max vertices, max
  primitives)` at the start (primitives past the program's count culled: written as 0,0,0);
  `exp pos/param` -> `verts[lane]`, `exp prim` -> `tris[lane]`. A barrier on the path only the
  allocating lane takes must go (it would hang the host).
* Runtime: mesh pipelines (stream with MS + the title's PS, no input layout), the user data
  visible to the mesh stage, `DispatchMesh(x, 1, 1)` for list slot 129 (counted today).
* Order: stage I/O of the mesh record (stage_io.cpp) -> translator MS emission + a GPU test that
  draws through a mesh pipeline (gpu_runner) -> runtime pipeline + DispatchMesh -> a game run.
* **Done (07:40): the translator side.** console_program reads the mesh record (all 8,765 of the
  game's mesh programs: 128 lanes, 128 vertices, 128 primitives, triangles, 13-17 user SGPRs);
  stage_io gives their per-vertex outputs as a vertex shader's (per-primitive outputs, PSG1, are
  refused for now); the translator makes `ms_6_6` programs: registers as the console starts them
  (eight system SGPRs, s3 bits 24-27 the wave in the group, user data from s8, v5 = group id x
  128), the console's lane (group lane & 63), `exp pos/param` -> `am_verts[lane]`, `exp prim` ->
  `am_tris[lane]`, `s_sendmsg 9` (the output request) a no-op with the counts set up front,
  barriers dropped in mesh programs without LDS. `am-shaders tsurvey`: all 8,765 translate
  (8,586 lanes as threads); five examples compile with DXC. The shader service maps mesh
  programs now and compiles the game's (restarted 07:38).
* **Next, the runtime:** mesh pipelines (D3D12.X's pipeline stream with a mesh stage: build the
  host stream with MS + the translated PS, the user data root constants visible to the mesh
  stage: the graphics root signature needs a mesh-visible parameter), and list slot 129
  DispatchMesh -> `DispatchMesh(x, 1, 1)`. Then a GPU test drawing through a translated mesh
  program (gpu_runner has graphics draws; add a mesh one), then a game run.
* Geometry (GS 195) and ES (195): not looked at yet; the ES/GS pairs are the old geometry
  pipeline, rarer than mesh.

## Giant 24ea: the chained form (2026-11-27, ~08:15)

* With the typed-load fixes 24ea is 277 K DXIL instructions (like 3db4's 262 K), but its DXC
  compile took 58 min (7.7 GB) and AMD's driver passed **20 GB in 86 s** (stopped). It is in the
  structurer's chained form (nested it would pass DXC's bracket limit): one merge point has 716
  predecessors, 9,829 phis in all. With per-lane branches that is divergent control flow the
  driver must restructure (quadratic in the merge's width).
* **Trying (commit a13362f):** programs in the chained form branch on the wave
  (`CfgBlock::cond_wave`, `WaveActiveAnyTrue` of the mask test; only chained programs change).
  Experiment running unattended: `tools\step3\bench-24ea-r6g.cmd` (DXC then the driver build,
  capped at 20 GB); results in `cache\shaders\bench\r6g-24ea\log.txt` and `driver-full.txt`.
  If the driver still blows up, next ideas: fewer exits per merge point (funnel the branches to
  a merge point through one exit per region), or the chain's merge regions as a loop over
  `am_go` (one join instead of one per chain).
* The translator-6 24ea DXIL (no wave branches) is installed in `dxil\rt6` for now (key
  bd0ba232..): the runtime would hand it to the driver, which on a 16 GB PC needs a big page
  file. The service will rebuild 24ea with the wave branches when it next starts (stale check).
* **Result (09:05): no.** With wave branches 24ea's DXIL grew 2.0 -> 3.8 MB and the driver still
  passed 20 GB (in 145 s). Taken out again. Next idea to try: the nested form instead of chains.
  The depth check (`too_deep`, > 200) counts expression parentheses with the block nesting;
  clang's real limit is 256 brackets, so 24ea's nested form may fit (3db4's nested form builds
  in 7.6 GB). Measure its nested depth first; if under ~250, raise the threshold.

## Afternoon 2026-11-27: a driver crash fixed, mesh pipelines draw, 24ea prebuilt once

* **The service's overnight result:** idle at 18,086 compiled programs; 590 refused, all mesh
  programs over D3D12's 32 KiB of mesh outputs (see below; fixed). Its refusal summary groups
  reasons by DXC's first 60 characters, which are often a warning: the real error is further on.
* **Game run (14:47): AMD's compiler crashed the process** (`amdxc64.dll+0x7380f8`, a null read
  in its `SCInstDataShareAtomic` code) building a loading-screen pixel shader (f99f80d5...).
  Found by keeping each graphics build's programs in `cache\d3d12\building\<thread>-{vs,ps,gs}.dxil`
  (the crashing thread is the one missing from hang.txt) and reproducing with
  `AM_PIPELINE_VS=... AM_PIPELINE_PS=... AM_PIPELINE_LAYOUT=COLOR,POSITION am_tests
  d3d12x_graphics_pipeline_from_files`. The crash needs three things together: `IsHelperLane()`,
  quad reads (the s_wqm translation) and a ballot feeding `WaveReadLaneAt` (v_readfirstlane);
  any one removed builds. Fix: pixel shaders read the first lane with `WaveReadLaneFirst`. 3,338
  pixel shaders compiled before need recompiling (the service's stale check does it): **do not
  run the game until the service is idle again**, or it may build an old one and crash.
* **Mesh pipelines draw** (not tested in a game run yet):
  - translator: mesh user data as vertex shaders take it (the runtime's constants are the SGPRs
    from s0, user data from s8), up to s31 (b0: s0-s19, b1 `am_user_hi`: s20-s31); outputs
    declared at their signature width when float4s would pass 16 rows a vertex (the 590).
  - runtime: `translated_mesh_root_signature()` (MS b0 20 + b1 12 constants, PS b0 20, table u0);
    console pipelines with a mesh program get a host mesh pipeline from a pipeline-state stream
    (`console_mesh_pipeline`); list slot 129 DispatchMesh -> `mesh_draw`: the mesh root
    signature, user data with MESH visibility (`user_data_sgprs`), `DispatchMesh(x*y*z, 1, 1)`.
    The first 8 DispatchMesh calls are logged with their arguments.
  - open: the pixel shader links to narrowed mesh outputs (float2) while declaring float4 inputs;
    if D3D12 refuses such pipelines, the log says `host pipeline for a mesh pipeline: FAILED`.
* **24ea:** its nested form is 261 deep (DXC's limit 256) and 44 MB of HLSL (the chained form
  17 MB): no way out there. Prebuilt once instead (the user's idea: a big page file for a
  one-time build), `tools\step3\prebuild-24ea.ps1`, log `cache\shaders\bench\prebuild-24ea.txt`.

## Our own DXIL compiler: the goto form through it (2026-11-27, evening)

**Why.** DXC is the bottleneck for the largest programs: the far-jump giants (24ea: 44 MB of
HLSL nested 261 deep, past DXC's 256; 17 MB in the chained form, 58 min in DXC, then over 20 GB
in AMD's driver) cannot be expressed as nested HLSL at all, and the structurer's workarounds
(chains, dispatcher) are what blow up the driver. The furthest reference does not use DXC for
translated shaders: its own SSA form and a DXIL (LLVM 3.7 bitcode) writer, ~37,000 lines. Ours
follows the same idea with our own code, in three layers under `src/shader/dxil/`:

* `dxil.h/.cpp` (`Module`): one entry function in blocks. Registers and locals are *variables*
  (`variable/read/write`); `finish()` makes them SSA (pruned phis on the dominance frontier, a
  renaming walk of the dominator tree with explicit stacks, for 100,000s of blocks). Resources,
  cbuffers, dx.op calls, shader flags and the metadata DXIL needs. A variable read before any
  write reads zero (the validator refuses undef operands).
* `bitcode.cpp` (`write_bitcode`): unabbreviated LLVM 3.7 records. `host::assemble_dxil` has
  DXC's assembler wrap them in a container and its validator sign it (no DXC compile step).
  AMD's driver crashes on a module without `llvm.ident`, so we always name the producer.
* `hlsl.h/.cpp` (`compile_hlsl`, ~2,400 lines): the HLSL *subset the translator writes* compiled
  straight into a Module, one pass, functions inlined at each call. One meaning for both paths:
  DXC for programs that nest, ours for any program, with **labels and gotos** (see hlsl.h for
  the accepted subset; anything else is refused with its line, never guessed).
* Translator: `TranslateInput::goto_form` writes the body as `am_block_<pc>:` labels with gotos
  (the console's own control flow, no structurer). Off, the output is unchanged.

**Testing.**
* `dxil_own_module_runs_on_the_gpu` (IR built by hand) and `dxil_hlsl_subset_runs_on_the_gpu`
  (HLSL with a macro, helper, loop and goto) run on the GPU.
* `AM_DXIL=1 am_tests translate_` runs every translator test in the goto form through our
  compiler instead of DXC (GPU results checked the same way). `AM_DXIL_DUMP=<folder>` writes
  each refused program there as `ours-<n>.hlsl`.
* `am_tests` now prints `CRASH <test>: exception ... at ...` with a symbolized stack.

**State: 25 of 27 translator tests pass through our compiler** (all 113 pass with DXC). Fixed
this evening: scopes in a deque (a `Local*` dangled when the scope vector grew while inlining);
`return` reads the function's return slot after evaluating its value (an inlined call in the
value could move `rets_`, which gave wrong results and "a value returned from a void
function"); groupshared and static const arrays are made at first use (the validator refuses
unused internal globals); `asuint(double, out lo, out hi)` (splitDouble).

**Left, in order:**
1. **Images** (`translate_images`): heap textures and samplers are "not yet" in hlsl.cpp
   (Texture1D/2D/3D/Cube and arrays, RW textures, typed buffers; Sample, SampleLevel,
   SampleGrad, SampleCmp, Load, Gather, GetDimensions, CalculateLevelOfDetail).
2. **Irreducible flow** (`translate_irreducible_control_flow`): DXIL needs reducible control
   flow, and the goto form keeps the console's as it is. Split nodes (copy a block entered from
   outside a loop's header) in the translator's goto form or in Module::finish. The test's
   `!structured` check is the structurer's; in the goto form it does not apply.
3. The whole pack through ours: an am-shaders option (goto form + compile_hlsl + assemble),
   coverage and speed against DXC, then the giants (24ea, 3db4, 0849) and their driver memory
   (`tools\step3\peak.ps1`).
4. An optimizer (what DXC's -O3 does for the driver): constant folding, dead code,
   `select(c, x, x)`, common subexpressions, simple phis.
5. The shader service takes ours for programs DXC cannot do, then for all once it matches.

## Our compiler: images, and how much of the game it covers (2026-11-27, late evening)

* **Images in hlsl.cpp:** heap textures of every kind the translator uses (1D/2D/3D/cube, arrays,
  MS, RW) and samplers (comparison too), with DXIL's resource properties as DXC writes them;
  Load, SampleLevel, SampleGrad, SampleCmpLevelZero, Gather*, GetDimensions; `t[at]` reads and
  writes of RW textures; Interlocked* on texels. Also `dot`, local initializer lists, vectors in
  memory arrays (flat), and vector components chosen by a value as places (`r[k] = x`).
* **Shader flags as DXC sets them:** double extensions (double division, fma, int<->double),
  tiled resources (a sample's clamp that is neither undef nor zero, judged after SSA), typed UAV
  loads of more than one component.
* **Tests:** with `AM_DXIL=1`, 26 of 27 translator tests pass through ours with the validator's
  flags equal to ours; only `translate_irreducible_control_flow` is left. All 113 pass with DXC.
* **`am-shaders ours-survey [N]`** (one container in N): every program in the goto form with the
  runtime binding, through ours and the validator; per stage, refusals grouped, flag
  differences, slowest; `cache\shaders\ours.json`. **1 in 20 (1,305 programs, 164 s):**
  - **compute 100 %** (115 of 115 compiled, validated and signed; flags equal). The largest
    (1.6-2.4 MB of HLSL) take 10-15 s in ours where DXC takes minutes.
  - **pixel, vertex, mesh: 0 %**: hlsl.cpp builds compute entry points only (491 refused as
    "an entry point other than a compute shader", 433 mesh by an entry parameter with a
    semantic, 249 by a global outside the subset: the mesh outputs).
  - geometry/ES (16, ~1 %): the translator does not take them yet.
  - **in all: 8.8 % of programs**; the remaining 91 % wait on graphics entry points.

**Next for our compiler, in order:**
1. Graphics entry points: input and output signatures (dx.op.loadInput / storeOutput, the
   signature metadata and system values, SV_Position, SV_Target, SV_Depth, SV_Barycentrics,
   nointerpolation), vertex then pixel shaders (~57 % of programs), then mesh (SetMeshOutputCounts,
   StoreVertexOutput / StorePrimitiveOutput, EmitIndices, groupshared payload; ~33 %).
2. Irreducible control flow (node splitting).
3. The full corpus survey (`ours-survey` with no N, ~55 min), then ours in the shader service
   for compute programs (the giants first: 24ea, 3db4, 0849), measured in the driver.

## Our compiler: graphics entry points (2026-11-27, night)

* **Vertex, pixel and mesh shaders** compile in hlsl.cpp (`compile_hlsl(..., profile)`; the stage
  from the profile, else from the entry). The entry's semantic fields (struct members keep their
  semantics and nointerpolation) become DXIL signature elements as DXC makes them: system values
  by name and kind, component type, interpolation (position 4, float 2, integer and
  nointerpolation 1, VS inputs and PS outputs 0), packed like DXC's prefix-stable packing (in
  order, sharing a row while it fits with the same interpolation; system values a row of their
  own; SV_Target at its index; depth, coverage and barycentrics unpacked; VS inputs a row each).
  Inputs are read at the start (`loadInput`), outputs written at the end (`storeOutput`); usage
  masks come from the loads and stores after SSA (`Module::finish`).
* Pixel: `discard`, `ddx/ddy` (coarse and fine), `IsHelperLane`, `GetAttributeAtVertex`
  (attributeAtVertex on the input's element and column), SV_Barycentrics (flag bit 22),
  SV_Depth. Mutable `static` globals are variables.
* Mesh: `out vertices` / `out indices` parameters are places whose stores become
  `storeVertexOutput` / `emitIndices`; `SetMeshOutputCounts`; the mesh state (tag 9: group, most
  vertices and primitives, topology). Per-primitive outputs and payloads: not yet.
* A read before any write is the type's one zero constant (`Module::zero`), so phis of zeros fold.
* **Tests:** `AM_DXIL=1` also takes the vertex+pixel draw (a triangle drawn with our VS and PS,
  pixels checked) and the depth-export pixel shader through ours. 26 of 27 pass through ours
  (irreducible flow left); 113 of 113 with DXC.
* `AM_SHADERS_OURS=1 am-shaders translate <marker> --raw <dir>`: one program through ours
  (writes its DXIL; says when the validator wants other flags).
* **Survey 1 in 20 (1,305 programs): 97.7 % through ours and the validator** (compute 100,
  vertex 100, pixel 99.8 = every one translated, mesh 97.0). The rest is not ours to fix:
  16 geometry/ES programs (the translator does not take them), 13 mesh programs over the
  32 KiB of mesh outputs (DXC refuses them too: 20 outputs for 128 vertices do not fit however
  packed), 1 untranslated operand. 3 pixel shaders get the tiled-resources flag from a clamp in
  a register that DXC proves zero (constant propagation): the validator's flags are taken.
* Full corpus survey: `cache\shaders\bench\ours-survey-full.txt` (run at the end of this session).

**Next:** our compiler's output is valid DXIL, but only the tests check it computes the same as
DXC's on the GPU (compute, VS/PS draw). Before the service uses it: (1) a GPU comparison of real
programs, ours against DXC's (compute: same inputs, compare memory; mesh: a DispatchMesh draw
test); (2) irreducible flow; (3) an optimizer pass (constant propagation, dead code) for the
driver's sake and exact flags; (4) the shader service compiling with ours, the giants first.
