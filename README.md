# Triade — L2 multipath ring driver for Linux

Connect 3+ hosts into a **ring**, each wired to its two neighbours by two
NICs (or two Thunderbolt cables). Triade aggregates bandwidth across **both
ring directions** without a switch, while tolerating one link/node failure.
Primary target: switchless cluster interconnect (Ceph/Proxmox-style).

Design: see [DESIGN.md](DESIGN.md) and [docs/adr/](docs/adr/). Terminology in
[CONTEXT.md](CONTEXT.md).

## Status

| Milestone | What it adds | Status |
|---|---|---|
| M1 | Skeleton: `triade` link kind, enslave 2 ports, trivial xmit/rx | done |
| M2 | Tagless ring: dst-MAC forward + src-MAC self-discard | done |
| M3 | Supervision frames, node table, flood-seq dedup | done |
| M4 | Per-flow spillover scheduler + ADR-0001 measurement gate | done |
| M5 | XDP relay (Intel NICs) + software fallback (Thunderbolt) | done |
| Hardening | debugfs counters, lab assertions, this README | done |

## Layout

```
src/                kernel module sources (C; out-of-tree build)
bpf/                XDP relay BPF program (separate build)
tools/              lab harness, smoke test, gate test, XDP loader
docs/adr/           architecture decision records (ADR-0001..0005)
CONTEXT.md          glossary of Triade-specific terms
DESIGN.md           one-page architecture overview
```

## Build

### Kernel module

Requires matching kernel headers (`apt install pve-headers-$(uname -r)` on
Proxmox; `linux-headers-$(uname -r)` on Debian/Ubuntu) and `gcc-13`.

```bash
make            # produces triade.ko
make clean
```

The Makefile pins `ARCH=x86_64`, `CROSS_COMPILE=` and `CC=gcc-13` to defeat
any cross-compile env that may leak from the login profile.

### XDP relay (optional, Intel NICs only)

```bash
apt install clang libbpf-dev linux-tools-common
make -C bpf     # produces bpf/triade_xdp.bpf.o
```

## Run

### Lab (3-node ring in netns, no hardware needed)

```bash
sudo tools/triade-lab.sh
```

Expected last line: `PASS: ring forwards (closed), survives open, dedup +
supervision verified.` The script asserts via debugfs that supervision is
running, dedup is firing, and at least one self-discard fired.

### Single-host smoke (M1 sanity)

```bash
sudo tools/triade-smoke.sh
```

### ADR-0001 single-fat-flow gate (real hardware)

On one Triade node:
```bash
iperf3 -s
```

On the other:
```bash
sudo tools/triade-gate.sh 10.99.0.2 30
```

Prints `PASS` (keep tagless), `FAIL` (build the L2 resequencer), or
`RE-MEASURE` per the ADR-0001 decision rule.

### XDP relay (on Intel-NIC nodes only)

After `triade0` is up with two slaves enslaved:

```bash
sudo tools/triade-xdp-load.sh triade0
# verify
ip link show <slave>              # 'xdp' or 'xdpgeneric' line
bpftool prog show                 # triade_xdp_relay
# unload
sudo tools/triade-xdp-unload.sh triade0
```

Thunderbolt nodes: skip the XDP step — `thunderbolt-net` has no native XDP,
so the kernel software path is the production path.

## Inspect

The module exposes one debugfs subtree per master under
`/sys/kernel/debug/triade/<ifname>/`:

```bash
cat /sys/kernel/debug/triade/triade0/stats
cat /sys/kernel/debug/triade/triade0/nodes
```

Counters explained:

| Counter | Meaning |
|---|---|
| `rx_relayed` | unicast not-for-me, sent to peer port (pass-through) |
| `rx_self_discard` | `eth.src == my MAC`, dropped (frame circled the ring) |
| `rx_flood_relayed` | flood frames cloned and forwarded |
| `rx_super` | supervision frames accepted (after dedup) |
| `rx_flood_dup` | flood/super dropped by sliding-window dedup |
| `tx_flood` | locally-originated multicast wrapped + sent |
| `tx_spilled` | unicast moved off the preferred port (M4 backpressure) |
| `node_aged_out` | nodes evicted from the table (dual-signal timeout) |

`nodes` lists each learned ring neighbour with per-direction hop distance
and `last_seen` age. In a healthy 3-node ring each node should list two
others with `hops=[1,-]` or `[-,1]` depending on which port saw them.

## Bring up the real ring (3 nodes, 2 NICs each)

1. Cable each node's two ring NICs neighbour-to-neighbour: A↔B, B↔C, C↔A.
2. On each node, ensure the two ring NICs are NOT in any bridge:
   `ip link set <nic> nomaster && ip addr flush dev <nic>`.
3. Create the master and enslave:
   ```bash
   ip link add triade0 type triade
   ip link set <nic1> up
   ip link set <nic2> up
   ip link set <nic1> master triade0
   ip link set <nic2> master triade0
   ip addr add 10.99.0.<n>/24 dev triade0
   ip link set triade0 up
   ```
4. `cat /sys/kernel/debug/triade/triade0/nodes` after ~3s should show the
   other two nodes. `ping` all-pairs.
5. (Intel) `sudo tools/triade-xdp-load.sh triade0` to enable the XDP fast
   path. (Thunderbolt) skip step 5.

Keep one separate 1G/100M Ethernet path as the out-of-band recovery channel
— Triade does **not** consume it; it just gives you a way to recover if
both ring directions break.

## Troubleshooting

- **Module fails to load:** kernel mismatch. Build with matching
  `pve-headers-$(uname -r)`. The three Proxmox nodes (pve / stg / think)
  currently run three different kernels and each needs its own build.
- **`ip link add ... type triade` says "Unknown device type":** module not
  loaded (`lsmod | grep triade`), or built against a different kernel than
  the running one (`modinfo triade.ko | grep vermagic`).
- **Ping fails between two nodes that share a link:** check
  `cat /sys/kernel/debug/triade/triade0/nodes` — if the peer isn't in the
  table after 3s, supervision isn't getting through. Likely the slave is
  in a bridge (`ip link show <slave>` — should not say `master vmbr0`) or
  multicast is being filtered.
- **`rx_self_discard` huge:** loop guard is firing more than expected — a
  third path exists somewhere (e.g. both ring NICs accidentally bridged to
  the same external switch). Triade's loop guard catches it but the
  pass-through path is wasting cycles.
- **No XDP after load:** `dmesg | tail` will show the verifier output. The
  `thunderbolt-net` driver has no native XDP — the loader will fall back to
  generic XDP which works but doesn't accelerate.

## Repo conventions

- `src/` files are compiled into one out-of-tree kernel module.
- ADRs in `docs/adr/` capture decisions that were settled in design grilling.
  If a decision is overturned, add a new ADR rather than editing the old one.
- Counters: file-local `static` for module-private state; `atomic_long_t`
  per-priv for anything debugfs needs to read.

License: GPL-2.0.
