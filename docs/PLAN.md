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

## 4. Hardware: any capable PC

Project Americas is a program for anyone's PC, not for one machine. It detects what the host
offers at start-up (the same checks `am-probe` prints) and takes the fastest path the hardware
supports. Every optional feature has a working fallback; only true requirements stop it, with a
message saying what is missing.

### Requirements (provisional, tightened as milestones measure)

| Part | Requirement | Why |
|---|---|---|
| OS | Windows 11 x64, or Windows 10 22H2 with the D3D12 Agility SDK | D3D12 Shader Model 6.6+, placeholder memory APIs |
| CPU | x86-64 with the instruction sets the game uses too often to emulate. AVX2, FMA3, BMI1/2 and F16C are expected; the M1.1 census decides. | Console code uses Zen 2 features without checking; emulating hot instructions would be far too slow |
| GPU | Direct3D 12, Shader Model 6.6, resource binding tier 3, mesh shaders | Console NGG geometry becomes mesh shaders; descriptors are heap-indexed |
| Memory | 16 GB RAM and a page file large enough for the chosen console profile | The game budgets itself to the console's title memory |

### Used when present, with a fallback when not

| Feature | When present | When absent |
|---|---|---|
| Zen 2-only instructions (SSE4a, SHA, CLWB, CLZERO, RDPID, RDPRU, MONITORX/MWAITX) | The code runs unpatched | Patched in memory at load: a same-length equivalent or a jump to a stub; an exception handler catches the rest |
| Resizable BAR (D3D12 GPU upload heaps) | Per-frame CPU-written GPU data (memory class C) and upload staging live in CPU-visible VRAM | That data lives in GPU-readable system memory, and changed ranges are uploaded into VRAM |
| 64-lane waves (AMD) | Console wave64 programs run as wave64 | Wave64 programs are emulated on 32-lane waves (NVIDIA, Intel): correct but slower |
| Hardware ray tracing (DXR 1.1) | Optional host DXR path for the game's ray tracing (M8) | BVH intersection emulated in shader code |
| Host upscaler (FidelityFX SDK; vendor upscalers later) | The game's FSR call goes to the host upscaler | The console FSR DLL runs through the shader recompiler |
| Enhanced barriers | Used for command translation | Legacy resource barriers |
| Large pages + "Lock pages in memory" | The game's big heaps use 2 MB pages | 4 KB pages |
| More cores and threads | The game's workers spread over more host processors | Workers are scheduled onto fewer processors |
| More RAM and page file | Series X console profile | Series S console profile (smaller title memory) |
| HDR display | HDR output | SDR output |

**Performance note.** The game is built to keep ~7 Zen 2 cores busy at 30 fps. On hosts with
fewer or slower cores, the CPU itself, not our layer, sets the frame rate in busy scenes.

### Test machines

| Machine | CPU | RAM | GPU | Notes (from `am-probe`) |
|---|---|---|---|---|
| Dev PC 1 | Intel i7-7700K, 4C/8T (Kaby Lake) | 16 GB + 16 GB page file | AMD RX 9060 XT 16 GB (RDNA4): FL 12_2, SM 6.8, waves 32–64, mesh shaders, DXR 1.1 | Lacks all 7 Zen 2-only instruction sets. No Resizable BAR (Z270, PCIe 3.0). The fallback paths get exercised here first. |

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
        audio (AcpHal/XApu/XDsp → WASAPI)  input (GameInput)      host GPU     cache\shaders
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
* **Address range.** Title memory lies where the console keeps it: from 4 GiB (the low 4 GiB stay
  free for requests that need a 32-bit address) up to 2^40, the GPU's address width. Title images
  the loader has to move go there too.
* **Physical pages.** `XMemAllocatePhysicalPages` and `XMemMapPhysicalPages(Scatter)` use one
  page-file-backed section plus placeholder reservations (`VirtualAlloc2` + `MapViewOfFile3`).
  One physical page can then appear at several addresses, as on the console. A page is committed
  when it is first mapped; pages never mapped cost nothing.
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
3. **Translation, version 1: a faithful wave** (`shader/translate`). One HLSL thread per
   console lane, at the program's own wave size (`[WaveSize]`):
   * SGPRs, VCC, EXEC, M0 and SCC are variables every lane holds alike; VGPRs are per lane;
   * EXEC is kept exactly: each vector write is predicated on the lane's EXEC bit, and compares
     build their lane masks with `WaveActiveBallot`;
   * this GPU's branches are always wave-uniform, so control flow is a dispatcher loop, a
     `switch` over block starts. That is correct for every program without structuring;
   * memory goes through a binding model's functions: the tests map GPU addresses onto one
     buffer; the runtime's model (the graphics layer's page table and descriptors) comes with
     M4.

   Correct before fast: the steps below turn it into fast code without changing its meaning.
   The GPU tests check every change.
