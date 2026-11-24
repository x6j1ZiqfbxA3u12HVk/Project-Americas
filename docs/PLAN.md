# Project Americas — plan

Run the Xbox Series X|S build of Grand Theft Auto VI (package version 1.0.2.1) natively on
Windows 11, written in C++.

## 1. The core idea

The Xbox Series X|S CPU is an AMD Zen 2. The game's executable is an ordinary,
already unpacked PE32+ x86-64 image (checked below). **So we do not emulate or recompile any CPU code.**
We load the executable into our own process and jump to its entry point. Its code runs at full
native speed.

What a PC lacks is the console's **system libraries** and its **GPU**. We write those:

* our own implementations of the console-only DLLs the game imports (memory, Direct3D 12.X,
  DirectStorage, audio hardware, input, GDK services);
* a GPU layer that turns the console's Direct3D 12.X work into host Direct3D 12 work;
* a shader recompiler that turns the console's GPU machine code (RDNA2 ISA) into DXIL **once,
  ahead of time**. After that, shaders run as native GPU code like any PC game's.

That leaves three places where our layer costs time at runtime. The performance work in this
plan targets all three:

1. **API calls into our libraries.** These are bound straight into the import table: a normal
   DLL call, no thunks.
2. **Command list translation.** Console command records become host D3D12 commands. This is
   CPU work we add.
3. **The memory-model gap.** The console has one unified memory pool. A PC has system RAM plus
   VRAM behind PCIe.

## 2. Ground rules

1. **The dump is read-only.** `E:\Movies\GTA6.Xbox.v1.0.2.1` is never written, renamed or
   deleted. Every handle to it is opened read-only, and a guard in our file layer refuses any
   write or delete there. Code patches (section 5.1) are made in memory only.
2. **No game data in the repository.** That means no assets, code bytes, shader dumps, extracted
   tables, decompiled code or screenshots. Allowed: our own format code, build identities
   (hashes), and per-version offsets and constants that we write ourselves.
3. **Everything generated lives under `C:\Project Americas`.** That covers build trees,
   binaries, shader and pipeline caches, save data and logs.
4. **The network is off by default.** The game contains a crash reporter that uploads dumps.
   Non-loopback WinHTTP and Winsock traffic fails unless enabled explicitly.
5. **No decryption.** The files are already unpacked. RPF archive tables of contents are
   encrypted, but the game reads its own archives with its own code; we only provide file I/O.
   Offline tools work without the table of contents.
6. **Independent implementation.** Third-party code only when its licence fits and we choose it
   deliberately (list in section 8).

## 3. What the dump contains (surveyed read-only)

| Item | Finding |
|---|---|
| `game_scarlett_master.exe` | 120 MB PE32+ AMD64, GUI subsystem, image base `0x140000000`, ASLR + NX, **no CFG**. `.text` 100 MB of plain (unencrypted) code, ~299,000 functions in `.pdata`, static TLS (5.2 KB template), relocations, retpoline section. 587 imported functions from 68 DLLs, 1 export. |
| Title DLLs | `Bink2Scarlett.dll` (video), `XboxBvhBuilder.dll` (ray-tracing BVH builder on D3D12.X), `amd_fidelityfx_loader_xbox.dll` + `amd_fidelityfx_upscaler_xbox.dll` (FSR 4 for Scarlett), `libHttpClient.GDK.dll`, `XCurl.dll` (network), console VC++ runtime (`msvcp140*`, `vcruntime140*`, `concrt140`, `vccorlib140`, `vcomp140` = OpenMP). All plain x86-64 and runnable natively. |
| Archives | ~209 GB of RPF9 archives (`levels.rpf` 76 GB, `audio.rpf` 28.5 GB, `movies.rpf` 8.8 GB, `shaders.rpf` 412 MB …), texture archives `data/ttdb/**/*.ttpf` (`RDS5` header), `pack/` chunks including prebuilt BVH archives (`*_bvh4.rpf`). |
| Shaders | `shaders.rpf` holds **26,503 shader records** (pixel, mesh/NGG, compute, vertex, geometry). Each record is raw deflate around a DXBC container (`SFI0`, `ISG1`/`OSG1` signatures, `PSV0`, `HASH`, `DXIL`, `PRIV`). The `DXIL` part holds **console data (`XDAT`…), not LLVM bitcode**, so recompiling the GPU code is required. |
| `MicrosoftGame.Config` | Title id `7952987A`, executable `game_scarlett_master.exe`, persistent local storage 2048 MB, SMT on, "Advanced" title memory on both Series X (Anaconda) and Series S (Lockhart). |
| `gameos.xvd` | The console OS image. Not needed. |

