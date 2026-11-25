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
