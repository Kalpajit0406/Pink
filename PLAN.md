# Plan: the black world, the missing audio, and the access violation at the intro

What was reported on this PC, and what the runs on it measure. Counts and file names only: the logs, captures and dumps
stay in `E:\Leonida\runs` (AGENTS.md, "No game data").

## The report

The title gets past the loading screen into the first scene; after the "two years ago" caption about 20 s play with
**only the subtitles visible**, a **black picture**, and **no sound** (the opening cutscene had sound), and then the
process ends.

## What the runs show

| Run | Settings | Outcome |
|---|---|---|
| `run94` | 240 s, async graphics on | 2083 presents, 35/35 frames non-black, stops inside the loading screen, no crash |
| `run95` | 600 s (requested), async graphics on | **dies by itself at 242 s, `0xC0000005`** in the title, `game_scarlett_master.exe+0x4e335b4`, read of `0x1238` with `RAX = 0` |
| `run96` | 420 s, `LEONIDA_ASYNC_GRAPHICS=0` | survives the full 420 s, 60/60 frames non-black, `replay_skips = 0`, but only 2135 presents (5 fps) and still inside the loading screen at the end |
| `run97` | 300 s, async graphics on, `LEONIDA_STACKS=230,240,250,255` | thread call chains printed around the moment `run95` died |

Both crashes point into title code, not into the layer: `run95` and the hand-launched run of 28 Nov 01:22 both die inside
`game_scarlett_master.exe`. `run95`'s chain is
`+0x4e335b4 ← +0x4e33350 ← +0x4e3329d ← +0x3b1e9e ← +0x35b3f51 ← +0x4eeaa59 ← +0x2677819 ← +0x256fb83`, and
`+0x4eeaa59` sits next to the measured XAPU call sites (`+0x4eaa870` `XApuGetResultCount`, `+0x4eaa89f`
`XApuDequeueResult`, `+0x4ee997d` `XApuEnqueueCommand`), so the dying path runs through the audio engine.

## Problem 1 — the world is black while the subtitles draw

> **CORRECTION (28 Nov, from the captures themselves).** The "skipped draws" cause below is **not** what the black
> frames show. `cap_run102/frame_02041` is black with a subtitle, and that run recorded 131,072 indirect graphics
> draws and emitted 98,616 of them (`replay_skips=37144` of 131,072, i.e. 28%): draws were flowing through the black
> frame. `run102` also applied **none** of the three world patches, so they cannot be the cause either.
>
> What the capture sequence actually shows, run by run:
>
> | Run | World patches? | Captured frames | End state |
> |---|---|---|---|
> | `run102` | no | loading artwork, then an **animated 3D scene** (flamingos, arches, depth-of-field blur - real engine output, not artwork), then subtitle-over-black | survived 601 s |
> | `run105` | no | same animated scene, then dark (~6/255) | **died at 281 s, `0xC0000005`** |
> | `run108` | no | artwork, ends bright | a device-lost thread started inside `Bink2Scarlett.dll` |
> | `run118`/`run126` | yes (all three) | loading artwork, then **frozen** (identical mean every frame) | - |
>
> So there are two different failures, not one: `run102` ends with the movie/intro plane black while the subtitle
> plane still draws, and `run105` dies by access violation partway through the same scene. The subtitle-over-black in
> `run102` is a **plane 0** failure in the present path (plane 1 keeps drawing), a different mechanism from a skipped
> draw: the draws reached the GPU and the composed output was still black.
>
> Second correction: **VRAM over-commits**. `run102` sat at 100-106% of its 5,233 MB budget from t=185 s onward, and a
> live run sample showed **7,804 MB against 5,233 MB (149%)** before it hung. `LEONIDA_MAX_VRAM_MB=6144` in the
> harness never binds, because `effective_vram_budget` takes the *minimum* of that and the driver's own 5,233 MB
> (`runtime/src/sotm_budget.h:97`), so the setting is inert on this GPU. Render targets (1,887 MB) are the largest
> single consumer.
>
> The text below is kept because the mechanism it describes is real, but it is not the black-frame cause.

Measured cause: the draws are being **skipped, not failed**. With async graphics on (`run94`, `run95`):
`indirect graphics: recorded=37523 emitted=5156 replay_skips=32768 [pending_pipeline=32768]`. With it off (`run96`):
`replay_skips=0`. A skipped indirect draw is a draw the title believes it issued and the layer never submits: the 2D UI
(subtitles) has its pipelines, the scene geometry does not yet, so the subtitle sits on black.

`runtime/src/vulkan_replay.h:2854` is the skip: an asynchronous pipeline whose variant is not built yet is counted as
`pending_pipeline` and the draw is dropped for that replay. The variant is built on a worker and will be there in a
later frame, so the picture fills in over time — but a frame that needs thousands of new variants is black until they
land, and one variant takes seconds (`run94` compute builds: 11.6 s, 18.8 s, 19.7 s, 47.0 s).

### The fix, in order

