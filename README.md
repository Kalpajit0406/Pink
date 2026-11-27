<h1 align="center">
  <br>
  <a>
  <img src="assets/logo.svg" alt="Pink" width="200">
  </a>

  <br>
  Pink
  <br>
</h1>

<h4 align="center">Grand Theft Auto VI (Xbox Series 1.0.2.1) running natively on this PC, from my own copy.</h4>

<p align="center">
  <a href="#status">Status</a> |
  <a href="#device">Device</a> |
  <a href="#my-progress">My progress</a>
</p>

Pink is my desktop copy of Leonida, built on this PC from my own copy of the game. It is a static recompilation: the
Xbox Series x86-64 CPU code runs natively, and its RDNA2 GPU programs are translated to SPIR-V for Vulkan. Models,
textures and audio are read from my package.

> [!NOTE]
> This is my desktop copy of [NotRexed's Leonida project](https://github.com/ItsNotRexed/Leonida), built from its full
> commit history. The credits for the work up to here belong to NotRexed and the contributors in that history.

## Status

**My build is playable to a point:** the title screen renders, then a scripted gamepad gets through the first-run pages
into the story-load loading screen. The next thing blocking the world is asset streaming: during that loading stage the
DirectStorage path reports `E_DSTORAGE_DECOMPRESSION_ERROR` (0x89240030) with reason `missing-profile` on several
TTD-hosted texture dictionaries (`data/ttdb/...`). That is how far I have gone and what is next on this PC.

| What | Status |
|---|---|
| Booting the game | After the title and first-run pages, into the story-load loading screen (scripted gamepad input) |
| Story mode, anything playable | ❌ No — stuck at the loading screen; world not reached |
| Asset streaming (world load pass) | ❌ No — DirectStorage `missing-profile` failures on TTD texture dictionaries during the load |
| Shaders used in this run | RDNA2 → SPIR-V, 26,503 of 26,503 carved, all SPIR-V-validated on this build; pipelines made: 25,450 native graphics, 2,683 fspx_compute loaded in the reached run |

## Device

- PC this build runs on
- GPU: AMD Radeon RX 7900 XTX (or whatever this machine is — when known)
- OS: Windows 11 (x64)
- Graphics backend used in this run: Vulkan

## My progress on this PC

- Title screen: renders an identical “Americas” collage for the whole 180 s window.
- First-run pages: a scripted gamepad press sequence moves from the landing prompt through the navigation-experience, brightness
  and legal-notices pages into the loading screen.
- Run that shows it: `run70.err` — captures change past the collage; captures include the navigation-experience page, the
  brightness slider page, and the legal-notices page (Copyright / photosensitivity), then a dark loading screen with a spinner.
- Pipeline/present numbers (reached run, from its log): native PSO ok 25,450, failed 0, compute misses 0, graphics-stage misses 0,
  presents and draws much higher than the title-only runs; the DirectStorage `missing-profile` stream starts once the load begins.

## Why it is stuck here (confirmed, not guessed)

- The DirectStorage BCPack decompressor (`runtime/src/bcpack.cpp`) returns `reason = "missing-profile"` for some streamed TTD
  texture requests. `docs/directstorage.md` describes the related `history.bcph` / `LEONIDA_DSTORAGE_HISTORY` / `tools/build_dstorage_history.py`
  path. This is the lead for the next fix: find whether a history file is intended to be supplied for these stream requests, or
  whether the request is missing metadata that should be passed through.

## What I am not claiming on this PC

- I am not claiming playability past the loading screen from this PC yet.
- I am not claiming any AMD/NVIDIA/Intel cross-platform results, Linux/Proton, macOS or CI from this PC.
- I am not editing this file's social links or credits here — those live in the upstream repo and in the fork's README.

## How I built and measured this

- Built on Windows with MSVC + CMake + Ninja, runtime in `runtime/build/gta6rt.exe`.
- Ran with `LEONIDA_D3D12=vulkan`, `LEONIDA_SHADERS` pointing at my shader workspace, `LEONIDA_INPUT_SCRIPT` for the gamepad
  presses, `LEONIDA_MEM_PROBE=1` on one run so the title bar could show a real pipeline count.
- Measured with `--trace-d3d12` and PPM captures every N frames from `E:\Leonida\captures...`; identified screens from the captures.

## What is left for me to get past the loading screen

1. Resolve the DirectStorage missing-profile lead (history file vs. request metadata) and re-run until the world frame is real and
   not a black or textured-but-flat frame.
2. Once a real world frame appears, capture one that is visibly not the title collage and record its md5/mean colour like the
   earlier runs.
