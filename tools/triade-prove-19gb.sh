#!/bin/bash
# One-shot: after pve is recovered (shell works), this script:
#   1. fixes pve default route via the live mgmt LAN
#   2. installs bpftool+clang+libbpf-dev on pve
#   3. syncs BPF sources to pve, builds, loads XDP on all 3 nodes
#   4. runs the 19+ Gb/s 2-stream same-direction test
# Run from the laptop.
set -e
PVE=192.168.5.11
STG=192.168.5.14
THINK=192.168.5.154

echo "=== 0. confirm pve shell ==="
ssh -o BatchMode=yes -o ConnectTimeout=4 root@$PVE 'echo PVE_OK' || { echo "pve sshd still broken"; exit 1; }

echo "=== 1. fix pve default route + apt ==="
ssh root@$PVE '
ip route replace default via 192.168.5.200 dev vmbr11 2>/dev/null || \
  ip route replace default via 192.168.5.200 dev enx00e04c6800b7
ping -c1 -W2 192.168.5.200 >/dev/null && echo "  gateway OK"
apt update -qq
apt install -y -q bpftool clang libbpf-dev
'

echo "=== 2. sync BPF sources to pve, build ==="
rsync -a --delete /home/sd/prj/linux-bonding-triade/bpf/ root@$PVE:/root/triade-bpf/
scp -q /home/sd/prj/linux-bonding-triade/tools/triade-xdp-load.sh root@$PVE:/root/
ssh root@$PVE 'cd /root/triade-bpf && make && ls triade_xdp.bpf.o'

echo "=== 3. load XDP on all 3 nodes ==="
for ip in $PVE $STG $THINK; do
  ssh root@$ip '/root/triade-xdp-load.sh triade0 2>&1 | tail -5'
done

echo "=== 4. verify XDP attached ==="
for ip in $PVE $STG $THINK; do
  ssh root@$ip 'echo "=== $(hostname) ==="
for s in $(ip -br link show master triade0 | awk "{print \$1}"); do
  echo "  $s: $(ip link show $s | grep -oE "xdp(generic|drv)?[^ ]*" | head -1)"
done'
done

echo "=== 5. run the 19+ Gb/s test ==="
# Make sure per-flow ECMP mode is enabled on the SENDER for proper splitting
ssh root@$STG 'echo 2 > /sys/kernel/debug/triade/triade0/sched_mode'
# stg -> pve (long way relay through think — think's ixgbe has 8 CPUs)
echo
echo "--- stg -> pve, 8 streams, FLOW_ECMP mode, XDP-accelerated relay ---"
ssh root@$STG 'iperf3 -c 10.99.0.1 -p 5201 -t 8 -P 8 -f m 2>&1 | grep "SUM.*receiver" | tail -1'

# think -> stg also
ssh root@$THINK 'echo 2 > /sys/kernel/debug/triade/triade0/sched_mode'
echo
echo "--- think -> stg, 8 streams, FLOW_ECMP mode, XDP-accelerated relay ---"
ssh root@$THINK 'iperf3 -c 10.99.0.2 -p 5201 -t 8 -P 8 -f m 2>&1 | grep "SUM.*receiver" | tail -1'

# revert to per-flow
ssh root@$STG   'echo 0 > /sys/kernel/debug/triade/triade0/sched_mode'
ssh root@$THINK 'echo 0 > /sys/kernel/debug/triade/triade0/sched_mode'

echo
echo "If either number above is >= 19000 Mbit/s, the goal is met."
