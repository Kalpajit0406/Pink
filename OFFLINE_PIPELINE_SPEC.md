# Offline Pre-Game Translation, Shader Precompilation and Cost-Cutting — Specification

Status: proposal v1 (2026-11-27). Audience: the AI coding agents working in `github.com/kalpajit0406/pink` (Leonida continuation).
Companion to `MEMORY_SPEC.md` (runtime memory architecture). **Do not edit `MEMORY_SPEC.md` from this track.** Section 2 defines who owns what.

Items marked **VERIFY** must be checked against the code or hardware before you rely on them. Numbers marked *proposal* are starting targets to be replaced by measurements from milestone O0.

---

## 0. Rules of engagement

1. **Measure first.** Every artifact and every technique needs a before/after number (frame time, hitch count, RAM, VRAM, disk, build time). No number, no merge.
2. **Everything generated stays local.** All outputs are built on the user's machine from the user's own copy of the game. Never commit, attach or upload game files, extracted assets, generated shaders, packs, patch tables, caches or saves. The repo's `.gitignore` must cover every output directory this spec creates.
3. **No circumvention.** Never add code that decrypts, unlocks or bypasses protection. The input is a package whose files are already readable on disk, as the README states.
4. **Never write a modified game executable to disk.** Code patches (section 7.4) are applied **in memory at load**, are byte-checked, and the patch table stores only offsets and hashes of expected byte windows, not the game's bytes.
5. **Every artifact is optional at runtime.** If an artifact is missing, stale or fails validation, the runtime falls back to the current live path and logs why. No artifact may make the game unable to start.
6. **Every artifact is keyed and versioned** (section 5). A key mismatch means rebuild, never "use anyway".
7. **Keep a reference path.** A flag (`LEONIDA_OFFLINE=0`) disables all offline artifacts so results can be A/B compared.
8. **Licensing.** The repo is GPL-3.0-or-later. Copy code only from GPL-3.0-or-later sources, name the source repo and commit in the commit message. Do not copy from GPL-2.0-only projects (for example XCL); read them for behaviour only.
9. **Do not assume the architecture.** The README describes a Vulkan backend. The owner also reports DX12.x, DirectStorage and an NVIDIA shader-cache hook. Answer the section 12 questions from the code first.

---

## 1. Purpose and scope

### 1.1 The question this spec answers

"Can we translate and compile ahead of time, play from pre-translated files, and so improve performance and cut memory?"

### 1.2 Honest answer

- **Shader translation is already offline.** Leonida's `shaders` stage carves, disassembles and translates all 26,503 RDNA2 shaders to SPIR-V before play, and all validate. Translating "earlier" will not speed up steady-state frames.
- **What is still live and can move offline:**
  1. Driver compilation of pipelines (hitches, not average fps).
  2. Texture detiling and BCPack decode on the CPU (CPU time, RAM, streaming stalls).
  3. Discovery of the game's write sites for memory sync (frame time, mirror size).
  4. Allocation classification (boot-time behaviour).
  5. Shader variant selection and optimisation (some GPU time).
  6. Analysis that finds cheap or skippable passes (cost cutting).
- **What offline work cannot do:** remove per-draw API translation, the game's own memory traffic, or the driver's own compile step; make a GPU-bound laptop fast. On the owner's RTX 3050 (6 GB), the GPU may remain the wall. Resolution scaling and pass-level cost cutting are the biggest GPU levers (section 8).

### 1.3 In scope

- Build-system stages, manifests and cache keys for offline artifacts.
- Shader archive, shader metadata, optimised and variant SPIR-V.
- Pipeline manifest recording and background pre-warming.
- GPU-ready texture pack.
- Executable analysis producing a byte-checked patch table.
- Memory profile generation (consumed by `MEMORY_SPEC.md` C4).
- Cost-cutting technique catalogue and the offline analyses that drive it.
- A runbook for running all of this on the owner's laptop.

### 1.4 Out of scope

- Obtaining or decrypting game files.
- Runtime memory policy (page table, eviction, residency): see `MEMORY_SPEC.md`.
- Ray tracing (leave optional and off by default).

---

## 2. Ownership boundary with MEMORY_SPEC.md

Two agent teams work in parallel. This table prevents duplicate or conflicting work.

| Topic | This spec (offline) | MEMORY_SPEC.md (runtime) |
|---|---|---|
| Memory profile (`memory_profile.bin`) | **Generates and versions it** (O8) | Consumes it in the classifier (C4) |
| Provenance records | **Texture pack provides (file, offset, codec) mapping** (O6) | Consumes in restreaming (M6) |
| Patch table for descriptors/constants/uploads | **Generates it** (O7) | Uses the in-place data path (C7, M5) |
| Shader modules on demand | **Archive format and metadata** (O3) | Overhead diet C3 uses it |
| Pipeline cache and warm list | **Owns recording, format, warming** (O4) | Pipeline-cache trimming by RAM (C3) |
| Texture residency and budgets | Not owned | **Owns** (C8) |
| Page table, handoff, governor | Not owned | **Owns** |
| Observability, replay harness | **Shares** (needs replay for verification) | Owns the harness (C12); this spec only adds assertions |

