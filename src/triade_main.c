// SPDX-License-Identifier: GPL-2.0
/*
 * Triade ring driver - module init, rtnl_link plumbing, NETDEV notifier.
 *
 *   ip link add triade0 type triade
 *   ip link set eth1 master triade0
 *   ip link set eth2 master triade0
 *
 * The notifier surfaces slave link transitions to the log; the actual
 * scheduler reaction is M4 (the dual-signal state machine has the
 * supervision-timeout half wired up via triade_framereg_age).
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/percpu.h>
#include <linux/u64_stats_sync.h>
#include <linux/version.h>
#include <net/rtnetlink.h>

#include "triade.h"

u64 triade_stat_read(const struct triade_priv *triade, size_t off)
{
	u64 total = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		const struct triade_pcpu_stats *s =
			per_cpu_ptr(triade->stats, cpu);
		const u64_stats_t *counter = (const u64_stats_t *)
			((const u8 *)s + off);
		unsigned int start;
		u64 v;

		do {
			start = u64_stats_fetch_begin(&s->syncp);
			v = u64_stats_read(counter);
		} while (u64_stats_fetch_retry(&s->syncp, start));
		total += v;
	}
	return total;
}

/* rtnl_link_ops->newlink changed signature in 6.17: pre-6.17 takes the
 * (src_net, dev, tb, data, extack) tuple; 6.17+ wraps src_net/tb/data into
 * struct rtnl_newlink_params *. The body doesn't touch those args.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
static int triade_newlink(struct net_device *dev,
			  struct rtnl_newlink_params *params,
			  struct netlink_ext_ack *extack)
#else
static int triade_newlink(struct net *src_net, struct net_device *dev,
			  struct nlattr *tb[], struct nlattr *data[],
			  struct netlink_ext_ack *extack)
#endif
{
	struct triade_priv *triade = netdev_priv(dev);
	int err;

	triade->dev = dev;
	spin_lock_init(&triade->lock);
	triade->nports = 0;
	triade->running = false;

	triade->stats = alloc_percpu(struct triade_pcpu_stats);
	if (!triade->stats)
		return -ENOMEM;

	err = triade_framereg_init(triade);
	if (err)
		goto err_stats;

	err = register_netdevice(dev);
	if (err)
		goto err_framereg;
	triade_debugfs_add(triade);
	return 0;

err_framereg:
	triade_framereg_destroy(triade);
err_stats:
	free_percpu(triade->stats);
	triade->stats = NULL;
	return err;
}

static void triade_dellink(struct net_device *dev, struct list_head *head)
{
	/* All teardown lives in ndo_uninit (triade_uninit in triade_device.c)
	 * so it runs uniformly on both explicit `ip link del` and on netns
	 * destruction. Here we just request the unregister, which fires
	 * ndo_uninit synchronously.
	 */
	unregister_netdevice_queue(dev, head);
}

static struct rtnl_link_ops triade_link_ops __read_mostly = {
	.kind		= "triade",
	.priv_size	= sizeof(struct triade_priv),
	.setup		= triade_setup,
	.newlink	= triade_newlink,
	.dellink	= triade_dellink,
};

/* Dual-signal convergence (ADR-0003): local NETDEV_UP/DOWN is the fast half,
 * supervision timeout in the node table is the slow half for non-local breaks.
 * The TX path naturally avoids a port without carrier (triade_port_usable
 * gates pick), so the work here is to keep the framereg consistent with
 * carrier state instead of letting stale hop information mislead pref_port,
 * and to kick supervision so neighbours converge faster than 1Hz aging.
 */
static int triade_slave_port_idx(struct triade_priv *triade,
				 const struct net_device *slave)
{
	int i;

	rcu_read_lock();
	for (i = 0; i < TRIADE_MAX_PORTS; i++) {
		const struct triade_port *p = rcu_dereference(triade->port[i]);

		if (p && p->dev == slave) {
			rcu_read_unlock();
			return i;
		}
	}
	rcu_read_unlock();
	return -1;
}

static int triade_netdev_event(struct notifier_block *nb,
			       unsigned long event, void *ptr)
{
	struct net_device *slave = netdev_notifier_info_to_dev(ptr);
	struct net_device *master;
	struct triade_priv *triade;
	int port_idx;
	bool up;

	if (!slave)
		return NOTIFY_DONE;
	master = netdev_master_upper_dev_get(slave);
	if (!master || master->rtnl_link_ops != &triade_link_ops)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
		triade = netdev_priv(master);
		port_idx = triade_slave_port_idx(triade, slave);
		up = netif_carrier_ok(slave);
		netdev_info(master, "slave %s (port %d): carrier %s\n",
			    slave->name, port_idx, up ? "up" : "down");
		if (port_idx < 0)
			break;
		if (!up) {
			/* Forget the hops we learned via this port so the
			 * scheduler stops preferring it the instant carrier
			 * drops, instead of waiting for the 3 s aging timeout.
			 */
			triade_framereg_invalidate_port(triade,
							(u8)port_idx);
		} else {
			/* New carrier: nudge a supervision burst out so the
			 * neighbour learns the (port_idx, hop=1) entry without
			 * waiting up to one whole TRIADE_SUPER_INTERVAL_MS.
			 */
			triade_super_kick(triade);
		}
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block triade_notifier = {
	.notifier_call = triade_netdev_event,
};

static int __init triade_init(void)
{
	int err;

	triade_debugfs_module_init();

	err = register_netdevice_notifier(&triade_notifier);
	if (err) {
		pr_err("triade: failed to register netdev notifier: %d\n", err);
		goto err_debugfs;
	}
	err = rtnl_link_register(&triade_link_ops);
	if (err) {
		pr_err("triade: failed to register rtnl_link_ops: %d\n", err);
		goto err_notifier;
	}
	pr_info("triade: ring driver loaded\n");
	return 0;

err_notifier:
	unregister_netdevice_notifier(&triade_notifier);
err_debugfs:
	triade_debugfs_module_exit();
	return err;
}

static void __exit triade_exit(void)
{
	rtnl_link_unregister(&triade_link_ops);
	unregister_netdevice_notifier(&triade_notifier);
	triade_debugfs_module_exit();
	pr_info("triade: ring driver unloaded\n");
}

module_init(triade_init);
module_exit(triade_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Triade L2 multipath ring driver");
MODULE_ALIAS_RTNL_LINK("triade");