1. **A per-frame compile budget instead of a blanket skip.** The layer already has the synchronous path (build the
   variant in place) and the asynchronous one (skip). Add a third: a frame may spend `LEONIDA_PIPELINE_BUDGET_MS`
   (default 12) building pending variants synchronously; past the budget, draws keep skipping. The world then appears
   within the first seconds of a scene instead of after minutes, and no frame stalls for a 47 s build. Needs the
   building entry point (`variant(unbound, trigger)` on `PipelineState`) plumbed to the replay skip site, which today
   only holds the `leonida::GraphicsPipeline*`.
2. **Warm the variants the scene needs while the loading screen shows.** The load is where the time is: 4 compute
   builds and a 19 MB pipeline cache in `run94`. `gta6rt --precompile-pipelines` already exists as the offline warmer;
   the same manifest should also cover the graphics variants the loading screen's own command lists name.
3. **Only then** consider the trade the boot harness makes today: `LEONIDA_ASYNC_GRAPHICS=1` is fast and black,
   `0` is correct and 5 fps. A budget makes the choice unnecessary.

## Problem 2 — sound in the opening cutscene, silence afterwards

Measured cause: the in-game path produces **silence by construction**.

- `run94`/`run95` log `[rt] xapu: connected for Opus decoding, 256 streams; packets counted, not decoded`: the APU
  command queue shim counts the frames an Opus packet would decode to and writes zeros where the PCM goes
  (`runtime/shims/xapu_queue.cpp`, the no-decoder path of `fill`).
- Nothing in `runtime/CMakeLists.txt` mentions the decoder crate, so `LEONIDA_HAVE_OPUS_DECODER` is never defined and
  the decoding path in `runtime/opus_decoder` (`leonida-opus-decoder`, `opus-pure`) is dead code. `cargo` and
  `rustc` 1.102 are installed on this PC and the registry cache exists.
- Even decoded, nothing on this PC plays the samples: the console's output is the audio coprocessor
  (`AcpHal.dll`/`xapu.dll`/`chad.dll`), whose shims keep the call shapes and discard the audio
  (`docs/audio_hal_exe_calls.md`), and the only WASAPI object in the tree is the *silent* endpoint
  (`runtime/shims/silent_audio.cpp`): "every sample discarded".

The cutscene that does have sound is the movie path (`Bink2Scarlett.dll`, relocated in the boot trace), which is why the
sound stops exactly when gameplay audio takes over.

### The fix, in order

1. **Decode for real**: build `runtime/opus_decoder` from CMake (cargo, release, staticlib) and define
   `LEONIDA_HAVE_OPUS_DECODER` for `xapu_queue.cpp`. Gate: `[rt] xapu: ... packets decoded`, and a `.wav` written from
   one decoded stream matching what `tools/awc_opus.py` exports for the same stream.
2. **Give the mix a sink**: find the buffer the title's mixer writes last (the APU heap from `ApuCreateHeap`,
   `0x226E00` bytes at the measured site, is the candidate) and render it through WASAPI in `silent_audio.cpp`'s
   place, 48 kHz float, using the device's own endpoint. Gate: audible output, no underruns reported by the sink.
   This is the step that needs the most measurement; it is not a stub and must not be faked by playing the per-stream
   decode, which is not a mix.
3. Do **not** enable the silent endpoint as a "fix": it is there so a title with no output endpoint keeps running, and
   `LEONIDA_AUDIO=silent` makes silence deliberate.

## Problem 3 — the access violation at 242 s

Facts: `READ` of `0x1238` with `RAX = 0` (a null object plus a member offset), `R12` a guest-heap pointer, `R13 = 0x55`,
no device fault, no `OUT_OF_DEVICE_MEMORY`, memory map at the crash: `guest_heap_committed 3097 MB`,
`texture_vram 1279 MB`, `rendertarget_vram 1874 MB`, `buffer_vram 1040 MB`, `host_imports 525 MB`, system commit
40 524 / 68 591 MB with a peak of 78 287 MB.

Two candidate causes, and the run that separates them:

1. **An audio-engine object that our shims leave empty.** The chain runs through the XAPU call sites; the title is in
   its first streamed scene, which is when the audio engine starts streaming voices. `run97`'s `LEONIDA_STACKS` chains
   at 230–255 s name the thread and the last shim it called.
2. **Commit exhaustion.** the peak commit (78 GB) is above the machine's limit (68 GB), so an allocation did fail at
   some point; if the title's allocator returned null and the caller did not check, the same shape of fault follows.
   `LEONIDA_MEM_*` and the SOTM budget switches are the lever.

Order: read `run97`'s chains first (it costs nothing more), then either fix the audio object the chain names, or bound
the memory (`LEONIDA_TITLE_MEMORY_MB`, `LEONIDA_MAX_HOST_RAM_MB`, the arena and mip switches) and re-run the same
300 s window to see the fault move or disappear.

## What each step is judged by

- Problem 1: a captured frame in the first scene whose mean RGB is a picture, not black, with the subtitle still
  drawn — and `pending_pipeline` low.
- Problem 2: audible gameplay audio at 48 kHz, with the decoder line saying "packets decoded".
- Problem 3: `run` of 300 s and then 600 s with no `[rt-crash]` line.
