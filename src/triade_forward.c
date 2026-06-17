// SPDX-License-Identifier: GPL-2.0
/*
 * Triade ring driver - forwarding plane.
 *
 * Decisions for each frame seen on a ring port:
 *
 *   eth.src == my MAC ................ self-discard (frame circled the ring).
 *   eth.proto == TRIADE_ETH_P_CTRL ... control frame:
 *     - SUPERVISION: hand to triade_super_rx (dedup, learn hop, relay+1).
 *     - FLOOD:       dedup on (originator, flood-seq), unwrap for local
 *                    delivery, relay a clone with TTL+1.
 *   eth.dst is multicast ............. unwrapped flood (from a non-Triade
 *                                      source or pre-M3 traffic): deliver up,
 *                                      do NOT relay (no dedup info).
 *   eth.dst == my MAC ................ deliver up (PACKET_HOST).
 *   otherwise ........................ pass-through: relay out the peer port.
 *
 * Unicast is tagless so NIC offloads (GRO/GSO/checksum/RSS) and MTU stay
 * untouched (ADR-0002).
 */
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>

#include "triade.h"
#include "triade_proto.h"

struct triade_port *triade_peer_port(struct triade_priv *triade,
				     const struct triade_port *self)
{
	u8 peer_idx = self->index ^ 1;

	return rcu_dereference(triade->port[peer_idx]);
}

/* Relay an unwrapped unicast frame out @peer. Pushes L2 header back into
 * skb->data (rx_handler runs after eth_type_trans pulled it). Consumes skb.
 */
static void triade_relay_unicast(struct triade_port *peer, struct sk_buff *skb)
{
	skb->dev = peer->dev;
	skb_push(skb, ETH_HLEN);
	skb_reset_mac_header(skb);
	dev_queue_xmit(skb);
}

/* Re-emit a control-wrapped frame out @peer with the given TTL. skb->data
 * points at the control header (eth_type_trans pulled the L2 header). We
 * push the L2 header back, bump TTL in place, and forward. Consumes skb.
 *
 * Caller must ensure the skb is writable (call site clones if needed).
 */
void triade_relay_ctrl(struct triade_port *peer, struct sk_buff *skb, u8 ttl)
{
	struct triade_ctrl_hdr *ctrl;

	if (skb_ensure_writable(skb, TRIADE_CTRL_HDR_LEN)) {
		kfree_skb(skb);
		return;
	}
	ctrl = (struct triade_ctrl_hdr *)skb->data;
	ctrl->ttl = ttl;

	skb->dev = peer->dev;
	skb_push(skb, ETH_HLEN);
	skb_reset_mac_header(skb);
	dev_queue_xmit(skb);
}

/* In-place unwrap of a TRIADE_CTRL_FLOOD frame: remove the 8-byte control
 * header so the skb looks like the original user multicast frame. Caller
 * holds an exclusive reference to the skb (we make it writable).
 *
 * Layout before:  [L2 0x88B5][ctrl 8B][inner L3...]
 *                            ^skb->data
 * Layout after:   [L2 inner_eth][inner L3...]
 *                                ^skb->data
 */
static int triade_unwrap_flood(struct sk_buff *skb, __be16 inner_eth)
{
	unsigned char *mac;

	if (skb_ensure_writable(skb, TRIADE_CTRL_HDR_LEN))
		return -ENOMEM;

	mac = skb_mac_header(skb);
	/* Slide the L2 header forward by 8 bytes, overlapping the ctrl hdr. */
	memmove(mac + TRIADE_CTRL_HDR_LEN, mac, ETH_HLEN);
	/* Patch the relocated L2's ethertype back to the original. */
	*(__be16 *)(mac + TRIADE_CTRL_HDR_LEN + 2 * ETH_ALEN) = inner_eth;

	skb_pull(skb, TRIADE_CTRL_HDR_LEN);
	skb->mac_header += TRIADE_CTRL_HDR_LEN;
	/* __netif_receive_skb_core only calls skb_reset_network_header() once,
	 * before our rx_handler runs. After RX_HANDLER_ANOTHER re-enters at
	 * `another_round:` (below that reset), so the stale network_header
	 * would point 8 bytes behind the new skb->data - causing arp_hdr() and
	 * ip_hdr() to read the wrong bytes. Reset both here.
	 */
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	skb->protocol = inner_eth;
	return 0;
}

