# Where the port stands, and what is left for frame rate and stutter

This is an evaluation snapshot, not the upstream repository. It is the tracked working tree of the Leonida port as of
27 November 2026, with the uncommitted source changes that were present, and without build output, binaries, or any
game data. Read this file first; it points at the code that matters and states what is verified and what is not.

Everything below was checked by reading this tree. No claim here is from memory.

## The measured situation

`evidence/hud_titlebar.png` is the runtime's own title-bar HUD during a real run: **13 fps, 75.9 ms average frame,
worst 138 ms**, RAM 12149 MB, VRAM 439/5233 MB. That is the number to move.

The HUD is `runtime/shims/title_hud.h`, on by default (`LEONIDA_TITLE_HUD=0` hides it). It reports the average and the
*worst* frame of the last half second. That is two numbers — it cannot show stutter properly, because stutter lives in
the tail of the distribution, not in a single worst frame of a 500 ms window. Fixing the measurement is a prerequisite
for judging every item below.

## Already done in this tree (do not re-propose these)

These were open in an earlier review and have since been implemented:

| Item | Where |
|---|---|
| Async **graphics** pipeline compile, draws skipped while building | `runtime/shims/d3d12_pipeline_compile.h:40` (`async_graphics_compile()`), `global_draws_skipped()` |
| `MakeResident`/`Evict`/`SetResidencyPriority` now pass priority to WDDM | `runtime/shims/d3d12_vulkan_device.cpp:16780-16815`, enabling at `:22215-22219` |
| `VK_EXT_memory_priority` + `VK_EXT_pageable_device_local_memory` | same, enabled only when the device exposes them |
| Swapchain asks for `max(minImageCount + 1, 3)` images | `runtime/src/vulkan_present.h:1686` |
| Process priority is configurable, defaults to `HIGH` not `REALTIME` | `runtime/src/main.cpp:672-679` reads `LEONIDA_PRIORITY` |
| NVIDIA profile setting IDs | `runtime/src/nvidia_drs.h:75-76` — now `0x00AC8497` (`PS_SHADERDISKCACHE_MAX_SIZE`) and `0x20C1221E` (`OGL_THREAD_CONTROL`) |

One caveat on the last row: the IDs are now correct, but `OGL_THREAD_CONTROL` (threaded optimization) is an **OpenGL**
setting. It cannot affect a Vulkan or D3D12 title, so that half of the profile write is inert by nature, not by bug.
`PS_SHADERDISKCACHE_MAX_SIZE` is the D3D/GL shader cache; NVIDIA's Vulkan driver keys its cache by
`pipelineCacheUUID` and executable name instead (`docs/d3d12_vulkan.md:2464`), so the path that actually warms Vulkan
is still `gta6rt.exe --precompile-pipelines`.

## The launcher is still promising features the runtime does not implement

`launcher_config.json` ships four profiles. The `max_performance` profile is described as "Locked 720p presentation
with FSR, 6GB clamped VRAM priority". Verified by grep over `runtime/src` and `runtime/shims`:

| Variable | Set by launcher | Read by runtime |
|---|---|---|
| `LEONIDA_MIN/MAX_WIDTH/HEIGHT` | yes | yes, but **swapchain extent only** (`vulkan_present.h:1578-1579`, `dxgi_com.cpp`) |
| `LEONIDA_FSR` | yes | **no** |
| `LEONIDA_MAX_VRAM_MB` | yes | **no** |
| `LEONIDA_MAX_HOST_RAM_MB` | yes | **no** |

So the "720p" profiles shrink the presented surface while the title keeps rendering at its own internal size and gets
scaled down at present. The user loses sharpness and gains nothing. FSR only runs if the title itself calls into
`amd_fidelityfx_upscaler_xbox.dll` through the layer.

This is the highest-value *correctness* fix in this area, because it makes users measure no improvement and conclude
the port is slow. Either wire these variables to something real (a render-scale path in the swapchain/present chain)
or remove them from the launcher so the profiles stop claiming them.

## What is left, ranked by expected effect

### 1. Measure the tail, per frame

No ranking below can be validated without this. `runtime/src/submit_phases.h` already times each phase of
`VulkanQueue::execute_now` and reports per-phase milliseconds every `LEONIDA_SUBMIT_PHASES_SECONDS`. What is missing is
(a) a nearest-rank **p95/p99** frame-time summary over a few hundred frames rather than worst-of-500ms, and (b) a
per-frame reason tag, so a 138 ms frame says *which* phase consumed it.

