#!/bin/bash
# Print line coverage per source file after the tests ran in a coverage
# build. Used by `make coverage`.
#   unittest/coverage.sh <build-dir>
set -u
build=$1
total=0
covered=0
printf '%-22s %8s %7s\n' file lines covered
for src in asnw/*.c wrapper/ut_*.c; do
    obj=$build/${src%.c}.o
    [[ -f ${obj%.o}.gcda ]] || continue
    line=$(gcov -n -o "$(dirname "$obj")" "$src" 2>/dev/null | grep -A1 "File '$src'" | grep 'Lines executed')
    pct=$(sed -E 's/.*:([0-9.]+)% of ([0-9]+)/\1/' <<<"$line")
    num=$(sed -E 's/.*:([0-9.]+)% of ([0-9]+)/\2/' <<<"$line")
    printf '%-22s %8s %6s%%\n' "$src" "$num" "$pct"
    total=$((total + num))
    covered=$(awk -v c="$covered" -v p="$pct" -v n="$num" 'BEGIN { printf "%d", c + p * n / 100 + 0.5 }')
done
printf '%-22s %8s %6s%%\n' total "$total" "$(awk -v c="$covered" -v t="$total" 'BEGIN { printf "%.1f", t ? c * 100 / t : 0 }')"
echo "line by line: gcov -o $build/asnw asnw/<file>.c   (writes <file>.c.gcov, ##### marks lines never run)"
