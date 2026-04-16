# Marp Slide Deck Best Practices

Lessons learned from building the CHI Protocol slide deck.

## Authoring Contract

- Keep the authored deck in one Markdown source file.
- Keep Mermaid blocks in the source deck for readability and review.
- Pre-render Mermaid only as a build step for Marp export.
- Treat the rendered markdown and generated SVGs as build artifacts, even if cached in git.

## Custom Theme First

Do not build serious decks on `theme: default` with a huge inline `style:` block.

Create a custom Marp theme file instead:

```css
/* @theme gem5-chi */
@import 'default';

:root {
  --chi-ink: #16324f;
  --chi-blue: #2563eb;
  --chi-teal: #0f766e;
  --chi-gold: #c58b1b;
  --chi-violet: #7c3aed;
}

section {
  font-family: Inter, "Avenir Next", sans-serif;
  font-size: 22px;
  color: var(--chi-ink);
}
```

Then reference it from the deck front matter:

```yaml
---
marp: true
theme: gem5-chi
paginate: true
size: 16:9
---
```

And load the theme during export:

```bash
npx @marp-team/marp-cli \
  --allow-local-files \
  --html \
  --theme gem5-chi.css \
  slides_rendered.md \
  --pdf \
  -o slides.pdf
```

For multi-deck workspaces with several named themes, `--theme-set` is still useful.

### Why this matters

- One place defines typography, spacing, color, and component styling.
- Multiple decks can share one visual system.
- The slide source becomes content-focused instead of CSS-heavy.
- Theme changes invalidate less authoring context than editing the deck front matter.

## Mermaid Diagrams