### Console-only DLLs the game imports (we implement these)

| DLL | Imports | Role |
|---|---|---|
| `d3d12_xs.dll` | `D3D12XboxCreateDevice`, `D3D12ConfigureVirtualMemoryX`, `D3D12SerializeVersionedRootSignature` | Direct3D 12.X: the entry to the entire GPU API |
| `xg_xs.dll` | `XGCreateTextureComputer`, `XGSuggestSwizzleMode` | Texture memory layout computer |
| `xmem.dll` | 12 (`XMemAlloc*`, `XMemAllocatePhysicalPages`, `XMemMapPhysicalPages(Scatter)`, …) | Console memory manager |
| `XCRDAPI.dll` | `DStorageGetFactory` | Console DirectStorage |
| `AcpHal.dll` | 9 (`AcpHalCreate`, shape contexts, `Apu*` heap) | Audio control processor: hardware decode / resample / mix "shapes" |
| `xapu.dll` | 5 (`XApuConnect`, enqueue / dequeue) | Audio processing unit command queue |
| `chad.dll` | 5 (`XDsp*`) | Audio DSP (effects) |
| `GameInput.dll` | ordinal 1 | Console GameInput |
| XGameRuntime | loaded at runtime (`QueryApiImpl`) | GDK services: user, saves, package, store, system, task queues… |

Windows API sets also exist on a PC, but some need console behaviour on top: memory
(`VirtualAlloc` with console flags), files (console paths), process topology (affinity),
`psm-appnotify` (app lifecycle), library loader (console DLL names), sysinfo (memory status),
WER, WinHTTP / Winsock (network policy), and the `ext-ms-win-rtcore-ntuser` window functions.

## 4. This machine, and what it means

| Part | Found | Consequence |
|---|---|---|
| CPU | Intel i7-7700K, 4 cores / 8 threads (Kaby Lake) | (a) Console code assumes Zen 2 and may use instructions this CPU lacks: `CLWB`, `CLZERO`, `RDPID`, `RDPRU`, `MONITORX`/`MWAITX`, SSE4a (`EXTRQ`/`INSERTQ`/`MOVNTSS`/`MOVNTSD`), SHA. A byte scan is inconclusive, so a proper disassembly census is task M1.1. (b) The game is built to keep ~7 Zen 2 cores busy at 30 fps. This CPU has roughly half that multithreaded capacity. **In busy scenes the CPU, not our layer, will likely set the frame rate.** Our job is to add as close to zero CPU cost as possible and to schedule the game's workers well over 8 logical processors. |
| RAM | 16 GB, page file 16 GB (commit limit 31.9 GB, of which other programs held 23.4 GB during the M0 probe) | Series X title memory reaches ~13.5 GB, plus Windows and our process. **Start by presenting the console as a Series S**: the game then budgets itself to the Series S memory size, a mode it was built and tested for. Series X mode comes later, with a larger page file. |
| GPU | AMD Radeon RX 9060 XT 16 GB (RDNA4): feature level 12_2, Shader Model 6.8, waves 32–64 lanes, mesh shaders, ray tracing 1.1, 64-bit atomics, enhanced barriers (measured by `am-probe`) | The target. RDNA4 helps a lot: the same vendor's wave semantics as the console, native wave64, mesh shaders, FSR 4. Adapter selection is explicit and rejects software adapters. (A GTX 1070 entry seen earlier was the leftover driver of a card no longer installed; it was removed.) |
| Platform | Z270-era board: PCIe 3.0 x16 to the GPU. **Resizable BAR is unavailable: `am-probe` reports no GPU upload heaps.** | The CPU cannot write most of VRAM directly. So unified memory is emulated by keeping GPU data in VRAM and uploading only what changes (section 5.2). The design does not depend on Resizable BAR; where it exists, it is a bonus. |

## 5. Architecture

