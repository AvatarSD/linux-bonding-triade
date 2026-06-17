#!/usr/bin/env bash
# Triade lab: a 3-node ring inside network namespaces, exercising the full
# M1..M4 stack with debugfs-based assertions on M3 supervision/dedup/self-discard.
#
# Topology (each pair is a veth):
#
#     ns-a (triade-a, 10.99.0.1) --ab/ba-- ns-b (triade-b, 10.99.0.2)
#         \                                  /
#          ac/ca                       bc/cb
#             \                         /
#              ns-c (triade-c, 10.99.0.3)
#
# Each namespace owns its own triade master (named differently because
# debugfs is not netns-aware and three "triade0"s would collide).
#
# Run as root:   sudo tools/triade-lab.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
KO="$HERE/triade.ko"
DBG=/sys/kernel/debug/triade

NETNS=(ns-a ns-b ns-c)
NAMES=(triade-a triade-b triade-c)
ADDRS=(10.99.0.1 10.99.0.2 10.99.0.3)
declare -A SLAVES=( [ns-a]="ab ac" [ns-b]="ba bc" [ns-c]="cb ca" )
PAIRS=("ab ba" "bc cb" "ca ac")

cleanup() {
	set +e
	for ns in "${NETNS[@]}"; do
		ip netns del "$ns" 2>/dev/null
	done
	for v in ab ba bc cb ca ac; do
		ip link del "$v" 2>/dev/null
	done
	rmmod triade 2>/dev/null
}
trap cleanup EXIT

[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }
[ -f "$KO" ] || { echo "build first: make"; exit 1; }

echo "== load module =="
rmmod triade 2>/dev/null || true
insmod "$KO"
dmesg | tail -1 | grep -q "ring driver loaded" && echo "  loaded OK"

echo "== create namespaces =="
for ns in "${NETNS[@]}"; do
	ip netns add "$ns"
	ip -netns "$ns" link set lo up
done

echo "== wire veth ring =="
for pair in "${PAIRS[@]}"; do
	read -r a b <<<"$pair"
	ip link add "$a" type veth peer name "$b"
done
ip link set ab netns ns-a; ip link set ac netns ns-a
ip link set ba netns ns-b; ip link set bc netns ns-b
ip link set cb netns ns-c; ip link set ca netns ns-c

echo "== build each ring master =="
for i in 0 1 2; do
	ns="${NETNS[$i]}"
	name="${NAMES[$i]}"
	addr="${ADDRS[$i]}"
	ip -netns "$ns" link add "$name" type triade
	for s in ${SLAVES[$ns]}; do
		ip -netns "$ns" link set "$s" up
		ip -netns "$ns" link set "$s" master "$name"
	done
	ip -netns "$ns" addr add "$addr/24" dev "$name"
	ip -netns "$ns" link set "$name" up
done

# 2 supervision cycles so node tables converge.
sleep 2

echo
echo "== all-pairs ping (3 pkts each) =="
fail=0
for i in 0 1 2; do
	src_ns="${NETNS[$i]}"
	for j in 0 1 2; do
		[ "$i" = "$j" ] && continue
		dst="${ADDRS[$j]}"
		if ip netns exec "$src_ns" ping -c3 -W2 -q "$dst" >/dev/null 2>&1; then
			echo "  $src_ns -> $dst   OK"
		else
			echo "  $src_ns -> $dst   FAIL"
			fail=1
		fi
	done
done

# Helpers for reading the per-master debugfs counters from the root ns.
dbg_get() { awk -v k="$2" '$1 == k { print $2 }' "$DBG/$1/stats"; }
dbg_dump() { sed 's/^/    /' "$DBG/$1/stats"; }

echo
echo "== debugfs counters (after closed-ring ping) =="
for name in "${NAMES[@]}"; do
	echo "--- $name ---"
	dbg_dump "$name"
done

echo
echo "== assertions =="
assert_gt() {
	local name=$1 key=$2 lo=$3
	local v
	v=$(dbg_get "$name" "$key")
	v=${v:-0}
	if [ "$v" -gt "$lo" ]; then
		printf "  %-12s %-22s = %s  (> %s)  OK\n" "$name" "$key" "$v" "$lo"
	else
		printf "  %-12s %-22s = %s  (need > %s)  FAIL\n" "$name" "$key" "$v" "$lo"
		fail=1
	fi
}

# Supervision MUST be running (each node should see 2 peers' frames per second).
for name in "${NAMES[@]}"; do
	assert_gt "$name" rx_super 1
	assert_gt "$name" node_count 1
done
# In a closed 3-ring, each supervision and each ARP flood arrives twice; the
# second copy MUST be dedup-dropped.
for name in "${NAMES[@]}"; do
	assert_gt "$name" rx_flood_dup 0
done
# At least one ARP-time broadcast originating here must have circled back.
saw_self=0
for name in "${NAMES[@]}"; do
	v=$(dbg_get "$name" rx_self_discard)
	[ "${v:-0}" -gt 0 ] && saw_self=1
done
if [ "$saw_self" = 1 ]; then
	echo "  rx_self_discard fired on at least one node  OK"
else
	echo "  rx_self_discard never fired - dedup may be eating before self-discard  WARN"
fi

echo
echo "== node table per master =="
for name in "${NAMES[@]}"; do
	echo "--- $name ---"
	sed 's/^/    /' "$DBG/$name/nodes"
done

echo
echo "== open the ab link, then re-ping a<->b in both directions =="
ip -netns ns-a link set ab down
sleep 2
if ip netns exec ns-a ping -c3 -W2 -q "${ADDRS[1]}" >/dev/null 2>&1; then
	echo "  ns-a -> ns-b via ns-c   OK"
else
	echo "  ns-a -> ns-b via ns-c   FAIL"
	fail=1
fi
if ip netns exec ns-b ping -c3 -W2 -q "${ADDRS[0]}" >/dev/null 2>&1; then
	echo "  ns-b -> ns-a via ns-c   OK"
else
	echo "  ns-b -> ns-a via ns-c   FAIL"
	fail=1
fi
ip -netns ns-a link set ab up

echo
echo "== reclose: all pairs again =="
sleep 2
for i in 0 1 2; do
	src_ns="${NETNS[$i]}"
	for j in 0 1 2; do
		[ "$i" = "$j" ] && continue
		dst="${ADDRS[$j]}"
		ip netns exec "$src_ns" ping -c2 -W2 -q "$dst" >/dev/null 2>&1 \
			|| { echo "  reclose FAIL: $src_ns -> $dst"; fail=1; }
	done
done
[ "$fail" = 0 ] && echo "  all pairs OK after reclose"

echo
echo "== dmesg (triade) =="
dmesg | grep -i triade | tail -10

echo
if [ "$fail" = 0 ]; then
	echo "PASS: ring forwards (closed), survives open, dedup + supervision verified."
else
	echo "FAIL: see above."
	exit 1
fi
