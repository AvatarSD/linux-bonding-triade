#!/usr/bin/env bash
# ADR-0001 measurement gate: do we ever need an L2 resequencer?
#
# The Triade per-packet spillover path would, in principle, reorder packets of
# a single TCP flow across the two ring directions. ADR-0001 bets that the
# combination of short cables + TCP RACK absorbs that reordering "for free"
# and saves us from building (and upstreaming) an L2 resequencer.
#
# This script is the measurement that decides whether to honour or revoke
# that bet. It runs a single fat TCP flow from one node to a peer over the
# Triade ring, samples ss -ti during the run, and reports the key signal:
#
#     - retrans / segs_out       (loss rate as TCP sees it)
#     - reordering value         (RACK's reordering estimate)
#     - cwnd vs. expected BDP    (how much TCP could open)
#
# Decision rule (ADR-0001):
#
#     retrans rate < 0.1 % AND   reordering <= 8           -> keep tagless
#     retrans rate > 1.0 % OR    reordering > 16           -> build the
#                                                            resequencer
#     anything in between                                   -> re-measure
#                                                            with longer runs
#                                                            and/or larger MTU
#
# Inputs:
#   $1   destination IP on the triade0 of the peer (default 10.99.0.2)
#   $2   duration in seconds (default 30)
#   $3   sampling interval (default 0.5)
#
# Requires: iperf3 (both ends), ss, awk. Real signal only on real 10G NICs;
# the netns/veth lab is bandwidth-unlimited and won't stress the scheduler.

set -euo pipefail

DST="${1:-10.99.0.2}"
DUR="${2:-30}"
INTERVAL="${3:-0.5}"
PORT="${IPERF3_PORT:-5201}"

command -v iperf3 >/dev/null || { echo "iperf3 missing"; exit 1; }
command -v ss     >/dev/null || { echo "iproute2 ss missing"; exit 1; }

# Sanity: peer must be reachable and have iperf3 -s running.
if ! ping -c1 -W1 "$DST" >/dev/null; then
	echo "FAIL: $DST not reachable (is the ring up on both ends?)"
	exit 1
fi
if ! timeout 3 bash -c "</dev/tcp/$DST/$PORT" 2>/dev/null; then
	echo "FAIL: no listener at $DST:$PORT - start 'iperf3 -s' on the peer"
	exit 1
fi

echo "== ADR-0001 single-fat-flow gate =="
echo "    target:    $DST"
echo "    duration:  ${DUR}s"
echo "    iperf3 port:    $PORT"
echo

ssdump=$(mktemp /tmp/triade-gate-ss.XXXXXX)
iperfout=$(mktemp /tmp/triade-gate-iperf.XXXXXX)
cleanup() { rm -f "$ssdump" "$iperfout"; }
trap cleanup EXIT

# Start ss sampler in background, target the destination port.
(
	end=$(( $(date +%s) + DUR + 2 ))
	while [ "$(date +%s)" -lt "$end" ]; do
		ss -tin "( dst $DST and dport = :$PORT )" >> "$ssdump" 2>/dev/null || true
		echo "----" >> "$ssdump"
		sleep "$INTERVAL"
	done
) &
SSPID=$!

iperf3 -c "$DST" -p "$PORT" -t "$DUR" -J > "$iperfout" 2>&1 || true
wait "$SSPID" 2>/dev/null || true

# Extract throughput from iperf3 JSON output (last sum_received).
bps=$(awk '
	/"sum_received"/,/}/ {
		if ($1 ~ /"bits_per_second"/) {
			gsub(/[,:]/, " "); print $2; exit
		}
	}' "$iperfout")
mbps="(unknown)"
if [ -n "$bps" ]; then
	mbps=$(awk -v b="$bps" 'BEGIN { printf "%.0f", b/1e6 }')
fi

# Pull peak retrans and reordering from the ss dump.
peak_retrans=$(awk '
	/retrans:/ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^retrans:/) {
				split($i, a, /[:\/]/)
				if (a[3]+0 > best) best = a[3]+0
			}
		}
	}
	END { print best+0 }' "$ssdump")

peak_reorder=$(awk '
	/reordering/ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^reordering/) {
				split($i, a, ":")
				if (a[2]+0 > best) best = a[2]+0
			}
		}
	}
	END { print best+0 }' "$ssdump")

segs_out=$(awk '
	/segs_out:/ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^segs_out:/) {
				split($i, a, ":")
				if (a[2]+0 > best) best = a[2]+0
			}
		}
	}
	END { print best+0 }' "$ssdump")

echo "== results =="
printf "    throughput:    %s Mbit/s\n" "$mbps"
printf "    segs_out peak: %s\n" "$segs_out"
printf "    retrans peak:  %s\n" "$peak_retrans"
printf "    reordering:    %s\n" "$peak_reorder"

if [ "$segs_out" -gt 0 ]; then
	rate=$(awk -v r="$peak_retrans" -v s="$segs_out" \
		'BEGIN { printf "%.4f", (r*100.0)/s }')
	echo "    retrans rate:  ${rate} %"
else
	rate="n/a"
fi

echo
echo "== verdict =="
case "$rate" in
	n/a)
		echo "    INDETERMINATE - no segments captured; rerun longer."
		exit 2;;
esac

# Numeric decision.
verdict=$(awk -v rate="$rate" -v reord="$peak_reorder" 'BEGIN {
	if (rate+0 < 0.1 && reord+0 <= 8) {
		print "PASS: tagless data path holds. Keep ADR-0001."
	} else if (rate+0 > 1.0 || reord+0 > 16) {
		print "FAIL: reordering is hurting. Revoke ADR-0001 and build a resequencer."
	} else {
		print "RE-MEASURE: results are in the grey zone. Try longer runs / larger MTU / different qdisc."
	}
}')
echo "    $verdict"
