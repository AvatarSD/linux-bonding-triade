# Triade — L2 Multipath Ring Driver for Linux

> This document reflects the architecture **after** the design grilling session.
> Canonical references: terminology in [CONTEXT.md](CONTEXT.md), decisions in
> [docs/adr/](docs/adr/). (An earlier draft of this file proposed a tag-based,
> resequencer-centric design — that has been overturned; see ADR-0001/0002.)

## Context

Connect 3+ hosts into a **ring**, each wired to its two neighbours by two NICs (or two
**Thunderbolt** cables). Traffic uses *both* ring directions to aggregate bandwidth
**without a switch**. A ring node's two ports face *different* neighbours (unlike a normal
bond), so nodes must **pass through** frames not addressed to them, stop frames circling
forever, and cope with the ring opening on link/node failure. Primary use: switchless
cluster interconnect (Ceph/Proxmox-style). Deliverable: an out-of-tree loadable module,
tested on Intel-NIC and Thunderbolt rings, structured for an eventual upstream RFC.

## The shape, in one paragraph

`triade0` is a virtual netdev over **exactly two slave ports** (a node always has two
neighbours). The node's identity is the MAC of its first port. **Unicast is tagless**:
forward by destination MAC (== my MAC → deliver and stop; else relay out the other port),
prevent loops by **source-MAC self-discard** (a frame whose source is my own MAC has come
full circle → drop). Direction is implicit in the ingress port. Because nothing is pushed
onto unicast frames, **NIC offloads and MTU are untouched**. Only *flood* frames
(broadcast/multicast/unknown-unicast) carry a small header — a 16-bit **flood-seq** inside
a dedicated **control EtherType** — so the open-ring both-direction flood can be deduped
by `(src-MAC, flood-seq)`. The same control EtherType carries **supervision frames** that
build each node's view of ring membership and per-direction hop distance. Bandwidth comes
from **adaptive spillover**: prefer the shortest-path direction, and when its link
saturates, send the overflow the other way.

## Key decisions (see ADRs)

- **No L2 resequencer in v1** ([ADR-0001](docs/adr/0001-no-l2-resequencer-bet-on-tcp-rack.md)):
  bet that short-cable LAN/Thunderbolt skew + TCP RACK absorb per-packet spillover
  reordering; a measured `iperf3`/`ss -ti` gate decides if one is ever built.
- **Tagless unicast data path** ([ADR-0002](docs/adr/0002-tagless-unicast-data-path.md)):
  dst-MAC forward + src-MAC self-discard; offloads + MTU preserved; flood-seq only on
  floods; a tag returns only at the resequencer or bridging milestone.
- **Dual-signal, per-node convergence** ([ADR-0003](docs/adr/0003-dual-signal-per-node-convergence.md)):
  `NETDEV_UP/DOWN` for local-port failure + supervision-timeout for breaks elsewhere; each
  node recomputes its own table; no consensus; hysteresis on reclose.
- **XDP relay + software fallback** ([ADR-0004](docs/adr/0004-xdp-relay-with-software-fallback.md)):
  software pass-through is the baseline and the only path on Thunderbolt (no native XDP);
  `XDP_REDIRECT` accelerates relay on Intel NICs; middle-node forwarding ceiling is a
  headline measurement.

## Scheduler — adaptive spillover

- *Per-flow spillover* (default): on shortest-path saturation (BQL/qdisc backlog crossing a
  watermark, hysteresis), move whole flows to the other direction. No reordering; needs ≥2
  flows to help.
- *Per-packet spillover* (opt-in): split one fat flow, metered by **arrival-time
  equalization** so both paths deliver in near-order — the only mode that accelerates a
  single flow, and the only one that reorders.
- Spillover is capped at the measured middle-node forwarding ceiling.

## File layout (mirrors net/hsr)

| File | Responsibility | Status |
|------|----------------|--------|
| `src/triade_main.c` | module init, `rtnl_link_ops` kind `triade` | M1 done |
| `src/triade.h` | structs (`triade_priv`, `triade_port`) | M1 done |
| `src/triade_device.c` | netdev_ops, `ndo_start_xmit`, setup | M1 done (xmit trivial) |
| `src/triade_slave.c` | enslave/release, rx_handler | M1 done (rx delivers, no relay) |
| `src/triade_forward.c` | tagless relay, self-discard, flood handling | M2 |
| `src/triade_framereg.c` | node table, hop distance, flood-seq dedup | M3 |
| `src/triade_super.c` | supervision frames, state machine | M3 |
| `src/triade_sched.c` | spillover, BQL watermarks, equalizer | M4 |
| `src/triade_xdp.c` + BPF | XDP relay + map sync | M5 |
| `src/triade_netlink.c` | tunables on the link kind | as needed |
| `src/triade_debugfs.c` | node table / state / ceiling / dedup stats | as needed |

## Milestones

1. **Skeleton** *(done)*: module + link kind + enslave 2 ports + bring-up; trivial
   xmit/rx.
2. **Tagless ring + ceiling**: dst-MAC forward + src-MAC self-discard; verify single
   traversal/no storms; **measure middle-node software forwarding ceiling**.
3. **Topology + state machine**: supervision, node table, hop distance, dual-signal
   convergence, open/closed transitions; state-aware flood + flood-seq dedup.
4. **Aggregation**: per-flow then per-packet spillover + equalizer; **run the ADR-0001
   gate**.
5. **XDP relay**: BPF-map `XDP_REDIRECT` on Intel + software fallback for Thunderbolt.
6. **Hardening**: kselftest, debugfs, upstream RFC. (Resequencer only if the gate fails;
   bridging phase B optional.)

## Build & test

- **Build:** `make` (Makefile forces native `gcc-13` / `ARCH=x86_64`, overriding the
  cross-compile env that leaks from the login profile). Produces `triade.ko`.
- **M1 smoke (root):** `sudo tools/triade-smoke.sh` — loads the module, creates `triade0`,
  enslaves two veth ports, brings the stack up, prints topology, tears down.
- **Dev harness (later):** `tools/triade-lab.sh` — N-node ring via netns + veth on one box
  (veth supports XDP).
- **Hardware:** Intel-NIC ring and Thunderbolt ring (each node 2 TB ports).
- **Perf:** middle-node forwarding ceiling; `iperf3` adjacent (toward 2×) and all-pairs;
  single fat flow + `ss -ti` = the ADR-0001 gate. Baselines: single link, `balance-rr`
  bond, FRR routed mesh.
