// SPDX-License-Identifier: GPL-2.0
/*
 * Triade ring driver - slave (ring port) management and RX entry.
 *
 * Each ring port is a slave netdev with a registered rx_handler. The handler
 * does the bare minimum (loopback bypass + share-check) and delegates the
 * tagless forwarding decision to triade_forward.c.
 */
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <net/rtnetlink.h>

#include "triade.h"

static rx_handler_result_t triade_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct triade_port *port;

	if (unlikely(skb->pkt_type == PACKET_LOOPBACK))
		return RX_HANDLER_PASS;

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (unlikely(!skb))
		return RX_HANDLER_CONSUMED;
	*pskb = skb;

	port = rcu_dereference(skb->dev->rx_handler_data);
	if (unlikely(!port))
		return RX_HANDLER_PASS;

	return triade_forward_rx(port, skb);
}

int triade_add_slave(struct net_device *dev, struct net_device *slave_dev,
		     struct netlink_ext_ack *extack)
{
	struct triade_priv *triade = netdev_priv(dev);
	struct triade_port *port;
	int idx = -1, i, err;

	if (slave_dev->type != ARPHRD_ETHER || (slave_dev->flags & IFF_LOOPBACK)) {
		NL_SET_ERR_MSG(extack, "triade: slave must be an Ethernet device");
		return -EINVAL;
	}
	if (netdev_master_upper_dev_get(slave_dev)) {
		NL_SET_ERR_MSG(extack, "triade: device is already enslaved");
		return -EBUSY;
	}
	if (netdev_is_rx_handler_busy(slave_dev)) {
		NL_SET_ERR_MSG(extack, "triade: device already has an rx_handler");
		return -EBUSY;
	}

	spin_lock(&triade->lock);
	if (triade->nports >= TRIADE_MAX_PORTS) {
		spin_unlock(&triade->lock);
		NL_SET_ERR_MSG(extack, "triade: ring node already has two ports");
		return -EBUSY;
	}
	for (i = 0; i < TRIADE_MAX_PORTS; i++) {
		if (!rcu_access_pointer(triade->port[i])) {
			idx = i;
			break;
		}
	}
	spin_unlock(&triade->lock);
	if (idx < 0)
		return -EBUSY;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;
	port->dev = slave_dev;
	port->triade = triade;
	port->index = idx;

	/* Promisc is needed because we relay frames addressed to *other* nodes,
	 * not just ourselves. BUT promisc alone caused a nasty regression: when
	 * frames addressed to the master MAC arrive on the slave whose own NIC
	 * MAC differs, some drivers (notably ixgbe) route them through the
	 * NIC's promisc-overflow path, which bypasses the normal RSS hash and
	 * stripes a single TCP flow's packets across all RX queues -> 30 %+
	 * out-of-order delivery -> TCP cwnd collapse. The fix is to ALSO add
	 * the master MAC as a secondary unicast filter (below): frames-for-me
	 * now match the uc filter and take the normal addressed-to-me RX path
	 * with correct RSS hashing; relay-bound frames still come in via
	 * promisc.
	 */
	err = dev_set_promiscuity(slave_dev, 1);
	if (err)
		goto err_free;

	err = netdev_rx_handler_register(slave_dev, triade_handle_frame, port);
	if (err)
		goto err_promisc;

	err = netdev_master_upper_dev_link(slave_dev, dev, NULL, NULL, extack);
	if (err)
		goto err_unreg;

	/* The node's identity is the MAC of its first port. */
	if (triade->nports == 0)
		eth_hw_addr_inherit(dev, slave_dev);

	/* Add the master MAC as a secondary unicast filter on the slave so the
	 * NIC's hardware MAC filter matches frames-for-me before promisc does -
	 * this is what fixes the ixgbe RSS-in-promisc reordering. Idempotent
	 * for the first slave (its own MAC already matches the master).
	 */
	err = dev_uc_add(slave_dev, dev->dev_addr);
	if (err)
		goto err_unlink;

	slave_dev->flags |= IFF_SLAVE;

	spin_lock(&triade->lock);
	rcu_assign_pointer(triade->port[idx], port);
	triade->nports++;
	spin_unlock(&triade->lock);

	netdev_info(dev, "added ring port %d: %s\n", idx, slave_dev->name);
	return 0;

err_unlink:
	netdev_upper_dev_unlink(slave_dev, dev);
err_unreg:
	netdev_rx_handler_unregister(slave_dev);
err_promisc:
	dev_set_promiscuity(slave_dev, -1);
err_free:
	kfree(port);
	return err;
}

/* Caller must hold RTNL. Must not hold triade->lock (this may sleep). */
static void triade_release_port(struct triade_priv *triade,
				struct triade_port *port)
{
	struct net_device *slave_dev = port->dev;

	dev_uc_del(slave_dev, triade->dev->dev_addr);
	netdev_rx_handler_unregister(slave_dev);
	netdev_upper_dev_unlink(slave_dev, triade->dev);
	dev_set_promiscuity(slave_dev, -1);
	slave_dev->flags &= ~IFF_SLAVE;
	kfree_rcu(port, rcu);
}

int triade_del_slave(struct net_device *dev, struct net_device *slave_dev)
{
	struct triade_priv *triade = netdev_priv(dev);
	struct triade_port *port = NULL;
	int i;

	spin_lock(&triade->lock);
	for (i = 0; i < TRIADE_MAX_PORTS; i++) {
		struct triade_port *p =
			rcu_dereference_protected(triade->port[i],
						  lockdep_is_held(&triade->lock));
		if (p && p->dev == slave_dev) {
			port = p;
			RCU_INIT_POINTER(triade->port[i], NULL);
			triade->nports--;
			break;
		}
	}
	spin_unlock(&triade->lock);

	if (!port)
		return -ENODEV;

	triade_release_port(triade, port);
	netdev_info(dev, "removed ring port: %s\n", slave_dev->name);
	return 0;
}

void triade_release_all_slaves(struct triade_priv *triade)
{
	int i;

	spin_lock(&triade->lock);
	for (i = 0; i < TRIADE_MAX_PORTS; i++) {
		struct triade_port *p =
			rcu_dereference_protected(triade->port[i],
						  lockdep_is_held(&triade->lock));
		if (!p)
			continue;
		RCU_INIT_POINTER(triade->port[i], NULL);
		triade->nports--;
		spin_unlock(&triade->lock);
		triade_release_port(triade, p);
		spin_lock(&triade->lock);
	}
	spin_unlock(&triade->lock);
}
