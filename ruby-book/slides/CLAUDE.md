# Agent guide

This file provides guidance to coding agents working in this directory. It is the canonical source; `AGENTS.md` and `CLAUDE.md` both resolve to the same content.

This directory is a Marp slide-authoring workspace that exports PDF/HTML decks accompanying the gem5 Ruby book. The repo-root `CLAUDE.md` covers the gem5 simulator and the book itself; read this file for slide-workflow specifics only.

## Build commands

Run everything from this directory (`ruby-book/slides/`).

```sh
npm install                 # one-time: installs Marp CLI + Mermaid CLI
npm run build               # render Mermaid + export PDF (CHI_Protocol_Deck.pdf)
npm run html                # render Mermaid + export PDF + HTML
python3 render_mermaid.py   # equivalent to `npm run build`
```

`npm run build` and `npm run pdf` both just invoke `render_mermaid.py`, which internally calls `marp-cli --pdf`. There is no separate "render-only" mode.

## Build pipeline

`render_mermaid.py` is the single source of truth. In order:

1. Reads `CHI_Protocol_Deck.md` (the authored deck — Mermaid stays in source).
2. Extracts every ` ```mermaid ` block, injects the shared `MERMAID_INIT` theme block, and MD5-hashes the themed code.
3. Renders each block via `npx @mermaid-js/mermaid-cli` into `mermaid_svg/diagram_<NN>_<hash>.svg`. Cached on hash — unchanged diagrams are skipped; stale SVGs whose hashes no longer appear are deleted.
4. Render width is context-aware: `--width 600` for diagrams inside `<div class="columns">`, `--width 1200` otherwise (detected by `COLS_PAT`).
5. Writes `CHI_Protocol_Deck_rendered.md` with Mermaid blocks replaced by `![diagram](mermaid_svg/...)` image refs.
6. Invokes `npx @marp-team/marp-cli --allow-local-files --html --theme gem5-chi.css ... --pdf` to produce `CHI_Protocol_Deck.pdf`.

Consequences:
- Never hand-edit `CHI_Protocol_Deck_rendered.md` or files in `mermaid_svg/` — they are build artifacts.
- After editing `gem5-chi.css`, re-run the build (no Mermaid re-render needed; hashes are unchanged).
- After editing `MERMAID_INIT` in `render_mermaid.py`, every diagram's hash changes and all SVGs get re-rendered.
- `puppeteer-config.json` passes `--no-sandbox` to Chromium so `mmdc` works on sandbox-restricted hosts.

## Authoring conventions

Follow `MarpBestPractices.md` — it captures the hard-won rules for this deck. Key points that affect every edit:

- **Custom theme, not inline CSS.** All styling lives in `gem5-chi.css` (imported as `@theme gem5-chi`). Do not pile `style:` blocks into the deck front matter.
- **Channel color semantics are load-bearing:** REQ=blue, SNP=gold, RSP=green, DAT=violet, warnings=red. These colors are shared between slide cards (`.channel-card.req/.snp/.rsp/.dat`, `.pill.req/...`) and Mermaid diagrams (via `MERMAID_INIT` variables in `render_mermaid.py`). Keep them in sync if you change one side.
- **Speaker notes convention:** wrap long narration in `<!-- Speaker Notes: ... -->` HTML comments placed immediately before the `---` slide separator. This is grep-able (`grep -c "Speaker Notes:" CHI_Protocol_Deck.md`) and survives PDF/HTML export cleanly. Do not use Marp's native `<!-- _notes: ... -->` directive for long-form prose — it is per-slide metadata, not a narration format.
- **Image sizing** is handled globally in `gem5-chi.css` (`img { max-height: 540px; object-fit: contain; }` and `.columns img { max-height: 250px; }`). Prefer fixing overflow in CSS rather than with per-image Marp sizing syntax.
- **Reusable CSS classes:** `.hero`, `.columns`, `.comparison-grid`, `.card-grid.two/.three`, `.channel-card`, `.state-grid`, `.takeaway`, `.callout.warning`, `.metric-strip`, `.pill`, `.accent-{blue,teal,gold,violet,green,red,slate}`. Prefer these over ad-hoc markup.
- **16:9 slide budget:** ~1180 px usable width, ~540 px usable height under a title. One idea, one focal visual per slide. If you need `.smaller`, consider splitting instead.

## Deck identity

**Title:** *CHI in gem5 — From Protocol to Ruby and Garnet*

**Scope:** introduce the AMBA 5 CHI protocol, then walk the gem5 implementation stack: Ruby (coherence framework, SLICC-generated CHI controllers) and Garnet (NoC router/link model). Mirrors the book's "From Requests to Routers to DRAM" framing, scoped to CHI.

Filename note: the source file is still `CHI_Protocol_Deck.md` (inherited from v1 and hardcoded in `render_mermaid.py`). Rename is possible later but not required.

## Deck layout: new title → new content → Backup divider → v1 archive

`CHI_Protocol_Deck.md` is structured as:

1. **SLIDE 1: Title (new deck)** — hero slide with the new deck title. Edit here to tweak the deck's opening.
2. **SLIDE 1a: Blank placeholder** — empty slide, `_paginate: false`. This is where the new deck's content is being built. Replace/extend this region as the new deck grows.
3. **SLIDE 1b: Backup divider** — a centered "Backup" slide (scoped CSS, 96 px heading, `_paginate: false`) marking the boundary.
4. **SLIDE 1c: v1 Title (archived)** — the original v1 hero slide, kept as the opening of the archived content.
5. **SLIDE 2 onward — v1 archive** — the original CHI deck content, frozen reference material. Do not evolve it in place.

Implications for agents:
- New slide work goes between SLIDE 1 (new title) and SLIDE 1b (Backup divider). Replace/expand the "SLIDE 1a" blank placeholder.
- Leave SLIDE 1c and everything from "SLIDE 2: Why CHI?" onward untouched unless explicitly asked.
- The theme (`gem5-chi.css`), pipeline (`render_mermaid.py`), and playbook (`MarpBestPractices.md`) are reusable starting points for the new deck but are open for redesign.
- `render_mermaid.py` hardcodes `CHI_Protocol_Deck.md` as its input — the archived deck and the new deck share one source file and one build, which is fine for now.
- After any edit, rebuild with `npm run build` and confirm `CHI_Protocol_Deck.pdf` regenerates without errors.

## Files at a glance

- `CHI_Protocol_Deck.md` — authored source (Mermaid kept inline). Edit this.
- `gem5-chi.css` — the Marp theme. Edit this for any visual change.
- `render_mermaid.py` — build driver and Mermaid theming. Edit to change the pipeline or diagram palette.
- `MarpBestPractices.md` — authoring playbook. Consult before non-trivial slide edits.
- `CHI_Protocol_Deck_rendered.md`, `CHI_Protocol_Deck.pdf`, `CHI_Protocol_Deck.html`, `mermaid_svg/` — build artifacts (gitignored except the cached SVGs, which are intentionally committed for reproducibility).