**Interface contract:** every artifact has a versioned header (`magic`, `format_version`, `key`, `created_utc`). The runtime reader must reject unknown versions and fall back. Format changes need a version bump and a note in both specs' change logs.

---

## 3. What exists today (from the README, to be verified in code)

Build stages in `make_game.py`: `check` (always first), then `shaders`, `assets`, `runtime` (independent), then `pipelines`.

| Stage | What it does today | Cost |
|---|---|---|
| `check` | Finds the package, recognises version 1.0.2.1 by file sizes/hashes | Seconds |
| `shaders` | Carves DXBC from `shaders.rpf`, unpacks `XBCH`->`FSSM` native RDNA2 code, disassembles with LLVM `llvm-mc` (gfx1013), translates to SPIR-V (`tools/rdna2spirv.py`). All 26,503 translate. Longest CPU step, runs on `--jobs` processes (half the CPU threads by default). Work files about 30 GB. | Long, CPU-bound |
| `assets` | Exports models (OBJ), textures (DDS), audio. About 370 GB of exports. **Not needed to play.** | Hours, huge disk |
| `runtime` | Builds `gta6rt.exe` and the D3D12->Vulkan layer. Needs MSVC and Windows SDK 10.0.26100+ | Minutes |
| `pipelines` | Creates every translated shader on **your GPU driver**, reports skipped ones, saves `pipeline_cache.bin` in `--out` | CPU/GPU |

Runtime behaviour: seeds the Vulkan pipeline cache from the file beside the executable **only if** its header matches the device and pipeline-cache UUID, otherwise starts empty. Finds translated shaders through `shader_path.txt` or `LEONIDA_SHADERS`. Maps the package in place; use `gta6rt --overlay <writable folder>` if the package drive is read-only.

