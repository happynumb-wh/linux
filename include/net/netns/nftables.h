/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NETNS_NFTABLES_H_
#define _NETNS_NFTABLES_H_
#include <linux/ck_kabi.h>

struct netns_nftables {
	unsigned int		base_seq;
	u8			gencursor;

	CK_KABI_RESERVE(1)
};

#endif
