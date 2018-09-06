/*
 * Microsemi RISCVPC Board Setup
 *
 * Copyright (c) 2018 Microsemi Corporation.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/swiotlb.h>

#include <linux/of_device.h>
#include <linux/of_platform.h>

static struct dma_map_ops riscvpc_dma_ops;
extern const struct dma_map_ops *dma_ops;

static int riscvpc_pcie_dma_supported(struct device *pdev, u64 mask)
{
	if (mask > DMA_BIT_MASK(32)) // not supported on RISCVPC
		return 0;

	return dma_riscv_ops.dma_supported(pdev, mask);
}

static int __init riscvpc_init_patch_dma_ops(void)
{
	struct device_node *node;

	node = of_find_node_by_path("/");
	if (node) {
		if (of_get_property(node, "limit_dma_to_low32b_addr", NULL)) {
			struct dma_map_ops const *srcOps =
				get_arch_dma_ops(NULL);

			memcpy(&riscvpc_dma_ops, srcOps,
				sizeof(riscvpc_dma_ops));
			riscvpc_dma_ops.dma_supported =
				riscvpc_pcie_dma_supported;

			dma_ops = &riscvpc_dma_ops;
		}
	}

	return 0;
}

core_initcall(riscvpc_init_patch_dma_ops);