```
 E:\...\GTA6 dump (read-only)            C:\Project Americas
 ────────────────────────────            ───────────────────────────────────────────
 game_scarlett_master.exe ─┐             bin\americas.exe  (launcher + loader)
 title DLLs ───────────────┤                 │ maps images, binds imports, patches CPU
                           │                 ▼
                           └──────────► title code (native x86-64)
                                             │ imports
               ┌─────────────────────────────┼──────────────────────────────┐
               ▼                             ▼                              ▼
        runtime (kernel, xmem,         gdk (XGameRuntime            gpu (D3D12.X → host D3D12)
        files, threads, net policy)    services)                       │       │
               │                             │                         │       ▼
               ▼                             ▼                         │   shader (RDNA2 → DXIL)
        host Windows                  saves in userdata\               │       │
                                                                       ▼       ▼
        audio (AcpHal/XApu/XDsp → WASAPI)  input (GameInput)      RX 9060 XT   cache\shaders
        storage (DirectStorage, zlib, BCPack)
```

### 5.1 Loader (`src/loader`)

* **Mapping.** Map the executable at its preferred base `0x140000000` (the launcher is built
  elsewhere so that range stays free), apply relocations, and set section protections. Title
  DLLs are mapped the same way.
* **Import binding.** A routing table decides, for each `dll!function`:
  1. our implementation;
  2. else the host function (allowed DLLs only);
  3. else a title-shipped DLL;
  4. else a generated stub that names itself when called (trap or return 0, configurable).

  Function pointers are written straight into the import table, so every call costs exactly a
  normal DLL call. Delay imports, ordinals and forwarders are handled too, and so are
  `LoadLibrary` / `GetProcAddress` for console DLL names (XGameRuntime is loaded that way).
* **TLS.** The executable's code reaches its static TLS as slot 0. The launcher has no TLS of its
  own and reserves slot 0 as a carrier block, at least the size of the game's template, which is
  filled with that template. The game's TLS access is then exactly what it is on the console,
  with no code patching. Title DLLs that need TLS get small "carrier" modules of their own. A
  thread-start wrapper runs TLS callbacks and thread attach/detach for title modules.
* **Exceptions.** `RtlAddFunctionTable` for each image's `.pdata`, so C++ exceptions, SEH and
  stack unwinding work natively.
* **CPU compatibility pass.** Disassemble every function listed in `.pdata` (Zydis) and find
  instructions the host CPU lacks. Patch each one in memory:
  * a same-length equivalent where the meaning allows it, e.g. `CLWB` becomes a no-op on
    coherent PC memory, `MOVNTSS` becomes `MOVSS`, `MONITORX`/`MWAITX` become `PAUSE`;
  * otherwise a short jump to a stub that does the same work (`CLZERO`, `RDPID`, SSE4a, SHA).

  A last-resort exception handler catches anything missed and logs it, so a missed instruction
  is slow but never fatal. On a Zen 2 or later host the pass patches nothing.
* **Crash reports.** A vectored handler writes the faulting `module+RVA`, registers and an
  unwound stack to `logs\crash-*.txt`. Minidumps are optional and stay local.
* **Processor topology.** The game sees the console topology (what it was tested with). Its
  affinity masks are remapped onto host logical processors. A "host topology" mode is an
  experiment for M8.

### 5.2 Memory (`src/runtime/memory`)

* **Title allocations.** `XMemAlloc`, `XMemVirtualAlloc` and `VirtualAlloc` with console flags
  (graphics memory, 64 KB / 2 MB pages, cache modes) map onto host memory. Memory budget answers
  match the selected console (S or X).
* **Physical pages.** `XMemAllocatePhysicalPages` and `XMemMapPhysicalPages(Scatter)` use
  page-file-backed sections plus placeholder reservations (`VirtualAlloc2` + `MapViewOfFile3`).
  One physical page can then appear at several addresses, as on the console. Memory is committed
  only when touched.
