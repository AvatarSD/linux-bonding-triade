# Triade — operator deployment notes

Reference configs for running a triade ring on bare metal (validated on a
3-node Proxmox cluster, kernels 6.8 and 6.17, ixgbe + bnxt_en NICs).

## Files

| File | Destination | Purpose |
|---|---|---|
| `interfaces.snippet` | append to `/etc/network/interfaces` (before `source /etc/network/interfaces.d/*`) | Brings up `triade0` + `triade0.1000` on boot, also re-attaches XDP via `post-up` |
| `corosync.conf.example` | `/etc/pve/corosync.conf` (with `config_version` bumped) | Puts Proxmox cluster sync on the triade ring via VLAN 1000 |
| `triade-xdp-load.sh` | `/usr/local/sbin/triade-xdp-load.sh` (`chmod 0755`) | System loader called from `post-up`. Looks for the BPF object at `/usr/local/share/triade/triade_xdp.bpf.o` |
| `bpf/triade_xdp.bpf.o` (from `make -C bpf`) | `/usr/local/share/triade/triade_xdp.bpf.o` | The XDP relay program |
| `triade.ko` (from `make`) | `/lib/modules/$(uname -r)/extra/triade.ko` + `depmod -a` | The kernel module itself |

## Bring-up order on a fresh node

```
make                                                            # builds triade.ko
make -C bpf                                                     # builds triade_xdp.bpf.o
install -m 0644 triade.ko /lib/modules/$(uname -r)/extra/
install -m 0644 bpf/triade_xdp.bpf.o /usr/local/share/triade/
install -m 0755 deploy/triade-xdp-load.sh /usr/local/sbin/
depmod -a
# substitute $SLAVE0, $SLAVE1, $TRIADE_IP, $CLUSTER_IP into interfaces.snippet,
# append to /etc/network/interfaces
ifreload -a
```

After that, both `triade0` and `triade0.1000` should be visible in the PVE web
UI under Datacenter > Node > Network, with the matching IPs.

## Reboot survival

Everything in the snippet survives reboot:

- Module loads via `pre-up modprobe triade` (also: stg/think can keep their
  `/etc/modules-load.d/triade.conf` for early-boot load).
- Master + slaves come up via the `pre-up` chain.
- Cluster VLAN child `triade0.1000` comes up automatically.
- **XDP re-attaches via `post-up`** — this was the missing piece before
  `deploy/triade-xdp-load.sh` existed.

### bnxt_en hosts (Broadcom)

The `pre-up sleep 2` is mandatory on bnxt_en hosts. The bnxt_re RoCE driver
attempts to register InfiniBand GIDs on the slaves during early boot and
races our slave enslavement (`netdev_rx_handler_register`), which previously
panicked the kernel when triade was auto-loaded via `modules-load.d`. The
late-load-plus-settle workaround in the snippet avoids the panic; the real
fix is on the upstream punchlist (defer rx_handler register until bnxt_re
is past its initial GID phase).

### ixgbe hosts (Intel X520/X540/X550)

The `pre-up ethtool -L $SLAVE1 combined 1` is recommended. ixgbe in
promiscuous mode has its own RSS quirks; pinning slave_b to one RX queue
keeps relay packets on a known CPU. bnxt_en doesn't need this.

## Migration from a mgmt-LAN cluster ring

If you're already on a Proxmox cluster with corosync on a normal mgmt-LAN
VLAN (`vmbrN.1000` with 10.10.10.x), the swap to triade is two ifreload
cycles:

1. Bring up `triade0` + `triade0.1000` alongside the existing `vmbrN.1000`,
   using a *different* temporary IP on `triade0` (e.g. 10.99.0.x). Verify
   mesh ping.
2. Move the cluster IP from `vmbrN.1000` to `triade0.1000` (one stanza
   swap + `ifreload -a` on each node). Corosync is still on its old
   address during this step.
3. Bump `corosync.conf`'s `config_version` (no address change needed if
   you re-used the same IP). Restart corosync on all nodes.

The triade0 untagged IP (10.99.0.x) can stay as a debugging / smoke-test
address or be removed once you trust the ring.