**Marp does not render Mermaid natively** ([github.com/marp-team/marp-core/issues/139](https://github.com/marp-team/marp-core/issues/139)).
The practical workflow is still: keep Mermaid in source, render to SVG during build, then let Marp place the SVGs.

### Base rendering command

```bash
npx @mermaid-js/mermaid-cli \
  -i diagram.mmd \
  -o diagram.svg \
  -b transparent \
  -p puppeteer-config.json
```

### Puppeteer sandbox issue

On Ubuntu 23.10+ and similar distros, `mmdc` may fail with `No usable sandbox!`.
Use:

```json
{
  "args": ["--no-sandbox"]
}
```

Then run:

```bash
mmdc -i in.mmd -o out.svg -p puppeteer-config.json
```

## Theme Mermaid to Match the Deck

The default Mermaid palette rarely matches the slide theme.
If the deck is blue/teal and Mermaid exports are purple/yellow, the diagrams will look bolted on.

Inject a shared Mermaid init block during rendering:

```python
MERMAID_INIT = {
    "theme": "base",
    "themeVariables": {
        "background": "transparent",
        "fontFamily": "Inter, Avenir Next, sans-serif",
        "primaryColor": "#eff6ff",
        "primaryBorderColor": "#2563eb",
        "secondaryColor": "#e7f8f5",
        "secondaryBorderColor": "#0f766e",
        "tertiaryColor": "#fff5df",
        "tertiaryBorderColor": "#c58b1b",
        "lineColor": "#516b84",
        "textColor": "#16324f",
        "noteBkgColor": "#fff5df",
        "noteBorderColor": "#c58b1b",
        "noteTextColor": "#16324f"
    }
}
```

### Recommended color semantics

Reuse the same colors across slides and diagrams:

- `REQ` = blue
- `SNP` = gold / amber
- `RSP` = green
- `DAT` = violet
- warnings = red-orange
- neutral metadata = gray-blue

This helps transaction slides feel continuous instead of re-explained.

### Render width

Use `--width` to control the Mermaid viewport:

- Full-width diagram: `--width 1200`
- Diagram inside columns: `--width 600`

This does not crop content.
It changes the layout space Mermaid uses before exporting.

## Image Auto-Scaling

### The problem

SVGs rendered by `mmdc` have fixed dimensions.
Tall diagrams, especially sequence diagrams, can overflow vertically even when width is constrained.

### The fix

Constrain both dimensions:

```css
img {
  max-width: 100%;
  max-height: 540px;
  width: auto;
  height: auto;
  object-fit: contain;
}
```

`object-fit: contain` is the key.

### Images inside columns

```css
.columns img {
  max-height: 250px;
}
```

### Marp image sizing syntax

```markdown
![w:200px h:100px](image.png)
![w:50% h:auto](image.png)
```

For Mermaid exports, prefer deck-level CSS constraints over per-image tweaks.

## Slide Layout Patterns

Do not default every technical slide to “table on the left, diagram on the right.”
It becomes visually flat very quickly.

Use a small set of repeatable slide archetypes instead.

### 1. Comparison grid

Best for protocol families, option trade-offs, and modes.

```html
<div class="comparison-grid">
  <div class="card accent-blue">...</div>
  <div class="card accent-gold">...</div>
  <div class="card accent-violet">...</div>
  <div class="card accent-teal">...</div>
</div>
```

Use this instead of a 4-row comparison table when the point is contrast, not lookup.

### 2. Card grid

Best for node roles, channel roles, message categories, or configuration modes.

```html
<div class="card-grid two">
  <div class="card">...</div>
  <div class="card">...</div>
</div>
```

### 3. Diagram + takeaway

Best for transaction flows and mechanism slides.

- One main diagram.
- One short takeaway box.
- Speaker notes carry the detailed narration.

### 4. Metrics strip

Best for summary slides and “what changed” slides.

```html
<div class="metric-strip">
  <div class="metric"><strong>4</strong><span>channels</span></div>
  <div class="metric"><strong>6</strong><span>node types</span></div>
</div>
```

## Replace Tables More Aggressively

Tables are still useful for:

- file inventories
- API parameter reference
- dense opcode lookup
- appendix material

But for presentation slides, replace about half of the tables with cards or comparison panels.

### Use a table when

- the audience needs to scan rows precisely
- exact cross-column comparison matters
- the slide is more reference than persuasion

### Use cards when

- the audience needs a quick conceptual distinction
- each item deserves its own emphasis
- you want color or visual grouping to carry meaning

### Use a takeaway panel when

- the real point is one sentence, and the details are supporting evidence

## Background Images

Avoid inline base64 data URIs.

`![bg](data:image/svg+xml;base64,...)` does not reliably render in Marp CLI PDF output.

Use file references instead:

```markdown
![bg right:34% 78%](logo.svg)
```

Run Marp CLI with `--allow-local-files`.

## Useful Theme Classes

Recommended reusable classes:

- `.hero` for title slides
- `.columns` for side-by-side content
- `.comparison-grid` for 2x2 trade-off slides
- `.card-grid` for role/category slides
- `.channel-card` for transport-lane semantics
- `.state-grid` for coherence state summaries
- `.takeaway` for one-message summary panels
- `.callout.warning` for failure modes
- `.metric-strip` for summary slides

## Slide Budgets

Work within explicit limits.

- 16:9 viewport is 1280×720
- available height under a normal title is about 540 px
- usable width after padding is about 1180 px
- inside two columns, each side gets about 560 px

Practical budget rules:

- one main idea per slide
- one main visual focus per slide
- one table or one diagram, not both unless one is very small
- if you need `.smaller`, ask whether the slide should split

## Speaker Notes

Keep slides visually light by moving the narration off-slide.
Every substantive slide should carry a block of speaker notes — the slide is the scaffold, the notes are the lecture.

### Canonical format

Use an HTML comment that starts with the literal marker `Speaker Notes:` and end it immediately before the `---` slide separator:

```markdown
## ReadShared Transaction

<!-- slide body: diagram + takeaway -->

<!-- Speaker Notes:
Let's trace a complete ReadShared transaction — the most common operation
in any multi-core system. This is what happens when a CPU core does a
load that misses its L1 cache.

Step 1: The CPU issues a load that misses. The RN-F allocates a
Transaction Buffer Entry (TBE) to track this in-flight operation...
-->

---
```

### Why this convention over Marp's native notes

Marp supports an HTML comment directive for notes, but it is strictly a per-slide metadata field.
Plain HTML comments are more robust for long-form narration:

- Multi-paragraph prose with code, colons, and apostrophes needs no escaping.
- Comments are invisible in every renderer (PDF, HTML, PPTX) — zero risk of leaking into the exported deck.
- Grep-friendly: `grep -c "Speaker Notes:" deck.md` counts covered slides; a diff of the speaker-notes hunks reviews cleanly in PRs.
- Portable across Marp, Pandoc, and plain Markdown viewers.

Trade-off: the notes do not appear in Marp's presenter-mode notes pane.
For workflows that produce a PDF for distribution and a written narration track, that does not matter.
If live presenter-mode notes are required, mirror a one-paragraph summary into a `<!-- _notes: ... -->` directive as well.

### Content guidance

- **Prose, not bullets.** Notes are meant to be read aloud. Bullets reward scanning, sentences reward delivery.
- **Budget 30–60 lines (~200–400 words) per substantive slide.** Cover *why this slide exists*, the mechanics not shown on the slide, one concrete example, and a gotcha or misconception.
- **Extend, don't repeat.** If the slide already says it, don't say it again in notes. Say the thing the slide can't fit.
- **Anchor to code and files.** Speaker notes are the right place for `src/mem/ruby/protocol/chi/CHI-cache-actions.sm`-style references that would be noise on the slide.
- **Skip notes on pure dividers and title slides.** If the slide is one sentence, the notes are one sentence, which is not worth the block.

### Why this helps slide layout

Speaker notes relieve pressure on the slide itself.
If you find yourself shrinking type, adding a fourth column, or extending bullets past five items, the real fix is usually to move that content into the notes and keep one clean idea on the slide.
The notes block is effectively an overflow buffer that also improves the delivered presentation.

## Build Pipeline

### Recommended script structure

```
render_mermaid.py
├── Extract ```mermaid blocks from source .md
├── Inject shared Mermaid theme init
├── Hash each themed block for stable cache filenames
├── Render missing diagrams with mmdc
├── Replace Mermaid blocks with local SVG image refs
├── Write intermediate rendered markdown
└── Run marp-cli with the custom theme to export PDF / HTML
```

### SVG caching

Use content hashes in filenames such as `diagram_03_6d2a7469.svg`.

Benefits:

- unchanged diagrams are reused
- changed diagrams invalidate naturally
- cached assets are safe to commit if you want reproducible exports

## Review Checklist

Before calling a deck “done,” check:

1. Does the deck use a custom theme instead of piling CSS into the front matter?
2. Do Mermaid diagrams visually match the deck palette and typography?
3. Did we replace enough tables with cards or comparison layouts?
4. Is each slide understandable in about 10 seconds?
5. Is there one visual focal point per slide?
6. Did we split dense slides instead of shrinking everything?
7. Does the PDF export look identical in tone to the HTML preview?
8. Does every substantive slide carry a `<!-- Speaker Notes: ... -->` block?

## Common Pitfalls

| Pitfall | Symptom | Fix |
|---------|---------|-----|
| Staying on `theme: default` | Deck looks like polished notes, not a designed presentation | Create a custom Marp theme and load it during export |
| Mermaid keeps its default palette | Diagrams feel visually disconnected from the deck | Inject a shared Mermaid init block with theme variables |
| Every comparison is a table | Slides read like documentation pages | Replace at least half of conceptual tables with cards / panels |
| Tall diagram overflows slide | Bottom clipped in PDF | `max-height` + `object-fit: contain` |
| Column content overflows | Text or images push outside slide | `min-width: 0` on flex children |
| Base64 bg image missing | Background does not appear in PDF | Use file reference + `--allow-local-files` |
| `mmdc` sandbox crash | `No usable sandbox!` error | `puppeteer-config.json` with `--no-sandbox` |
| SVG rendered too wide in a column | Diagram feels cramped or clipped | Render with a narrower viewport such as `--width 600` |
| Slide is crowded because narration lives on it | Type shrinks, bullets grow past five items | Move the detail into `<!-- Speaker Notes: ... -->` |