Translator flags recorded next to the shaders; changing them retranslates: `--wave64-emulate auto|on|off`, `--no-float-controls`, `--bary-fallback`, `--texel-offset-fallback`, `--mesh-compute` (on by itself for Intel's Windows driver), `--native-mesh`.

Shader census (README): 2,236 VS, 12,101 PS, 2,689 CS, 9,282 mesh, 195 geometry. 23,841 are wave64. NVIDIA and Intel GPUs have 32-wide subgroups: compute and mesh shaders are exact with wave64 emulation, but cross-half wave ops in VS/PS/GS are still approximate.

Known gaps relevant here: BC6H/BC7 chunks that need a history table the repo does not ship are recorded as `BCPackLimitation`; the console command-list tail and descriptor-table record model are open in the D3D12->Vulkan layer; nothing is playable yet and no world frame is verified.

---

## 4. Live costs that offline work can attack

| ID | Live cost | Evidence | Offline remedy | Section |
|---|---|---|---|---|
| L1 | Driver pipeline compile on first draw | Belobug findings: 270-660 s of a session in frames over 200 ms, almost all while new pipelines were made | Pipeline manifest + background warm | 7.2 |
| L2 | All shaders resident as modules | 26,503 SPIR-V files | Memory-mapped archive, create modules on demand | 7.1 |
| L3 | Wave64 emulation applied broadly on 32-wide GPUs | 23,841 wave64 shaders | Per-shader cross-lane analysis, native-32 variant where safe | 7.1 |
| L4 | Texture detiling and BCPack decode on the CPU | Texture-store experiments cost about 8 ms/frame in the Belobug fork (off by default) | Converted texture pack, direct upload | 7.3 |
| L5 | Page-fault write tracking and descriptor scans | About 600,000 descriptor slots scanned per frame; guards and refreshes dominate the submission thread | Byte-checked patch table reporting writes explicitly | 7.4 |
| L6 | Unknown allocation classes at boot | Needs warm-up to classify | Saved memory profile | 7.5 |
| L7 | Unoptimised SPIR-V from ISA translation | Translator output quality unknown | Offline `spirv-opt`, per-shader variants, verified by replay | 7.1 |
| L8 | Expensive passes that the target hardware cannot afford | RTX 3050 laptop is much weaker than the reference GPUs | Pass cost table, presets, reduced-cost shader variants | 8 |

---

## 5. Artifact catalogue, keys and directory layout

### 5.1 Artifacts

| Artifact | File(s) | Producer | Consumer | GPU-specific? | Key inputs |
|---|---|---|---|---|---|
| SPIR-V (existing) | `work/shaders/spirv/*`, `index.json` | `shaders` stage | runtime | No | package version, shader blob hashes, translator sources hash, llvm-mc command+version, translator flags (incl. wave64 mode) |
| Shader archive | `shaders.pack` + `shaders.idx` | `shaderpack` stage | runtime | No | SPIR-V key + spirv-opt version + variant policy |
| Shader metadata | inside `shaders.idx` | `shaderpack` | runtime, tools | No | same |
| Pipeline cache | `pipeline_cache.bin` (existing) | `pipelines` stage | runtime | **Yes** | vendorID, deviceID, pipelineCacheUUID, driver version, shaderpack hash |
| Pipeline manifest | `pipelines.manifest` | runtime recorder, merged offline | warm tool | Partly (state hashes portable; compile results not) | shaderpack hash, layer version |
| Warm cache | extra entries inside the Vulkan pipeline cache | `warm` stage | runtime | **Yes** | pipeline manifest + GPU key |
| Texture pack | `textures.pack` + `textures.idx` | `texpack` stage | runtime | No (BC blocks portable) | package version, decoder commit (`third_party/rpf9-explorer`, `tools/rpf9dump`), `tools/bcpack.py` hash, pack format version |
| Patch table | `patches.tbl` | `patches` stage | runtime loader | No | SHA-256 of the exact game executable, analyzer version |
| Memory profile | `memory_profile.bin` | `memprofile` stage (from recorded sessions) | runtime classifier | No | package version, layer version, profile format version |
| Pass cost table | `passes.json` | `costs` stage (from replays) | quality presets | **Yes** (timings) | package version, GPU key, layer version |

### 5.2 Header (all binary artifacts)

```c
struct ArtifactHeader {
  char     magic[8];          // e.g. "LNDSHPK1"
  uint32_t format_version;
  uint32_t header_size;
  uint8_t  key[32];           // SHA-256 over the key inputs listed in 5.1
  uint64_t created_utc;
  uint64_t payload_size;
  uint8_t  payload_sha256[32];
};
```

### 5.3 Directory layout (all outside the repo, paths recorded in `*_path.txt` like `shader_path.txt`)

```
<out>/                      # --out, default Documents\Leonida  (small)
  gta6rt.exe  ...           # runtime
  shaders.pack  shaders.idx
  pipeline_cache.bin
  pipelines.manifest
  textures.idx              # index; textures.pack may live on <work>
  patches.tbl
  memory_profile.bin
  passes.json
  build_manifest.json       # section 6.2
<work>/                     # --work (large)
  shaders/ (dxbc, fssm, disasm, spirv)
  textures.pack             # optional location
  assets/                   # only if the assets stage is run
  logs/
```

---

## 6. Build system design

### 6.1 New stages

Extend `make_game.py` with resumable stages. Existing stages are unchanged.

| Stage | Depends on | Output |
|---|---|---|
| `shaderpack` | `shaders` | `shaders.pack/idx`, per-shader metadata, optional optimised and variant modules |
| `texpack` | `check` (package) | `textures.pack/idx` |
| `patches` | `check` (executable) | `patches.tbl` |
| `memprofile` | recorded sessions | `memory_profile.bin` |
| `costs` | recorded replays | `passes.json` |
| `warm` | `pipelines`, `shaderpack`, `pipelines.manifest` | warmed pipeline cache |

Existing `pipelines` stays the "every shader creates on this GPU" check and baseline cache; `warm` adds the draw-state pipelines seen in play.

### 6.2 `build_manifest.json`

One file records, per stage: inputs (with hashes), tool versions, flags, start/end time, duration, peak RAM, disk used, outcome, and the artifact key. A stage is skipped only if the recorded key equals the freshly computed key **and** the artifact validates (hash check). Changing any key input rebuilds that stage and everything downstream.

Record at minimum: package version/hash, executable SHA-256, translator source hash, `llvm-mc` command and version, SPIRV-Tools version, `spirv-opt` flags, wave64 mode, GPU vendor/device/UUID/driver, Windows build, layer git commit.

### 6.3 Behaviour requirements

- Resumable: a stopped build continues where it left off (matches today's behaviour).
- Parallel with a global job budget (`--jobs`); never exceed RAM (see 9.3).
- Never write inside the repo checkout; outputs go to `--out` / `--work`.
- Progress lines with percent and ETA per stage; full logs under `<work>/logs/`.
- `--check-artifacts` command: validates every artifact header, key and hash and prints a clear verdict, rebuilds nothing.

---

## 7. Artifact designs

### 7.1 Shader archive, metadata and variants (O3, O5)

**Goals:** cut RAM (modules on demand), enable per-shader decisions, hold optimised/variant SPIR-V.

**Format sketch**

```
shaders.idx   : header + sorted array of entries (binary search by 64-bit shader hash)
shaders.pack  : concatenated SPIR-V blobs (memory-mapped read-only)

struct ShaderEntry {
  uint64_t shaderHash;       // the hash the layer already uses to identify a console shader
  uint8_t  stage;            // VS PS CS MS GS (TS if present)
  uint8_t  flagsA;           // bit0 wave64, bit1 usesCrossLane, bit2 usesCrossHalfLane, bit3 emulatedW64,
                             // bit4 usesQuadOps, bit5 usesMeshOutputs, bit6 hasLoopHeavyBody
  uint16_t resourceLayoutId; // index into a layout table (push constants, bindless usage)
  uint32_t variantCount;
  struct { uint32_t kind; uint64_t offset; uint32_t length; } variants[];
  // kind: 0 = as translated, 1 = spirv-opt, 2 = native32 (no emulation), 3 = low-cost (section 8)
};
```

**Rules**
- `spirv-val` every blob at build time; refuse to pack a failure.
- Pin SPIRV-Tools to 2026.1 or later. Leonida's design notes say 2025.1 reports a known error on `CullPrimitiveEXT`.
- The runtime creates a `VkShaderModule` on demand and destroys it after pipeline creation (unless a pipeline library needs it alive; **VERIFY**).
- The `index.json` that exists today stays valid; the archive is an additional, faster path.

**Cross-lane analysis (drives L3).** Static scan of the disassembled RDNA2 (the `.s` files in `work/shaders/dxbc/disasm`) or of the translator IR for: `v_readlane`, `v_readfirstlane` patterns that matter, `v_permlane*`, `ds_swizzle`, `ds_permute/bpermute`, DPP modifiers, wave reductions/ballots, quad ops, `s_*` EXEC manipulation across halves. Output `flagsA` bits. A shader with none of the cross-lane bits behaves identically when run as two independent 32-lane halves, so it can use a plain 32-wide translation with no emulation overhead. **Do not trust the static scan alone:** verify each flagged-clean shader with the replay A/B (section 10). **VERIFY** how the translator currently applies `--wave64-emulate` (globally or per shader) before changing it.

**Optimisation (drives L7).** Offline variants with `spirv-opt` (try `-O` and `-Os` plus targeted passes such as dead-code elimination, copy propagation, local access-chain conversion, SROA-like passes). The NVIDIA driver re-optimises SPIR-V, so gains may be small: **measure** per-shader GPU time with and without. Keep only variants that win measurably and validate identically.

**Acceptance:** O3 and O5 criteria in section 11.

### 7.2 Pipeline manifest and background warm (O4)

**Why:** the `pipelines` stage creates each translated shader on the driver, but real graphics pipelines depend on draw state seen during play. Those compile on first draw and cause hitches.

**Recording (runtime, behind `LEONIDA_PIPE_RECORD=1`)** — one record per distinct pipeline:

```
PipelineRecord {
  shaderHashes[stage...];      // all stages
  renderStateHash;             // hash of the fields below
  colorFormats[], depthFormat, stencilFormat, sampleCount,
  blendState[], depthStencilState, rasterState, topology/primitive mode,
  vertexInputLayoutHash, specializationConstants[], pipelineLayoutId,
  dynamicStateMask;
  uint32_t seenCount; uint32_t firstSeenFrame; float maxCompileMs;
}
```

Merge records across sessions into `pipelines.manifest` (dedupe by shader hashes + state hash; keep counts).

**Warm tool (`warm` stage and `gta6rt --warm`)**
- Reads the manifest, compiles in descending `seenCount` order on background threads at below-normal priority, saves into the Vulkan pipeline cache.
- Budget: stop when pipeline memory crosses a configured cap (pipeline code lives in device memory; warm only the used set, not everything).
- Resumable and safe to interrupt.
- Re-run automatically when the GPU key changes (driver update, new GPU).
- Cache key = `vendorID, deviceID, pipelineCacheUUID, driverVersion, shaderpack hash`. The Vulkan cache header already carries vendor/device/UUID; validate it before use and rebuild on mismatch.

**Runtime miss policy (with MEMORY_SPEC C3 cooperation):** a pipeline not in the cache compiles on a background thread; the draw is skipped or replaced by a cheaper placeholder for those frames rather than blocking the render thread. Log every miss with its record so the next session pre-warms it.

**Extensions to evaluate (VERIFY each on the RTX 3050 driver):** `VK_EXT_graphics_pipeline_library` (link from pre-built parts, fewer state combinations), `VK_EXT_shader_object`, `VK_KHR_pipeline_binary`. Prefer whichever measurably cuts compile stalls; keep the plain path as the fallback.

**Also:** make sure the NVIDIA driver's own shader cache is not being thrashed (Control Panel "Shader Cache Size" unlimited). If an "NVIDIA shader-cache hook" exists in the repo, document what it stores and where (section 12).

**Metric:** count of frames over 50 ms and over 200 ms per 10-minute session, before and after.

### 7.3 Texture pack (O6)

**Why:** textures are converted at runtime from the console's tiled/BCPack form. A pack in a GPU-friendly layout removes CPU work and gives the memory spec its provenance records.

**What is known:** `tools/rpf9dump` decodes BCPack BC1, BC3, BC4, BC5 in strict mode and skips BC6H, BC7 and chunks needing unconfirmed history; the assets stage completes what it can with a clean-room decoder (`tools/bcpack.py` via `tools/ttpf_mips.py`) and records the rest as a `BCPackLimitation` in `manifest.json`. BC6H/BC7 chunks and unconfirmed-history ones need a local history table the repo does not ship.

**Design**
- Output plain BC blocks (BC1/3/4/5/6H/7 as stored) with full mip chains and mip-tail grouping, linear (untiled) layout per mip, aligned for direct `vkCmdCopyBufferToImage` or GPU upload. **Never expand to RGBA8.**
- Index entry: `{ matchKey, width, height, mips, format, arraySize, offset, length, sourceFile, sourceOffset, codec }`.
- `matchKey`: how the runtime recognises "this guest texture is that pack entry". Prefer `(sourceFile, sourceOffset)` from the loader; otherwise a hash over a fixed window of the console-format bytes plus size and format. **VERIFY** which is available at the point the layer sees the texture.
- Textures that hit a `BCPackLimitation` stay out of the pack and use the live path.
- Optional GPU detiling: if some textures must stay tiled, convert in a compute shader at upload instead of on the CPU (still cheaper than CPU detiling). Evaluate after the pack works.
- Compression of the pack for disk (for example GDeflate if a DirectStorage path is used; **VERIFY** that DirectStorage fits the Vulkan runtime, since it is a D3D12 API that GPU-decompresses only GDeflate).
- Size: the README measured about 337 GB for an assets export before audio. A runtime-only pack should be much smaller because it holds only needed textures and no OBJ/audio; **measure and report**. Provide a `--texpack-budget-gb` and an LRU-of-use build option (from recorded sessions) so low-disk machines pack only what they use.

**Runtime contract (with MEMORY_SPEC C8/C9):** on texture creation or upload request, look up `matchKey`; on hit, upload straight from the pack (memory-mapped) and leave the guest pages uncommitted; if the CPU later touches those pages, fall back to the old path. Record `provenance = (textures.pack, offset, length, codec=raw-BC)`.

### 7.4 Patch table for write sites (O7)

**Why:** most of the submission thread's time in the reference forks goes to noticing the game's writes (page faults, guards, descriptor scans). If the exact executable is analysed offline, the writes can be reported explicitly.

**Reference ideas (from the Belobug fork; not code to copy blindly):** report only the descriptor slots the game's code writes instead of scanning about 600,000 slots per frame (`VICTORY_PORT_DESCRIPTORS`); read the 174 MB constants heap in place (`VICTORY_PORT_CONSTANTS`); read constant buffers in place as raw bytes (`VICTORY_PORT_CBV`); check stores into guarded memory first and reset fewer guards.

**Design**
- Analyse the exact executable (version 1.0.2.1) with the repo's PE/RTTI tools (`tools/pe_inspect.py`) plus a disassembler library. Find: descriptor-table store sequences, constants/upload-heap write patterns, known hot writers.
- Generate `patches.tbl`: per entry `{ rva, windowLength, windowSHA256, patchKind, handlerId }`. **Store hashes, not the game's bytes.** Build it locally; never commit it.
- Loader applies patches in memory after mapping the image; any hash mismatch skips that entry and logs it; a mismatch in more than N entries disables the table.
- Key: SHA-256 of the executable. A different executable build means no table (fallback to the live path), never a guess.
- Handlers feed the in-place data path owned by `MEMORY_SPEC.md` C7.

### 7.5 Memory profile (O8)

- Record, per session, every guest allocation's range, size, creation flags, observed behaviour (CPU touches after first GPU use, read-back needs) and the final class from `MEMORY_SPEC.md` 4.3.
- Merge sessions with a confidence score; only classes seen consistently over several sessions are promoted.
- Output `memory_profile.bin` keyed on package and layer version. The runtime classifier (C4) loads it at boot; unknown allocations default to `CpuOnly`.

---

## 8. Cost-cutting catalogue

For the owner's hardware (RTX 3050 laptop 6 GB, 24 GB RAM) the GPU is likely the limit, so most gains come from doing less work. Every item needs a measured before/after and a visual comparison. Gains are **unmeasured**; the "Expected" column is qualitative.

| ID | Technique | Where it lives | Expected | Risk |
|---|---|---|---|---|
| C-01 | Lower internal resolution (about 540p-720p) with an upscaler (DLSS super resolution works on RTX 30-series; FSR as fallback). The title's TAA inputs (depth, motion vectors, exposure) must be exposed to the upscaler | Runtime + FidelityFX/NGX bridge | Largest GPU win | Image quality; integration effort |
| C-02 | Series S profile (about 8 GB game budget, 1080p output mode in the forks) | Runtime switch | Lower memory; small speed gain (the Belobug fork measured only about 5 ms on a 7900 XTX) | Softer image |
| C-03 | Mesh draw launch through an amplification/task stage (removes the clamp pass and full waits between mesh draws; Kelvaroq reported a 40% heavy-frame drop from this class of change) | D3D12->Vulkan layer | Large CPU/GPU idle reduction | Correctness of indirect dispatch counts (65,535-group limit) |
| C-04 | Native 32-wide variants for shaders without cross-lane ops | Shader archive (7.1) | Less ALU/register pressure on NVIDIA | Wrong flagging corrupts output; verify by replay |
| C-05 | Offline `spirv-opt` variants | Shader archive | Small, uncertain (driver re-optimises) | Time; validate |
| C-06 | Pass cost table + presets: skip or substitute the most expensive passes (volumetrics, SSR, shadows, SSAO and similar, found by per-pass timing, not guessed) | `passes.json` + runtime | Large on weak GPUs | Visible quality loss; some passes are required by later passes |
| C-07 | Low-cost shader variants: lower sample or step counts in loop-heavy passes (spec constants or SPIR-V patching of trip counts, behind a quality flag) | Shader archive variant kind 3 | Medium | Artifacts; experimental |
| C-08 | Texture mip floor and texture pool cap | Memory spec residency + presets | VRAM and bandwidth | Blurrier textures |
| C-09 | Barrier and copy audit: use `synchronization2`, minimal barriers; analyse recorded command streams offline to find redundant barriers, copies, clears and full-pipeline waits | Layer + offline analysis | Removes GPU idle bubbles | Sync bugs; validate with replay |
| C-10 | Pipeline libraries / shader objects | 7.2 | Fewer hitches | Driver support |
| C-11 | In-place constants and descriptor patching | 7.4 + memory spec C7 | Less CPU time | Executable-specific |
| C-12 | Ray tracing off by default; handle dropped RT draws cheaply (Kelvaroq reports about 515 dropped draws per frame because BVH structures aren't built) | Layer | Avoids wasted work | Lighting differences |
| C-13 | Frame pacing: limiter at 30 or 20 fps, present at most two frames ahead | Runtime | Stable pacing | Latency |
| C-14 | Shader module lifetime (create on demand, destroy after pipeline creation) | 7.1 | RAM | Pipeline libraries may need modules alive |

**Rule for C-06 and C-07:** decisions come from `passes.json`, produced offline from replays with per-pass GPU timings (timestamp queries). Never guess a pass is expensive. Identify passes by their shader-hash sets and render-target signatures, not by names.

**Reality check to keep in docs and issues:** a 7900 XTX is very roughly 5-8x faster than a 3050 laptop, and the best public numbers there are about 19-22 fps (Belobug) and about 10 fps on a 4090-class machine (Kelvaroq, self-reported). Reaching a stable 20-30 fps at 720p-1080p on the 3050 needs resolution scaling plus pass-level cuts, not shader tweaks alone. The first task is to find out whether the frame is CPU- or GPU-bound (render at 540p and 1080p; if frame time barely changes it is CPU-bound and the patch table and memory work matter more).

---

## 9. Running the offline build on the owner's PC

### 9.1 Can the laptop do it?

Yes for the stages needed to play: `check, shaders, runtime, pipelines`. Requirements and fit:

| Requirement | Detail | Owner's laptop |
|---|---|---|
| OS | Windows 10/11 x64 | Assumed OK (**VERIFY** build) |
| Disk for play-only stages | About 30 GB work files, plus the package itself | **VERIFY** free space and drive |
| Disk for full build | About 400 GB (assets export about 337-370 GB) | Not needed to play; skip the `assets` stage unless you want the asset tools |
| CPU | Shader translation is the longest CPU step | i5-13450HX: 6 P-cores + 4 E-cores, 16 threads (**VERIFY**) |
| RAM | Each translation worker uses memory (**measure**) | 24 GB; cap `--jobs` if RAM pressure appears |
| Toolchain | Python 3.12+, .NET 8 SDK, Vulkan SDK (`spirv-as`, `spirv-val`), LLVM 21+ with the AMDGPU target for `llvm-mc`, Visual Studio 2022 with Desktop development with C++ and Windows SDK 10.0.26100+, optional ffmpeg | Install all |
| GPU for `pipelines` | The cache is valid only for the GPU that built it | Must be the **RTX 3050**, not the Intel integrated GPU |

### 9.2 Step-by-step runbook (Windows, PowerShell)

1. **Prerequisites.** Install Python 3.12+ (add to PATH), .NET 8 SDK, Vulkan SDK, Visual Studio 2022 Build Tools (Desktop development with C++, Windows SDK 10.0.26100 or newer), git. Optional: ffmpeg.
2. **`llvm-mc` with AMDGPU support.** The official Windows LLVM build lacks the AMDGPU target. Choose one:
   - `scripts\build_llvm_mc.ps1` (builds a native `llvm-mc.exe`, about 20-40 minutes, sets `LLVM_MC`), or
   - conda: `conda install -c conda-forge llvm-tools`, then set `LLVM_MC` to the full path of that `llvm-mc.exe` (`where.exe llvm-mc` inside the environment), or
   - WSL: install `llvm-21`, then set `LLVM_MC="wsl -d Ubuntu -e /usr/lib/llvm-21/bin/llvm-mc"`.
3. **Clone with the submodule.** `git clone --recursive <your repo URL>`. A ZIP download omits `third_party/rpf9-explorer`; the build fetches it if git is installed.
4. **Check your package.** `python -I make_game.py --find` lists copies found; `python -I make_game.py --check` verifies version 1.0.2.1 and builds nothing. Add `--full-check` for a SHA-256 of every file (slow).
5. **Build the play-only stages.**

```
python -I make_game.py --game "D:\path\to\your\package" `
  --out "D:\Games\Leonida" --work "D:\leonida-work" `
  --stages check,shaders,runtime,pipelines `
  --wave64-emulate auto --jobs 8
```

   - `--wave64-emulate auto` asks `vulkaninfo` and turns emulation on if any GPU lacks 64-wide subgroups. NVIDIA and Intel both need it.
   - `--jobs` defaults to half the CPU threads. Lower it if RAM fills; raise it only if RAM and temperatures allow.
   - Put `--work` on the drive with space. Pass the same `--work` on later runs so stages resume.
   - Use `--no-tui` for plain console output.
   - Do **not** add `assets` unless you want the asset tools and have about 370 GB free.
6. **Hybrid-graphics check (important on laptops).** The laptop has an NVIDIA GPU and an Intel integrated GPU. Make sure `pipelines`, `vk_pipeline_check` and the runtime use the **NVIDIA** GPU: set Windows Graphics settings to "High performance" for `gta6rt.exe` and `vk_pipeline_check.exe` (**VERIFY** which adapter the build picks; log it). A cache built on the wrong GPU has the wrong UUID and is ignored. Intel's Windows driver also triggers `--mesh-compute` automatically; NVIDIA should use native mesh shaders.
7. **Verify the outputs.** Confirm `shader_path.txt`, `pipeline_cache.bin` and `index.json` exist; run `vk_pipeline_check <work>\shaders\spirv` and read the list of skipped shaders; run `--check-artifacts` (O2) once it exists.
8. **Run.** `gta6rt.exe` from `<out>`, with `LEONIDA_D3D12=vulkan` as the README describes. If the package drive is read-only, use `--overlay <writable folder>`.
9. **Laptop hygiene for long builds:** plugged in, High performance power plan, good cooling, no sleep. Builds resume if interrupted.

### 9.3 Resource guard rails for the build

- Record peak RAM per stage in `build_manifest.json`. If free RAM drops below 3 GB, reduce workers automatically instead of letting Windows swap.
- Warn before starting if free disk space on `--work` is below the stage's estimate.
- Refuse to run `pipelines` or `warm` when the selected adapter is not the discrete NVIDIA GPU, unless forced.

### 9.4 Time expectations

The README gives no translation time. Measure it on the first run (progress lines show percent and ETA) and record it in O1. Treat anything quoted from other machines as a guess.

---

## 10. Verification strategy

1. **Reference path:** `LEONIDA_OFFLINE=0` disables all artifacts.
2. **Shader checks:** `spirv-val` on every blob; GPU tests from `scripts/run_gpu_tests.py` for translator contract changes; `vk_pipeline_check` creates every shader on the real GPU and reports skips.
3. **Replay A/B:** record guest memory writes and GPU submissions; replay with artifacts on and off; compare render-target image hashes per submission. Any shader variant (native32, optimised, low-cost) must be bit-identical to the baseline, except variants explicitly marked lossy (C-06, C-07), which need a visual review and are quality-flagged.
4. **Determinism:** building twice from the same inputs yields identical artifact hashes (except timestamps in headers).
5. **Invalidation tests:** change one key input (translator flag, driver UUID, executable hash) and confirm exactly the right stages rebuild.
6. **Corruption tests:** truncate or flip bytes in each artifact and confirm the runtime rejects it and falls back.
7. **Soak:** 60-minute sessions with a screenshot per second; compare hitch counts, RAM/VRAM curves and crash logs with artifacts on vs off.

---

## 11. Milestones

Numbers are *proposals*; replace them with O0 measurements.

### O0 — Inventory and baseline
- Do: answer section 12 from the code; run the existing stages once; record per-stage time, peak RAM, disk, GPU/adapter used; count frames over 50 ms and 200 ms in a 10-minute session; run the CPU-vs-GPU-bound test (540p vs 1080p).
- Accept: a written baseline report with those numbers and the section 12 answers.

### O1 — Reproducible build on the owner's PC
- Do: follow section 9.2; fix any breakage; write `docs/offline_build.md`.
- Accept: a clean run of `check,shaders,runtime,pipelines` on the laptop using the NVIDIA GPU, resumable after an interruption, with timings logged; `pipeline_cache.bin` accepted by the runtime (header match logged).

### O2 — Build manifest, keys, validation
- Do: `build_manifest.json`, artifact headers (5.2), `--check-artifacts`, invalidation tests.
- Accept: all section 10 items 4-6 pass.

### O3 — Shader archive and metadata
- Do: 7.1 archive, flags, on-demand module creation.
- Accept: runtime starts and renders the baseline scenes from the archive with identical output; resident shader-module memory reduced (report MB before/after); startup time not worse than baseline.

### O4 — Pipeline manifest and warm
- Do: 7.2 recorder, merge, `warm` stage, miss policy.
- Accept: on a second session over the same route, frames over 200 ms reduced by at least 70% and frames over 50 ms reduced by at least 40% versus baseline (*proposal*); cache rejected correctly when the GPU/driver key changes.

### O5 — Per-shader wave64 variants and optimised variants
- Do: cross-lane analysis, native32 variants, `spirv-opt` variants, per-shader selection.
- Accept: identical replay output for all shaders flagged clean; measured GPU time change reported per variant; variants that do not win are dropped.

### O6 — Texture pack
- Do: 7.3 build, index, matching, runtime upload path.
- Accept: CPU time spent on texture conversion per frame reduced (report ms); host RAM for texture heaps reduced after upload (with memory-spec handoff); pack size reported; `BCPackLimitation` textures cleanly fall back.

### O7 — Patch table
- Do: 7.4 analyzer, table generator, loader, hash-mismatch fallback.
- Accept: submission-thread time reduced (report ms) with identical replay output; patched run and unpatched run both stable for 30 minutes; mismatching executable hash disables the table.

### O8 — Memory profile and pass cost table
- Do: 7.5 generator; `costs` stage producing `passes.json` from replays.
- Accept: classifier boots with the profile and shows fewer reclassification events than without; `passes.json` ranks passes by GPU time with a reproducible method.

### O9 — Presets, integration, CI
- Do: quality presets from `passes.json` (C-06, C-07), upscaler integration (C-01) if not done elsewhere, CI replay regression, docs.
- Accept: preset "Low" reaches the best frame time measured so far on the owner's laptop at the chosen internal resolution without visual regressions flagged in review; CI blocks merges when replay output differs.

---

## 12. Questions the agent must answer from the code first

1. Which backend is active (Vulkan `LEONIDA_D3D12=vulkan` or a D3D12 path)? Where do DirectStorage and the "NVIDIA shader-cache hook" fit, and what do they store?
2. How does the translator apply `--wave64-emulate` (global or per shader)? Where is the per-shader hash defined and is it stable across rebuilds?
3. Does the runtime already record pipeline creation? What key does it use for `pipeline_cache.bin`?
4. At what point does the layer see a texture, and what identifies it (source file and offset, or only guest address)?
5. Which `vk_pipeline_check` and runtime adapter-selection logic exists for hybrid-graphics laptops?
6. How are the loader and decompression paths structured (for provenance hooks shared with `MEMORY_SPEC.md`)?
7. Which Vulkan extensions does the RTX 3050 driver expose: graphics pipeline library, shader object, pipeline binary, mesh shader, `synchronization2`, memory budget, sparse residency?
8. What tools for disassembling x86-64 are already in the repo for the patch analyzer?
9. Does a replay/record harness exist, and who is building it in the memory track?

---

## 13. Configuration surface (proposal)

```
LEONIDA_OFFLINE=1              # use offline artifacts (0 = reference path)
LEONIDA_SHADERPACK=<path>      # override shaders.pack/idx location
LEONIDA_PIPE_RECORD=1          # record pipeline manifest during play
LEONIDA_WARM=background|off    # background pipeline warm at start
LEONIDA_WARM_BUDGET_MB=<n>     # cap on warmed pipeline memory
LEONIDA_TEXPACK=<path>         # override texture pack location
LEONIDA_PATCHES=1              # apply patch table (0 = off)
LEONIDA_MEMPROFILE=<path>      # override memory profile
LEONIDA_PRESET=low|medium|high|auto
```

Existing variables to preserve: `LEONIDA_D3D12=vulkan`, `LEONIDA_SHADERS`, `LEONIDA_W64_EMULATE`, `LEONIDA_RT_AS`, `LEONIDA_INPUT_SCRIPT`.

---

## 14. Risks and mitigations

| Risk | Mitigation |
|---|---|
| A stale artifact is used after a driver, translator or game change | Keys and header validation; rebuild on mismatch; `--check-artifacts` |
| Native-32 variant flagged clean but is not (corrupt output) | Replay A/B on every flagged shader; conservative default; per-shader kill switch |
| Pipeline warm consumes too much VRAM or CPU | Budget cap; low priority threads; warm only the used set |
| Texture matching gives a wrong pack entry | Prefer source (file, offset) keys; verify size/format/mips; hash-window fallback; per-entry kill switch |
| Patch table applied to the wrong executable | Hash gate on the whole executable and on each byte window; disable on any mismatch |
| Build blows disk or RAM on a laptop | Budget checks; resumable stages; skip `assets`; `--texpack-budget-gb` |
| Cache built on the integrated GPU | Adapter check and refusal (9.3) |
| Offline gains disappoint because the GPU is the wall | O0 CPU-vs-GPU test first; prioritise C-01, C-03, C-06 |
| Generated files leak into the repo | `.gitignore` for all output directories; CI check that fails if forbidden paths are staged |
| Legal exposure | Local-only generation; no distribution of game-derived data; no circumvention code; GPL attribution |

---

## 15. Glossary and references

- **Artifact:** a file generated locally from the user's copy of the game.
- **Key:** SHA-256 over the inputs that determine an artifact's content; a mismatch means rebuild.
- **Pipeline manifest:** the record of distinct pipelines (shaders plus state) seen during play.
- **Warm:** compiling manifest pipelines in the background before they are needed.
- **Provenance:** which file region a piece of memory was loaded from (see `MEMORY_SPEC.md`).
- **BCPack:** the console's extra compression over BC texture formats.
- **Cross-lane operation:** an instruction where lanes of a wave exchange data.

References (as of 2026-11-25/27):
- `https://github.com/Kalpajit0406/Pink` (README: stages, flags, shader census, status)
- `https://github.com/ItsNotRexed/Leonida` (original project; design notes on wave64 emulation and SPIR-V validation)
- `https://github.com/Belobug/VIctory-Belobug-AMD` and `.../blob/a-publier/docs/findings.md` (pipeline stalls, descriptor and constants patches, memory breakdown, switches)
- `https://github.com/avatarmanz/VIctory-7900XTX`
- `https://github.com/DexNom/VIctory` (`BENCHMARK.md`, upload budget, texture admission)
- `https://github.com/stellarr-studio/kelvaroq` (self-reported figures; mesh launch via amplification stage; hardware profile table)
- Upstream lineage: `Marvisak/VIctory`, `fxceless/victory-fork`
