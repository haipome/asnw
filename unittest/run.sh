#!/bin/bash
# Run the unit tests of one build variant.
#   unittest/run.sh <build-dir> <debug|release|asan|tsan|valgrind> [test-name]
#
# t_*   must pass.
# kb_*  minimal reproducers for known, not yet fixed bugs. They are expected
#       to FAIL (reported as XFAIL) and run in the asan variant only. Once the
#       bug is fixed the test shows up as XPASS and the run fails: rename the
#       file from kb_ to t_ to make it a regression test.
set -u
build=$1
mode=$2
only=${3:-}
dir=$build/unittest
log=$dir/log
mkdir -p "$log"

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:detect_stack_use_after_return=1
export UBSAN_OPTIONS=print_stacktrace=1
export TSAN_OPTIONS=halt_on_error=1

pass=0 fail=0 xfail=0 xpass=0
for bin in "$dir"/t_* "$dir"/kb_*; do
    name=$(basename "$bin")
    [[ $name == *.* || ! -x $bin ]] && continue
    [[ -n $only && $name != "$only" ]] && continue
    [[ $name == kb_* && $mode != asan ]] && continue

    cmd=("$bin")
    [[ $mode == valgrind ]] && export ASNW_UT_VALGRIND=1 && cmd=(valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite "$bin")
    timeout 60 "${cmd[@]}" >"$log/$name.log" 2>&1
    rc=$?

    if [[ $name == kb_* ]]; then
        if [[ $rc -ne 0 ]]; then
            xfail=$((xfail + 1)); printf 'XFAIL  %-32s (known bug, rc=%d)\n' "$name" $rc
        else
            xpass=$((xpass + 1)); printf 'XPASS  %-32s (bug fixed? rename to t_)\n' "$name"
        fi
    elif [[ $rc -eq 0 ]]; then
        pass=$((pass + 1)); printf 'PASS   %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL   %-32s (rc=%d, see %s)\n' "$name" $rc "$log/$name.log"
        tail -n 15 "$log/$name.log" | sed 's/^/       | /'
    fi
done

echo "[$mode] pass=$pass fail=$fail xfail=$xfail xpass=$xpass   logs: $log/"
[[ $fail -eq 0 && $xpass -eq 0 ]]
