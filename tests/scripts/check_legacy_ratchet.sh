#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
fail=0
note() { echo "legacy ratchet violation: $1"; fail=1; }

# 1. Budgeted legacy dirs may only shrink.
while IFS='=' read -r dir budget; do
    case "$dir" in ''|\#*) continue ;; esac
    if [[ ! -d "$dir" ]]; then
        note "$dir is gone — delete its stale budget line"
        continue
    fi
    loc=$(find "$dir" \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 --no-run-if-empty cat | wc -l)
    if (( loc > budget )); then
        note "$dir grew to $loc LOC (budget $budget) — legacy is frozen; new code goes in src/{kernel,spi,drivers,frontends,compat}"
    fi
done < tests/scripts/legacy_budget.txt

# 2. No unlisted public headers.
while IFS= read -r f; do
    rel="${f#include/grab/}"
    grep -qxF "$rel" tests/scripts/public_header_allowlist.txt \
        || note "unlisted public header: include/grab/$rel"
done < <(find include/grab -name '*.hpp' | sort)

exit "$fail"
