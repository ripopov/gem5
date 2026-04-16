# Marp Slide Deck Best Practices

Lessons learned from building the CHI Protocol slide deck.

## Mermaid Diagrams

**Marp does not render Mermaid natively** ([github.com/marp-team/marp-core/issues/139](https://github.com/marp-team/marp-core/issues/139)).
A PR to add support ([#719](https://github.com/marp-team/marp-cli/pull/719)) was rejected — the maintainers want it as a plugin, and none exists yet.

**Workaround**: pre-render all Mermaid blocks to SVG using `@mermaid-js/mermaid-cli` (`mmdc`), then reference them as images.

```bash
npx @mermaid-js/mermaid-cli -i diagram.mmd -o diagram.svg -b transparent -p puppeteer-config.json
```

### Puppeteer sandbox issue

On Ubuntu 23.10+ and similar distros, `mmdc` fails with `No usable sandbox!`.
Fix: pass a puppeteer config file with `--no-sandbox`:

```json
// puppeteer-config.json
{
  "args": ["--no-sandbox"]
}
```

Then: `mmdc -i in.mmd -o out.svg -p puppeteer-config.json`

### Render width

Use `--width` to control SVG viewport size:

- **Full-width slides**: `--width 1200`
- **Inside columns (half-width)**: `--width 600`

This doesn't crop content — it sets the viewport Mermaid lays out within. Narrower viewports produce more compact diagrams.

## Image Auto-Scaling

### The problem

SVGs rendered by `mmdc` have fixed pixel dimensions. Large diagrams (especially tall sequence diagrams) overflow slides vertically. Setting only `max-width: 100%` fixes horizontal overflow but not vertical.

### The fix

Constrain **both** dimensions with CSS:

```css
img {
  max-width: 100%;
  max-height: 540px;   /* 720px slide - ~180px for title/padding */
  width: auto;
  height: auto;
  object-fit: contain; /* preserve aspect ratio within bounds */
}
```

`object-fit: contain` is the key — it scales the image to fit within the `max-width` × `max-height` box while maintaining aspect ratio.

### Images inside columns

When two diagrams are stacked inside a `.columns` flex container, each needs a smaller height budget:

```css
.columns img {
  max-height: 250px;   /* 2 × 250 = 500px fits a column */
}
```

### Marp image sizing syntax

Marp supports inline image resizing via alt text keywords:

```markdown
![w:200px h:100px](image.png)    /* explicit size */
![w:50% h:auto](image.png)       /* percentage width */
```

For pre-rendered SVGs, prefer CSS constraints over per-image directives — it's fewer changes and works consistently.

## Background Images

### Avoid inline base64 data URIs

`![bg](data:image/svg+xml;base64,...)` does **not** render in Marp CLI's PDF output (Puppeteer/Chrome doesn't load `data:` URIs in this context).

**Fix**: save the SVG to a file and reference it:

```markdown
![bg right:30% 80%](logo.svg)
```

Run Marp CLI with `--allow-local-files` so it can read local file paths.

## Slide Layout

### Columns with flex

```css
.columns {
  display: flex;
  gap: 30px;
  align-items: flex-start;
}
.columns > div {
  flex: 1;
  min-width: 0;  /* prevent flex children from overflowing */
}
```

`min-width: 0` on flex children is essential — without it, long content (wide tables, code blocks) can push the column wider than `50%` and break the layout.

### Font sizes

```css
section { font-size: 22px; }     /* base text */
table  { font-size: 17px; }      /* tables can be smaller */
.smaller { font-size: 18px; }    /* utility class for dense slides */
```

### Slide dimensions

16:9 is standard (`size: 16:9` in front-matter). The rendered viewport is 1280×720px. Plan content accordingly:

- Available height below title: ~540px
- Available width with padding: ~1180px (50px padding each side)
- In columns: ~560px per column (30px gap)

## Build Pipeline

### Recommended script structure

```
render_mermaid.py
├── Extract ```mermaid blocks from source .md
├── Hash each block → cache key for SVG filename
├── If SVG not cached, render with mmdc
├── Replace ```mermaid blocks with ![diagram](path.svg)
├── Write intermediate .md
└── Run marp-cli to produce PDF
```

### SVG caching

Use content hashes in filenames (e.g., `diagram_03_6d2a7469.svg`). This way:

- Re-running the script skips unchanged diagrams
- Only modified diagrams are re-rendered
- Safe to commit `mermaid_svg/` to version control

### Full command

```bash
npx @marp-team/marp-cli \
  --allow-local-files \   # load local SVGs
  --html \                # allow raw HTML (columns, styled blocks)
  slides_rendered.md \
  --pdf \
  -o slides.pdf
```

## Common Pitfalls

| Pitfall | Symptom | Fix |
|---------|---------|-----|
| Mermaid shows as source code | Raw text in PDF | Pre-render to SVG with `mmdc` |
| Tall diagram overflows slide | Bottom clipped in PDF | `max-height` + `object-fit: contain` |
| Column content overflows | Text/images push outside slide | `min-width: 0` on flex children |
| Base64 bg image missing | Title slide shows raw `![bg]` text | Use file reference + `--allow-local-files` |
| `mmdc` sandbox crash | `No usable sandbox!` error | `puppeteer-config.json` with `--no-sandbox` |
| Two diagrams in one column | Second diagram cut off | `.columns img { max-height: 250px }` |
| SVG rendered too wide | Diagram fills only half column | Set `--width 600` for column diagrams |