static rx_handler_result_t triade_handle_ctrl(struct triade_port *port,
					      struct sk_buff *skb)
{
	struct triade_priv *triade = port->triade;
	struct net_device *master = triade->dev;
	const struct ethhdr *eth = eth_hdr(skb);
	struct triade_ctrl_hdr ctrl_copy;
	struct triade_port *peer;
	struct sk_buff *clone;
	u8 originator[ETH_ALEN];
	u16 seq;
	u8 ttl;
	__be16 inner_eth;
	bool fresh;

	if (unlikely(!pskb_may_pull(skb, TRIADE_CTRL_HDR_LEN))) {
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
	memcpy(&ctrl_copy, skb->data, TRIADE_CTRL_HDR_LEN);

	if (ctrl_copy.version != TRIADE_PROTO_VERSION) {
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}

	ttl = ctrl_copy.ttl;
	seq = ntohs(ctrl_copy.seq);
	ether_addr_copy(originator, eth->h_source);

	switch (ctrl_copy.type) {
	case TRIADE_CTRL_SUPERVISION:
		return triade_super_rx(port, skb, ttl, seq, originator);

	case TRIADE_CTRL_FLOOD:
		fresh = triade_framereg_observe_flood(triade, originator, seq);
		if (!fresh) {
			atomic_long_inc(&triade->stats.rx_flood_dup);
			kfree_skb(skb);
			return RX_HANDLER_CONSUMED;
		}
		inner_eth = ctrl_copy.inner_ethertype;
		peer = triade_peer_port(triade, port);

		/* Clone for relay (with TTL+1), unwrap original for delivery. */
		if (peer && netif_carrier_ok(peer->dev)) {
			clone = skb_clone(skb, GFP_ATOMIC);
			if (clone) {
				triade_relay_ctrl(peer, clone, ttl + 1);
				atomic_long_inc(&triade->stats.rx_flood_relayed);
			}
		}
		if (triade_unwrap_flood(skb, inner_eth)) {
			kfree_skb(skb);
			return RX_HANDLER_CONSUMED;
		}
		dev_sw_netstats_rx_add(master, skb->len);
		skb->dev = master;
		return RX_HANDLER_ANOTHER;

	default:
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
}

rx_handler_result_t triade_forward_rx(struct triade_port *port,
				      struct sk_buff *skb)
{
	struct triade_priv *triade = port->triade;
	struct net_device *master = triade->dev;
	const struct ethhdr *eth = eth_hdr(skb);
	struct triade_port *peer;

	/* Self-discard: I originated this frame, it came back around the ring. */
	if (unlikely(ether_addr_equal(eth->h_source, master->dev_addr))) {
		atomic_long_inc(&triade->stats.rx_self_discard);
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}

	if (eth->h_proto == htons(TRIADE_ETH_P_CTRL))
		return triade_handle_ctrl(port, skb);

	peer = triade_peer_port(triade, port);

	/* Unwrapped multicast (foreign or pre-M3): deliver up only. Relaying
	 * without flood-seq would risk storms; M3 originates all flood through
	 * the wrapped path.
	 */
	if (is_multicast_ether_addr(eth->h_dest)) {
		dev_sw_netstats_rx_add(master, skb->len);
		skb->dev = master;
		return RX_HANDLER_ANOTHER;
	}

	/* Unicast for me: deliver up. */
	if (ether_addr_equal(eth->h_dest, master->dev_addr)) {
		skb->pkt_type = PACKET_HOST;
		dev_sw_netstats_rx_add(master, skb->len);
		skb->dev = master;
		return RX_HANDLER_ANOTHER;
	}

	/* Unicast not for me: pass-through to the opposite ring port. */
	if (!peer) {
		DEV_STATS_INC(master, rx_dropped);
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
	atomic_long_inc(&triade->stats.rx_relayed);
	triade_relay_unicast(peer, skb);
	return RX_HANDLER_CONSUMED;
}
