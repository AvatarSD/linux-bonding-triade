// SPDX-License-Identifier: GPL-2.0
/*
 * Triade XDP relay (M5).
 *
 * This program runs on each ring slave's RX path (native XDP on Intel NICs;
 * generic XDP on anything that doesn't speak it, including thunderbolt-net -
 * where the software fallback in the kernel module remains the real path).
 *
 * The relay does one thing: turn pass-through unicast into XDP_REDIRECT to
 * the peer port. Everything else goes XDP_PASS, so the kernel module owns
 * supervision, dedup, flood, local delivery and the local TX scheduler.
 *
 * Decision tree, per RX frame on ingress port P:
 *
 *   ether_type == TRIADE_ETH_P_CTRL .. PASS  (kernel: supervision / flood)
 *   src-MAC    == my node MAC ........ DROP  (self-discard; circled the ring)
 *   dst-MAC    == my node MAC ........ PASS  (local delivery)
 *   dst-MAC    is multicast .......... PASS  (kernel: dedup + flood relay)
 *   otherwise ........................ REDIRECT to cfg.peer_ifindex
 *
 * Userspace (tools/triade-xdp-load.sh) reads triade0's MAC and its two
 * slaves' ifindexes, then populates:
 *
 *   triade_config[port_ifindex] = { my MAC, peer ifindex }
 *   triade_redirect[peer_ifindex] = peer_ifindex
 *
 * No per-frame map update is needed for the v1 ring: pass-through always
 * goes to the opposite port. Kernel-side node-table sync for richer
 * decisions (e.g. multi-hop redirect) is deferred to M5.5 - see
 * docs/adr/0005-xdp-map-sync.md.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define TRIADE_ETH_P_CTRL 0x88B5

struct triade_xdp_cfg {
	__u8	mac[ETH_ALEN];
	__u8	_pad[2];
	__u32	peer_ifindex;
};

/* Per-ingress-port config: my node MAC + which peer to redirect to. */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);			/* ingress ifindex */
	__type(value, struct triade_xdp_cfg);
} triade_config SEC(".maps");

/* Redirect targets, keyed by ifindex (BPF_REDIRECT_MAP looks this up). */
struct {
	__uint(type, BPF_MAP_TYPE_DEVMAP_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, __u32);
} triade_redirect SEC(".maps");

static __always_inline int mac_eq(const __u8 *a, const __u8 *b)
{
	return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] &&
	       a[3] == b[3] && a[4] == b[4] && a[5] == b[5];
}

SEC("xdp")
int triade_xdp_relay(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;
	__u32 ifindex = ctx->ingress_ifindex;
	struct ethhdr *eth = data;
	struct triade_xdp_cfg *cfg;

	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	cfg = bpf_map_lookup_elem(&triade_config, &ifindex);
	if (!cfg)
		return XDP_PASS;	/* not a Triade-managed port */

	if (eth->h_proto == bpf_htons(TRIADE_ETH_P_CTRL))
		return XDP_PASS;

	if (mac_eq(eth->h_source, cfg->mac))
		return XDP_DROP;	/* self-discard */

	if (mac_eq(eth->h_dest, cfg->mac))
		return XDP_PASS;	/* deliver locally */

	if (eth->h_dest[0] & 1)
		return XDP_PASS;	/* multicast: kernel does dedup + relay */

	return bpf_redirect_map(&triade_redirect, cfg->peer_ifindex, 0);
}

char _license[] SEC("license") = "GPL";
