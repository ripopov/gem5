# Marp workspace

## Install

Install Marp CLI and Mermaid CLI globally (one-time, system-wide):

```sh
npm install -g @marp-team/marp-cli @mermaid-js/mermaid-cli
```

## Build PDF

```sh
npm run build
```

The authored source stays in `CHI_Protocol_Deck.md` and keeps Mermaid code
blocks directly in the markdown.
`render_mermaid.py` pre-renders those diagrams to SVG, writes
`CHI_Protocol_Deck_rendered.md`, and then exports the PDF with Marp CLI.

## Export HTML

```sh
npm run html
```
