# Single-Owner Tiered Memory (SOTM) — Specification for the Leonida/Pink PC Port

Status: proposal v1 (2026-11-27). Audience: the AI coding agent working in `github.com/kalpajit0406/pink`.
Read this whole document before changing code. Where a statement is marked **VERIFY**, check it against the actual code or hardware before relying on it.

---

## 0. Rules of engagement (read first)

1. **Measure before you change.** Milestone M0 comes first. No optimisation lands without a before/after number.
2. **Keep the existing mirror path as the reference implementation.** Every new memory policy sits behind a feature flag and must be verifiable against the mirror path (see section 9).
3. **One change per commit, one flag per policy.** Name flags `LEONIDA_MEM_*`. Defaults stay off until the milestone's acceptance criteria pass.
4. **Never crash on memory pressure.** Out-of-memory must degrade quality (lower mips, lower resolution, smaller pools) and log it. It must not abort.
5. **Legal and repo hygiene.**
   - Never add code that decrypts, unlocks or bypasses protection on game files. The repo's stated policy is: it needs the user's own copy, with files already readable on disk.
   - Never commit or attach game files, extracted assets, generated shaders, keys or saves.
   - The repo is GPL-3.0-or-later. Code copied from other GPL-3.0 projects needs attribution (name the source repo and commit in the commit message). Do **not** copy code from GPL-2.0-only projects (for example XCL); read them for behaviour only, as the Belobug fork does.
6. **Do not assume the architecture.** The README describes a Vulkan backend (`LEONIDA_D3D12=vulkan`, SPIR-V shaders, `gta6rt` runtime). The owner also reports DX12.x, DirectStorage and an NVIDIA shader-cache hook. Section 12 lists questions to answer from the code first.

---

## 1. Problem statement

The Xbox Series version of the game runs natively as x86-64 code. A compatibility layer reimplements the console's system software and translates its GPU work. The console has **one** memory pool shared by CPU and GPU. A PC has two (system RAM and VRAM). Today the layer keeps the game's memory in system RAM **and** a copy in VRAM, and synchronises them.

Observed on the owner's machine when the game crashed after the "Two Years Ago" card:

- RAM 24 / 24 GB used, VRAM 6 / 6 GB used, about 3 GB of "shared GPU memory" used.

### 1.1 Reading those numbers correctly

- "Shared GPU memory" is a window onto system RAM (Windows allows up to about half of RAM, here ~11 GB). It is **not** additional memory. The machine's physical total is 24 + 6 = **30 GB**, and Windows itself uses roughly 3-5 GB.
- So the process is at the ceiling of the machine, not above it.
- Also check the **commit limit** (RAM + pagefile). If commit charge reaches the limit, allocations fail even when physical RAM looks free.

### 1.2 The crash may not be memory

`README` status says: the game boots through the story load and into the "world phase", with **no verified world frame**. "Two Years Ago" is at that boundary. The first job (M0) is to find out why it dies: out-of-memory, a missing import or shim, an access violation in a guard handler, a device-lost, or an unimplemented D3D12/Vulkan path (the repo lists mesh-shader draws, indirect draw state, stream output and the console command-list tail as open).

---

## 2. Goals and non-goals

### Goals

