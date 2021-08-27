/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Samsung Electronics.
 *
 */

#ifndef __CPIF_NETRX_MNG_H__
#define __CPIF_NETRX_MNG_H__

#include "modem_prj.h"
#include "link_device_memory.h"
#include "cpif_vmapper.h"

struct cpif_addr_pair {
	u64	cp_addr; /* cp address */
	void	*ap_addr; /* ap virtual address */

	struct list_head	addr_item;

};

struct netrx_page {
	struct page	*page;
	bool		usable;
	int		offset;
};

struct cpif_netrx_mng {
	u64 num_packet;
	u64 max_packet_size;
	u64 total_buf_size;

	struct cpif_va_mapper *desc_map;
	struct cpif_va_mapper *data_map;

	struct netrx_page	**recycling_page_arr;
	struct netrx_page	*tmp_page;
	struct list_head	data_addr_list;
	u32			rpage_arr_idx;
	u32			rpage_arr_len;
	bool			using_tmp_alloc;
};

#if IS_ENABLED(CONFIG_EXYNOS_CPIF_IOMMU)
struct cpif_netrx_mng *cpif_create_netrx_mng(struct cpif_addr_pair *desc_addr_pair,
						u64 desc_size, u64 databuf_cp_pbase,
						u64 max_packet_size, u64 num_packet);
void cpif_exit_netrx_mng(struct cpif_netrx_mng *cm);
struct cpif_addr_pair *cpif_map_rx_buf(struct cpif_netrx_mng *cm,
					unsigned int skb_padding_size);
void *cpif_unmap_rx_buf(struct cpif_netrx_mng *cm,
			u64 cp_data_paddr, bool free);
#else
static inline struct cpif_netrx_mng *cpif_create_netrx_mng(
				struct cpif_addr_pair *desc_addr_pair,
				u64 desc_size, u64 databuf_cp_pbase,
				u64 max_packet_size, u64 num_packet) { return NULL; }
static inline void cpif_exit_netrx_mng(struct cpif_netrx_mng *cm) { return; }
static inline struct cpif_addr_pair *cpif_map_rx_buf(struct cpif_netrx_mng *cm,
				unsigned int skb_padding_size)
{ return NULL; }
static inline void *cpif_unmap_rx_buf(struct cpif_netrx_mng *cm,
			u64 cp_data_paddr, bool free) {return NULL; }
#endif
#endif /* __CPIF_NETRX_MNG_H__ */
