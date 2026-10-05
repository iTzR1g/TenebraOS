#!/bin/sh
#
# fuzz_deb.sh -- differential fuzz of zbra's Debian version comparator
# against dpkg itself.
#
# dpkg is the authority here. A package manager that orders versions
# differently from the distribution's own tool will resolve a different
# version than apt would, so agreement with dpkg matters more than agreement
# with any table of expected values written by hand.
#
# dpkg exit codes for --compare-versions:
#   0  the relation is TRUE
#   1  the relation is FALSE
#   2  invalid version syntax -- dpkg gives no verdict at all
#
# Case 2 must be skipped rather than counted as "equal": that mistake makes
# a correct comparator look broken, because dpkg rejects any version whose
# epoch is empty, whose post-colon component is empty, or whose upstream part
# does not begin with a digit. tests/vercmp_probe.c only generates valid
# versions, but the guard stays so the script stays honest if that changes.
#
# Both "lt" and "gt" are consulted: "lt" being FALSE only proves a >= b,
# which still leaves a == b indistinguishable from a > b.

set -u

pairs=${1:-20000}
seed=${2:-123456789}

probe=${PROBE:-tests/probe}
cmp=${CMP2:-tests/cmp2}

if [ ! -x "$probe" ] || [ ! -x "$cmp" ]; then
	echo "fuzz: build tests/probe and tests/cmp2 first" >&2
	exit 2
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT INT TERM

"$probe" "$pairs" "$seed" > "$tmp"

total=0
bad=0

while IFS="$(printf '\t')" read -r a b; do
	dpkg --compare-versions "$a" lt "$b" >/dev/null 2>&1
	rc=$?
	[ "$rc" -eq 2 ] && continue

	if [ "$rc" -eq 0 ]; then
		want=-1
	else
		dpkg --compare-versions "$a" gt "$b" >/dev/null 2>&1
		rc2=$?
		[ "$rc2" -eq 2 ] && continue
		if [ "$rc2" -eq 0 ]; then
			want=1
		else
			want=0
		fi
	fi

	total=$((total + 1))
	got=$("$cmp" "$a" "$b")

	if [ "$want" != "$got" ]; then
		bad=$((bad + 1))
		if [ "$bad" -le 15 ]; then
			printf 'MISMATCH: [%s] vs [%s] dpkg=%s zbra=%s\n' \
				"$a" "$b" "$want" "$got"
		fi
	fi
done < "$tmp"

echo "fuzz: checked=$total mismatches=$bad seed=$seed"
[ "$bad" -eq 0 ]