This is cheap and it is the difference between guessing and knowing.

### 2. Graphics pipeline compilation still stalls the frame

`runtime/shims/d3d12_pipeline_compile.h:56-58` records the measured cost: after the story load, **graphics creates
take about 4.5 s each**, every submission waits at the compile gate, and frames stopped for **130 s**. Async graphics
compile now exists (`LEONIDA_ASYNC_GRAPHICS`), which is the right shape — skip the draw, count it, resume later.

The remaining work is not the mechanism but the *coverage*: confirm the counter is surfaced to the user (a silently
skipped draw reads as a rendering bug) and confirm the fallback path is exercised. `LEONIDA_FAST_PIPELINES=all` in the
`max_performance` profile builds every pipeline with `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`, which shortens the
build at the cost of slower shaders — that trade is fine for reaching the world, wrong as a permanent default for a
performance profile.

### 3. Precompile the graphics half offline

`--precompile-pipelines` currently covers compute only; `docs/d3d12_vulkan.md:2464` states plainly that "Graphics
pipelines are not covered". So every fresh install eats the 4.5 s creates regardless of precompiling.

Two extensions would close this, both currently absent from the tree (verified by grep):

- **`VK_EXT_graphics_pipeline_library`** — splits VS/PS compilation from fixed-function linkage, so the shader half can
  be built offline the way compute already is. This is the smaller change and the direct attack on the 4.5 s creates.
  vkd3d-proton adopted it for exactly this stutter.
- **`VK_KHR_pipeline_binary`** — lets an application compute the driver's key up front (`vkGetPipelineKeyKHR`) and
  store key/data pairs itself. That is the better long-term fit for `runtime/src/pipeline_cache_journal.h`, because it
  makes the cache fillable by a *separate process* for pipelines it never draws — cross-process pre-warming, which GPL
  alone does not give.

`VK_EXT_shader_object` is the more radical option (no pipeline objects at all) and is the largest rewrite; not a first
step.

### 4. Present path

`VK_KHR_swapchain_maintenance1` is in use, but these are absent:

| Missing | Why it matters at 13 fps |
|---|---|
| `VK_KHR_present_wait` / `present_id` | The pacer in `runtime/src/frame_pacer.h` *models* vblanks on the CPU. Present-wait reports when a frame was actually displayed, which is what a pacer needs to correct itself. |
| `VK_PRESENT_MODE_FIFO_LATEST_READY_EXT` | Tear-free with lower latency than FIFO. At 13 fps on a 60 Hz panel frames land in a 5:4 judder pattern; this mode collapses the queue each vblank instead of showing stale images. |

The 3-image swapchain is already in place, so that item is done.

### 5. Timer resolution and thread scheduling

`timeBeginPeriod(1)` is called in three places — `runtime/src/main.cpp`, `runtime/shims/acphal.cpp:751`,
`runtime/shims/silent_audio.cpp:204` — with no matching `timeEndPeriod`. That is benign for a game process (the
resolution dies with it), but it is three independent asks with no owner. Still absent from the tree: MMCSS
registration (`AvSetMmThreadCharacteristics`) for the audio thread, and
`SetProcessInformation(ProcessPowerThrottling)` to opt out of EcoQoS on hybrid CPUs. On a 12th-gen-or-later Intel or a
laptop part, EcoQoS parking a render thread is a direct stutter source and nothing here prevents it.

Note `tools/launcher_app/GameRunner.cs:161` applies its own priority to the child process from `LEONIDA_PRIORITY`,
duplicating what `main.cpp:672-679` now does. Two mechanisms, one measured — pick one.

## Not worth doing

`VK_EXT_memory_decompression` (GDEFLATE) is the GPU-side DirectStorage path. This tree decompresses BCPack on the CPU
(`runtime/src/bcpack.cpp`) — a different format — so it is only relevant if a GDeflate path appears.

## How to reproduce the numbers

The HUD screenshot came from a real run of the retail title. Nothing in this repository contains game data; the game
must come from the user's own Xbox 1.0.2.1 package, per `docs/first_run_ui.md` and `README.md`. `docs/design.md` is
the format and translator contract; `docs/README.md` indexes the other 97 notes.

## Status of this snapshot

- 2,937 files, ~49 MB, no build output, no binaries, no game data.
- 63 modified paths relative to commit `a73a57e13` are included as working-tree state.
- `docs/performance_plan.md` is referenced in earlier discussion but **does not exist** in this tree, in the stash, or
  in any ref. If a plan of that name is expected, it has to be written, not recovered.
