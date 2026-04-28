#!/usr/bin/env python3
"""Keep Mermaid in source markdown and render it for Marp export."""

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
SLIDES = BASE_DIR / "CHI_Protocol_Deck.md"
OUT_DIR = BASE_DIR / "mermaid_svg"
OUT_MD = BASE_DIR / "CHI_Protocol_Deck_rendered.md"
OUT_PDF = BASE_DIR / "CHI_Protocol_Deck.pdf"
PUPPETEER_CFG = BASE_DIR / "puppeteer-config.json"
THEME = BASE_DIR / "gem5-chi.css"

os.makedirs(OUT_DIR, exist_ok=True)

COLS_PAT = re.compile(
    r'<div class="columns[^"]*">.*?</div>\s*</div>', re.DOTALL
)
MERMAID_INIT = {
    "theme": "base",
    "look": "classic",
    "flowchart": {"curve": "basis"},
    "sequence": {"showSequenceNumbers": False},
    "themeVariables": {
        "background": "transparent",
        "fontFamily": "Inter, Avenir Next, Avenir, Segoe UI, sans-serif",
        "primaryColor": "#eff6ff",
        "primaryTextColor": "#16324f",
        "primaryBorderColor": "#2563eb",
        "secondaryColor": "#e7f8f5",
        "secondaryTextColor": "#16324f",
        "secondaryBorderColor": "#0f766e",
        "tertiaryColor": "#fff5df",
        "tertiaryTextColor": "#16324f",
        "tertiaryBorderColor": "#c58b1b",
        "lineColor": "#516b84",
        "textColor": "#16324f",
        "mainBkg": "#eff6ff",
        "secondBkg": "#e7f8f5",
        "tertiaryBkg": "#fff5df",
        "clusterBkg": "#f7fbff",
        "clusterBorder": "#cddceb",
        "nodeBorder": "#2563eb",
        "defaultLinkColor": "#516b84",
        "titleColor": "#16324f",
        "edgeLabelBackground": "#f7fbff",
        "actorBkg": "#eff6ff",
        "actorBorder": "#2563eb",
        "actorTextColor": "#16324f",
        "actorLineColor": "#516b84",
        "signalColor": "#516b84",
        "signalTextColor": "#16324f",
        "labelBoxBkgColor": "#f7fbff",
        "labelBoxBorderColor": "#cddceb",
        "labelTextColor": "#16324f",
        "loopTextColor": "#16324f",
        "noteBkgColor": "#fff5df",
        "noteBorderColor": "#c58b1b",
        "noteTextColor": "#16324f",
        "activationBorderColor": "#2563eb",
        "activationBkgColor": "#dbeafe",
        "sequenceNumberColor": "#16324f",
    },
}


def is_inside_columns(text, pos):
    for m in COLS_PAT.finditer(text):
        if m.start() <= pos <= m.end():
            return True
    return False


def extract_mermaid_blocks(text):
    pattern = re.compile(r"```mermaid\n(.*?)```", re.DOTALL)
    return [(m.start(), m.end(), m.group(1)) for m in pattern.finditer(text)]


def themed_mermaid(code):
    init = f"%%{{init: {json.dumps(MERMAID_INIT, sort_keys=True)} }}%%\n"
    if code.lstrip().startswith("%%{init:"):
        return code
    return init + code


def mermaid_to_svg(code, out_path, width=None):
    tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".mmd", delete=False)
    try:
        tmp.write(themed_mermaid(code))
        tmp.close()
        cmd = [
            "mmdc",
            "-i",
            tmp.name,
            "-o",
            str(out_path),
            "-b",
            "transparent",
            "-p",
            str(PUPPETEER_CFG),
        ]
        if width:
            cmd.extend(["--width", str(width)])
        subprocess.run(cmd, check=True, capture_output=True, timeout=60)
    finally:
        os.unlink(tmp.name)


def main():
    with open(SLIDES) as f:
        text = f.read()

    blocks = extract_mermaid_blocks(text)
    print(f"Found {len(blocks)} mermaid blocks")

    replacements = []
    active_svgs = set()
    for i, (start, end, code) in enumerate(blocks):
        h = hashlib.md5(themed_mermaid(code).encode()).hexdigest()[:8]
        in_cols = is_inside_columns(text, start)
        svg_name = f"diagram_{i:02d}_{h}.svg"
        svg_path = os.path.join(OUT_DIR, svg_name)
        rel_path = os.path.relpath(svg_path, OUT_MD.parent)
        active_svgs.add(svg_name)

        render_width = 600 if in_cols else 1200

        if not os.path.exists(svg_path):
            label = "cols" if in_cols else "full"
            print(f"  Rendering block {i} ({label}): {svg_name}")
            try:
                mermaid_to_svg(code, svg_path, width=render_width)
            except subprocess.CalledProcessError as e:
                print(
                    f"  ERROR rendering block {i}: {e.stderr.decode()[:200]}"
                )
                continue
        else:
            print(f"  Using cached: {svg_name}")

        img_ref = f"![diagram]({rel_path})"
        replacements.append((start, end, img_ref))

    for start, end, ref in reversed(replacements):
        text = text[:start] + ref + text[end:]

    for stale_svg in OUT_DIR.glob("diagram_*.svg"):
        if stale_svg.name not in active_svgs:
            stale_svg.unlink()
            print(f"Removed stale cache: {stale_svg.name}")

    with open(OUT_MD, "w") as f:
        f.write(text)
    print(f"Wrote rendered markdown: {OUT_MD}")

    subprocess.run(
        [
            "marp",
            "--allow-local-files",
            "--html",
            "--theme",
            str(THEME),
            str(OUT_MD),
            "--pdf",
            "-o",
            str(OUT_PDF),
        ],
        check=True,
    )
    print(f"Generated PDF: {OUT_PDF}")


if __name__ == "__main__":
    main()
