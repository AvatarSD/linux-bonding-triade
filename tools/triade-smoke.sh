#!/usr/bin/env bash
# Milestone 1 smoke test for the Triade ring driver.
#
# Loads triade.ko, creates triade0, enslaves two veth ports, brings the stack
# up, prints the topology, then tears everything down. Pure local loopback —
# no real ring yet (that needs Milestone 2 forwarding). Run as root:
#
#     sudo tools/triade-smoke.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
KO="$HERE/triade.ko"

cleanup() {
	set +e
	ip link del triade0           2>/dev/null
	ip link del veth-a            2>/dev/null   # deletes its peer veth-b too
	ip link del veth-c            2>/dev/null   # deletes its peer veth-d too
	rmmod triade                  2>/dev/null
}
trap cleanup EXIT

echo "== load module =="
[ -f "$KO" ] || { echo "build it first: make"; exit 1; }
insmod "$KO"
dmesg | tail -1 | grep -q "ring driver loaded" && echo "  loaded OK"

echo "== create veth ports (stand-ins for the two ring NICs) =="
ip link add veth-a type veth peer name veth-b
ip link add veth-c type veth peer name veth-d

echo "== create triade0 and enslave veth-a, veth-c =="
ip link add triade0 type triade
ip link set veth-a master triade0
ip link set veth-c master triade0

echo "== bring everything up + assign an address =="
ip addr add 10.99.0.1/24 dev triade0
ip link set veth-a up
ip link set veth-c up
ip link set triade0 up

echo "== topology =="
ip -d link show triade0
echo "--- slaves ---"
ip -d link show veth-a | sed -n '1,2p'
ip -d link show veth-c | sed -n '1,2p'

echo "== dmesg (triade) =="
dmesg | grep -i triade | tail -8

echo
echo "PASS: module loads, triade0 created, two ports enslaved, link up."
echo "(Teardown runs automatically on exit.)"
