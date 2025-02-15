// SPDX-License-Identifier: GPL-2.0
/*
 * AMD CDX bus driver MSI support
 *
 * Copyright (C) 2022-2023, Advanced Micro Devices, Inc.
 */

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/msi.h>
#include <linux/cdx/cdx_bus.h>

#include "cdx.h"

#define REQ_ID_SHIFT	10

/*
 * Convert an msi_desc to a globally unique identifier.
 */
static irq_hw_number_t cdx_domain_calc_hwirq(struct cdx_device *dev,
					     struct msi_desc *desc)
{
	return ((irq_hw_number_t)dev->req_id << REQ_ID_SHIFT) | desc->msi_index;
}

static void cdx_msi_set_desc(msi_alloc_info_t *arg,
			     struct msi_desc *desc)
{
	arg->desc = desc;
	arg->hwirq = cdx_domain_calc_hwirq(to_cdx_device(desc->dev), desc);
}

static void cdx_msi_write_msg(struct irq_data *irq_data,
			      struct msi_msg *msg)
{
	struct msi_desc *msi_desc = irq_data_get_msi_desc(irq_data);
	struct cdx_device *cdx_dev = to_cdx_device(msi_desc->dev);
	struct cdx_controller *cdx = cdx_dev->cdx;
	struct cdx_device_config dev_config;
	int ret;

	msi_desc->msg = *msg;
	dev_config.msi.msi_index = msi_desc->msi_index;
	dev_config.msi.data = msi_desc->msg.data;
	dev_config.msi.addr = ((u64)(msi_desc->msg.address_hi) << 32) |
			      msi_desc->msg.address_lo;

	dev_config.type = CDX_DEV_MSI_CONF;
	ret = cdx->ops->dev_configure(cdx, cdx_dev->bus_num, cdx_dev->dev_num,
				      &dev_config);
	if (ret)
		dev_err(&cdx_dev->dev, "Write MSI failed to CDX controller\n");
}

int cdx_msi_domain_alloc_irqs(struct device *dev, unsigned int irq_count)
{
	int ret;

	ret = msi_setup_device_data(dev);
	if (ret)
		return ret;

	msi_lock_descs(dev);
	if (msi_first_desc(dev, MSI_DESC_ALL))
		ret = -EINVAL;
	msi_unlock_descs(dev);
	if (ret)
		return ret;

	ret = msi_domain_alloc_irqs(dev_get_msi_domain(dev), dev, irq_count);
	if (ret)
		dev_err(dev, "Failed to allocate IRQs\n");

	return ret;
}
EXPORT_SYMBOL_GPL(cdx_msi_domain_alloc_irqs);

int cdx_enable_msi(struct cdx_device *cdx_dev)
{
	struct cdx_controller *cdx = cdx_dev->cdx;
	struct cdx_device_config dev_config;
	int ret;

	dev_config.type = CDX_DEV_MSI_ENABLE;
	dev_config.msi_enable = true;
	ret = cdx->ops->dev_configure(cdx, cdx_dev->bus_num, cdx_dev->dev_num,
				      &dev_config);
	if (ret)
		dev_err(&cdx_dev->dev, "MSI enable failed\n");

	return ret;
}
EXPORT_SYMBOL_GPL(cdx_enable_msi);

void cdx_disable_msi(struct cdx_device *cdx_dev)
{
	struct cdx_controller *cdx = cdx_dev->cdx;
	struct cdx_device_config dev_config;
	int ret;

	dev_config.type = CDX_DEV_MSI_ENABLE;
	dev_config.msi_enable = false;
	ret = cdx->ops->dev_configure(cdx, cdx_dev->bus_num, cdx_dev->dev_num,
				      &dev_config);
	if (ret)
		dev_err(&cdx_dev->dev, "MSI disable failed\n");
}
EXPORT_SYMBOL_GPL(cdx_disable_msi);

static struct irq_chip cdx_msi_irq_chip = {
	.name			= "CDX-MSI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= msi_domain_set_affinity,
	.irq_write_msi_msg	= cdx_msi_write_msg
};

static int cdx_msi_prepare(struct irq_domain *msi_domain,
			   struct device *dev,
			   int nvec, msi_alloc_info_t *info)
{
	struct cdx_device *cdx_dev = to_cdx_device(dev);
	struct device *parent = dev->parent;
	struct msi_domain_info *msi_info;
	u32 dev_id = 0;
	int ret;

	/* Retrieve device ID from requestor ID using parent device */
	ret = of_map_id(parent->of_node, cdx_dev->req_id, "msi-map",
			"msi-map-mask",	NULL, &dev_id);
	if (ret) {
		dev_err(dev, "of_map_id failed for MSI: %d\n", ret);
		return ret;
	}

	/* Set the device Id to be passed to the GIC-ITS */
	info->scratchpad[0].ul = dev_id;

	msi_info = msi_get_domain_info(msi_domain->parent);

	return msi_info->ops->msi_prepare(msi_domain->parent, dev, nvec, info);
}

static struct msi_domain_ops cdx_msi_ops = {
	.msi_prepare	= cdx_msi_prepare,
	.set_desc	= cdx_msi_set_desc
};

static struct msi_domain_info cdx_msi_domain_info = {
	.ops	= &cdx_msi_ops,
	.chip	= &cdx_msi_irq_chip,
	.flags	= MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		  MSI_FLAG_ALLOC_SIMPLE_MSI_DESCS | MSI_FLAG_FREE_MSI_DESCS
};

struct irq_domain *cdx_msi_domain_init(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct fwnode_handle *fwnode_handle;
	struct irq_domain *cdx_msi_domain;
	struct device_node *parent_node;
	struct irq_domain *parent;

	fwnode_handle = of_node_to_fwnode(np);

	parent_node = of_parse_phandle(np, "msi-map", 1);
	if (!parent_node) {
		dev_err(dev, "msi-map not present on cdx controller\n");
		return NULL;
	}

	parent = irq_find_matching_fwnode(of_node_to_fwnode(parent_node),
					  DOMAIN_BUS_NEXUS);
	if (!parent || !msi_get_domain_info(parent)) {
		dev_err(dev, "unable to locate ITS domain\n");
		return NULL;
	}

	cdx_msi_domain = msi_create_irq_domain(fwnode_handle,
					       &cdx_msi_domain_info, parent);
	if (!cdx_msi_domain) {
		dev_err(dev, "unable to create CDX-MSI domain\n");
		return NULL;
	}

	dev_dbg(dev, "CDX-MSI domain created\n");

	return cdx_msi_domain;
}
EXPORT_SYMBOL_GPL(cdx_msi_domain_init);
