#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Vincent Jardin <vjardin@free.fr>, Free Mobile
#
# check_log.sh MODE LOG -- strict verdict for one UML test run
#
# PASS needs all of:
#   - init.sh reached its summary line (no crash, panic or timeout)
#   - the summary reports 0 failed checks
#   - the kernel log has no splat (WARN/BUG/KASAN/UBSAN/lockdep/hung task);
#     those of the upstream bugs in known_splats.awk are named, not counted

mode=$1
log=$2

sum=$(grep -a -o "== $mode mode: [0-9]*/[0-9]* passed, [0-9]* failed ==" "$log" | head -1)
rc=$(grep -a -o 'UML_EXIT=[0-9]*' "$log" | tail -1 | cut -d= -f2)

if [ -z "$sum" ]; then
	echo "=== $mode TEST FAILED: no summary line (UML exit ${rc:-?}: crash, panic or timeout) ==="
	exit 1
fi

case "$sum" in
*" 0 failed =="*)
	;;
*)
	echo "=== $mode TEST FAILED: $sum ==="
	grep -a '^FAIL:' "$log"
	exit 1
	;;
esac

splat='cut here \]|(^|[^A-Za-z])BUG: |KASAN:|UBSAN:|Oops|Kernel panic|possible circular locking|possible recursive locking|inconsistent lock state|sleeping function called from invalid context|blocked for more than|general protection fault'
filtered=$(awk -f "$(dirname "$0")/known_splats.awk" "$log")

if printf '%s\n' "$filtered" | grep -a -E "$splat" >/dev/null; then
	echo "=== $mode TEST FAILED: kernel splat in log ==="
	printf '%s\n' "$filtered" | grep -a -E -m5 "$splat"
	exit 1
fi

known=$(printf '%s\n' "$filtered" | grep -a '^KNOWN UPSTREAM ISSUE:' | sort -u | sed 's/^KNOWN UPSTREAM ISSUE: //' | tr '\n' ' ')
echo "=== $mode TEST PASSED ($sum)${known:+ [known upstream: ${known% }]} ==="
