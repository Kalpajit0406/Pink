# Findings: the validated 28 Nov build, and the prompt that eats the A press

Measurements only, from this PC. Counts, file names and title offsets; the logs, captures and dumps stay in
`E:\Leonida` (AGENTS.md, "No game data"). `PLAN.md` holds the plan; this file holds what the runs since it say.

## run100 — the 21:45 build, 360 s, `boot.ps1`

The binary that had never been run now has been. It is the fastest and the most stable run on this PC so far, and it
never leaves the landing screen.

| | run97 (02:07 build) | run100 (21:45 build) |
|---|---|---|
| `PresentX ok` | 3 292 | **27 147** |
| captured frames | 40, all non-black | 150, all non-black |
| `[rt] submit phases` | 26 fps, heaps 5.8 ms, textures 5.8 ms | **123 fps**, heaps 3.1 ms, **textures 0.0 ms** |
| `indirect graphics` lines | 73 (skips, `pending_pipeline`) | **none: no 3D draw is recorded at all** |
| last captured frame | title collage | **ACCOUNT PICKER prompt on a dark page** |
| ended by | harness time limit | harness time limit (no fatal at the end: the fatal dump is a handler artefact, see below) |

Frame sequence of run100: the key-art collage (frame 1), then from about frame 6 001 to the last capture at 17 881 —
roughly 60 s to 361 s — the same landing page: a dark blue field, mean rgb 32/32/44, with `ACCOUNT PICKER` and an `A`
badge in the lower right. 29 scripted `a` presses were delivered between 95 s and 356 s (`[rt] gameinput: press
a@95.2 (Scripted pad (Leonida))`) and the page did not change, did not play the publisher stinger, and never started
the story load.

So the current blocker before any 3D work is **input on the landing page**, not the pipeline budget: with
`LEONIDA_ASYNC_GRAPHICS=0` there is no `pending_pipeline` skip left to fix in this window (0 skips), because no scene
geometry is ever submitted.

## What the account-picker page needs (candidates, in test order)