4. **IR and analysis** (speed, after version 1).
   * SSA IR and control-flow graph;
   * structuring (loops, branches, far jumps);
   * EXEC-mask removal (lane masks become per-lane predicates);
   * uniformity analysis (scalar register math stays wave-uniform in DXIL);
   * resource tracing (each T#/V#/S# back to its root argument, table or heap slot).
5. **Stages.**
   * VS: the fetch-shader call becomes vertex inputs;
   * PS: interpolants, exports, discard, depth;
   * CS: LDS, wave64 through `WaveSize`;
   * NGG primitive shaders: mesh shaders;
   * GS, plus its copy stage;
   * ray-tracing instructions.
6. **Emit.** HLSL, then DXC (Windows SDK 10.0.26100 or a newer release), then DXIL. Wave size is
   kept where it matters, and RDNA4 runs wave64 natively. Direct DXIL emission is a later option
   to shorten the first-run compile time; runtime speed would be the same, because the output
   is cached.
7. **Cache.** Content-addressed: the hash of the console bytes plus the translator version,
   under `C:\Project Americas\cache\shaders`. An offline tool precompiles the whole pack in
   parallel. That is several CPU-hours once on 4 cores, so it runs overnight; the driver's PSO
   cache is warmed after it.
8. **Tests.**
   * per-instruction GPU tests: our own small programs, assembled with an assembler used by the
     tests only, run on the host GPU and checked against CPU reference semantics;
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
| M0 ✅ | **Foundations.** CMake + Ninja + MSVC (VS 2022), C++20; build tree under `C:\Project Americas\build`; `.gitignore` for build output; an asset-policy pre-commit check; `am-probe` (host GPU features incl. GPU upload heaps / Resizable BAR, SM level, wave64, mesh shaders, CPU instruction support, memory / page file); `am-inspect` (PE + import census with each import's planned route). | `am-inspect` lists all 587 imports with a route; `am-probe` reports the host. **Done**, see section 11. |
| M1 ✅ | **CPU census and loader.** M1.1: disassembly census of Zen 2-only instructions in the exe and DLLs. Then mapping, binding, TLS carrier, exception tables, CPU patch pass, crash reports. A synthetic "console title" built from our own code exercises the loader in tests. | The game's code runs past CRT start-up into its own initialisation, with a full call census. **Done**, see section 11. |
| M2 | **Boot to a frame loop.** Kernel, memory (done: M2.1), files, threads; XGameRuntime core (user, system, task queue / async done: M2.2; package and saves to come); D3D12.X device and presentation with a null GPU backend (commands accepted and dropped); GameInput. | The window opens and the game's frame loop runs at its own 30 fps pacing. |
| M3 | **Shader recompiler v1.** Decoder, IR, structuring, HLSL emit, DXC, cache; pack survey; precompile tool. | Pack translation rate tracked against a goal of 100 %; cache filled under `C:\Project Americas\cache\shaders`. |
| M4 | **Menus render.** Command list translation (graphics + compute), descriptors, pipelines, tiled textures, memory classes A / B / C, Bink video. | The intro video and menus are visible. |
| M5 | **Audio.** AcpHal / XApu / XDsp in software, XMA2 / Opus, WASAPI. | Menus and video have sound. |
| M6 | **Into the world.** Saves and settings, DirectStorage + BCPack, loading. | Loading completes and the world starts. |
| M7 | **World rendering.** Mesh / NGG, GS, heavy compute passes, ray tracing (BVH builder + emulated intersection), FSR. | The first mission's scene renders correctly. |
| M8 | **Performance pass.** Measure. Optimise command translation, memory classes, shader code quality (uniformity), thread scheduling for 4C/8T, large pages, host FSR 4, a host DXR option. | Numbers in `docs/perf.md` against the M7 baseline. |
| M9 | **Polish.** HDR, resolution options, keyboard / mouse mapping, Series X mode, robustness. | — |

### Order of work

M3 (the shader recompiler) runs **alongside M2**, not after it. Its batch jobs need hours of
machine time (section 7a), and the machine can work through them while M2's code is written.
By the time M4 needs shaders, the cache should be warm.

## 7a. Background jobs: what the PC precomputes

| Job | Tool | When it can start | Machine time (Dev PC 1, 4 cores) | Kept in |
|---|---|---|---|---|
| Shader corpus: every record of `shaders.rpf` inflated and de-duplicated | `am-shaders index` | **now (done)** | 48 s | `cache\shaders\pack` |
| Translation survey: every program through the recompiler, failures by reason | `am-shaders survey` (M3) | **now (decoder done)**; it will cover translation once that exists | 3 s for decoding all 26,084 programs | `cache\shaders\survey.json` |
| Precompile: every translated program through DXC to DXIL | `am-shaders precompile` (M3) | once the recompiler emits code | several CPU-hours on first run; afterwards only programs whose translation changed | `cache\shaders\dxil` (content-addressed) |
| Driver pipeline warm-up | the runtime | first game runs | per pipeline the game creates; the driver's cache and our pipeline library keep them | driver cache, `cache\pipelines` |
| CPU census per executable build | automatic at load | always | seconds, once per build | `cache\cpu` |

The batch jobs must be safe to leave running:
* resumable: an interrupted run continues where it stopped;
* below-normal priority, so the PC stays usable;
* content-addressed by the console bytes plus the recompiler version, so a recompiler change
  recompiles only what it changes;
* a summary written as they go.

Not precomputed, by design:
* textures: detiled on the GPU as they stream; converting ~150 GB ahead would only duplicate
  it;
* audio: decoded live;
* ray-tracing BVHs: built by the game's own builder at run time.

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
3. **Hosts differ.** Resizable BAR, wave64 and large pages are optional paths (section 4), and
   each fallback must be tested too. Dev PC 1 has none of the three, so it covers the fallbacks.
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
2. Default console profile: **chosen from the host's RAM and page file** (Series S on 16 GB
   machines, Series X where memory allows), with an override.
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

### M1.1: CPU instruction census (done)

* **Our own x86-64 decoder** (`src/x86`), written from the instruction-set rules; no third-party
  disassembler. It gives instruction length, operand layout (ModRM, SIB, displacement, immediates,
  RIP-relative), control flow, and the CPU feature each instruction needs.
* **`am-cpuscan`** scans every executable section in two passes:
  1. recursive descent from the exception table, entry point, exports and TLS callbacks
     ("confirmed" code);
  2. a linear sweep of whatever is left ("in function" or "outside", less certain).

  The whole executable takes about 10 s.
* **Checked against `dumpbin`** (Microsoft's disassembler) on 60 random 64 KiB chunks:
  * 1,063,570 instruction boundaries agree, except 20 bytes of data where `dumpbin` cannot decode
    EVEX;
  * every feature label matches `dumpbin`'s mnemonic;
  * all 41 census sites carry the mnemonic `dumpbin` gives them.
* **Results for the executable** (23.5 M instructions, 96 % confirmed; 0 decode errors and 0
  impossible encodings in confirmed code):

  | Feature | Confirmed sites | Note |
  |---|---|---|
  | AVX | 2,864,244 | requirement |
  | FMA3 | 198,018 | requirement |
  | AVX2 | 114,583 | requirement |
  | BMI1 | 58,967 | requirement |
  | BMI2 | 14,395 | requirement |
  | F16C | 3,149 | requirement |
  | ADX | 2,611 | requirement: Haswell lacks it, Broadwell and later have it |
  | AES-NI | 1,878 | requirement |
  | PCLMULQDQ | 606 | requirement |
  | MOVBE | 386 | requirement |
  | SSE4a (INSERTQ, immediate form) | 17 (+2 unconfirmed) | **patched** |
  | CLZERO | 10 | **patched**: counted zeroing loops |
  | CLWB | 8 | **patched** |
  | MONITORX / MWAITX | 4 | **patched** |
  | SHA, RDPID, RDPRU | 0 | not used |

  So the CPU requirement is roughly "Intel Broadwell / AMD Zen or newer". The 7 Zen 2-only
  instruction sets appear at only **41 sites**, all in the executable.
* **The DLLs** need nothing a PC lacks. `libHttpClient.GDK.dll` contains AVX-512 paths behind a
  runtime CPU check, so they run only where the host has AVX-512.

Patch plan for the 41 sites:
* CLWB becomes a same-length NOP;
* MONITORX becomes a NOP, MWAITX becomes PAUSE + NOP;
* INSERTQ and CLZERO become jumps to per-site stubs. CLZERO's 3 bytes are too short for a jump,
  so the following instruction moves into the stub with it;
* an illegal-instruction handler emulates any site not patched.

### M1.2: the loader (done)

* **`americas.exe` (launcher) + `americas_rt.dll` (runtime) + `am_tls_1..4.dll` (TLS carriers).**
  The launcher only holds what must live in the start executable: static TLS slot 0, given to
  the title's executable, and a TLS callback that forwards every thread start and exit to the
  title's modules. Each title DLL with TLS gets a carrier's slot. Carrier templates are
  overwritten with the title's and existing threads' blocks are filled, so title TLS works with
  no code patching.
* **Mapping** at the preferred base (relocations when taken), section protection, and the
  exception tables registered with `RtlAddFunctionTable`, so SEH, C++ exceptions and stack
  walks work in title code.
* **Binding by the import policy:**
  * our implementations;
  * wrapped host functions;
  * host functions (System32 only);
  * title DLLs, mapped on demand and initialized in dependency order;
  * stubs that name the import and return 0, or with `--unimplemented trap`, raise an
    exception.

  Wrapped functions we do not implement yet are **refused**, never passed to the host. The
  ones implemented so far:
  * module identity (`GetModuleHandle*`, `GetModuleFileName*`, `LoadLibrary*`,
    `GetProcAddress`, `RtlPcToFileHeader`);
  * the command line and `argv`;
  * the crash filter;
  * app lifecycle;
  * network refusal (loopback only);
  * threads, memory and system information, as marked pass-throughs.
* **`RtlPcToFileHeader`.** The C++ runtime finds a thrown object's type information through it.
  Ours answers for title images and is also patched into the host DLLs' import tables, so
  exceptions work whichever C++ runtime the title uses.
* **CPU compatibility** (`runtime/cpu_compat`):
  * the census runs once per build of an image and is cached in
    `C:\Project Americas\cache\cpu`;
  * confirmed sites with a same-length equivalent are patched in the mapped image;
  * the rest are emulated by an illegal-instruction handler: CLZERO, EXTRQ / INSERTQ,
    MOVNTSS / SD, RDPID, RDPRU, CLWB, MONITORX / MWAITX;
  * a CPU that lacks a hard requirement (AVX2, say) gets a clear refusal.

  On a CPU with all the console instruction sets, nothing is scanned or patched.
* **Crash reports:** exception, faulting `module+offset`, registers and an unwound stack, in
  `logs\crash.txt`. `logs\report.json` holds modules, stubs (with call counts), CPU patches
  and emulation hits.
* **Safety for unattended runs:** `--memory-limit <MiB>` (a job object) and `--timeout <s>`.
  With file and network access refused, a run cannot change anything outside our process: the
  game only reads the registry, and starts no processes.
* **Tests:** a fixture title built from our own code runs through the loader at its preferred
  base and relocated. Its 21 checks cover:
  * static TLS on new threads, TLS callbacks;
  * C++ exceptions through 8 frames, SEH;
  * module identity, the title's argv, relocations;
  * a title DLL's exports, DllMain and TLS;
  * a stubbed console import;
  * INSERTQ / EXTRQ / MOVNTSS / MOVNTSD / CLWB compiled into it.

  Six unit tests check the emulated instructions on hand-assembled code. On Dev PC 1 they
  really are emulated.

**First run of the game (Dev PC 1)**, `americas --memory-limit 6144 --timeout 120`:

1. The executable (176 MB) is mapped at `0x140000000`; `msvcp140`, `vcruntime140(_1)`,
   `XboxBvhBuilder`, `amd_fidelityfx_loader_xbox`, `libHttpClient.GDK` and `Bink2Scarlett` are
   mapped, relocated and initialized.
2. TLS: the executable takes 5,248 bytes of slot 0; two DLLs take carrier slots.
3. CPU: 12 sites patched, 29 left to emulation.
4. The entry point runs, the C runtime starts, and the game's own global initializers run.
5. Its first console call that needs a real answer, `xmem!XMemVirtualAlloc`, gets the stub's 0
   and the game stops at its own assertion (`int3`).

69 imports are stubs so far: 39 console functions and 30 refused wrappers.

### M3 groundwork: the shader corpus (done)

* **DEFLATE decoder** (`src/formats/inflate`): raw and zlib-wrapped, table-driven, with checks
  for truncated, corrupt and oversized streams. Tested with synthetic vectors (stored, fixed,
  fixed with matches, dynamic, zlib with Adler-32). DirectStorage's zlib will use it too.
* **Shader pack reader** (`src/shader/pack`). It finds records by their own framing, without
  the encrypted table of contents, and accepts one only if it inflates to exactly its stated
  size as a DXBC container. Tested on a synthetic pack with junk and a decoy.
* **`am-shaders index`** on the game's `shaders.rpf`:
  * 26,503 records, 25,889 distinct containers (234 MB), 48 s;
  * 0 rejected, 0 unreadable;
  * by kind: 12,101 pixel, 9,282 mesh, 2,689 compute, 2,236 vertex, 195 geometry.
* **Container layout.** Every container has the parts `SFI0 ISG1 OSG1 PSV0 HASH DXIL PRIV`.
  The `DXIL` part starts with the PC's program header (kind and shader model 6.6), followed by
  the console's `XDAT` block, then an `XBCH` block, and ends with a dummy 4-byte "bitcode".
  The RDNA2 machine code is inside the console blocks; mapping them is M3's first task.

### M2.1: memory (done)

Microsoft's XMem reference pages are closed to the public (an NDA program), so the facts come
from the game's own code: its 23 `XMemVirtualAlloc` call sites, its other `xmem` calls, the BVH
builder's `XMemVirtualQuery` calls, and the arguments logged at run time.

* **What the game asks for at start-up** (Dev PC 1):

  | Allocation | Type | XMem flags | Protection | What it is |
  |---|---|---|---|---|
  | two 24 GiB reservations of 64 KiB pages (16 GiB on another branch) | `0x20402000` | `0x21`, `0x3` | CPU read-write; CPU and GPU no access (`0x801`) | the CPU and GPU views of its physical memory; `D3D12ConfigureVirtualMemoryX` is called just before |
  | four commits of 2 MiB pages: 1.1 GiB, 2.1 GiB, 563 MiB, 596 MiB | `0x20003000` | `0x183`, `0x283` | read-write plus GPU read-write, coherent (`0x22004`) or write-combined (`0x2404`) | its large heaps: 4.4 GiB of commit, which costs no RAM until touched |
  | a 20 MiB commit of 64 KiB pages | `0x20403000` | `0x181` | read-write | |
  | elsewhere in its code: its own placeholder below 4 GiB, split into pieces made physical-page reservations | `0x42000`, then `0x20406000` | `7`, `0x27` | GPU read-only, GPU execute | apparently GPU code and constants |

* **Console bits** (`runtime/memory/console_flags`). Allocation type bit 29 selects 2 MiB
  pages, bits 29 + 22 select 64 KiB pages (on the host they would mean large pages and AWE). The
  protection carries the GPU's access in bits `0x800`–`0x40000` (the SDK's `PAGE_GRAPHICS_*`),
  kept for the graphics layer. Write-combined and uncached CPU access are dropped, because our
  layer reads that memory on the CPU. XMem flags: the low two bits are 1 for CPU memory and 3 for
  memory the GPU maps too, and bit 2 asks for an address below 4 GiB. The other bits seen (5,
  7–9) are recorded, not acted on.
* **Physical pages** (`runtime/memory/title_memory`): one page-file-backed section the size of
  the console's title memory, reserved, not committed. Each run of consecutive pages becomes
  one view over a placeholder. A 64 KiB-page reservation without a commit is kept as a
  placeholder, so pages can be mapped into it. Page numbers count 4 KiB frames (multiples of
  16) and stay below 2^25, because the game stores them shifted right by 4 in 21 bits.
  * Unmapping returns the range to placeholders.
  * Releasing a reservation releases its views and every placeholder piece.
  * A page is committed when first mapped.
  * Commit failures name the system's commit and point at the page file.
* **`XMemAlloc`** goes to the executable's allocation hooks, its only export
  (`__XMemAllocationHooks__`), which dispatch on the allocator id at attribute bits 16–23.
  `XMemAllocDefault` is ours: large blocks get their own allocation, small ones come from size
  classes in 2 MiB arenas, all below 2^40. The alignment is a power of two at bits 24–28.
* **`XMemVirtualQuery`** returns `MEMORY_BASIC_INFORMATION` plus the allocation's flags at `+0x30`.
  The BVH builder checks State, Protect and the flags' low two bits.
* **The console's `VirtualAlloc` / `VirtualFree` / `VirtualProtect`** take the same bits.
  `MEM_RELEASE` with the allocation's own size is accepted, as the console does.
* **Budget.** `XMemGetWorkingSetStatistics` reports the profile's title memory and, as used, our
  private commit plus the physical pages held. There is no tools memory (`XMemIsToolsMemAvailable`
  is false; `XMemTransferMemory` is refused).
  * The console profile (`console = auto | series_s | series_x` in `americas.ini`, or
    `--console`) picks Series X on hosts with 24 GB of RAM, Series S otherwise; Dev PC 1 gets
    Series S.
  * `--memory-limit` now also caps the physical pool's commit, which a job's limit does not
    count.
* **Loader fixes found on the way:**
  * the launcher now holds `0x140000000`–`0x180000000` from its first instruction and hands it to
    the runtime, because a random allocation sometimes took the executable's range and forced a
    relocation above 2^40;
  * images that must move go into title memory.
* **Tests:**
  * 9 unit cases: flag translation on the game's call shapes, pages aliased at two addresses,
    partial unmapping, scatter mapping, release of a split placeholder, the title's own
    placeholder pieces, 2 MiB alignment, 32-bit placement, the commit cap, the default
    allocator, the console choice;
  * 14 more fixture checks run through the loader. The fixture has 35 now.
* **The game now** (`--memory-limit 8192 --timeout 120`):
  1. it configures GPU virtual memory and makes its seven start-up allocations;
  2. `GetFileAttributesW` is refused;
  3. it calls `xgameruntime!InitializeApiImplEx2` and `QueryApiImpl`;
  4. the stub's 0 reads as `S_OK`, so the game calls through the null interface it was not given,
     at `+0x55de020`.

  XGameRuntime is the next blocker.
* **Not done yet:**
  * large pages for the 2 MiB-page heaps (an M8 option);
  * the GPU memory classes, which the recorded GPU access will feed (M4).

### M2.2: XGameRuntime core (done), GameInput without devices

* **How the game reaches the runtime** (its own code, the GDK's static part):
  1. it loads `xgameruntime.dll` by name and looks up `QueryApiImpl`, `InitializeApiImpl`,
     `InitializeApiImplEx`, `InitializeApiImplEx2`, `UninitializeApiImpl` and `XErrorReport`;
  2. it calls `InitializeApiImplEx2(0x633600002712, 0x65F400001EDD, flags 0xA, no options)`;
  3. every runtime function is a thunk: `QueryApiImpl(class, interface, &object)`, then one
     vtable slot with the object first, then `Release`.
* **The catalog** (`runtime/gdk`): the game has 161 thunks in 17 interfaces, listed by class
  GUID, interface GUID and slot as read from its code, with the GDK's public function names.
  * An area is offered once one of its methods exists, so the game's thunks fail cleanly for the
    rest.
  * A missing slot logs its name and fails: `E_NOTIMPL`, or 0 where the function returns a
    bool or a value.
  * `report.json` lists the areas asked for and the missing methods called.
* **XTaskQueue / XAsync / XThread**, complete, because the other areas build on them:
  * manual, thread-pool, serialized and immediate ports;
  * delayed callbacks, waiters, monitors, termination that cancels what is pending, composite
    queues and the process queue;
  * the XAsync provider protocol, results, cancellation (the provider's to do) and `XAsyncRun`.
* **The offline console:**
  * XUser: one local user, "Player", signed in, adult, without the privileges that need the
    Xbox network;
  * XSystem: made-up stable ids, the RETAIL sandbox, analytics `Windows.Xbox` and the profile's
    console;
  * XGame: the title id from `MicrosoftGame.Config` (`7952987A`);
  * XNetworking: initialized, no connectivity, UDP port 3074; online-only calls fail with
    `ERROR_NO_NETWORK`;
  * every runtime feature available (the game asks about 10 and 20, XNetworking and XGameEvent);
  * game events dropped.
* **Not implemented yet:** XStore, XGameSave, XPackage, XPersistentLocalStorage, XGameUi,
  XSpeechSynthesizer, XDisplay (HDR, timeout deferrals), XAppCapture, XLaunchUri.
* **GameInput** (`runtime/input`): `GameInputCreate` (ordinal 1) and IGameInput in the public
  API's layout, with no devices. The game uses slots 4–6 (readings), 9 (device callback), 13
  (unregister) and 16 (`FindDeviceFromId`).
* **Found on the way:**
  * thread start and exit took the loader's module lock under the loader lock and deadlocked
    against `GetProcAddress` loading a host DLL; they are lock-free now;
  * a timed-out run writes every thread's name and stack to `logs\hang.txt`;
    `americas_rt.dll` has a linker map, and `scripts/symbolize.py` names its frames.
* **Tests:** 6 unit cases (queues in every mode, delays, waiters, monitors, composite queues,
  termination, the async protocol) and 6 fixture checks through the pseudo module. The
  fixture has 41.
* **The game now:**
  1. it initializes the runtime and creates its task queues;
  2. it uses XUser and XNetworking;
  3. it registers its gamepad callback;
  4. it calls `XCRDAPI!DStorageGetFactory`; the stub's 0 reaches its own assertion
     (`int3` at `+0x1d8e505`);
  5. meanwhile other threads try `GetFileAttributesW`, `CreateFileW` and `_wfopen`, all refused.

### M3.1: the console programs and the RDNA 2 decoder (done)

* **Where the GPU code is** (`shader/console_program`, checked on all 25,889 containers):
  1. the DXIL part has an `XDAT` index at `+0x18`: entries of kind, offset, size;
  2. kind `0x0d` is an `XBCH` blob (version 3): a zlib stream of an `FSSM` table (version
     `0x101`);
  3. the table has a record per hardware stage (LS HS ES GS VS PS, mesh, compute). Each record
     gives the code's offset and size, wave32 or wave64, the VGPR and SGPR counts and LDS
     bytes; stage data (inputs, outputs, export patches) follows it.
  * 26,084 programs, 195 vertex shaders carry two (with and without a geometry shader).
  * By stage: 12,056 PS, 8,765 mesh, 2,682 CS, 2,191 VS, 195 ES + GS.
  * 23,430 are wave64 and 2,654 wave32. There are 233 MiB of code in all.
* **The decoder** (`shader/rdna2`): its tables are generated by `scripts/gen_rdna2_tables.py`
  from AMD's RDNA 2 XML (`shader/rdna2_tables.inl`, committed with AMD's MIT notice): 1,477
  instructions with their operands, and every encoding's field layout. The length rules are the
  ISA's:
  * literals;
  * SDWA, DPP16 and DPP8;
  * MIMG NSA addresses;
  * the always-literal forms.

  Four console facts the PC RDNA 2 specification does not cover:
  * **RDNA 1's MAD family stays:** `v_mac_f32`, `v_mad_f32` and `v_madak_f32` are filled from
    AMD's RDNA 1 XML (flag `kLegacy`). PC RDNA 2 reads VOP2 opcode 31 as `v_dot2c_f32_f16`; the
    console's code uses it beside `v_mad_f32` and `v_madak_f32`, which only RDNA 1 has, so it is
    `v_mac_f32`. The translator's first tests should confirm it.
  * MIMG has an eighth opcode bit at bit 0 (`image_bvh_intersect_ray` is 230), MUBUF at bit 25
    (the D16 buffer loads are 128 and up). The specification's layouts leave both out.
  * Every program starts with a 24-byte marker: two VOP3 `v_nop`s whose unused src2 is 255, so
    each carries a 32-bit literal (apparently a hash). A literal follows whenever any VOP3 source
    field is 255, used or not.
  * Code reaches past `s_endpgm` only through branches: some programs have zero dwords there
    (data). So programs are decoded **by control flow** from the entry (`scan_program`),
    following branches and calls, and what is never reached is data.
* **`am-shaders survey`:** **all 26,084 programs decode completely** (no unknown opcode, no
  branch into an instruction, no path off the end). That is 43,952,021 instructions of 392
  distinct kinds, in 3 s (`cache\shaders\survey.json`).
  * The commonest: `v_mul_legacy_f32` 5,922,950, `v_fmac_f32` 4,152,058, `s_waitcnt` 2,453,661, `v_fma_f32` 2,372,101, `v_mov_b32` 2,132,472, `v_cndmask_b32` 1,476,946, `v_sub_f32` 1,421,653, `s_mov_b32` 1,390,484, `v_add_f32` 1,302,111, `s_load_dwordx4` 921,536, `s_nop` 644,010, `v_and_b32` 635,037.
  * 995 programs jump indirectly (`s_setpc_b64`). The translator must follow those (jump
    tables or subroutines).
* **Checked against LLVM.** LLVM 23.1.6's `llvm-mc` was built from source with the AMDGPU target
  (`C:\Project Americas\tools\llvm-amdgpu-23.1.6`). `scripts/rdna2_oracle.py` gives it every
  instruction we reach, one per line, as RDNA 2 (gfx1030), then RDNA 1 with ray tracing
  (gfx1013).
  * On 1,500 random containers, **2,812,001 instructions: 100.000 % agree** on length and name.
  * The markers are skipped: LLVM refuses them.
* **Tests:** 5 unit cases, with instruction words LLVM confirms:
  * literals, SDWA, DPP, VOP3b, MIMG NSA;
  * the two opcode bits and the legacy MAD;
  * the control-flow scan;
  * a synthetic container through `read_console_shader`.

### M3.2: translator version 1, a faithful wave (in progress)

* **The model** (`shader/translate`): every console lane is one HLSL thread of a
  `[WaveSize(32|64)]` compute shader (`cs_6_6`), so a console wave is one host wave.
  * SGPRs are a wave-uniform array `s[128]` (VCC in 106/107, M0 124, EXEC 126/127) and `scc`.
  * VGPRs are a per-lane array; every vector write is predicated on the lane's EXEC bit.
  * Compares build their masks with `WaveActiveBallot`.
  * Branches are uniform (the console's are on SGPRs), so control flow is a dispatcher:
    `for (;;) switch (pc)` over the block starts.
  * Literal templates are filled in one pass, so sources holding the placeholder letters
    (hex literals) are safe.
* **The compute ABI** (from `COMPUTE_PGM_RSRC2`, read from each CS record at `+0x54+0x24`,
  with the thread counts after it): user SGPRs first, then the enabled group ids; v0..v2 hold
  the thread ids up to `TIDIG_COMP_CNT`.
* **Memory:** `s_load` and `s_buffer_load`, untyped `buffer_*` through the V# (base, stride,
  records, bounds), `global_*`, and LDS (`ds_read/write` b32, read2/write2).
* **Two binding models:** `Test` (memory at a GPU base address in one UAV, a register dump in
  another) for the tests; `Runtime` follows with M4.
* **Host side:** `host/dxc_compile` loads `dxcompiler.dll` from the found DXC and compiles in
  process.
* **`am-shaders translate <hash prefix>`** writes the HLSL and compiles it;
  **`am-shaders tsurvey [<n>]`** translates the whole corpus in 8 s, compiles `n` of the
  results and writes `cache\shaders\translation.json` with what blocks the rest.
* **Tests** (`tests/translate_tests.cpp`, `tests/gpu_runner.*`): console programs assembled with
  LLVM's AMDGPU assembler run on the host GPU, and every SGPR, the SCC, every lane's VGPRs and
  the memory are checked against the ISA: scalar ALU and loops, vector ALU with EXEC masking,
  `cmpx`, `mbcnt`, scalar/global/buffer memory. All pass on the RX 9060 XT.
* **First survey:** 69 of the 2,682 compute programs translate, and **all 69 compile with DXC**.
  What blocks the rest, by programs held up: image loads, stores and samples (1,932, 1,740,
  1,599), typed buffer access (`buffer_load_format_x` 1,595, `tbuffer_load_format_xy` 1,067),
  `v_fma_mix_f32` 1,411, SDWA/DPP forms (about 1,100 each for the common ALU ops),
  `v_alignbit_b32` 1,037, `v_permlanex16_b32` 902, the cube ops 794.

### M2.3: console files and DirectStorage (done)

* **Console paths** (`runtime/files`, `hle/files.cpp`):
  * the title's folder (`G:\` and friends) maps to the read-only game folder;
  * `S:\` (persistent local storage) and `T:\` (temp) map into `C:\Project Americas\userdata`;
  * everything else is refused;
  * writes that resolve into the game folder are refused too.
* **The console's DirectStorage** (`runtime/storage/dstorage.cpp`) opens files and runs queued
  reads into memory or physical pages, with zlib unpacking. BCPack and other transforms are
  refused until the BCPack tables have a legitimate source.
* **Result:** the game opens all its RPF archives through DirectStorage, and its crash handler
  writes its dumps into our temp folder (the upload is refused).

### M2.4: the D3D12.X device (in progress)

`d3d12_xs!D3D12XboxCreateDevice(adapter = null, &parameters, riid, &device)` returns our
device. The parameters are an 80-byte block: `0x20004` at `+0`, `0x100000` at `+8`, flags from
the game's settings. The console's graphics objects are learned from the game's calls
(`runtime/d3d12x`):

* **Every object** is `[vtable][console-layout fields]`, with our bookkeeping in a 64-byte
  header in front of it. Title code writes some objects' fields directly (the command list's
  record buffer), so nothing of ours may sit at their offsets. They have no C++ virtual
  functions, since a C++ vtable pointer would take offset 0.
* **Unimplemented slots are probes.** Each logs its first calls with their arguments and returns
  0. `report.json`'s `graphics` section lists every probe the game called.
* **Struct-returning methods:** the console's callers pass the return buffer first and the
  object second.

The device's slots, as learned so far:

| Slot | Method | Note |
|---|---|---|
| 0 | QueryInterface | D3D12 interfaces: the device; DXGI's (`IDXGIDevice`...): its DXGI view |
| 8 | CreateCommandQueue | D3D12's; the game's 1 direct queue |
| 9 | CreateCommandAllocator | D3D12's |
| 14 | CreateDescriptorHeap | D3D12's description followed by the memory's address and size: on the console a heap is title memory, and its start handle must be that address |
| 15 | GetDescriptorHandleIncrementSize | 32 for resource views, 16 for samplers, 64 for render-target and depth (ours) |
| 16 | CreateRootSignature | D3D12's; reads blobs in D3D12's serialized layout (raw, or a container's RTS0 part) |
| 17–24 | the view creators, CopyDescriptors(Simple) | D3D12's order. Views write a tagged record into their descriptor slot; copies move memory |
| 25 | GetResourceAllocationInfo | sizes from the texture layout model (`d3d12x/layout.h`) |
| 26 | GetCustomHeapProperties | the answers for a unified-memory adapter |
| 27 | CreateCommittedResource | D3D12's (the game's readback buffers) |
| 36 | CreateFence | D3D12's |
| 48 | CreateCommandQueueX | the console's own: 4 compute queues |
| 55, 57, 67 | pipeline creations | the console's own; `ID3D12PipelineState` objects, descriptions not read yet. 55 is `(0, count, descriptions, riid, out)` and the game passes counts of 2 |
| 62 | compute pipeline creation | `(D3D12_COMPUTE_PIPELINE_STATE_DESC, 0, 0, riid, out)`: the BVH builder's 40 pipelines |
| 73 | CreateCommandListX | 16-byte description, allocator, initial pipeline, riid |
| 78 | SetFrameIntervalX | `(output, microseconds, period, flags)`: the game asks for 33,333 us (30 fps) |
| 79 | ScheduleFrameEventX | `(type, offset in microseconds, ?, flags)` |
| 80 | WaitFrameEventX | `(type, timeout, ?, flags, &frame token)`: the render loop waits here; frames start every interval, each event type sees each frame once |
| 86 | CreatePipelineState | D3D12's stream description: root signature and shaders kept |
| 108 | GetResourceAllocationInfo2 | struct return, `(mask, count, D3D12_RESOURCE_DESC1s, per-resource info)`; the game asks with 4 KiB alignment first |
| 127 | committed resource creation | shaped like `CreateCommittedResource3`: heap properties, flags, description, initial barrier layout, clear value, protected session, castable formats (count, list), riid, out |
| 133 | CreatePlacedResourceX | `(GPU virtual address, description, initial layout, clear value, riid, out)`: a resource in the title's own memory, no heap |
| 39, 41 | CreateQueryHeap, CreateCommandSignature | D3D12's order (probes) |
| 61, 70, 74, 81 | not identified | probes; the game accepts their 0. 70 and 81 run once a frame (70 writes 32-byte descriptors in the big heap); 74 takes the 6.1 GiB of physical pages the game maps |

Other objects:
* **The DXGI view** (`IDXGIDevice`): adapter and output (the host's main display, reported at
  60 Hz). The output's slot 21 is the console's `GetDisplayModeList1`.
* **Command queue:** two slots later than D3D12's from Signal on: Signal 16, Wait 17,
  GetTimestampFrequency 18 (our first guess, D3D12's 16, wrote the frequency over a fence's
  vtable). Slot 32 is `PresentX(plane count, planes, parameters)`, once a frame; slot 35 runs
  twice a frame (a probe).
* **PresentX and the window** (`d3d12x/present.cpp`, `runtime/present`). A plane's parameters
  are 72 bytes: frame token +0, resource count +8, resources +0x10, source rectangles +0x18,
  destination placement +0x20, color space +0x28. The first resource of the first plane is
  shown in the game window: a window on a thread of its own, and a flip-model swap chain of
  the host's D3D12 (any D3D12 GPU, the high-performance one first) the size of the image,
  scaled by DXGI. Each frame the image is copied from the title's memory to an upload buffer
  and from there into the swap chain. The title bar shows the frame rate; closing the window
  ends the run (report written). The game presents its 3840x2160 RGBA8 back buffers at 30 fps;
  they stay black until the GPU back end runs the title's commands.
* **Fence:** D3D12's slots 8–10; slot 12 gives the address of the fence's 64-bit value, which
  the game's wait loop reads directly (the GPU's memory word on the console).
* **Command allocator.**
* **Command list:** 4 KiB of console fields; the game calls its slots 9, 131 and 132 after
  creating it.
* **Resource:** D3D12's `ID3D12Resource` slots (Map, Unmap, GetDesc, GetGPUVirtualAddress,
  GetHeapProperties). Memory comes from `XMemAlloc` with the graphics allocator id (128),
  large pages from 2 MiB, the alignment's power of two; zeroed unless `CREATE_NOT_ZEROED`.
  The game's hook routes allocator 128 through handlers it pushes per thread: around its back
  buffers, a bump allocator over a 64 MiB budget that takes only large-page requests, and an
  assertion that the back buffers filled it to within 2 MiB. Everything else reaches
  `XMemAllocDefault`.
* **Root signatures:** `d3d12_xs!D3D12SerializeVersionedRootSignature` writes D3D12's
  serialized layout (versions 1.0 to 1.2); the console's own flag bits (`0x3000000` in the BVH
  builder's) pass through.
* **Pipeline state:** keeps its root signature and copies of the console shader containers.
* **Descriptor heaps:** the game also passes D3D12's plain 16-byte description, with stack
  junk where the console's memory fields would be; those fields count only when the size fits
  the heap and the memory is committed.
* **Texture layouts and XG** (`d3d12x/layout.h`, `d3d12x/xg.cpp`): the description's Layout
  field is `0x100` plus an RDNA swizzle mode (0 linear; 4 KiB blocks 4-7, 20-23; 64 KiB blocks
  8-11, 16-19, 24-27). The game's texture headers name the mode; `XGSuggestSwizzleMode` picks
  one for the others (Z / R / S by usage, 4 KiB blocks for small textures). One layout model
  answers every size: each mip a grid of whole blocks shaped as on the GPU, layers one after
  another, no mip tail, so sizes are never below the console's; texels are linear inside the
  padded grid for now. The XG texture computer has no IUnknown (0 AddRef, 1 Release); slot 8
  is CopyIntoSubresource.
* **Probes log their call site** (`module+offset`), so each unknown slot leads to the code that
  calls it.

Null back end: queues finish work at once and fences reach their value when signalled.

**Physical pages commit on first touch.** The game maps 6.1 GiB of physical pages at once
(its streaming heap). Committing them at map time passed the run's memory limit, and on a
16 GB PC would take most of the system's commit. A mapped page is now committed by a fault
handler when first touched; `XMemVirtualQuery` and `VirtualQuery` show mapped pages as
committed; title `ReadFile` / `WriteFile` (the kernel cannot fault pages in) commit their
buffers first.

**CLZERO clearing loops run native.** The game clears memory with
`clzero; add rax, 64; cmp rax, reg; jb loop`, called about 10,000 times a second; emulating
each CLZERO cost millions of exceptions a minute. At load such loops now jump to a stub next to
the image that clears the line with eight stores (18 of the executable's 41 Zen 2-only sites
are patched, 23 left to emulation, none of them hot so far).

**Where the game is now: its frame loop, at 30 fps, in a window.** It records command lists,
presents once a frame into the game window and waits for frame events between frames, for
the whole of a run without a crash. The window is black: nothing runs the title's GPU
commands yet. Next: the per-frame probes (device 70 and 81, queue 35, the command list's
slots), then the GPU back end.

### Local tools (in `C:\Project Americas\tools`, never in the repository)

| Tool | Version, source | Use | Note |
|---|---|---|---|
| `amd-isa-xml\amdgpu_isa_rdna2.xml` | AMD machine-readable GPU ISA, 2026-08-06 release, gpuopen.com | The source for the RDNA 2 decoder tables: 1,153 instructions in every RDNA 2 encoding | MIT licence: generated tables that are committed must carry AMD's copyright notice |
| `downloads\amd-rdna2-isa\*.pdf` | AMD RDNA 2 ISA reference guide (Nov 2020), docs.amd.com | Reading the instruction semantics | AMD's terms forbid copying or redistributing it: local reference only |
| `llvm-23.1.6\bin\llvm-mc.exe`, `llvm-objdump.exe` | LLVM 23.1.6 Windows release, github.com/llvm | A second x86 disassembler next to `dumpbin` | **The official Windows build has no AMD GPU target** (x86 and ARM64 only), so it cannot check the RDNA 2 decoder |
| `zstd-1.5.7\zstd.exe` | zstd v1.5.7, github.com/meta/zstd | Unpacking LLVM's packages (`-d --long=31`); Windows' own `tar` refuses their window size | — |
| `amd-isa-xml\amdgpu_isa_rdna1.xml` | AMD machine-readable GPU ISA (same release) | RDNA 1's instructions the console GPU keeps (the MAD family) | MIT, like the RDNA 2 file |
| `llvm-amdgpu-23.1.6\bin\llvm-mc.exe`, `llvm-objdump.exe` | built here from `llvm-project-23.1.6.src.tar.xz` (179 MB, github.com/llvm, SHA-256 `d3b86ab2…35a31da`) with only the AMDGPU target, MSVC, about an hour on Dev PC 1; script and sources in `downloads\llvm-src` | **The RDNA 2 oracle**: `scripts/rdna2_oracle.py`; also an assembler for test programs | Apache 2.0 with LLVM exceptions; local tool only |


Next, in parallel:
* **M2.** The D3D12.X device (M2.4): the per-frame probes, then the rest of the device and
  command list slots the game reaches, `PresentX`,
  frame pacing and a window; threads and topology, GameInput devices, and the remaining
  XGameRuntime areas (XPackage, XPersistentLocalStorage, XDisplay HDR) as the game reaches
  them.
* **M3.** The translator: IR, structuring (and the indirect jumps), stage I/O from the FSSM
  records, resource tracing, HLSL emit through DXC. Then `am-shaders precompile`.
