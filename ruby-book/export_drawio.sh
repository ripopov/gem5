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

    # Post-process: white background, resolve light-dark() to light values.
    perl -pe '
        s/background:\s*transparent/background: #ffffff/g;
        s/background-color:\s*transparent/background-color: #ffffff/g;
        while (s/light-dark\(([^,()]*(?:\([^()]*\)[^,()]*)*),\s*[^()]*(?:\([^()]*\)[^()]*)*\)/\1/g) {}
    ' "$tmp" > "$dst"

    rm -f "$tmp"
done

echo "Done — exported ${#files[@]} diagram(s)."
