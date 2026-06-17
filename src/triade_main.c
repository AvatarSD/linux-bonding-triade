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
#include <linux/version.h>
#include <net/rtnetlink.h>

#include "triade.h"

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

	err = triade_framereg_init(triade);
	if (err)
		return err;

	err = register_netdevice(dev);
	if (err) {
		triade_framereg_destroy(triade);
		return err;
	}
	triade_debugfs_add(triade);
	return 0;
}

static void triade_dellink(struct net_device *dev, struct list_head *head)
{
	struct triade_priv *triade = netdev_priv(dev);

	triade_debugfs_remove(triade);
	triade_super_stop(triade);
	triade_release_all_slaves(triade);
	triade_framereg_destroy(triade);
	unregister_netdevice_queue(dev, head);
}

static struct rtnl_link_ops triade_link_ops __read_mostly = {
	.kind		= "triade",
	.priv_size	= sizeof(struct triade_priv),
	.setup		= triade_setup,
	.newlink	= triade_newlink,
	.dellink	= triade_dellink,
};

/* Slave-port link transitions. The forwarding plane checks carrier inline
 * (triade_port_usable) so this notifier just logs and counts; the supervision
 * timeout in the node table does the heavy lifting for non-local breaks.
 */
static int triade_netdev_event(struct notifier_block *nb,
			       unsigned long event, void *ptr)
{
	struct net_device *slave = netdev_notifier_info_to_dev(ptr);
	struct net_device *master;

	if (!slave)
		return NOTIFY_DONE;
	master = netdev_master_upper_dev_get(slave);
	if (!master || master->rtnl_link_ops != &triade_link_ops)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
		netdev_info(master, "slave %s: %s\n", slave->name,
			    netif_carrier_ok(slave) ? "carrier up" : "carrier down");
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
