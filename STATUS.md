# Status

## Fork relationship

This checkout has two relevant remotes:

- `origin`: `https://github.com/itsnotrexed/leonida` — the first-party Leonida repo (fetched and pushed here).
- `pink`: `https://github.com/Kalpajit0406/Pink.git` — a fork of this repo, currently on its own `main`.

At the time of writing, the fork's `main` is **identical to this repo's `main` except for one edit to `README.md`** — it removes the two social links:

- `Discord: https://discord.gg/ETf4qj5acN`
- `Telegram: https://t.me/mandyport`

The diff between the two `main` branches is exactly that: one file, two deletions. There is no code difference, no extra commit, and no new file in the fork.

## PortWatch VI

`https://github.com/4lwayz/portwatch-vi` (live at `portwatch-vi.xyz`) is a **separate repository and live tracker**, not this one. We are **not yet listed in it**, and this repo cannot edit or deploy that tracker — listing us there requires access to the `4lwayz/portwatch-vi` repo (its `data/tracker.json`, scripts and deploy), so it is a separate-auth task, not a change to this checkout.

## Current measured progress (this PC)

Measured with `scripts/windows-boot/boot.ps1 -Name run94 -Seconds 240` on the build in `E:\Leonida\gta6rt.exe`
(27 Nov 23:50; `LEONIDA_ASYNC_GRAPHICS=1`, `LEONIDA_COMPUTE_COMPILE_WORKERS=4`). Counts only; logs and captures stay in
`E:\Leonida\runs`.

- **The title screen paints.** 2083 `PresentX ok`, 0 failed, 0 device faults, 35 of 35 captured frames non-black. The
  frame sequence is the collage (mean rgb 126/103/124, held for the first ~960 presents), the first-run pages, then a
  fade to the dark loading screen (mean rgb 5.9 → 6.4).
- **No pipeline-creation failures.** 0 `CreateNativeGraphicsPipelineState failed`, 0 `no translated graphics stage`,
  0 `no translated compute shader`. 67 759 `ID3D12Device2::CreatePipelineState` and 25 450
  `XboxID3D12Device::CreateNativeGraphicsPipelineState` calls, 25 038 `PipelineStream::Mesh`.
- **The DirectStorage missing-profile wall is gone.** 0 `E_DSTORAGE` / 0 `missing-profile` lines in run94 (the run that
  still had them was run70). `LEONIDA_DSTORAGE_HISTORY=E:\Leonida\work\bcpack\history.bcph` is found by the harness.
- **What is left is compile latency, not a refusal.** 4 compute pipelines built in the window at 11.6 s, 18.8 s, 19.7 s
  and 47.0 s each (132 KB – 989 KB of SPIR-V); the pipeline cache wrote 19 071 553 bytes
  (`pipeline_cache.heavy.bin`). Indirect draws are skipped while their graphics variant is pending:
  `recorded=37523 emitted=5156 replay_skips=32768 [pending_pipeline=32768]` at the end of the window.
- **The window was too short to judge.** The loading screen only appears at capture frame 1741 of 2083 presents, i.e.
  roughly 40 s before the 240 s cut-off, so run94 measures the *start* of the load, not whether it finishes.
- A one-off crash on a hand-launched run (28 Nov 01:22) was an access violation *inside* the title
  (`game_scarlett_master.exe+0x7233`, write to `0x8`). It did not reproduce in run94's 242 s, so it is recorded here as
  unexplained rather than diagnosed.

## Next concrete actions

1. Run the load to completion: `boot.ps1 -Name run95 -Seconds 600` with a longer press script. Decide from its frames
   whether the loading screen resolves into a world frame; if it does not, the last `[rt]` lines of that run say which
   subsystem it is waiting on.
2. Cut compile latency if the load is gated on it. The backlog is graphics variants (`pending_pipeline`); the compute
   side already shows single builds of 11–47 s. Options, cheapest first: warm the shared cache with
   `gta6rt --precompile-pipelines` before the boot; keep `LEONIDA_FAST_PIPELINES=all` (no driver optimisation); raise
   `LEONIDA_GRAPHICS_COMPILE_WORKERS` and compare build-completion counts per window.
3. Explain the `game_scarlett_master.exe+0x7233` access violation if it returns: pair it with `LEONIDA_STACKS` around
   the moment it happens and the capture index to tell whether it follows a specific streamed asset or pipeline.
4. Optional, separate-auth task only if authorized: get Leonida listed in `4lwayz/portwatch-vi`'s tracker and rerun the
   site's scout so the listing refreshes.
5. Keep a window-screenshot crop of the title bar in the repo (the one showing the HUD) only if we accept it as a
   non-game-data proof; delete the larger game-pixel captures from the repo tree after viewing (repo rule: no game data).

## Push target

Pushes from this checkout go to `origin` (the first-party repo), not the `pink` fork. The `STATUS.md` changes here are committed to `main` and targeted for push to `origin`, which is the repo's default branch. Note: a push from this checkout currently cannot reach `origin` because it resolves as "Repository not found" here; only the fork remote `pink` and the local `ubiq` remote resolve from this checkout.
