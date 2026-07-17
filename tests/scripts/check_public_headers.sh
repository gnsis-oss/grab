#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
fail=0
note() { echo "public header violation: $1"; fail=1; }

# No unlisted public headers: include/grab/ is a closed, allowlisted surface.
while IFS= read -r f; do
    rel="${f#include/grab/}"
    grep -qxF "$rel" tests/scripts/public_header_allowlist.txt \
        || note "unlisted public header: include/grab/$rel"
done < <(find include/grab -name '*.hpp' | sort)

exit "$fail"
