# ADR-0006: Forward correctly when triade0 is bridged (learn behind-bridge local MACs)

## Status

Accepted (2026-07-01).

## Context

The M2 tagless data path ([ADR-0002](0002-tagless-unicast-data-path.md)) and the
XDP relay ([ADR-0004](0004-xdp-relay-with-software-fallback.md),
[ADR-0005](0005-xdp-map-sync.md)) both identify a node's own L2 identity by a
**single address — the `triade0` master MAC**:

- self-discard fires on `eth.src == master` (a frame I sent came back around);
- local delivery fires on `eth.dst == master`;
- everything else is pass-through, relayed to the peer port.

This is correct only when the node's L3 identity sits *directly* on `triade0`
(one MAC per node). A real deployment breaks that assumption: on the candle
Proxmox cluster `triade0` is **enslaved into a bridge (`vmbr0`)** so the ring
trunks every VLAN. The node's traffic then rides **behind-bridge MACs** — VM
taps, per-VLAN SVIs, the router's LAN ports — none of which equal the master
MAC. (Corosync happens to work only because its SVI MAC was manually pinned to
equal the `triade0` MAC.)

Under the single-MAC rules, every non-pinned VLAN is mis-forwarded:

- **Black-hole.** A unicast addressed to a behind-bridge local MAC has
  `dst != master`, so it is treated as pass-through and **relayed back onto the
  ring** instead of delivered up to the bridge. It never reaches the local port.
- **Loop.** A frame the node originated (`src` = a behind-bridge MAC, not the
  master) that circles the ring is **not** self-discarded, so it loops until a
  bridge drops it as its own source address
  (`vmbr0: received packet on triade0 with own address as source, vlan:90`).

Only frames the bridge *floods* (unknown-unicast, broadcast) get through,
because the M3 flood path dedups on `(originator, seq)` and delivers regardless
of MAC. The result is erratic, mostly-collapsed throughput for bridged VLANs
(measured: WAN download for hosts/VMs behind the ring pinned at ~20 Mbit while a
directly-attached node got line rate; ring self-tests: corosync VLAN 9.9 Gbit,
mgmt VLAN 0–5 Mbit with heavy loss).

## Decision

Learn the node's **local-MAC set** and treat "src/dst is a known local MAC"
exactly like "== master MAC".

1. **`triade_localreg.c`** — an RCU hash of local MACs, learned from the
   master's **TX egress** (`triade_xmit`). Every source MAC leaving `triade0`
   is local: the bridge never re-emits a frame out the port it arrived on, and
   the module's own relays go straight to the slave via `dev_queue_xmit`,
   bypassing `triade0`'s `ndo_start_xmit`. Lockless on the hot already-known
   path; entries age on the supervision cadence (`TRIADE_LOCAL_TIMEOUT_MS`,
   300 s, matching the bridge FDB default). Bounded at `TRIADE_LOCAL_MAX`.
2. **`triade_forward.c`** — self-discard on `src == master || is_local(src)`;
   deliver up on `dst == master || is_local(dst)`. Local-but-not-master
   delivery keeps `pkt_type = PACKET_OTHERHOST` (set by `eth_type_trans`) so the
   bridge above `triade0` forwards it to the right port rather than the frame
   being stolen by the master's IP stack.
3. **`bpf/triade_xdp.bpf.c`** — the XDP fast path cannot see the kernel's
   learned local set without the deferred kernel→map sync (ADR-0005, M5.5), so
   its `dst == master`-only redirect is unsafe under bridging. Until the sync
   lands, XDP **defers every unicast to the kernel** (`XDP_PASS`), keeping only
   the two MAC-invariant fast decisions (CTRL → PASS, `src == master` → DROP).
   The `triade_redirect` map + `cfg.peer_ifindex` stay populated by the loader
   so the redirect can be re-enabled unchanged once XDP is fed the local set.

## Consequences

- **Correctness first.** All ring unicast now flows through the software
  forwarding plane, which already sustained line rate for local delivery
  (corosync). Pure *transit* frames (3-node ring: A↔C via B) lose XDP
  acceleration and take the software relay; in a hub-and-spoke bridged
  deployment transit is rare, so the practical cost is negligible and the
  black-hole/loop is eliminated.
- **Observability.** `debugfs .../locals` lists the learned set;
  `stats` gains `rx_local_deliver` (frames delivered to a behind-bridge MAC)
  and `local_count`.
- **Thunderbolt / non-XDP** nodes were always on the software path; they gain
  the fix for free and lose nothing.
- **Follow-up (M5.5).** When the kernel→map sync of ADR-0005 is built, extend
  it to mirror the local-MAC set into a BPF map and restore the XDP redirect:
  `PASS if dst is local, REDIRECT peer otherwise`. That reinstates hardware
  transit acceleration without reintroducing the single-MAC assumption.

## Revisit when

- The XDP redirect is re-enabled via the M5.5 local-MAC map sync.
- A node legitimately needs to deliver to a local MAC that never transmits
  (receive-only) for longer than the aging window — then seed the set from the
  bridge FDB instead of (or in addition to) TX learning.
