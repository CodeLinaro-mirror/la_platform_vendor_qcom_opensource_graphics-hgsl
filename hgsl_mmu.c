// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2002,2007-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/component.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

#include "hgsl.h"
#include "hgsl_mmu.h"

void hgsl_mmu_putpagetable(struct hgsl_pagetable *pagetable)
{
	if (!IS_ERR_OR_NULL(pagetable))
		kref_put(&pagetable->refcount, hgsl_destroy_pagetable);
}

inline u64
hgsl_mmu_pagetable_get_ttbr0(struct hgsl_pagetable *pagetable)
{
	if (PT_OP_VALID(pagetable, get_ttbr0))
		return pagetable->pt_ops->get_ttbr0(pagetable);

	return 0;
}

/*
 * A macro for memory statistics - add the new size to the stat and if
 * the statisic is greater than _max, set _max
 */
static inline void HGSL_STATS_ADD(uint64_t size, atomic_long_t *stat,
		atomic_long_t *max)
{
	uint64_t ret = atomic_long_add_return(size, stat);

	if (ret > atomic_long_read(max))
		atomic_long_set(max, ret);
}

int hgsl_mmu_start(struct hgsl_mmu *mmu)
{

if (MMU_OP_VALID(mmu, mmu_start))
	return mmu->mmu_ops->mmu_start(mmu);

return 0;
}

enum hgsl_mmutype hgsl_mmu_get_mmutype(struct qcom_hgsl *hgsl_dev)
{
	return hgsl_dev ? hgsl_dev->mmu.type : HGSL_MMU_TYPE_NONE;
}

int
hgsl_mmu_map(struct qcom_hgsl *hgsl_dev, struct hgsl_pagetable *pagetable,
		struct hgsl_mem_node *memnode, bool internal, u32 dev_hnd)
{
	int size;
	int ret = 0;
	int dev_id = hgsl_hnd2id(dev_hnd);

	if (dev_id >= HGSL_DEVICE_NUM) {
		LOGE("Invalid dev handle %u", dev_hnd);
		ret = -EINVAL;
		goto exit;
	}

	if (!memnode) {
		LOGE("invalid mem node");
		ret = -EINVAL;
		goto exit;
	}

	if (memnode->memdesc.gpuaddr != 0) {
		LOGE("The GPU address should be 0 for mapping");
		ret = -EINVAL;
		goto exit;
	}

	if (hgsl_dev->use_single_pt) {
		ret = hgsl_iommu_map_using_single_pt(memnode,
				&(hgsl_dev->mmu.iommu.user_context[dev_id]), internal);
		if (ret) {
			LOGE("hgsl_iommu_map_single_pt_smmu_map() failed with ret=%d", ret);
			goto exit;
		}
	} else {
		size = hgsl_memnode_footprint(memnode);

		if (PT_OP_VALID(pagetable, mmu_map)) {
			ret = pagetable->pt_ops->mmu_map(pagetable, memnode, internal, dev_id);
			if (ret) {
				LOGE("mmu_map using per process pt failed with ret=%d", ret);
				goto exit;
			}
			atomic_inc(&pagetable->stats.entries);
		}
	}

	if (memnode->memdesc.gpuaddr == 0) {
		ret = -EINVAL;
		goto out;
	}

	memnode->mem_node_iommu_info.priv |= HGSL_MEMNODE_MAPPED;

out:
	if (ret && !hgsl_dev->use_single_pt && (PT_OP_VALID(pagetable, mmu_map)))
		atomic_dec(&pagetable->stats.entries);

exit:
	return ret;
}

int
hgsl_mmu_unmap(struct qcom_hgsl *hgsl_dev, struct hgsl_pagetable *pagetable,
				struct hgsl_mem_node *memnode, bool internal, u32 dev_hnd)
{
	int ret = 0;
	int dev_id = hgsl_hnd2id(dev_hnd);

	if (dev_id >= HGSL_DEVICE_NUM) {
		LOGE("Invalid dev handle %u", dev_hnd);
		ret = -EINVAL;
		goto out;
	}

	if (!memnode) {
		LOGW("invalid mem node");
		ret = -EINVAL;
		goto out;
	}

	if (memnode->memdesc.gpuaddr == 0) {
		LOGE("Invalid gpu address");
		ret = -EINVAL;
		goto out;
	}

	if (!(HGSL_MEMNODE_MAPPED & memnode->mem_node_iommu_info.priv)) {
		LOGE("Invalid request to unmap the buffer which is not mapped before");
		ret = -EINVAL;
		goto out;
	}

	if (hgsl_dev->use_single_pt) {
		ret = hgsl_iommu_unmap_using_single_pt(memnode, NULL, internal);
		if (ret)
			LOGE("hgsl_iommu_map_single_pt_smmu_unmap() failed ret=%d", ret);

		memnode->mem_node_iommu_info.priv &= ~HGSL_MEMNODE_MAPPED;
	} else {
		if (PT_OP_VALID(pagetable, mmu_unmap)) {
			uint64_t size;

			size = hgsl_memnode_footprint(memnode);

			ret = pagetable->pt_ops->mmu_unmap(pagetable, memnode, internal, dev_id);
			if (ret)
				goto out;

			atomic_dec(&pagetable->stats.entries);

			memnode->mem_node_iommu_info.priv &= ~HGSL_MEMNODE_MAPPED;
		}
	}

out:
	return ret;
}