- **G1.** Remove the duplicate copy: every byte of guest data has exactly one owner at a time (VRAM, system RAM, or the original package on disk).
- **G2.** Run on mid-tier PCs. Target tiers (RAM / VRAM):
  - Tier A: 16 GB / 4-6 GB (stretch target)
  - Tier B: 24 GB / 6 GB (the owner's machine, primary target)
  - Tier C: 32 GB / 8-12 GB
- **G3.** Degrade gracefully under pressure rather than crash.
- **G4.** Keep output identical to the reference mirror path at each step (bit-exact on recorded submissions where practical).

### Non-goals

- Reaching 30 fps at 4K on this hardware. The best public results are on far stronger machines (see section 3.3). Memory work makes the game *fit*; frame time is a separate project.
- Ray tracing (the repo lists the BVH path as in progress; keep it optional and off by default).
- Anything about obtaining, decrypting or distributing game files.

---

## 3. Background facts (what is known)

### 3.1 Target-console numbers (approximate, from public knowledge)

- Series X: 16 GB total (10 GB at 560 GB/s, 6 GB at 336 GB/s). About **13.5 GB available to games**.
- Series S: 10 GB total. About **8 GB available to games**.
- The title supports a Series S profile (the forks expose `--console series-s`: 1080p output, softer picture, about 5 ms faster on a 7900 XTX).

### 3.2 Owner's hardware

- CPU: Intel i5-13450HX. RAM: 24 GB DDR5. GPU: RTX 3050 Laptop, 6 GB VRAM (**VERIFY** bandwidth, about 168 GB/s, 96-bit bus).
- Windows reports ~11 GB shared graphics memory.
- PCIe link to the GPU is likely 4.0 x8: about **16 GB/s theoretical** per direction (**VERIFY** with GPU-Z or the NVIDIA tools). Plan around 50-60% of that in practice.
- Resizable BAR availability on this laptop is unknown (**VERIFY**: a Vulkan memory type that is `DEVICE_LOCAL | HOST_VISIBLE` with a heap far larger than 256 MB indicates ReBAR).

### 3.3 What the reference projects report (self-reported, unverified by us)

All figures come from each project's own README or docs as of 2026-11-25/26.

**Belobug/VIctory-Belobug-AMD** (3.3k stars, lineage: Marvisak/VIctory -> fxceless/victory-fork -> this; `avatarmanz/VIctory-7900XTX` is an earlier sibling)
- Rust, Direct3D 12 backend. Test PC: i9-12900K, 64 GB, RX 7900 XTX (24 GB).
- 19-22 fps in some scenes (about 45 ms/frame), 6-10 fps in a heavy street chase. About **30-46 GB private memory and 10.5 GB VRAM**; 64 GB RAM recommended.
- `docs/findings.md`: every large cost is *noticing* writes, *moving* data, or *waiting*. Memory breakdown of ~40-46 GB private: **command-list allocators 9 GB, windows 7 GB, uploads 4 GB** (use `VICTORY_MEMORY_MAP=1` there to list them).
- Useful ideas: read the 174 MB constants heap in place (`VICTORY_PORT_CONSTANTS`), raw constant-buffer reads (`VICTORY_PORT_CBV`), report only descriptor slots that were written instead of scanning ~600,000 per frame (`VICTORY_PORT_DESCRIPTORS`), write guards at 64 KiB block granularity (`VICTORY_PORT_GUARD`), texture read-back only for resources the CPU samples (ocean height maps; without it boats sank), textures uploaded on a copy queue and shown only after the upload ran (ended a device loss roughly every 15 minutes), pre-warming pipelines (`victory warm`).
- Measured and *not* worth it on that card: Series S profile for speed, smaller textures for speed, second draw queue, many micro-optimisations in shader code.

**DexNom/VIctory** ("DexNom Runtime", "GPU memory improvements")
- Reads back only changed pages and merges adjacent copy ranges.
- Texture handling: searches contiguous descriptor slots, **admits already-resident textures under memory pressure**, new textures and retries **share an upload budget**; previously refused buffer views are re-checked when the backing allocation changes.
- Sparse textures, ray-tracing descriptor guest-address resolution, optional CPU submission workers (`VICTORY_FILL_WORKERS`, `VICTORY_PARALLEL_PRELUDES`). See its `BENCHMARK.md`.

**stellarr-studio/kelvaroq** (Rust, on VIctory/victory-fork, 92 stars)
- States the core problem the same way: about two thirds of a heavy frame goes to keeping two copies of memory in sync.
- Claims an in-testing **unified memory layer** that gives the GPU the game's own memory: copies per frame down from >100 MB to <8 MB; 64 M of 115 M GPU lookups served from game memory without a copy; 0 GB left behind on exit (was ~19 GB). **Not public yet; no code to reuse.** Treat the numbers as unverified marketing-adjacent claims.
- Public build: about 10 fps in the prologue on an RTX 5080/4090-class machine. One external test on a 4090 with 32 GB showed RAM about 15.5 GB peak, about 10.5 GB steady, about 7 GB late.
- Has an automatic hardware profile table. For "smaller configurations" it uses per-frame texture staging 64 MB, card buffers 256 MB, queue-2 landing 32 MB (`VICTORY_TEXTURE_STAGE_MB`, `VICTORY_CARD_BUFFERS_MB`, `VICTORY_QUEUE2_LANDING_MB`). Texture residency follows the DXGI budget. Idle pipeline-library pages are trimmed by available RAM. Uses ReBAR CPU-visible VRAM for small read-only copies. Warns below 16 GiB RAM.

**Kalpajit0406/Pink** (this repo; continuation of ItsNotRexed/Leonida)
- Static recompilation: loads the console PE natively, reimplements system software (GDK, XMem, `d3d12_xs`), translates console RDNA2 shader code to SPIR-V for Vulkan. 26,503 / 26,503 shaders translate and validate. 23,841 are wave64: exact on 64-wide subgroups (AMD), compute/mesh exact on 32-wide with wave64 emulation (NVIDIA/Intel), cross-half wave ops in VS/PS/GS still approximate there.
- Xbox D3D12 -> Vulkan layer: placed, committed and reserved resources, descriptor heaps, root signatures, pipelines, barriers, queries; `AllocateUploadMemory` implemented as a mapped-upload arena (list slot 148). Open: the console list tail and descriptor-table record model.
- `gta6rt` maps the package in place; `--overlay <writable folder>` for writes.
- Nothing is playable yet.

### 3.4 Take-aways that drive this design

1. Duplication is structural (CPU/GPU split), not a bug to patch away.
2. Much of the multi-GB overhead in the forks is **not game data** (allocators, windows, staging). Cap those first.
3. Most guest memory is *write-once, GPU-read* asset data. After upload the host copy is dead weight.
4. Most guest asset data has a **backing file** (the package). It can be dropped and reloaded instead of held.

---

## 4. Design overview

### 4.1 Principle

> Every guest page has exactly one **owner tier** at a time. A second copy exists only transiently (during upload or read-back) or where measurement proves it pays off.

### 4.2 Tiers

| Tier | Holds | Cost | Notes |
|---|---|---|---|
| T0 VRAM | Active GPU working set: render targets, depth, BVH, resident textures and meshes | Scarcest (6 GB here) | Budget from `VK_EXT_memory_budget`, never from total size |
| T1 System RAM | CPU-owned guest state; dynamic buffers; recently evicted GPU data; small LRU of decompressed asset data | ~24 GB here, minus OS | Count commit, not just working set |
| T2 Package on disk | Anything loaded from an immutable pack and unmodified | Free in RAM; costs I/O + decompression to restore | Restore via async I/O |

### 4.3 Allocation classes

Every guest allocation is classified:

| Class | Who writes | Who reads | Target placement |
|---|---|---|---|
| `GpuOnly` | GPU (render targets, depth, UAV outputs not sampled by CPU, BVH) | GPU | T0 only; no host copy ever committed |
| `UploadOnce` | CPU once (asset load) | GPU | Upload to T0, then drop host copy (T1 -> T0 handoff). Restoreable from T2 if provenance exists |
| `CpuWriteGpuRead` | CPU every frame (constants, descriptors, dynamic vertex data) | GPU | One host-visible or ReBAR ring; GPU reads in place |
| `GpuWriteCpuRead` | GPU | CPU (e.g. ocean displacement sampled by physics) | Read back only for a **whitelist**; otherwise forbidden |
| `ColdShared` | CPU or none | GPU rarely | One copy in T1; GPU reads over PCIe via host-pointer import |
| `CpuOnly` | CPU | CPU | T1 only; never mirrored |

Classification sources, in priority order: (1) the persisted profile cache from earlier runs, (2) API facts (heap type, resource flags such as ALLOW_RENDER_TARGET, ALLOW_UNORDERED_ACCESS, depth-stencil), (3) observed behaviour in the first N frames (does the CPU read/write the range after the GPU first uses it?).

### 4.4 Page table

Granularity: **64 KiB** guest pages (matches the guard granularity the forks use; **VERIFY** that this matches the title's own heap alignment).

```cpp
enum class PageState : uint8_t {
  HostOnly,             // CPU owns; no device copy
  DeviceOnly,           // GPU owns; host pages decommitted (address stays reserved)
  BothClean,            // transient: uploaded, host copy not yet dropped
  BothDirty,            // CPU wrote after upload; needs re-upload
  EvictedRestreamable,  // neither tier holds it; provenance can rebuild it
};

enum class AllocClass : uint8_t {
  GpuOnly, UploadOnce, CpuWriteGpuRead, GpuWriteCpuRead, ColdShared, CpuOnly
};

struct Provenance {          // optional; absent for generated data
  uint32_t fileId;           // index into the package file table
  uint64_t offset;           // offset within the file
  uint32_t length;
  uint16_t codec;            // raw, zlib-like, BCPack, ...
};

struct PageEntry {           // keep small; there are ~2.2 M entries for 140 GB, ~220 K for 13.5 GB
  PageState   state;
  AllocClass  cls;
  uint32_t    lastUseFrame;
  uint32_t    deviceBlock;   // index into device allocation table, or invalid
  uint32_t    provIndex;     // index into provenance table, or invalid
};
```

(13.5 GB / 64 KiB is about 216 K entries; at about 16 bytes each that is about 3.5 MB. Cheap.)

### 4.5 State transitions

```
HostOnly --(first GPU use, class GpuOnly/UploadOnce)--> BothClean
BothClean --(upload finished, fence passed)--> DeviceOnly      [decommit host pages]
DeviceOnly --(CPU fault on page)--> BothClean                   [read back or restream, then map]
BothClean/DeviceOnly --(CPU write fault)--> BothDirty
BothDirty --(next submit uses it)--> BothClean                  [re-upload dirty blocks only]
DeviceOnly --(pressure, provenance present)--> EvictedRestreamable  [free VRAM]
EvictedRestreamable --(GPU needs it)--> DeviceOnly              [restream + decompress + upload]
```

Rules:
- Never decommit host pages until the upload's fence has signalled.
- A guest `free`/reuse of a range (the XMem free/alloc shim) invalidates its page entries and provenance immediately.
- A page that has ever been CPU-written after load loses its provenance (it is no longer identical to the file).
- Class `CpuWriteGpuRead` pages are never decommitted.

---

## 5. Components

### C1. Memory accounting (M0)

Tag every allocation by category and expose live totals. Minimum categories:

`guest_heap_committed`, `guest_heap_reserved`, `texture_vram`, `rendertarget_vram`, `buffer_vram`, `bvh_vram`, `upload_ring`, `staging`, `command_allocators`, `descriptor_mirrors`, `hash_buffers`, `pipeline_cache`, `shader_modules`, `window_surfaces`, `other`.

Output every N seconds and on crash: JSON with committed bytes, working set, VRAM from `VK_EXT_memory_budget` (budget and usage per heap), Windows commit charge and limit. A `--mem-map` switch lists the biggest allocations (like `VICTORY_MEMORY_MAP=1` in the forks).

### C2. Lazy commit for guest memory (M1)

- Reserve the console address layout with `MEM_RESERVE`; commit on first touch or on explicit allocator commit.
- **Audit the XMem shim:** if it commits and zeroes the full budget (about 13.5 GB) at startup, fix this first.
- Prefer `VirtualAlloc2` with placeholders so the same physical pages can later be mapped at more than one address (`MEM_RESERVE_PLACEHOLDER`, `MapViewOfFile3`, `UnmapViewOfFile2` with `MEM_PRESERVE_PLACEHOLDER`).

### C3. Overhead diet (M1)

Cap and recycle things that are not game data:

- Command-list allocators: pool, reset per frame, trim idle ones (target: <= 1 GB, was 9 GB in the Belobug measurement).
- Upload/staging: fixed ring (start 128-256 MB), per-frame texture staging cap (start 64 MB on small configs).
- Descriptor mirrors: record only written slots (see C7).
- Hash/compare buffers: size to the guarded range, not the heap.
- Pipeline cache: trim idle pages by available RAM; persist `pipeline_cache.bin`.

### C4. Allocation classifier and profile cache (M3)

- Intercept resource creation and heap/placed-resource calls in the D3D12->Vulkan layer; record (guest range, size, flags, class guess).
- Track post-upload CPU touches with guard faults to refine class.
- Persist `memory_profile.bin` per game build (key it on the package version hash and layer version), loaded at boot like a shader cache.
- Fallback when unknown: treat as `CpuOnly` (safe) and promote with evidence.

### C5. Single-owner handoff (M3)

- After an upload completes, `DeviceOnly` for `GpuOnly`/`UploadOnce` pages: decommit via `VirtualFree(MEM_DECOMMIT)` or `DiscardVirtualMemory`; keep the address reserved so guest pointers stay valid.
- Install a vectored exception handler (guard page or `PAGE_NOACCESS`) on decommitted ranges. On fault: if the data is wanted, read back from VRAM or restream; map pages; resume.
- Fault policy: if the same range faults repeatedly (e.g. 3 times in 60 frames), reclassify it `CpuWriteGpuRead` or `ColdShared` and stop evicting it.
- Be careful with: the title reading memory it wrote long ago (heap reuse), threads racing the handler, and 64 KiB blocks that straddle two resources.

### C6. Placed-resource aliasing (M3/M4)

Console titles place many resources over the same heap range at different times. For each guest heap, create **one** device allocation and bind overlapping guest placed resources to the same `VkDeviceMemory` offsets. Do not allocate a separate Vulkan allocation per placed resource. Handle aliasing barriers/initialisation correctly (console semantics: contents undefined after an aliasing switch unless the game says otherwise). **VERIFY** how the layer currently maps `CreatePlacedResource`.

### C7. Dynamic-data path (M5)

- One host-visible or ReBAR ring for constants/descriptors/small dynamic buffers; GPU reads in place. A mapped-upload arena already exists (`AllocateUploadMemory`, list slot 148): reuse it.
- Descriptor tables: patch the game's descriptor-writing sites (byte-checked) to report written slots, instead of scanning all slots each frame.
- Read constants in place as raw bytes in translated shaders (cf. `VICTORY_PORT_CBV`).
- GPU-written data the CPU needs: **whitelist** only (ocean height/displacement maps are the known case). Anything else the CPU reads from GPU-written memory is logged as a classification miss.

### C8. Texture residency manager (M4)

- Keep BC1/3/4/5/6H/7 compressed end to end. Never expand to RGBA8.
- Track per texture: mip chain, last-use frame, priority; stream only the mips needed. Use reserved/sparse images (`sparseBinding`, `sparseResidencyImage2D`; **VERIFY** on the RTX 3050) so only visible tiles are committed, or fall back to whole-mip residency.
- Budget: `VK_EXT_memory_budget`. Evict least-recently-used when usage > budget * 0.9. Set `VK_EXT_memory_priority` (render targets high, streaming textures low). On NVIDIA also enable `VK_EXT_pageable_device_local_memory` so the driver can page device-local memory under pressure (**VERIFY** support).
- Sub-allocate (VMA or equivalent): drivers cap `maxMemoryAllocationCount` (commonly 4096).
- Admission control (cf. DexNom): new textures and retries share the upload budget; if a texture does not fit, it keeps its lowest resident mip and a retry is scheduled. A texture that cannot be shown yet shows its low mip, not garbage.
- Show a texture only after its upload (copy-queue) fence has passed.
- Per-frame upload cap derived from PCIe: roughly 64-128 MB/frame on the 3050 laptop at 30 fps; make it adaptive.

### C9. Provenance and restreaming (M6)

- Hook the loader/decompression path so that every asset region copied into guest memory records `(file, offset, length, codec)`.
- Memory-map package files read-only. Clean file-backed pages can be dropped by the OS for free.
- `EvictedRestreamable` restore path: async read -> decompress (CPU, or GPU if the format is GDeflate) -> upload. Budget CPU threads; keep a small LRU (start 512 MB) of decompressed regions to avoid thrash.
- DirectStorage caveats: it is a D3D12 API and decompresses only GDeflate on the GPU; the game's pack formats differ. For a Vulkan runtime use overlapped I/O or IoRing (**VERIFY** what exists in the repo). Using DirectStorage purely as an I/O engine with CPU decompression is valid but does not remove duplicates.
- Invalidate provenance on CPU write, on guest free, and on file-version mismatch.

### C10. Zero-copy cold tier (M7, optional)

- For `ColdShared` data, import guest host memory into the GPU: Vulkan `VK_EXT_external_memory_host` (alignment from `minImportedHostPointerAlignment`; query `vkGetMemoryHostPointerPropertiesEXT`), or D3D12 `OpenExistingHeapFromAddress`. One copy in T1, read over PCIe.
- Use only for data read rarely or sequentially. Never for hot textures or render targets (PCIe about 16 GB/s vs about 168 GB/s VRAM).
- Gate by driver: check support and measure; keep a per-driver deny list.

### C11. Memory governor (M8)

- On startup: query RAM, commit limit, VRAM, ReBAR, PCIe link, and choose a preset.
- Budgets (initial proposal; revise after M0):

| Preset | RAM | VRAM | Internal res | Console profile | Texture pool | Staging/frame |
|---|---|---|---|---|---|---|
| A (stretch) | 16 GB | 4-6 GB | 1080p + upscale | Series S | 2.5 GB | 64 MB |
| B (owner) | 24 GB | 6 GB | 1080p-1440p + upscale | Series S then Series X | 3.5 GB | 64-128 MB |
| C | 32 GB | 8-12 GB | 1440p + upscale | Series X | 5+ GB | 128-192 MB |

- Pressure ladder (apply in order, log each step): (1) shrink staging and LRU caches, (2) lower texture mip floor, (3) evict restreamable data, (4) drop to Series S profile on next load, (5) lower internal resolution, (6) hard stop with a clear error and a memory report. Never an unhandled allocation failure.
- Handle `VK_ERROR_OUT_OF_DEVICE_MEMORY` / host allocation failure on every allocation path: retry after evicting, then degrade.

### C12. Observability and replay (M0, used by every milestone)

- Frame/submission record and replay (the forks have `docs/replay.md`-style tooling; build or adapt). The replay must reproduce guest memory writes and GPU submissions so policies can be A/B tested deterministically.
- Counters per frame: bytes uploaded, bytes read back, bytes copied host-to-host, faults taken, evictions, restreams, stalls > 10 ms, texture-miss count.
- Soak logging with a screenshot per second (as the forks do) for visual regression.

---

## 6. Memory budget worked example (Tier B: 24 GB RAM / 6 GB VRAM)

| Item | Budget |
|---|---|
| Windows + background | ~4 GB |
| Layer overhead (allocators, rings, tables, caches) | <= 2 GB |
| Guest CPU-owned state (T1, `CpuOnly` + `CpuWriteGpuRead`) | ~4-6 GB (**measure**) |
| Evicted GPU data kept in RAM (T1) | ~6-8 GB |
| Decompressed-region LRU | 0.5 GB |
| VRAM: render targets at 1080p-1440p | ~1-1.5 GB |
| VRAM: textures/meshes/BVH | ~3.5-4 GB |
| Headroom | >= 2 GB RAM, >= 0.3 GB VRAM |

Total: well under 24 GB RAM and 6 GB VRAM if single-owner works. Today's mirror layout is at 24/24 and 6/6.

---

## 7. Milestones

Targets below are **initial proposals**. After M0, replace them with numbers measured on the owner's machine.

### M0 — Baseline, crash root cause, observability
- Deliver: C1 accounting, crash capture (Vulkan validation/diagnostics, Windows crash dump, last-error and exit-code logging), memory JSON every 5 s, commit-limit monitoring, A/B harness skeleton.
- Do: set pagefile to 32-48 GB on NVMe; reproduce the crash; classify the cause.
- Accept: a written root-cause report for the "Two Years Ago" crash with logs; a per-category memory table at boot, at the loading slideshow and at the crash point; at least one replay recording that re-runs.

### M1 — Quick wins (no behaviour change)
- Deliver: lazy commit of guest memory (C2), overhead diet (C3), Series S profile + 1080p internal resolution selectable (`--console series-s`-style switch, **VERIFY** it exists in this repo).
- Accept: peak committed memory at the same scene reduced by >= 30% vs M0; no rendering diff against the baseline replay; no new crash.

### M2 — Page table and fault infrastructure (no policy)
- Deliver: C4's tracking half, page table (4.4), vectored exception handler, per-page statistics, counters.
- Accept: runs the boot sequence with the table enabled and < 3% frame-time overhead; guard faults handled without deadlock in a 30-minute soak; unit tests for each state transition.

### M3 — Classifier, single-owner handoff, aliasing
- Deliver: classifier + profile cache (C4), handoff (C5) for `GpuOnly` and `UploadOnce`, placed-resource aliasing (C6).
- Accept: host-committed bytes for texture/mesh heaps drop >= 80% after upload; replay output identical to the mirror path on at least 1,000 recorded submissions; fault rate < N per minute after warm-up (set N after M2 data); no corruption in a 60-minute soak.

### M4 — Texture residency manager
- Deliver: C8 with budget, LRU, admission control, per-frame cap, memory priority, optional sparse images.
- Accept: no out-of-device-memory error at 6 GB VRAM in the test scenes; VRAM usage stays within the reported budget; visible low-mip textures are replaced by full-res within a measured time; texture-miss counter reported.

### M5 — Dynamic-data path and write tracking
- Deliver: C7 (in-place constants, written-slot descriptors, whitelist read-back).
- Accept: host-to-host copy volume per heavy frame under 50 MB (stretch: under 10 MB; Kelvaroq's claim is <8 MB, unverified); no visual diff; ocean/boat physics still correct.

### M6 — Provenance and restreaming
- Deliver: C9.
- Accept: with a 16 GB RAM cap (job-object limit), the game reaches the same scene as M0 without commit failures; restream stall p99 < 50 ms (adjust); no corrupted assets over a 60-minute soak; correct invalidation tests (write after load, guest free/reuse).

### M7 — Zero-copy cold tier (optional)
- Deliver: C10 with a per-driver switch.
- Accept: measured net win (lower VRAM or RAM without a frame-time loss > 5%); driver deny list; safe fallback when import is unsupported.

### M8 — Governor, presets, CI
- Deliver: C11, presets A/B/C, pressure ladder, memory report on abort, CI replay regression and low-memory (job-object) tests.
- Accept: preset B runs the benchmark scene without any allocation failure; preset A (16 GB RAM cap) runs it with degraded quality but no crash; automatic selection picks the right preset on three different machine configs (can be simulated).

---

## 8. Configuration surface (proposal)

```
LEONIDA_MEM_LAZY_COMMIT=1        # C2
LEONIDA_MEM_PAGETABLE=1          # C4/M2
LEONIDA_MEM_HANDOFF=1            # C5
LEONIDA_MEM_ALIAS_PLACED=1       # C6
LEONIDA_MEM_RESIDENCY=1          # C8
LEONIDA_MEM_VRAM_BUDGET_MB=auto  # override budget
LEONIDA_MEM_STAGE_MB=64          # per-frame texture staging cap
LEONIDA_MEM_RING_MB=128          # upload ring
LEONIDA_MEM_PROVENANCE=1         # C9
LEONIDA_MEM_ZEROCOPY_COLD=0      # C10
LEONIDA_MEM_PRESET=auto|A|B|C    # C11
LEONIDA_MEM_REPORT=5             # seconds between reports
LEONIDA_MEM_MAP=1                # dump largest allocations
LEONIDA_MEM_SAFE=1               # force mirror reference path
```

Existing variables to keep in mind: `LEONIDA_D3D12=vulkan`, `LEONIDA_SHADERS`, `LEONIDA_W64_EMULATE`, `LEONIDA_RT_AS`, `LEONIDA_INPUT_SCRIPT`.

---

## 9. Verification strategy

1. **Reference path:** `LEONIDA_MEM_SAFE=1` keeps the current mirror behaviour. All A/B comparisons use it.
2. **Submission replay:** record guest memory writes and GPU submissions; replay under each policy; compare image hashes of render targets per submission; log any mismatch with the page range and policy involved.
3. **Low-memory simulation:** Windows job-object memory limits to emulate 16 GB RAM; override VRAM budget to emulate 4 GB.
4. **Fuzz/stress the fault handler:** concurrent threads touching evicted ranges, rapid free/realloc of guest ranges, straddling blocks.
5. **Soak:** 60-minute sessions with a screenshot per second; compare memory curves (no monotonic growth) and crash logs.
6. **Visual checks:** texture pop-in time, flicker, boats floating (the known read-back case).

---

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Guest reads an evicted/decommitted range unexpectedly (heap reuse) | Guard handler restores; fault-rate auto-reclassification; allow-list of never-evict ranges |
| Misclassification corrupts state | Default to `CpuOnly`; require evidence to promote; replay verification in CI |
| Fault-handler overhead or deadlock | M2 isolates the infrastructure with soak tests before any policy uses it |
| Restream stalls (decompress cost) | LRU of decompressed regions; prefetch by visibility; cap CPU threads |
| PCIe becomes the bottleneck on a 3050 laptop | Per-frame cap, adaptive; prefer low mips over stalls |
| Driver differences (sparse, host import, pageable memory) | Capability checks; per-driver deny list; fallbacks to whole-mip residency and plain staging |
| NVIDIA/Intel wave64 approximations affect rendering | Separate workstream; do not mix with memory changes |
| Reference projects' claims are self-reported | Treat as hypotheses; measure on the owner's hardware |
| Licensing mix | Only GPL-3.0-or-later code may be copied (with attribution); no GPL-2.0-only code |

---

## 11. Expectations to state honestly in docs and issues

- The best public results (about 19-22 fps on a 7900 XTX with 64 GB RAM; about 10 fps on a 4090-class machine) are far above this laptop's GPU. Memory efficiency will make the game *fit and stay stable*; it will not by itself make it fast.
- 16 GB RAM + 4 GB VRAM is a stretch target and may require the Series S profile and 1080p or lower.

---

## 12. Questions the agent must answer from the code before M1

1. Which backend is active: Vulkan (`LEONIDA_D3D12=vulkan`) or a D3D12 backend? How does DirectStorage fit (a D3D12 API) in a Vulkan runtime?
2. How does the XMem shim allocate guest memory (reserve vs commit; zeroing; large pages)?
3. How are `CreatePlacedResource` / reserved resources mapped to Vulkan memory (aliasing or one allocation per resource)?
4. Is there already a write-watch/guard mechanism (`GetWriteWatch`, guard pages)? At what granularity?
5. What does the "NVIDIA shader-cache hook" do and where does it store data?
6. Where does the loader/decompression path copy asset bytes into guest memory (for provenance hooks)?
7. Which Vulkan extensions does the 3050 driver expose: `VK_EXT_memory_budget`, `VK_EXT_memory_priority`, `VK_EXT_pageable_device_local_memory`, `VK_EXT_external_memory_host`, sparse residency, ReBAR memory type?
8. What is the actual failure at the "Two Years Ago" crash (error code, thread, call stack)?

---

## 13. Glossary

- **Guest memory:** the address space the console title believes it owns.
- **Mirror path:** the current approach: host copy plus device copy kept in sync.
- **Single-owner:** only one tier holds a committed copy of a page at a time.
- **Provenance:** the record of which package file region a page's contents came from.
- **ReBAR:** Resizable BAR; lets the CPU map large parts of VRAM.
- **Commit charge/limit:** Windows' accounting of promised memory (RAM + pagefile); allocation fails when it is reached.
- **BC / BCPack:** block-compressed texture formats; BCPack is the console's extra compression layer.
- **Placed resource:** a D3D12 resource created at an offset in an existing heap, allowing aliasing.

---

## 14. Source references

- `https://github.com/Kalpajit0406/Pink` (this repo; README: status, platforms, how it works)
- `https://github.com/Belobug/VIctory-Belobug-AMD` and `.../blob/a-publier/docs/findings.md`
- `https://github.com/avatarmanz/VIctory-7900XTX`
- `https://github.com/DexNom/VIctory` (README, `BENCHMARK.md`)
- `https://github.com/stellarr-studio/kelvaroq` (README, `documentation.md`, in French)
- Upstream lineage: `Marvisak/VIctory`, `fxceless/victory-fork`, `ItsNotRexed/Leonida`
