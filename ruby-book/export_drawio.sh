#!/usr/bin/env bash
# Export all .drawio files under resources/ to SVG.
#
# Usage:  ./export_drawio.sh
#
# Requires: drawio (draw.io desktop), xvfb-run (headless X), perl.
#
# The exported SVGs get a white background and all CSS light-dark()
# functions are resolved to their light-mode values so the diagrams
# render identically in every environment.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESOURCES="$SCRIPT_DIR/resources"

shopt -s nullglob
files=("$RESOURCES"/*.drawio)
shopt -u nullglob

if [[ ${#files[@]} -eq 0 ]]; then
    echo "No .drawio files found in $RESOURCES"
    exit 0
fi

for src in "${files[@]}"; do
    dst="${src%.drawio}.svg"
    tmp=$(mktemp)
    echo "Exporting $(basename "$src") -> $(basename "$dst")"

    # Export to a temp file (avoids Chromium stderr leaking into stdout).
    xvfb-run drawio --export --format svg --svg-theme light \
        --output "$tmp" "$src" >/dev/null 2>&1

    # Post-process:
    #   - white background
    #   - resolve light-dark() to light values
    #   - strip embedded base64 PNG fallbacks (drawio adds them as a
    #     fallback for viewers that can't render SVG foreignObject; they
    #     balloon the file 30-50x and are unneeded for modern browsers,
    #     GitHub, and markdown rendering).
    perl -pe '
        s/background:\s*transparent/background: #ffffff/g;
        s/background-color:\s*transparent/background-color: #ffffff/g;
        while (s/light-dark\(([^,()]*(?:\([^()]*\)[^,()]*)*),\s*[^()]*(?:\([^()]*\)[^()]*)*\)/\1/g) {}
        s{<image x="[^"]*" y="[^"]*" width="[^"]*" height="[^"]*" xlink:href="data:image/png[^"]*"\s*/?>}{}g;
    ' "$tmp" > "$dst"

    # Ensure trailing newline so pre-commit end-of-file-fixer is a no-op.
    [[ -n "$(tail -c 1 "$dst")" ]] && printf '\n' >> "$dst"

    rm -f "$tmp"
done

echo "Done — exported ${#files[@]} diagram(s)."
