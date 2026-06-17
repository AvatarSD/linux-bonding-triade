# SPDX-License-Identifier: GPL-2.0
# Shared helpers for the triade kselftest suite.
#
# Topology built by triade_setup_3node():
#
#     ns-a (triade-a, 10.99.0.1) --ab/ba-- ns-b (triade-b, 10.99.0.2)
#         \                                  /
#          ac/ca                       bc/cb
#             \                         /
#              ns-c (triade-c, 10.99.0.3)
#
# Per netns: own triade master (named differently because debugfs is not
# netns-aware and three "triade0"s would collide).

# Bash, sourced by every test. Don't `set -e` here so callers can run
# kselftest assertions without aborting the whole script.

TRIADE_REPO_ROOT="${TRIADE_REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)}"
TRIADE_KO="${TRIADE_KO:-$TRIADE_REPO_ROOT/triade.ko}"
TRIADE_DBG=/sys/kernel/debug/triade

TRIADE_NETNS=(ns-a ns-b ns-c)
TRIADE_NAMES=(triade-a triade-b triade-c)
TRIADE_ADDRS=(10.99.0.1 10.99.0.2 10.99.0.3)
declare -gA TRIADE_SLAVES=( [ns-a]="ab ac" [ns-b]="ba bc" [ns-c]="cb ca" )
TRIADE_PAIRS=("ab ba" "bc cb" "ca ac")

# --- TAP output ------------------------------------------------------------

TRIADE_TAP_N=0
TRIADE_TAP_FAIL=0

tap_plan()    { printf '1..%d\n' "$1"; }
tap_ok()      { TRIADE_TAP_N=$((TRIADE_TAP_N+1)); printf 'ok %d - %s\n' "$TRIADE_TAP_N" "$*"; }
tap_not_ok()  { TRIADE_TAP_N=$((TRIADE_TAP_N+1)); TRIADE_TAP_FAIL=$((TRIADE_TAP_FAIL+1)); printf 'not ok %d - %s\n' "$TRIADE_TAP_N" "$*"; }
tap_skip()    { TRIADE_TAP_N=$((TRIADE_TAP_N+1)); printf 'ok %d - %s # SKIP %s\n' "$TRIADE_TAP_N" "$1" "$2"; }
tap_diag()    { printf '# %s\n' "$*"; }
tap_summary() {
	if [ "$TRIADE_TAP_FAIL" = 0 ]; then
		printf '# all %d tests passed\n' "$TRIADE_TAP_N"
		return 0
	fi
	printf '# %d/%d tests FAILED\n' "$TRIADE_TAP_FAIL" "$TRIADE_TAP_N"
	return 1
}

# --- preconditions ---------------------------------------------------------

triade_require_root() {
	if [ "$(id -u)" != 0 ]; then
		echo "1..0 # SKIP need root"
		exit 0
	fi
}

triade_require_ko() {
	if [ ! -f "$TRIADE_KO" ]; then
		echo "1..0 # SKIP triade.ko not built ($TRIADE_KO missing)"
		exit 0
	fi
}

triade_require_cmds() {
	for c in "$@"; do
		if ! command -v "$c" >/dev/null; then
			echo "1..0 # SKIP need $c"
			exit 0
		fi
	done
}

# --- module + lab lifecycle ------------------------------------------------

TRIADE_LOADED_BY_US=0

triade_load_module() {
	# If triade is already loaded (e.g. host runs a live ring), use it as-is
	# and don't unload at the end. The tests run inside fresh netns so they
	# don't disturb the host's masters.
	if lsmod | grep -q '^triade '; then
		TRIADE_LOADED_BY_US=0
		return 0
	fi
	if insmod "$TRIADE_KO" 2>/dev/null; then
		TRIADE_LOADED_BY_US=1
		return 0
	fi
	return 1
}

triade_unload_module() {
	[ "$TRIADE_LOADED_BY_US" = 1 ] && rmmod triade 2>/dev/null
	return 0
}

triade_cleanup_3node() {
	# Explicitly delete the masters first so triade_dellink runs and
	# tears down the per-master debugfs entry. Otherwise ip netns del
	# alone leaves /sys/kernel/debug/triade/triade-<x> lingering and the
	# next setup_3node silently fails to create fresh ones (debugfs is
	# global, not netns-aware).
	for i in 0 1 2; do
		ip -netns "${TRIADE_NETNS[$i]}" link del "${TRIADE_NAMES[$i]}" 2>/dev/null
	done
	for ns in "${TRIADE_NETNS[@]}"; do
		ip netns del "$ns" 2>/dev/null
	done
	for v in ab ba bc cb ca ac; do
		ip link del "$v" 2>/dev/null
	done
	# Defensive: if previous test crashed before cleanup, kill stale
	# debugfs entries by name. We can't from userspace; the cleanest path
	# is to require module unload. Skipping when triade is in use
	# elsewhere on the host (we treat that as caller's responsibility).
	:
}

triade_setup_3node() {
	triade_cleanup_3node

	for ns in "${TRIADE_NETNS[@]}"; do
		ip netns add "$ns"
		ip -netns "$ns" link set lo up
	done

	for pair in "${TRIADE_PAIRS[@]}"; do
		read -r a b <<<"$pair"
		ip link add "$a" type veth peer name "$b"
	done
	ip link set ab netns ns-a; ip link set ac netns ns-a
	ip link set ba netns ns-b; ip link set bc netns ns-b
	ip link set cb netns ns-c; ip link set ca netns ns-c

	for i in 0 1 2; do
		ns="${TRIADE_NETNS[$i]}"
		name="${TRIADE_NAMES[$i]}"
		addr="${TRIADE_ADDRS[$i]}"
		ip -netns "$ns" link add "$name" type triade
		for s in ${TRIADE_SLAVES[$ns]}; do
			ip -netns "$ns" link set "$s" up
			ip -netns "$ns" link set "$s" master "$name"
		done
		ip -netns "$ns" addr add "$addr/24" dev "$name"
		ip -netns "$ns" link set "$name" up
	done
}

# Wait for the supervision frames to populate each master's node_count to N.
# Returns 0 if reached within @timeout (default 5s), 1 otherwise.
triade_wait_converge() {
	local want=${1:-2}
	local timeout=${2:-5}
	local deadline=$((SECONDS + timeout))
	while [ "$SECONDS" -lt "$deadline" ]; do
		local all_ok=1
		for name in "${TRIADE_NAMES[@]}"; do
			local n
			n=$(awk '$1 == "node_count" { print $2 }' "$TRIADE_DBG/$name/stats" 2>/dev/null)
			[ -z "$n" ] && n=0
			[ "$n" -lt "$want" ] && { all_ok=0; break; }
		done
		[ "$all_ok" = 1 ] && return 0
		sleep 0.5
	done
	return 1
}

triade_ping_all_pairs() {
	local fail=0
	for i in 0 1 2; do
		local src_ns="${TRIADE_NETNS[$i]}"
		for j in 0 1 2; do
			[ "$i" = "$j" ] && continue
			local dst="${TRIADE_ADDRS[$j]}"
			ip netns exec "$src_ns" ping -c2 -W2 -q "$dst" >/dev/null 2>&1 || fail=1
		done
	done
	return $fail
}

triade_dbg_get() { awk -v k="$2" '$1 == k { print $2 }' "$TRIADE_DBG/$1/stats" 2>/dev/null; }
