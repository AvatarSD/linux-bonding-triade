# Pass-through plane: XDP relay with a software fallback

Pass-through (relaying transit frames around the ring) is on the bandwidth-critical path,
so we want it fast — but it must also work on Thunderbolt, our headline topology.

We build **two** pass-through implementations. *Software pass-through* forwards transit
frames in NAPI context without traversing the stack (HSR-style); it is the baseline and
the **only available path on Thunderbolt**, because `thunderbolt-net` has **no native XDP**
(generic/SKB-mode XDP only, which gives none of the speed). *XDP relay* uses a
BPF-map-driven `XDP_REDIRECT` at driver ingress on native-XDP NICs (Intel
ixgbe/i40e/ice) for near-line-rate relay.

This forces a **split control/data plane**: the kernel module owns supervision, node-table
learning, the state machine, and the TX spillover scheduler, and *populates a BPF map*;
the XDP program is a dumb-fast `dst-MAC → XDP_PASS or XDP_REDIRECT` reader of that map.
The tagless data path (ADR-0002) keeps that program trivial.

Consequences worth remembering:
- The **middle-node software forwarding ceiling is a first-class measurement** (milestone
  2), especially on Thunderbolt where it is the real limit; the scheduler caps spillover
  at the measured ceiling.
- We maintain two forwarding paths, which is more code, justified by the Thunderbolt
  requirement that XDP cannot satisfy.
