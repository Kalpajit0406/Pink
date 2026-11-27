# Pink

**Grand Theft Auto VI (Xbox Series) running natively on Windows, built on your PC from your own copy.**

[Status](#project-status) | [This machine](#this-machine) | [How to build](#how-to-build) | [Memory](#memory) | [How it works](#how-it-works) | [Want to help?](#want-to-help) | [License](#license)

Pink is a static recompilation project. The Xbox Series CPU is x86-64 like a PC, so the game's code is loaded
and run natively, and the console's system software is reimplemented. The console's precompiled RDNA2 GPU programs
are translated to SPIR-V for Vulkan. The game's models, textures and audio are read from your package. All of this
happens **on your PC, from your own copy of the game**.

> [!IMPORTANT]
> This repository **does not contain the game, keys or an executable**, and it does not explain how to get them. You
> need your own copy, with its files already readable on disk. Nothing here decrypts or works around any protection.
> Everything the build creates (the generated shaders, extracted assets, the runtime folder and your saves) comes
> from your game and stays on your PC. Never share those files.

## Project status

**Early. Nothing is playable yet.** On the machine below, the game opens, shows the Rockstar logo and the Jason
and Lucia loading slideshow, then "Two Years Ago", runs the story load to its end, and crashes at world load —
24/24 GB RAM, 6/6 GB VRAM used. A first world frame has not been verified yet. On native Windows a scripted pad
alone (`LEONIDA_INPUT_SCRIPT`, no keyboard or controller) gets through every first-run page, including the
legal-terms page whose Confirm waits for both boxes, to the loading screen of the story load
([the route](docs/first_run_ui.md#through-the-first-run-pages-with-the-scripted-pad)).
[First-run UI on Windows](docs/first_run_ui.md) has the build and launch steps.

| What | Status |
|---|---|
| Booting the game | ⚠️ Through the story load into the world phase; crashes at world load, no world frame verified yet |
| Story mode, anything playable | ❌ No |
| Finding and checking your copy (Xbox Series 1.0.2.1) | ✅ Works |
| Shaders: carve, unpack native RDNA2 code, disassemble | ✅ All 26,503 |
| Shaders: RDNA2 → SPIR-V translation | ✅ **26,503 / 26,503 translated**: 2,236 VS, 12,101 PS, 2,689 CS, 9,282 mesh, 195 geometry, with a per-shader metadata index for pipeline creation. ⚠️ 23,841 are wave64: exact on 64-wide subgroups. On 32-wide subgroups (this machine), compute and mesh shaders are exact with wave64 emulation (`--wave64-emulate`, on by default); cross-half wave ops in vertex, pixel and geometry shaders are still approximate |
| SPIR-V validation (`spirv-val`) | ✅ **26,503 / 26,503 passed** |
| Shader translator checked on this GPU | ⚠️ The hand-written GPU tests (vertex fetch, texture sampling, exports, ALU ops, partial-register ops, barycentrics, wave-uniform branches, compute LDS/barriers/buffer stores, wave64 masks) run on this machine's RTX 3050 with wave64 emulation. These are small test shaders: the game's own shaders are checked by `spirv-val` and by driver pipeline creation (`vk_pipeline_check`), not by their output |
| Models (OBJ with real names, UVs, material references) | ✅ Exported. A ten-archive scan recovered **382,781 models** with zero readback decode failures. This includes **343,327** models from `levels.rpf`. A full OBJ export of that archive is pending. See [recount and export scope](docs/models.md#full-archive-re-count-at-e293917). |
| Texture dictionaries and world texture pools (DDS) | ✅ Exported. `rpf9dump` decodes BCPack BC1, BC3, BC4 and BC5 chunks in strict mode and skips BC6H, BC7 and chunks that need unconfirmed history; the assets stage then completes what it can with a clean-room decoder (`tools/bcpack.py`, which `tools/ttpf_assets.py` runs through `tools/ttpf_mips.py`) that rebuilds BC1, BC3, BC4, BC5, BC6H and BC7, and records the rest as a `BCPackLimitation` in `manifest.json` (most BC6H/BC7 chunks and the unconfirmed-history ones need a local history table, which the repo does not ship) ([notes](docs/textures.md)) |
| Audio banks (AWC: Opus, XMA2) | ✅ Exported whole. Opus repackaged **losslessly** as Ogg Opus; PCM (codec 0/2) as WAV; XMA2 decoded with ffmpeg. `--wav` adds 16-bit PCM WAV ([notes](docs/audio.md)) |
| Music/radio config | ? Measured. **1,589** audio-config records with exact durations and AWC stream ids; station grouping and content-id lists; no per-track start offsets ([notes](docs/audio_config.md)) |
| PE loader and import shims | ⚠️ Runs a stand-in executable end to end (`gta6rt --run`: CRT startup, statics, heap, files, threads, GDK imports through the shims, exit code back). Controllers through a GameInput shim on SDL3 (any controller SDL supports, rumble, DualSense trigger effects and lightbar; built when SDL3 is installed or with `-DLEONIDA_SDL3_FETCH=ON`, [notes](docs/gameinput.md)). `--trace-d3d12` logs every D3D12 call and its outcome for the first boot. Runs the game's own executable through the story load |
| Xbox D3D12 → Vulkan | ⚠️ In progress. A Vulkan-backed device behind the Xbox-ABI proxy creates queues, command lists, fences, descriptor heaps, committed, placed and reserved resources, render-target and depth views (3D targets included), texture views and samplers (2D, cube, 1D and 3D views, the depth and stencil planes of depth-stencil textures, packed formats such as B5G6R5, B4G4R4A4 and R9G9B9E5, buffer SRVs — raw, structured and typed; they go into the bindless set), root signatures, pipelines and query heaps, and draws run on Vulkan. Barriers, clears, uploads, copies (BC included), colour resolves, predication, CPU-side texture reads and writes, and occlusion, timestamp and pipeline-statistics queries run on Vulkan, checked on this GPU. The console-shaped command-list proxy, the measured `PresentX` queue slots (30/32, layout-detected) wired to the presentation core, and `AllocateUploadMemory` (list slot 148, a mapped-upload arena whose addresses translate to buffer device addresses in root descriptors and IA/SO views) cover the console-ABI surface the package measures. Still open: the console list tail and the descriptor-table record model ([plan](docs/d3d12_vulkan.md), [console ABI](docs/d3d12_xs_console_abi.md)) |
| Ray tracing (BVH) | ⚠️ In progress. `BuildRaytracingAccelerationStructure` builds BLASes and TLASes on the GPU in the console's XDXR BVH4 layout (the default; `LEONIDA_RT_AS=khr` uses Vulkan's own acceleration structures instead), which the translated shaders walk themselves through `image_bvh_intersect_ray` (all four triangle node types, TRI0-TRI3, with the barycentric remap byte); the GPU builder writes TRI0 and TRI1 leaves. Updates refit instead of rebuilding, and clone, compaction and the compacted/current postbuild sizes work; checked against a CPU reference on synthetic scenes. The build descriptions are recovered from the package's `XboxBvhBuilder.dll` ([notes](docs/xdxr.md)). The game's `*_bvh4.rpf` packs are measured: offline-built, dehydrated BLASes that rehydrate through that DLL with their geometry from the drawables in `levels.rpf` (measured on 8 of the 10 sampled BLAS; [notes](docs/bvh.md)). Not done in the builder: serialization. TRI1 pairing (no memory saved yet) and BOX16 nodes for `MINIMIZE_MEMORY` BLASes (an assumed switch) are in ([notes](docs/bvh.md)). Open: whether the game rehydrates the packs or rebuilds them |

The table above is written by hand. To measure it on your own checkout (unit and runtime tests, which rows of
the translator contract were confirmed on a GPU, measured imports, your corpus counts and, with `--github`, CI
and the `D3D-` items), run `python -I tools/progress_dashboard.py --out <folder outside the repo>` and open
`progress.html` ([notes](docs/progress_dashboard.md)).

## This machine

The only configuration this README covers.

| | |
|---|---|
| Machine | Dell G15 5530 |
| CPU | Intel Core i5-13450HX |
| RAM | 24 GB DDR5 |
| GPU | NVIDIA GeForce RTX 3050, 6 GB VRAM (11 GB shared graphics memory) |
| OS | Windows 11 |
| Backend | Vulkan (`LEONIDA_D3D12=vulkan`) |
| Console profile | Series S (`LEONIDA_CONSOLE_PROFILE=series-s`) |
| Render resolution | 720p |
| Memory preset | Governor Preset B — Series S, 3.5 GB texture pool, 64 MB staging, 128 MB ring |

If your machine is different, this README does not cover it. The numbers, the preset and the crash behaviour
above are all measured on this one machine.

## How to build

The port isn't downloaded ready-made. It's built on your PC from your own copy.

**The easy way:**

1. Install the prerequisites below.
2. Download this project **with its submodule**: `git clone --recursive https://github.com/Kalpajit0406/pink`. A
   "Download ZIP" from GitHub leaves out `third_party/rpf9-explorer`. The build fetches it if `git` is installed,
   and otherwise tells you what to do.
3. Double-click **`Build.bat`**.

It finds your copy of the game by itself: it looks in Downloads, Desktop, Documents (also where Windows keeps them,
such as in OneDrive), Games, your home folder and the top of every drive, two folders deep. It recognises the
version from a few file sizes, without reading anything large. If you have more than one copy it asks which one, and if it finds none it asks you to paste the
folder. Before starting it checks that everything below is installed and that there is enough free space, and it
lists anything missing in one go. The result goes to `Documents\Leonida`. A stopped build continues where it left
off.

Your game folder is the one that holds `MicrosoftGame.Config` and the `.rpf` archives, as plain files.

**Prerequisites:**

- **Your own copy** of the Xbox Series package, version 1.0.2.1.
- **Windows 10/11 x64**, with about **400 GB** free for work files (an assets export alone measured about 337 GB before its audio step, in about 2 h 35 min). Disk sizes here are as Windows Explorer shows them, which are GiB: 400 GiB is about 430 GB in decimal units. They can go on another drive with `--work`.
- [Python 3.12+](https://www.python.org/downloads/) (tick "Add python.exe to PATH").
- [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0).
- [Vulkan SDK](https://vulkan.lunarg.com/), for `spirv-as` / `spirv-val`.
- [LLVM 21+](https://github.com/llvm/llvm-project/releases) with the **AMDGPU target** for `llvm-mc`. The official
  Windows build has no AMDGPU target. On Windows, `scripts\build_llvm_mc.ps1` builds a native `llvm-mc.exe` with it
  (no WSL; needs git and the Visual Studio C++ tools below) and sets `LLVM_MC`; it takes about 20-40 minutes. A faster
  way, if you use [conda](https://docs.conda.io/): `conda install -c conda-forge llvm-tools` (its Windows build
  includes the AMDGPU target), then set `LLVM_MC` to the full path of the `llvm-mc.exe` it installs (`where.exe llvm-mc` shows it once the environment is active; in PowerShell a
  bare `where` is `Where-Object`).
- Visual Studio 2022 with **Desktop development with C++** and the **Windows SDK 10.0.26100 or newer**, for the
  runtime stage (the runtime uses version 1.2 root signatures, which older SDKs don't declare).
- [ffmpeg](https://ffmpeg.org/download.html) on PATH (optional: decodes the XMA2 streams, and required for `--wav`).

**Low disk space:** the ~400 GB figure is for the full build. To *run* the port, `--stages check,shaders,runtime`
needs about 30 GB; the `assets` stage (~370 GB) is only for the asset tools, and `--wav` adds several times the
Ogg Opus size (the preflight prints this choice when the assets stage is what does not fit). `--work` and `--out`
can sit on another drive. The runtime maps the package in place, so a read-only package drive needs
`gta6rt --overlay <writable folder>`; `--overlay` is a `gta6rt` option, not a `make_game.py` one. Leave the `.s`
files in `work/shaders/dxbc/disasm` in place: the next run checks them against the list in `disasm.done` and
disassembles the missing ones again (all of them for a folder whose `disasm.done` predates that list), then
translates every shader again. A `disasm` folder that is a symlink is refused when the stage has to clear it.

**Disk space and drives:** the build uses three folders, and each can be on its own drive:

| Folder | Flag | What is in it | Size | Read when playing |
|---|---|---|---|---|
| your package | `--game` | your copy of the game; only read, never written | as copied | yes, in place (`game_path.txt`) |
| work | `--work` (default `<out>\work`) | `shaders` (carved, disassembled, translated) and `assets` (exports) | about 30 GB + about 370 GB | only `shaders\spirv` (`shader_path.txt`) |
| port | `--out` (default `Documents\Leonida`) | `gta6rt.exe`, its DLL, the path files, the pipeline cache, `overlay` | small | yes |

The free-space check before a build counts the `--work` drive only, because that is where the large files go.
Nothing in `<work>\assets` is read by the game, so with a small SSD you can keep what the game loads on the SSD and
put the exports on a hard drive, in two runs:

```powershell
# on the SSD: the package, the port and the translated shaders (about 30 GB of work files)
python -I make_game.py --game "S:\Games\package" --out "S:\Games\Leonida" --work "S:\Games\Leonida\work" --stages check,shaders,runtime,pipelines
# on the hard drive: the asset exports (about 370 GB), only for the asset tools
python -I make_game.py --game "S:\Games\package" --out "S:\Games\Leonida" --work "H:\leonida-assets" --stages check,assets
```

The second run does not rebuild the runtime, so `shader_path.txt` keeps pointing at the SSD. Pass the same
`--work` again for later runs of each stage so they resume. A single `--work "H:\leonida-work"` also works; the
game then loads its translated shaders from that drive.

**From a terminal**, with the same behaviour and more control:

```powershell
python -I make_game.py --find                          # list the copies found on this PC
python -I make_game.py --pick                          # choose the game folder in a folder dialog
python -I make_game.py --check                         # check your copy, build nothing
python -I make_game.py --check --full-check            # also compare SHA-256 of every file (slow)
python -I make_game.py --game "D:\path\to\your\package" --out "D:\Games\Leonida" --work "E:\leonida-work"
python -I make_game.py --stages shaders,assets         # only some stages
python -I make_game.py --game "D:\path\to\your\package" --wav   # also decode Opus to PCM WAV (large)
python -I make_game.py --wave64-emulate off            # auto (default), on or off; see below
```

`--wave64-emulate` decides whether wave64 compute and mesh shaders are translated to be exact on any subgroup size
(see "Wave64 emulation" in [docs/design.md](docs/design.md)). This GPU has 32-wide subgroups and needs it. The
default, `auto`, asks `vulkaninfo` (part of the Vulkan SDK) and turns emulation on if any GPU lacks 64-wide
subgroups or its subgroup size is unknown. It also checks whether the runtime's GPU can run compute and mesh
shaders 64-wide. With `--mesh-compute`, only compute subgroup support is needed.

The mode is saved next to the translated shaders, and changing it translates them again. `auto` can normally
reuse previously emulated shaders.
Use `--wave64-emulate on` to force emulation or `--wave64-emulate off` to force native wave64.

Four fallback flags are forwarded to the translator the same way, saved next to the translated shaders, and changing
them translates them again: `--no-float-controls` (drop `SignedZeroInfNanPreserve`, for devices whose pipeline check
skips a shader without `shaderSignedZeroInfNanPreserveFloat32`), `--bary-fallback`
(read raw barycentrics from an interpolated input, for devices without `VK_KHR_fragment_shader_barycentric`),
`--texel-offset-fallback` (fold `image_sample*_o` offsets into the coordinate, for devices without
`VK_KHR_maintenance8`) and `--mesh-compute` (mesh shaders as compute, for devices without `VK_EXT_mesh_shader`).
Each is documented in [docs/design.md](docs/design.md).

The build records which llvm-mc command and version produced the disassembly. Changing either, or reusing a work
folder that predates this record, reruns disassembly and translation while keeping the extracted shader inputs.

The build records which translator sources produced the shaders. A change to those files reruns translation using the
saved disassembly. Older work folders need one translation after upgrading to record this information.

The build also records which decoder (the `third_party/rpf9-explorer` commit and the `tools/rpf9dump` sources) exported
each archive's models and textures and each texture pool. When it changes, the assets stage removes that export and
makes it again. Older work folders have no record, so their models, textures and pools are exported once more after
upgrading.

Stages: `check` always runs first; then `shaders`, `assets` and `runtime` run together when selected (they do not
depend on each other). `pipelines` starts once the translated shaders are ready and, when this run includes runtime,
that stage has finished rebuilding `vk_pipeline_check`, so it can overlap the rest of assets; it creates every
translated shader on your GPU driver, reports any it has to skip, and saves a pipeline cache to `pipeline_cache.bin`
in `--out`, which also holds `gta6rt.exe` after the `runtime` stage. The runtime seeds its Vulkan pipeline cache
from the file beside the executable when its header matches the device and pipeline cache UUID. It falls back to
an empty cache if the file is missing, incompatible, or rejected by the driver. Shader translation is the longest
CPU step. It runs on `--jobs` processes (half the CPU threads by default). When shaders and assets share the machine,
that budget is split between them and passed to `rpf9dump` as `--jobs`. A solo assets run still lets `rpf9dump` use
every core. Progress lines include percent and a rough ETA for translation shards, asset steps, and overall stage
completion.

On an interactive terminal, a setup screen lets you set jobs and whether stages overlap, then a key-switchable
per-stage log view (`1`–`4` / `Tab`) keeps parallel output demuxed; full stage logs go under `work/logs/`. Use
`--no-tui` (or redirect stdout) for the plain console.

`--wav` is off by default because 16-bit PCM WAV is several times larger than Ogg Opus. It decodes the repackaged Ogg
Opus streams to `work\assets\audio\wav` after the audio bank step; existing WAV files are skipped when a build resumes.

When the Vulkan backend is selected with `LEONIDA_D3D12=vulkan`, the packaged runtime finds the translated shaders
in `<work>/shaders/spirv` using `shader_path.txt` beside `gta6rt.exe`. Set `LEONIDA_SHADERS` to use another directory.

## Memory

The crash at world load is a memory crash: the title's RAM copy and the GPU's VRAM copy hold the same data twice,
and this machine runs out of both. The memory programme attacks that duplication — see
[MEMORY_SPEC.md](MEMORY_SPEC.md) for the full brief:

- **Accounting (M0):** 15-category tracking at every allocation — guest heap, texture/buffer/render-target VRAM,
  staging, upload ring — reported to `sotm_memory.jsonl` every 5 s, plus a crash dump (`crash_memory.json`).
- **Governor (M8):** auto-detects this machine as Preset B and applies the Series S profile, 64 MB staging,
  128 MB ring.
- **WDDM overcommit paging (M4):** `VK_EXT_pageable_device_local_memory` + `VK_EXT_memory_priority` let
  allocations that would fail at world load page to system RAM instead of crashing.
- **Budget enforcement (Phase 1):** `LEONIDA_MAX_VRAM_MB` / `LEONIDA_MAX_HOST_RAM_MB` are real ceilings now, with
  pressure tiers (Normal / Elevated / Critical / OverBudget) and an adaptive mip cap (1024 → 512 → 256 as VRAM fills).
- **Eviction (Phase 2):** idle title textures evicted under pressure, per-frame staging slice, scratch shrink.
- **Page handoff (Phase 3):** single-owner 64 KiB page handoff with `MEM_DECOMMIT` and ColdShared recovery.

Still open: in-place heaps (the GPU reads the title's own pages, no second copy) and GPU decompression in the
DirectStorage path — nobody in this family has built either yet.

## How it works

| Piece | What it does | Where |
|---|---|---|
| Version check | Every package file's name, size and SHA-256 for each supported version | `game/`, `make_game.py` |
| Shader pipeline | Carves DXBC containers from `shaders.rpf` without its encrypted table of contents. Unpacks the `XBCH` → `FSSM` native RDNA2 program, disassembles it with LLVM (`gfx1013`) and translates it to SPIR-V | `tools/shader_carve.py` ([optional Rust scanner](tools/shader_carve_native/README.md)), `fssm_unpack.py`, `shader_disasm.py`, `rdna2spirv.py` |
| Asset export | Finds validated resources inside RPF9 archives and the loose `.ttpf` texture pools | `tools/rpf9dump` (on rpf9-explorer's Core) |
| Runtime | Maps the PE image, resolves imports to shims (GDK, XMem, `d3d12_xs`) | `runtime/` |
| Research tools | PE/RTTI inspection, raw carving, texture tiling probes, GPU test harnesses | `tools/`, `runtime/test/` |
| Progress dashboard | One offline page of what passes, what changed since it last passed and what blocks next, built from test results and the docs' own tables; nothing is written inside the repo | `tools/progress_dashboard.py` |

Localized string tables are decoded by `tools/text_tables.py` ([docs/text.md](docs/text.md)); their language is
guessed from script (kana, hangul, CJK, Cyrillic, Arabic) or Latin words, and subtitles are joined to dialogue lines
across languages.

The translator's runtime contract (user SGPRs, push constants, memory model, bindless images, vertex fetch,
system values) and all the format notes are in [docs/design.md](docs/design.md); the format, asset, runtime
and validation notes are indexed in [docs/README.md](docs/README.md). The Xbox D3D12 plan is in
[docs/d3d12_vulkan.md](docs/d3d12_vulkan.md). Audio bank formats and the export tools are in
[docs/audio.md](docs/audio.md).

## Want to help?

The biggest open pieces:

- **First world frame**: the story load finishes, but no world frame has been verified yet. The crash is memory:
  RAM and VRAM both fill at world load. See [Memory](#memory) and [MEMORY_SPEC.md](MEMORY_SPEC.md).
- **In-place heaps**: let the GPU read the title's own pages instead of a second copy. The hook point is the
  placed-resource fallback (slot 133).
- **GPU decompression**: the DirectStorage path is CPU-only (disk → RAM staging → CPU decode → title RAM → VRAM).
  Nobody in this family has built GPU decompression yet.
- **Shader translator on hardware**: compute, mesh and geometry now translate (see the translator contract in
  [docs/design.md](docs/design.md)); still open is wave64 cross-half code in VS/PS/GS on 32-wide subgroups. The
  vertex-shader input layout is measured: all 2,236 VS are legacy (non-NGG), with `v0` = `VertexIndex` (the
  *Inputs and outputs* row of [docs/design.md](docs/design.md)).
- **Model recovery**: investigate the 71 unrecovered fragment candidates in [`levels.rpf`](docs/models.md#full-archive-re-count-at-e293917).

See [CONTRIBUTING.md](CONTRIBUTING.md). Never attach game files, extracted assets or generated shaders to issues or
pull requests.

## Credits

- [rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer) by breadbbbb: the RPF9 model, texture-pool and AWC
  decoders used by `tools/rpf9dump` (git submodule).
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT): GFX10 resource-descriptor layout (`tools/gfx10_formats.json`),
  and the ACO rules used as reference for 16-bit results and shader system registers.
- [libHttpClient](https://github.com/microsoft/libHttpClient) (MIT) by Microsoft: the XTaskQueue/XAsync implementation
  in `third_party/xtaskqueue`, used by the offline `xgameruntime` shim.
- [LLVM](https://llvm.org/): AMDGPU disassembler.
- Logo: ["Grand Theft Auto VI" icon by SlyDevil](https://www.steamgriddb.com/icon/122618) on SteamGridDB.
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools): assembly and validation.

Grand Theft Auto and all related names are trademarks of their owners. This project is not affiliated with or
endorsed by them.

## License

GPLv3 (or any later version). See [LICENSE](LICENSE). Third-party code keeps its own license.
