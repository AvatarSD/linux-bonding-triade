/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Triade - on-the-wire control format.
 *
 * Unicast data carries no Triade header at all (see ADR-0002). The control
 * EtherType wraps two kinds of frames only:
 *
 *   TRIADE_CTRL_SUPERVISION - per-second multicast that builds each node's
 *                             view of the ring (peers, hop distance).
 *   TRIADE_CTRL_FLOOD       - a user multicast/broadcast frame wrapped so the
 *                             receivers can dedup it on (originator, seq) and
 *                             know when to stop forwarding it.
 *
 * Both share the same 8-byte header (triade_ctrl_hdr) sitting immediately
 * after the outer Ethernet header. The outer eth.src is always the
 * originator's node MAC; the outer eth.dst is either the supervision group
 * (for SUPERVISION) or the original user dst (for FLOOD).
 */
#ifndef _TRIADE_PROTO_H
#define _TRIADE_PROTO_H

#include <linux/if_ether.h>
#include <linux/types.h>

/* IEEE 802 "Local Experimental EtherType 1" (0x88B5/0x88B6 reserved range). */
#define TRIADE_ETH_P_CTRL	0x88B5
#define TRIADE_PROTO_VERSION	1

/* Locally-administered multicast destination for supervision frames.
 * First octet 0x01 sets the multicast (LSB) and locally-administered (bit 1)
 * bits. The 0x15:4E:00:00:01 tail is Triade-specific; not coordinated with
 * any registry (a real upstream RFC would request one).
 */
#define TRIADE_SUPER_MAC { 0x01, 0x15, 0x4e, 0x00, 0x00, 0x01 }

enum triade_ctrl_type {
	TRIADE_CTRL_SUPERVISION	= 1,
	TRIADE_CTRL_FLOOD	= 2,
};

/* 8 bytes, immediately after the outer Ethernet header. */
struct triade_ctrl_hdr {
	__u8	type;			/* enum triade_ctrl_type */
	__u8	version;		/* TRIADE_PROTO_VERSION */
	__u8	ttl;			/* hops travelled (incremented per relay) */
	__u8	_pad;
	__be16	seq;			/* super-seq (SUPERVISION) or flood-seq */
	__be16	inner_ethertype;	/* original ethertype (FLOOD only; 0 for SUPER) */
} __packed;

/* Follows triade_ctrl_hdr when type == TRIADE_CTRL_SUPERVISION. */
struct triade_super_payload {
	__u8	originator[ETH_ALEN];
	__u8	ring_state;		/* 0=closed, 1=open (originator's view) */
	__u8	_pad;
} __packed;

#define TRIADE_CTRL_HDR_LEN		((unsigned int)sizeof(struct triade_ctrl_hdr))
#define TRIADE_SUPER_PAYLOAD_LEN	((unsigned int)sizeof(struct triade_super_payload))
#define TRIADE_SUPER_FRAME_LEN		(ETH_HLEN + TRIADE_CTRL_HDR_LEN + TRIADE_SUPER_PAYLOAD_LEN)

#endif /* _TRIADE_PROTO_H */