bool hgsl_mmu_gpuaddr_in_range(struct hgsl_pagetable *pagetable,
		uint64_t gpuaddr, uint64_t size)
{
	if (PT_OP_VALID(pagetable, addr_in_range))
		return pagetable->pt_ops->addr_in_range(pagetable, gpuaddr, size);

	return false;
}

int hgsl_mmu_pagetable_get_asid(struct hgsl_pagetable *pagetable,
		struct hgsl_context *context)
{
	if (PT_OP_VALID(pagetable, get_asid))
		return pagetable->pt_ops->get_asid(pagetable, context);

	return -ENOENT;
}

void hgsl_mmu_pagetable_init(struct hgsl_mmu *mmu,
		struct hgsl_pagetable *pagetable, u32 name)
{
	kref_init(&pagetable->refcount);

	spin_lock_init(&pagetable->lock);
	//TO DO: Need to implement _deferred_destroy
	INIT_WORK(&pagetable->destroy_ws, _deferred_destroy);

	pagetable->mmu = mmu;
	pagetable->name = name;

	atomic_set(&pagetable->stats.entries, 0);
	atomic_long_set(&pagetable->stats.mapped, 0);
	atomic_long_set(&pagetable->stats.max_mapped, 0);
}

static int hgsl_mmu_bind(struct device *dev, struct device *master, void *data)
{
	struct qcom_hgsl *hgsl = data;
	struct platform_device *pdev = to_platform_device(dev);
	struct hgsl_mmu *mmu = NULL;
	int ret = 0;

	if (!hgsl) {
		dev_err(dev, "Invalid hgsl passed by master\n");
		ret = -EINVAL;
		goto out;
	}

	if (!hgsl->fv_on) {
		LOGI("FV is disabled so exit !!!");
		goto out;
	}

	mmu = &hgsl->mmu;
	/*
	 * Try to bind the IOMMU and if it doesn't exist for some reason
	 * go for the NOMMU option instead
	 */
	ret = hgsl_iommu_bind(pdev, hgsl);
	if (!ret || ret == -EPROBE_DEFER)
		return ret;

	mmu->type = HGSL_MMU_TYPE_NONE;
out:
	return ret;
}

static void hgsl_mmu_unbind(struct device *dev, struct device *master,
		void *data)
{
	struct qcom_hgsl *hgsl = data;
	struct hgsl_mmu *mmu = NULL;

	if (!hgsl) {
		dev_err(dev, "Invalid hgsl passed by master\n");
		return;
	}

	if (!hgsl->fv_on)
		return;

	mmu = &hgsl->mmu;

	if (MMU_OP_VALID(mmu, mmu_close))
		mmu->mmu_ops->mmu_close(mmu);
}

static const struct component_ops hgsl_mmu_component_ops = {
	.bind = hgsl_mmu_bind,
	.unbind = hgsl_mmu_unbind,
};

static int hgsl_mmu_cb_bind(struct device *dev, struct device *master, void *data)
{
	return 0;
}

static void hgsl_mmu_cb_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops hgsl_mmu_cb_component_ops = {
	.bind = hgsl_mmu_cb_bind,
	.unbind = hgsl_mmu_cb_unbind,
};

static int hgsl_mmu_dev_probe(struct platform_device *pdev)
{
	if (of_device_is_compatible(pdev->dev.of_node, "qcom,smmu-hgsl-cb"))
		return component_add(&pdev->dev, &hgsl_mmu_cb_component_ops);

	/* Fill out the rest of the devices in the node */
	of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);

	return component_add(&pdev->dev, &hgsl_mmu_component_ops);
}

static int hgsl_mmu_dev_remove(struct platform_device *pdev)
{
	if (of_device_is_compatible(pdev->dev.of_node, "qcom,smmu-hgsl-cb")) {
		component_del(&pdev->dev, &hgsl_mmu_cb_component_ops);
		return 0;
	}

	component_del(&pdev->dev, &hgsl_mmu_component_ops);

	of_platform_depopulate(&pdev->dev);
	return 0;
}

static const struct of_device_id mmu_match_table[] = {
	{ .compatible = "qcom,hgsl-smmu-v2" },
	{ .compatible = "qcom,smmu-hgsl-cb" },
	{},
};

static struct platform_driver hgsl_mmu_driver = {
	.probe = hgsl_mmu_dev_probe,
	.remove = hgsl_mmu_dev_remove,
	.driver = {
		.name = "hgsl-iommu",
		.of_match_table = mmu_match_table,
	}
};

int __init hgsl_mmu_init(void)
{
	return platform_driver_register(&hgsl_mmu_driver);
}

void hgsl_mmu_exit(void)
{
	platform_driver_unregister(&hgsl_mmu_driver);
}
