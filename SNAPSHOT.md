# Pink — Leonida port evaluation snapshot

A private, point-in-time copy of the Leonida GTA VI port at 27 November 2026, assembled so the current state can be
evaluated. Start with [EVALUATION.md](EVALUATION.md); the upstream project README is [README.md](README.md).

This repository contains **no game data**. No archives, executables, carved shaders, textures, audio, or reports
quoting them. The Xbox package and its executable stay on the machine that owns them, per
[docs/design.md](docs/design.md) and [AGENTS.md](AGENTS.md).

## What is here

| Path | What it is |
|---|---|
| `EVALUATION.md` | Read this first: measured state, what is already done, what is left, ranked |
| `evidence/` | Runtime HUD title bar from a live run (13 fps, 75.9 ms, worst 138 ms) |
| `runtime/` | The PE loader and the D3D12→Vulkan layer (`src/`, `shims/`, `test/`) |
| `docs/` | 98 measured format and behaviour notes; `docs/README.md` indexes them |
| `tools/`, `scripts/` | Shader translator, test runners, launcher, build helpers |
| `tests/` | Translator regression suite and the standard-library unit tests |

## What was left out, and why

Build output (`runtime/build`, `runtime/build_msvc`), compiled binaries (`bin_publish`, `bin/`, `obj/`), the CPU
microcode blob `ucode.elf`, Python bytecode, and this project's own generated artifacts. None of it is needed to
evaluate the state, and all of it is regenerable.

The `third_party/rpf9-explorer` submodule's *files* are present, but not as a linked submodule: the checkout is
committed as ordinary files so a reader (or an AI) sees the source without needing a `git submodule update`. Remove
that directory and `git submodule update --init` restores the real thing.

## This is a snapshot, not a fork

Nothing here is a substitute for upstream work. Do not open pull requests against this repository; it is a
point-in-time copy for evaluation. The upstream contribution rules are in [CONTRIBUTING.md](CONTRIBUTING.md) and
[AGENTS.md](AGENTS.md).

## Licence

GPL-3.0-or-later, inherited from the upstream project. See [LICENSE](LICENSE).
