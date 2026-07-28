#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

cd "${BUNDLE_ROOT}"
while IFS= read -r path; do
    printf '%s  %s\n' "$(sha256_file "${path}")" "${path}"
done < <(
    find . -type f ! -name MANIFEST.sha256 | LC_ALL=C sort
) > MANIFEST.sha256
echo "wrote ${BUNDLE_ROOT}/MANIFEST.sha256"