* **GPU visibility (the key design).** On the console, CPU and GPU see the same addresses. Each
  GPU-visible range gets a GPU home by class:
  * **A: GPU-produced** (render targets, depth, UAV textures and buffers). Lives in VRAM only.
    The rare CPU reads go through an explicit readback.
  * **B: CPU-produced, GPU-read-mostly** (streamed textures and meshes, BVH data). The
    authoritative copy is in VRAM. Uploads happen on known change paths (DirectStorage
    completions, `ReadFile` into title memory, copy calls). Other CPU writes are caught by page
    protection: the first write marks a page dirty, and dirty ranges are uploaded at submit. The
    cost scales with what changes, not with what exists.
  * **C: CPU-produced every frame** (constant buffers, upload rings, indirect arguments). The GPU
    reads them in place from system memory (a D3D12 heap opened on the title's own pages, zero
    copy), or from CPU-visible VRAM where Resizable BAR exists.

  The class comes from allocation attributes, D3D12.X resource flags and observed access, and
  ranges move between classes when access patterns show they were misclassified.
* **Addresses in shaders.** Console shaders compute raw addresses. Generated shaders translate
  an address to "host heap + offset" through a GPU page table. That lookup is hoisted into
  wave-uniform code wherever the address is uniform, which is almost always the case for
  descriptor-based access. The usual cost is one scalar load and an add.
* **Optional large pages** (needs the "Lock pages in memory" privilege) for the game's big heaps,
  to save TLB misses on the CPU side.

### 5.3 OS services (`src/runtime`)

* **Threads.** A `CreateThread` wrapper handles TLS callbacks, names, affinity remapping,
  priority mapping and stack sizes; console thread APIs are implemented too. Synchronisation,
  time and fibers pass straight to the host.
