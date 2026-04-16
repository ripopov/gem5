#!/usr/bin/env python3
"""Keep Mermaid in source markdown and render it for Marp export."""

import hashlib
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

os.makedirs(OUT_DIR, exist_ok=True)

COLS_PAT = re.compile(r'<div class="columns">.*?</div>\s*</div>', re.DOTALL)


def is_inside_columns(text, pos):
    for m in COLS_PAT.finditer(text):
        if m.start() <= pos <= m.end():
            return True
    return False


def extract_mermaid_blocks(text):
    pattern = re.compile(r"```mermaid\n(.*?)```", re.DOTALL)
    return [(m.start(), m.end(), m.group(1)) for m in pattern.finditer(text)]


def mermaid_to_svg(code, out_path, width=None):
    tmp = tempfile.NamedTemporaryFile(mode="w", suffix=".mmd", delete=False)
    try:
        tmp.write(code)
        tmp.close()
        cmd = [
            "npx",
            "--yes",
            "@mermaid-js/mermaid-cli",
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
    for i, (start, end, code) in enumerate(blocks):
        h = hashlib.md5(code.encode()).hexdigest()[:8]
        in_cols = is_inside_columns(text, start)
        svg_name = f"diagram_{i:02d}_{h}.svg"
        svg_path = os.path.join(OUT_DIR, svg_name)
        rel_path = os.path.relpath(svg_path, OUT_MD.parent)

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

    with open(OUT_MD, "w") as f:
        f.write(text)
    print(f"Wrote rendered markdown: {OUT_MD}")

    subprocess.run(
        [
            "npx",
            "@marp-team/marp-cli",
            "--allow-local-files",
            "--html",
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
