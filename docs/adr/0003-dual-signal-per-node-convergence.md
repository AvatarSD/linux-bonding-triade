# Ring topology: dual-signal, per-node convergence (no consensus)

A ring with loops invites an STP/consensus-style protocol where all nodes agree on one
global topology. We rejected that for v1.

We detect ring state with **two signals**: local `NETDEV_UP/DOWN` for instant detection
of *this node's own* port failing, and **supervision-frame timeout** as the authority for
breaks anywhere else — necessary because a chain-interior node still sees both its own
links UP when the ring is broken elsewhere. Each node then independently recomputes its
own node table from the supervision frames it still receives. There is **no global
consensus**; the system is eventually consistent, like a routing protocol.

We accept the trade-off: **brief transient loss during convergence** (a node may briefly
route toward a now-dead direction until its supervision timeout fires; TCP recovers) and
detection latency bounded by the supervision interval. In exchange we avoid a heavyweight
agreement protocol, its slower convergence, and a large amount of state and code that
would be hard to get right and to upstream. Reclose uses **hysteresis** so a flapping link
does not thrash routing.

This is surprising to a reader expecting STP-like loop management in a ring, hence the
record.