1. **The title never reads the scripted pad.** The shim without SDL3 (`runtime/shims/gameinput_no_sdl.cpp`) serves one
   scripted pad and logs the presses it holds, but nothing in run100 shows that the title polled it. `docs/first_run_ui.md`
   records the same harness getting past this page on 25 Nov with the scripted pad alone, on a build configured with
   SDL3 (`-DLEONIDA_SDL3_FETCH=ON`); today's binary prints `served without SDL3: ... controllers and the keyboard pad
   need SDL3`, so it has no keyboard pad and no controller at all.
2. **The press is seen but the page needs a different button or a longer hold.** The page's badge is `A`; the script
   holds each button for the script's own press window.
3. **The page waits on a user/session change, not a button.** `XUserRegisterForChangeEvent` in the user shim keeps a
   token and never invokes the callback ("the user never changes offline"). If the landing page renders a picker
   because it is waiting for a signed-in user event, no button will ever move it.

The diagnostic added in this change tells 1 and 2 apart: `GetCurrentReading` now counts its calls and logs
`[rt] gameinput: reading <n>, buttons <mask>` (first three readings and every 10 000th) plus one line
`[rt] gameinput: the title read a press, buttons ...` the first time a reading with a button down is read. No reading
lines at all means the title never polls the pad.

## The `0xC0000005` in the user shim is real but survivable

`run100` recorded 22 exceptions, 20 of them `0xc0000005`, at `gta6rt.exe+0x3fcca7`, `+0x3e79d7`, `+0x3fd298` and
`+0x4840ff`, all called from `gta6rt.exe+0x273429`, all writing to pages inside the host image itself
(`image+0x0`, `image+0x3FC000`, `image+0x40002C`), at 1.07 s, 1.33 s and 76.8 s. The matching PDB resolves the first
two to `user_register_change` and `user_resolve_issue_utf16_result` in `runtime/shims/xgameruntime_user.cpp`, the third
to `user_is_store_user`/`std::shared_mutex::unlock_shared` — that is, the user shim called with a bad object pointer,
which is what the loader's own SEH stubs then swallow (`... safe SEH handler at ...`, `... safe SEH
REGDB_E_CLASSNOTREG handler ...` in the boot lines).

This is a bug in the shim or in the loader's patched call site, not in the title, and it is the same shape as the
fatal dump of 21:09. It did not stop run100, but it is the next stability item after input.

## Changes in this tree

1. **Compile budget instead of the busy-wait.** `runtime/src/vulkan_replay.h` no longer spins 100 × 2 ms (up to 200 ms
   per skipped draw) waiting for a worker. The draw site now calls `GraphicsPipeline::build_now` — a try-lock build in
   `runtime/shims/d3d12_vulkan_device.cpp` (`variant_now`) that never waits on a compile worker — and spends at most
   `LEONIDA_PIPELINE_BUDGET_MS` (default 12) of such building per replayed list, reset at the start of every
   `replay()` call. Past the budget the draw skips exactly as before.
2. **The input diagnostic** above, in `runtime/shims/gameinput_no_sdl.cpp`.

## Next steps, in order

1. Run the new binary 180 s and read the `[rt] gameinput: reading` lines. No lines ⇒ the title never polls: build with
   SDL3 so the keyboard pad and a real controller exist (the source is already in `runtime/build_msvc/_deps/sdl3-src`).
2. If readings appear but no press is read, lengthen the hold in the input script and try `start`, `down`, `a`.
3. If a press is read and the page still does not move, treat it as a user/session wait: fire one
   `XUserChangeEvent` to the registered token after the first press and see whether the page advances (the change
   event is what the console sends when the picker resolves).
4. Then the world: `indirect graphics` lines and `pending_pipeline` counts come back once a scene submits geometry,
   and the budget is what keeps those draws in the frame.
5. Fix the user-shim call sites behind `+0x3fcca7` / `+0x3fd298` (bad object pointer) — same shape as the 21:09 dump.

## run101 — the diagnostic + budget build, 200 s, no D3D12 trace

Built from the tree with `build.bat` (below) and run with `-NoD3D12Trace`. **The landing page now takes the press.**

- `[rt] gameinput: the title read a press, buttons 0x4 at reading 324` — the title polls the scripted pad and reads
  A (bit 0x4). `reading 1..3, buttons 0x0` before it, then the press at 80.1 s. So the account picker was never a
  device-enumeration problem: the pad is read.
- After that press the picture changes: frame 4 921 is the key-art loading card (mean rgb 145/112/131), frames 5 041
  and 5 161 are the dark letterboxed load screen (mean rgb 7.2/7.2/9.3 and 5.9/5.9/8.1). The picker is behind us;
  this is the load/logo screen the runs before this one never reached.
- Then the GPU stops: `[rt] device-stall: queue retirement has not advanced for 15 s; submitted ticket 26419, last
  observed retired ticket 26383`, then 8 lines of in-flight work — `fspx_compute/lib_cs_01725`, `lib_cs_01728`,
  `lib_cs_01457`, `lib_cs_01460`, `lib_cs_01468` at 32x1x1 and 23x1x1 groups, on the title's async list 31 and on a
  compute queue. No `VK_ERROR_DEVICE_LOST` and no `OUT_OF_DEVICE_MEMORY` line: the queue simply stops retiring while
  the process stays alive (SOTM lines to 175 s, private RAM 13.1 GB). Presents stop with it.
- `[rt] d3d12_xs: fence slot 12 mirrors: 5 live; in 10 s 0 waits ended on an event, 250 on the 10 ms timeout` — every
  later wait is a timeout poll of that frozen queue.

So the new blocker is a **hang in the FSPX compute dispatches of the load screen**, not the pipeline skip and not
input. There is no environment switch for the FSPX path (`grep LEONIDA_FSPX` finds nothing); the programs come from
`amd_fidelityfx_upscaler_xbox.dll` (`docs/fspx.md`).

### Building this tree

A plain shell fails every translation unit with `cannot open include file 'windows.h'`, and an incremental link fails
with `LNK1168/LNK1201` while a stale `llvm-symbolizer.exe` still holds `gta6rt.pdb`. `E:\Leonida\build.bat` does the
first (it calls `VC/Auxiliary/Build/vcvars64.bat` of the installed Visual Studio 18, then builds `gta6rt`); kill a
leftover symbolizer before linking.

### Next steps, revised after run101

1. Find which dispatch stops retirement: replay the load window with `LEONIDA_STRICT_REPLAY=1` (a failing list then
   names itself) and with the FSPX dispatches' stage counters, or bisect the async list 31 with
   `LEONIDA_SLOT31_ASYNC=0`; a dispatch that never retires is either a translated shader loop or a descriptor that
   reads unmapped memory.
2. The three `fspx_compute/lib_cs_*` programs are the FidelityFX upscaler's; `docs/fspx.md` has the payload format and
   `runtime/src/shader_library.h` the index. Compare the translated SPIR-V of `lib_cs_01725` and `lib_cs_01728` against
   the payload's disassembly for a loop bound the translator gets wrong.
3. Keep the `gameinput` reading diagnostic: it is what told the picker apart from an enumeration bug, and the next
   input question (does the load screen take an A as well) is answered by the same counter.
4. The pipeline budget is still unexercised: no 3D draw has been recorded yet, so `pending_pipeline` is 0 in every run
   since run97. It will matter the moment the load screen submits world geometry.