* **Files.** Console drives become host folders:
  * the game root → the dump, read-only;
  * `S:\` (persistent local storage, 2 GB) and `T:\` (temp) → `C:\Project Americas\userdata`;
  * writes aimed at the game root → an overlay folder.

  The game gets real host handles, so its `ReadFile` and overlapped I/O run at native speed.
* **Lifecycle.** App state notifications report "running"; "constrained" when minimised is
  optional.
* **Network policy.** Non-loopback connections are refused by default. Error-reporting calls go
  to the local log.

### 5.4 GDK services (`src/gdk`)

XGameRuntime reaches the game through per-area function tables (`QueryApiImpl`). We implement:

* XUser: one offline local user;
* XGameSave: containers and blobs as files, with atomic commits;
* XPackage: every chunk installed;
* XStore / licensing: licensed, offline;
* XSystem: Series S or X;
* XTaskQueue / XAsync: complete and correct, since other services depend on them;
* XGameUI: text entry and message dialogs as small native dialogs;
* XNetworking: offline;
* XAccessibility; XSpeechSynthesizer (silent at first);
* capture, event and invite APIs: no-ops.

Online play is out of scope.

### 5.5 Graphics (`src/gpu`)

* **Device.** `D3D12XboxCreateDevice` returns our device, with the console's vtable layout (slot
  order differs from PC D3D12). Most methods translate their descriptions (formats, flags) and
  forward to the host device. Console-only behaviour is ours:
  * resources placed at title addresses;
  * descriptor heaps that live in title memory;
  * frame pacing API;
  * fences that write memory;
  * console pipeline-stream subobjects;
  * `D3D12ConfigureVirtualMemoryX`.

  Console-only formats (7e3 / 6e4 floats, packed 16-bit targets) map onto the nearest exact
  host format.
* **Command lists.** The game's own code writes command records straight into the list object
  (the console API is inlined into it). Our list objects therefore have the console memory
  layout. At `Close()`, still on the game's recording thread, we translate the records into a
  host command list. The game's parallel recording stays parallel, and redundant state is
  filtered out. `ExecuteCommandListsX` then only submits.
* **Descriptors.** Each console descriptor heap (title memory) gets a host shader-visible heap
  with the same slots. Console view creation writes both. Generated shaders turn the descriptor
  address they compute into the host heap index arithmetically, so no tables are built per
  draw. Raw descriptor copies made by game code are caught by the dirty-page mechanism and
  re-translated at submit.
* **Pipelines.** Console pipeline streams become translated shaders (from the cache) and then
  host PSOs. PSOs are created on worker threads and kept in a persistent pipeline library under
  `cache\`. A frame never waits for a compile: an unready pipeline skips its draw once, and it
  is logged. After the offline precompile this should not happen at all.
* **Textures.** GFX10 tiling (2D and 3D swizzle modes, mip tails, arrays, cubes) is detiled on
  the GPU with compute shaders as data uploads; the CPU is the scarce resource here. Our
  `xg_xs` answers layout queries exactly as the console does, because the game lays out memory
  from those answers.
* **Presentation.** `PresentX` goes to a flip-model swap chain (tearing / VRR). Frame events come
  from a precise timer, and the game's own 30 fps pacing is respected. HDR comes later.
* **Upscaler.** The game calls FSR 4 for Xbox through its loader DLL.
  1. First, run the console DLL as is; its GPU programs go through the recompiler.
  2. Then, implement the loader's exports on the host FidelityFX SDK. FSR 4 runs natively on
     RDNA4, which is faster and better on this GPU.
* **Ray tracing.** `XboxBvhBuilder.dll` runs natively against our D3D12.X and builds console
  BVHs. Console shaders traverse them with BVH-intersection instructions, which our generated
  code emulates. That is slower than hardware, so M8 considers a host DXR path. Series S mode may
  avoid some ray-tracing paths altogether (to check).

### 5.6 Shader recompiler (`src/shader`): the largest single piece

The input is whatever bytes the game passes at pipeline creation. Offline, the same code runs over
the 26,503 records of `shaders.rpf`.

1. **Container parser.** Programs, signatures (`ISG1`/`OSG1`/`PSV0`), user-data and resource
   metadata, and pixel-shader export patch tables.
2. **GFX10 (RDNA2) decoder.** Every encoding: SOP*, SMEM, VOP1/2/3/3P/C, DPP, SDWA, DS,
   MUBUF/MTBUF, MIMG, FLAT/GLOBAL/SCRATCH, EXP, interpolation.
3. **IR and analysis.**
   * SSA IR and control-flow graph;
   * structuring (loops, branches, far jumps);
   * EXEC-mask removal (lane masks become per-lane predicates);
   * uniformity analysis (scalar register math stays wave-uniform in DXIL);
   * resource tracing (each T#/V#/S# back to its root argument, table or heap slot).
4. **Stages.**
   * VS: the fetch-shader call becomes vertex inputs;
   * PS: interpolants, exports, discard, depth;
   * CS: LDS, wave64 through `WaveSize`;
   * NGG primitive shaders: mesh shaders;
   * GS, plus its copy stage;
   * ray-tracing instructions.
5. **Emit.** HLSL, then DXC (Windows SDK 10.0.26100 or a newer release), then DXIL. Wave size is
   kept where it matters, and RDNA4 runs wave64 natively. Direct DXIL emission is a later option
   to shorten the first-run compile time; runtime speed would be the same, because the output
   is cached.
6. **Cache.** Content-addressed: the hash of the console bytes plus the translator version,
   under `C:\Project Americas\cache\shaders`. An offline tool precompiles the whole pack in
   parallel. That is several CPU-hours once on 4 cores, so it runs overnight; the driver's PSO
   cache is warmed after it.
7. **Tests.**
   * per-instruction GPU tests: our own small programs, assembled with an assembler used by the
     tests only, run on the RX 9060 XT and checked against CPU reference semantics;
   * a pack-wide survey: translate everything and count failures by reason.

### 5.7 DirectStorage (`src/storage`)

* Console queues: file and memory sources; title memory and texture destinations.
* zlib and BCPack decompression. BCPack is the console's texture compression for BC1, BC3, BC4,
  BC5, BC6H and BC7.
* Host reads use overlapped I/O or Windows I/O rings from the dump.
* Decompression runs on a worker pool. BCPack may run on GPU compute to spare the 4-core CPU.
* Completions feed memory class B with exact ranges, so nothing is scanned.

### 5.8 Audio (`src/audio`)

The game does not use XAudio2 itself. Its engine drives the console audio hardware:

* AcpHal "shapes": XMA2 and Opus decode, sample-rate conversion, EQ / compressor, volume, mix;
* XApu command queues;
* XDsp effects.

We implement that hardware behaviour in software: our own XMA2 decoder, Opus through libopus,
resampling, filters and the mixer. It runs on dedicated audio threads with SIMD and outputs
through low-latency WASAPI. Bink (`Bink2Scarlett.dll`) uses host XAudio2 2.9 and Media Foundation
calls go to the host, both directly.

### 5.9 Input (`src/input`)

* GameInput maps onto XInput pads, or onto host GameInput where installed (which gives trigger
  rumble).
* Keyboard and mouse act as a pad.

### 5.10 Diagnostics

* **Logging.** Per-thread ring buffers flushed by a background thread. Hot paths stay silent in
  release builds.
* **`report.json` at exit.** Called stubs, unknown device and list slots, XGameRuntime calls,
  shader translation results.
* **GPU debugging.**
  * D3D12 debug layer, GPU-based validation and DRED as switches;
  * frame dumps;
  * PIX: we are a normal D3D12 application to PIX, a big debugging advantage.
* **Performance HUD.** Frame time, the CPU time of each of our subsystems (command translation,
  uploads), and GPU time.

## 6. Performance principles

1. Game CPU code runs unmodified at native speed. The only changes are in-memory patches of
   Zen 2-only instructions, each costing the same as the original or one jump.
2. Imports are bound directly; no per-call logging in release; hot paths (allocation,
   synchronisation) forward straight to the host without locks of ours.
3. All GPU code is compiled ahead of time; nothing is interpreted per draw.
4. Command translation runs at `Close()` on the game's own threads, filtered for redundancy.
   Target, measured from M4 on: ≤ 1 ms of added CPU per frame across all threads.
5. GPU data stays resident in VRAM. Uploads are proportional to change. No per-frame scans of
   whole memory.
6. Nothing ever stalls on a compile: offline precompile, async PSOs, persistent caches.
7. Every milestone records measured numbers in `docs/perf.md`, so regressions are visible.

## 7. Milestones

Each milestone ends in a result that can be checked.

| # | Milestone | Done when |
|---|---|---|
| M0 ✅ | **Foundations.** CMake + Ninja + MSVC (VS 2022), C++20; build tree under `C:\Project Americas\build`; `.gitignore` for build output; an asset-policy pre-commit check; `am-probe` (host GPU features incl. GPU upload heaps / Resizable BAR, SM level, wave64, mesh shaders, CPU instruction support, memory / page file); `am-inspect` (PE + import census with each import's planned route). | `am-inspect` lists all 587 imports with a route; `am-probe` reports this machine. **Done**, see section 11. |
| M1 | **CPU census and loader.** M1.1: disassembly census of Zen 2-only instructions in the exe and DLLs. Then mapping, binding, TLS carrier, exception tables, CPU patch pass, crash reports. A synthetic "console title" built from our own code exercises the loader in tests. | The game's code runs past CRT start-up into its own initialisation, with a full call census. |
| M2 | **Boot to a frame loop.** Kernel, memory, files, threads; XGameRuntime core (user, package, system, task queue / async); D3D12.X device and presentation with a null GPU backend (commands accepted and dropped); GameInput. | The window opens and the game's frame loop runs at its own 30 fps pacing. |
| M3 | **Shader recompiler v1.** Decoder, IR, structuring, HLSL emit, DXC, cache; pack survey; precompile tool. | Pack translation rate tracked against a goal of 100 %; cache filled under `C:\Project Americas\cache\shaders`. |
| M4 | **Menus render.** Command list translation (graphics + compute), descriptors, pipelines, tiled textures, memory classes A / B / C, Bink video. | The intro video and menus are visible. |
| M5 | **Audio.** AcpHal / XApu / XDsp in software, XMA2 / Opus, WASAPI. | Menus and video have sound. |
| M6 | **Into the world.** Saves and settings, DirectStorage + BCPack, loading. | Loading completes and the world starts. |
| M7 | **World rendering.** Mesh / NGG, GS, heavy compute passes, ray tracing (BVH builder + emulated intersection), FSR. | The first mission's scene renders correctly. |
| M8 | **Performance pass.** Measure. Optimise command translation, memory classes, shader code quality (uniformity), thread scheduling for 4C/8T, large pages, host FSR 4, a host DXR option. | Numbers in `docs/perf.md` against the M7 baseline. |
| M9 | **Polish.** HDR, resolution options, keyboard / mouse mapping, Series X mode, robustness. | — |

## 8. Proposed dependencies (permissive, chosen on purpose)

* **Zydis** (MIT): x86 decoding for the CPU census and patcher.
* **libdeflate or zlib** (MIT / zlib licence): inflate.
* **libopus** (BSD): Opus.
* **D3D12 Agility SDK** (Microsoft redistributable): current D3D12 features.
* **DXC** (`dxcompiler.dll` + `dxil.dll`, from the Windows SDK or a DXC release).
* **doctest** (MIT): unit tests.

## 9. Risks and open questions

1. **CPU capacity (4C/8T).** Probably below 30 fps in dense scenes whatever our layer does. We
   minimise our own cost and schedule well; the rest is hardware.
2. **16 GB RAM.** Risk of paging. Mitigations: Series S profile, lazy commit, and a page file of
   32 GB or more.
3. **Resizable BAR is unavailable** on this machine (confirmed by `am-probe`). The design does
   not depend on it.
4. **Undocumented console behaviour** (command record layouts, descriptor and tiling bits, audio
   hardware semantics). We learn it from what the game's own code does: static analysis of its
   binaries and runtime tracing. It takes time but can be done.
5. **BCPack constant tables.** A BCPack decoder needs the format's constant tables. We need a
   legitimate source for them. Open question.
6. **Shader corner cases:** wave-size-dependent code, 64-bit image atomics, ray-tracing builder
   programs.
7. **Game updates.** A new executable moves offsets. All version-specific knowledge lives in one
   table keyed by the executable's hash.
8. **Legal.** Run only from files the user owns; no decryption; never distribute game data or
   anything derived from it.

## 10. Decisions (recommended defaults in bold)

1. Graphics backend: **host Direct3D 12** or Vulkan. D3D12.X is a superset of D3D12, so devices,
   resources, heaps, root signatures, PSOs, queues and fences map one to one; PIX works; DXIL is
   the shader target. Vulkan's raw GPU pointers and descriptor buffers would fit the console
   memory model more directly. The shader IR stays backend-neutral so a SPIR-V backend remains
   possible.
2. Default console profile: **Series S** (fits 16 GB RAM) or Series X.
3. Licence for our code: **MIT** (or keep private).
4. Page file: **raise to 32 GB or more** before the first in-world runs.

## 11. Progress

### M0: foundations (done)

* **Build.** `CMakeLists.txt` and `CMakePresets.json` (Ninja + MSVC 14.44, C++20, `/W4 /WX`,
  LTO in release); build trees under `C:\Project Americas\build\<preset>`. `scripts\env.ps1`
  enters the developer shell; `scripts\setup.ps1` enables the hooks and writes
  `C:\Project Americas\americas.ini`.
* **Libraries.**
  * `src/core`: read-only file mapping, path containment, SHA-256, JSON, settings.
  * `src/pe`: bounds-checked PE32+ parser, fuzzed with truncated and corrupted input.
  * `src/host`: CPU, memory, GPU and DXC probing, plus the target-GPU selection the runtime will
    use.
  * `src/loader/import_policy`: the routing rules the M1 loader binds by.
* **Asset policy.** `scripts/check_assets.py` runs from `.githooks/pre-commit`. It refuses
  binaries, files over 512 KiB, and hex, byte-list or base64 runs.
* **Tests.** 16 unit tests plus 3 tool runs (`ctest --preset release`), all passing.
* **`am-probe` on this PC.**
  * Missing console instruction sets: SSE4a, SHA, CLWB, CLZERO, RDPID, RDPRU, MONITORX/MWAITX.
  * RX 9060 XT selected; no GPU upload heaps, so no Resizable BAR.
  * DXC 1.9.2602.17.
* **`am-inspect` on the dump** (17 modules: the executable and 16 DLLs, 1,924 imports):
  * routes: 46 ours, 177 wrapped, 1,234 host, 467 title, **0 unresolved**;
  * every host function the game imports exists on this PC.

  Console-only work, by module:
  * `xmem` 13 functions;
  * `acphal` 9;
  * `chad` 5;
  * `xapu` 5;
  * `d3d12_xs` 4 (the fourth, `D3D12CreateRootSignatureDeserializer`, is imported by a title DLL);
  * `xg_xs` 2;
  * `xcrdapi` 1;
  * `gameinput` 1.

  Executable identity: SHA-256 `e95020b2…a08993`, CodeView age 1. Its TLS template is 5.1 KiB
  with 2 callbacks.

Next: **M1.** First the disassembly census of Zen 2-only instructions (adds Zydis), then the
loader.
