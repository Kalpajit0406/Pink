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

- The title screen is reachable: the game presents an identical “Americas” collage (md5 `8929bf1dfd2e`) in a 180 s window with native PSO failures at 0 and compute-shader misses at 0 in the reached run.
- With input enabled (scripted pad via `LEONIDA_INPUT_SCRIPT` on this build, which serves no SDL3 devices), the title leaves that landing prompt and the capture sequence changes past the collage: navigation experience, brightness, legal notices, then a dark loading screen.
- The next concrete blocker on our side is **asset streaming**: during that loading stage the DirectStorage path reports `E_DSTORAGE_DECOMPRESSION_ERROR` (0x89240030) with reason `missing-profile` on several TTD-hosted texture dictionaries (`data/ttdb/...`). That is the lead for getting past the loading screen.
- Title-bar HUD is now on by default and the counters read in MB; a window-screenshot crop of the title bar exists and shows the caption plus FPS/RAM/VRAM MB and the pipeline counter (`PSO -` because `LEONIDA_MEM_PROBE` was not enabled in that grab).

## Next concrete actions

1. Follow the DirectStorage missing-profile lead: locate how the BCPack predictor/decoder history is supplied for streamed TTD requests (`runtime/src/bcpack.cpp` returns `reason = "missing-profile"`, and `docs/directstorage.md` describes the `history.bcph` / `LEONIDA_DSTORAGE_HISTORY` / `tools/build_dstorage_history.py` path). Decide whether a history file is the expected fix here or whether a request is missing metadata that must be passed through.
2. Get past the loading screen to a world frame and capture one that is visibly not the title collage (or, if it truly cannot leave, write a precise one-line statement of the blocking condition with the log lines that prove it).
3. Optional, separate-auth task only if authorized: get Leonida listed in `4lwayz/portwatch-vi`'s tracker and rerun the site's scout so the listing refreshes.
4. Keep a window-screenshot crop of the title bar in the repo (the one showing the HUD) only if we accept it as a non-game-data proof; delete the larger game-pixel captures from the repo tree after viewing (repo rule: no game data).

## Push target

Pushes from this checkout go to `origin` (the first-party repo), not the `pink` fork. The `STATUS.md` changes here are committed to `main` and targeted for push to `origin`, which is the repo's default branch. Note: a push from this checkout currently cannot reach `origin` because it resolves as "Repository not found" here; only the fork remote `pink` and the local `ubiq` remote resolve from this checkout.
