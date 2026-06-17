// SPDX-License-Identifier: GPL-2.0
/*
 * Triade - supervision (M3).
 *
 * Every TRIADE_SUPER_INTERVAL_MS the master floods one supervision frame out
 * both ring ports. The frame carries the originator's MAC, a monotonic seq,
 * a ring_state byte and an 8-bit TTL that each relay increments. Receivers
 * learn (originator, port, hop_distance) and update last_seen.
 *
 * The supervision RX path is invoked by triade_forward_rx when it spots the
 * Triade control EtherType + type=SUPERVISION; this file just owns the timer,
 * the frame builder and the relay decision.
 */
#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>

#include "triade.h"
#include "triade_proto.h"

static const u8 triade_super_mac[ETH_ALEN] = TRIADE_SUPER_MAC;

static struct sk_buff *triade_build_super(struct triade_priv *triade, u16 seq)
{
	struct sk_buff *skb;
	struct ethhdr *eth;
	struct triade_ctrl_hdr *ctrl;
	struct triade_super_payload *payload;
	unsigned int len = TRIADE_SUPER_FRAME_LEN;

	skb = netdev_alloc_skb(triade->dev, len + NET_IP_ALIGN);
	if (!skb)
		return NULL;
	skb_reserve(skb, NET_IP_ALIGN);

	eth = skb_put(skb, ETH_HLEN);
	ether_addr_copy(eth->h_dest, triade_super_mac);
	ether_addr_copy(eth->h_source, triade->dev->dev_addr);
	eth->h_proto = htons(TRIADE_ETH_P_CTRL);

	ctrl = skb_put(skb, TRIADE_CTRL_HDR_LEN);
	ctrl->type = TRIADE_CTRL_SUPERVISION;
	ctrl->version = TRIADE_PROTO_VERSION;
	ctrl->ttl = 1;	/* counts the first hop (originator -> neighbour) */
	ctrl->_pad = 0;
	ctrl->seq = htons(seq);
	ctrl->inner_ethertype = 0;

	payload = skb_put(skb, TRIADE_SUPER_PAYLOAD_LEN);
	ether_addr_copy(payload->originator, triade->dev->dev_addr);
	payload->ring_state = 0;	/* TODO: derive from node table (M4) */
	payload->_pad = 0;

	skb_reset_mac_header(skb);
	skb->protocol = htons(TRIADE_ETH_P_CTRL);
	return skb;
}

static void triade_super_send(struct triade_priv *triade)
{
	struct triade_port *p0, *p1;
	u16 seq;

	rcu_read_lock();
	p0 = rcu_dereference(triade->port[0]);
	p1 = rcu_dereference(triade->port[1]);
	if (!p0 && !p1) {
		rcu_read_unlock();
		return;
	}
	seq = triade_framereg_next_super_seq(triade);

	if (p0 && netif_carrier_ok(p0->dev)) {
		struct sk_buff *skb = triade_build_super(triade, seq);

		if (skb) {
			skb->dev = p0->dev;
			dev_queue_xmit(skb);
		}
	}
	if (p1 && netif_carrier_ok(p1->dev)) {
		struct sk_buff *skb = triade_build_super(triade, seq);

		if (skb) {
			skb->dev = p1->dev;
			dev_queue_xmit(skb);
		}
	}
	rcu_read_unlock();
}

static void triade_super_work_fn(struct work_struct *w)
{
	struct delayed_work *dw = container_of(w, struct delayed_work, work);
	struct triade_priv *triade = container_of(dw, struct triade_priv,
						  super_work);

	if (!triade->running)
		return;

	triade_super_send(triade);
	triade_framereg_age(triade, TRIADE_NODE_TIMEOUT_MS);

	schedule_delayed_work(&triade->super_work,
			      msecs_to_jiffies(TRIADE_SUPER_INTERVAL_MS));
}

void triade_super_start(struct triade_priv *triade)
{
	if (triade->running)
		return;
	INIT_DELAYED_WORK(&triade->super_work, triade_super_work_fn);
	triade->running = true;
	/* First tick slightly delayed so the slaves' carriers have time to
	 * come up after a fresh ifconfig.
	 */
	schedule_delayed_work(&triade->super_work,
			      msecs_to_jiffies(TRIADE_SUPER_INTERVAL_MS / 2));
}

void triade_super_stop(struct triade_priv *triade)
{
	if (!triade->running)
		return;
	triade->running = false;
	cancel_delayed_work_sync(&triade->super_work);
}

/* Called from triade_forward_rx after it has parsed the control header and
 * confirmed type == SUPERVISION. Skb still has the control header + payload
 * sitting at skb->data (no L2 header — eth_type_trans pulled it).
 *
 * Returns RX_HANDLER_CONSUMED in all cases (supervision frames are never
 * delivered to the master's IP stack).
 */
rx_handler_result_t triade_super_rx(struct triade_port *port,
				    struct sk_buff *skb, u8 ttl, u16 seq,
				    const u8 *originator)
{
	struct triade_priv *triade = port->triade;
	struct triade_port *peer;
	bool fresh;

	fresh = triade_framereg_observe_super(triade, port->index, originator,
					      seq, ttl);
	if (!fresh) {
		atomic_long_inc(&triade->stats.rx_flood_dup);
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
	atomic_long_inc(&triade->stats.rx_super);

	/* Relay out the opposite port with TTL+1, unless that would loop back
	 * (peer missing or no carrier).
	 */
	peer = triade_peer_port(triade, port);
	if (!peer || !netif_carrier_ok(peer->dev)) {
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
	triade_relay_ctrl(peer, skb, ttl + 1);
	return RX_HANDLER_CONSUMED;
}
