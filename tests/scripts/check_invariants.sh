#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"
fail=0
note() { echo "invariant violation: $1"; fail=1; }

# 1. public headers must not include src/ or platform headers
if grep -rnE '#include ["<](input|event|screen|core|session|transport|codec|platform|kernel|spi|drivers)/' include/grab; then
    note "src/ include in public headers"; fi

# 2. raw sleeps in core outside the (shrink-only) allowlist.
#    Count occurrences per file so extra sleeps in an allowlisted file are caught.
mapfile -t allow < <(grep -vE '^\s*(#|$)' tests/scripts/sleep_allowlist.txt)
allowed_count() {   # expected sleep count for an allowlisted file, else -1
    for entry in "${allow[@]}"; do
        [[ "${entry%%=*}" == "$1" ]] && { echo "${entry##*=}"; return; }
    done
    echo -1
}
while IFS= read -r file; do
    n=$(grep -cE 'sleep_for|usleep\(|nanosleep\(' "$file")
    exp=$(allowed_count "$file")
    if [[ "$exp" == "-1" ]]; then note "raw sleep in non-allowlisted file: $file ($n)";
    elif (( n > exp )); then note "more sleeps than allowlisted in $file: $n > $exp"; fi
done < <(grep -rlE 'sleep_for|usleep\(|nanosleep\(' src --include='*.cpp' --include='*.hpp')

# 3. kill()/killpg() calls outside process_ref.cpp
if grep -rnE '\b(kill|killpg)\(' src --include='*.cpp' | grep -vE 'kernel/lifecycle/process_ref\.cpp'; then
    note "raw kill/killpg outside process_ref"; fi

# 4. kernel must be platform-free (dir may not exist yet)
if [[ -d src/kernel ]] && grep -rnE '#include [<"](X11/|xcb/|wayland|windows\.h|Carbon/)' src/kernel; then
    note "platform header in kernel"; fi

exit $fail
