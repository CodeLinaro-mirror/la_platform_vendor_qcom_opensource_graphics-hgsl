// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2011-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/bitfield.h>
#include <linux/compat.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/qcom-iommu-util.h>
#include <linux/qcom-io-pgtable.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/regulator/consumer.h>
#include <soc/qcom/secure_buffer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>

#include "hgsl.h"
#include "hgsl_iommu.h"

#define HGSL_IOMMU_IDR1_OFFSET 0x24
#define IDR1_NUMPAGENDXB GENMASK(30, 28)
#define IDR1_PAGESIZE BIT(31)

#if IS_ENABLED(CONFIG_DEBUG_FV)
static atomic_t map_cnt = ATOMIC_INIT(0);
static atomic_t int_map_cnt = ATOMIC_INIT(0);
#endif

static const struct hgsl_mmu_ops hgsl_iommu_ops;
static const struct hgsl_mmu_pt_ops iopgtbl_pt_ops;

/* Zero page for non-secure VBOs */
static struct page *hgsl_vbo_zero_page;

/*
 * One page allocation for a guard region to protect against over-zealous
 * GPU pre-fetch
 */
static struct page *hgsl_guard_page;

static struct kmem_cache *addr_entry_cache;

/*
 * struct hgsl_iommu_addr_entry - entry in the hgsl_pagetable rbtree.
 * @base: starting virtual address of the entry
 * @size: size of the entry
 * @node: the rbtree node
 */
struct hgsl_iommu_addr_entry {
	uint64_t base;
	uint64_t size;
	struct rb_node node;
};

/* These are dummy TLB ops for the io-pgtable instances */
static void _tlb_flush_all(void *cookie)
{
}

static void _tlb_flush_walk(unsigned long iova, size_t size,
			size_t granule, void *cookie)
{
}

static void _tlb_add_page(struct iommu_iotlb_gather *gather,
			unsigned long iova, size_t granule, void *cookie)
{
}

static const struct iommu_flush_ops hgsl_iopgtbl_tlb_ops = {
	.tlb_flush_all = _tlb_flush_all,
	.tlb_flush_walk = _tlb_flush_walk,
	.tlb_add_page = _tlb_add_page,
};

static struct iommu_domain *to_iommu_domain(struct hgsl_iommu_context *context)
{
	return context->domain;
}

void hgsl_mmu_pagetable_add(struct hgsl_mmu *mmu, struct hgsl_pagetable *pagetable)
{
	unsigned long flags;
	struct qcom_hgsl *hgsl_dev = HGSL_MMU_DEVICE(mmu);

	spin_lock_irqsave(&hgsl_dev->ptlock, flags);
	list_add(&pagetable->list, &hgsl_dev->pagetable_list);
	spin_unlock_irqrestore(&hgsl_dev->ptlock, flags);

	/* To DO :Create the sysfs entries */
	//pagetable_add_sysfs_objects(pagetable);
}

void
hgsl_mmu_detach_pagetable(struct hgsl_pagetable *pagetable)
{
	unsigned long flags;
	struct qcom_hgsl *hgsl_dev = HGSL_MMU_DEVICE(pagetable->mmu);

	spin_lock_irqsave(&hgsl_dev->ptlock, flags);

	if (!list_empty(&pagetable->list))
		list_del_init(&pagetable->list);

	spin_unlock_irqrestore(&hgsl_dev->ptlock, flags);

	/* To DO :Remove the sysfs entries */
	// pagetable_remove_sysfs_objects(pagetable);
}

void hgsl_destroy_pagetable(struct kref *kref)
{
	struct hgsl_pagetable *pagetable = container_of(kref, struct hgsl_pagetable, refcount);
	struct qcom_hgsl *hgsl_dev = HGSL_MMU_DEVICE(pagetable->mmu);

	hgsl_mmu_detach_pagetable(pagetable);

	queue_work(hgsl_dev->release_wq, &(pagetable->destroy_ws));
}

void _deferred_destroy(struct work_struct *ws)
{
	struct hgsl_pagetable *pagetable = container_of(ws, struct hgsl_pagetable, destroy_ws);

	WARN_ON(!list_empty(&pagetable->list));

	pagetable->pt_ops->mmu_destroy_pagetable(pagetable);
}

#ifdef CONFIG_PER_PROCESS_PT
static int hgsl_iopgtbl_alloc(struct hgsl_iommu_context *ctx, struct hgsl_iommu_pt *pt)
{
	struct adreno_smmu_priv *adreno_smmu = dev_get_drvdata(&ctx->pdev->dev);
	const struct io_pgtable_cfg *cfg = NULL;

	/* Get the pagetable configuration from the domain */
	if (adreno_smmu->cookie)
		cfg = adreno_smmu->get_ttbr1_cfg(adreno_smmu->cookie);
	if (!cfg)
		return -ENODEV;

	pt->cfg = *cfg;
	/* The incoming cfg will have the TTBR1 quirk enabled */
	pt->cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	pt->cfg.tlb = &hgsl_iopgtbl_tlb_ops;

	pt->pgtbl_ops = alloc_io_pgtable_ops(ARM_64_LPAE_S1, &pt->cfg, NULL);

	if (!pt->pgtbl_ops)
		return -ENOMEM;

	/*
	 * If this is the first pagetable that we've allocated, send it back to
	 * the arm-smmu driver as a trigger to set up TTBR0
	 */
	if (atomic_inc_return(&ctx->pagetables) == 1) {
		/* Enable stall on iommu fault: */
		/* TO DO: adreno_smmu->set_stall(adreno_smmu->cookie, true); */
		adreno_smmu->set_ttbr0_cfg(adreno_smmu->cookie, &pt->cfg);
	}

	pt->ttbr0 = pt->cfg.arm_lpae_s1_cfg.ttbr;

	return 0;
}

static struct hgsl_pagetable *hgsl_iopgtbl_pagetable(struct hgsl_mmu *mmu,
	u32 name, uint32_t iommu_mask)
{
	struct qcom_hgsl *hgsl_dev = HGSL_MMU_DEVICE(mmu);
	struct hgsl_iommu *iommu = &mmu->iommu;
	struct hgsl_iommu_pt *new_pt = NULL;
	int ret;

	new_pt = kzalloc(sizeof(*new_pt), GFP_KERNEL);
	if (!new_pt)
		return ERR_PTR(-ENOMEM);

	hgsl_mmu_pagetable_init(mmu, &new_pt->base, name);

	new_pt->base.fault_addr = U64_MAX;
	new_pt->base.rbtree = RB_ROOT;
	new_pt->base.pt_ops = &iopgtbl_pt_ops;

	/* TO DO: Need to check if compat_va_ * and svm_ * start and end needs to be assigned */
	//pt->base.compat_va_start = HGSL_IOMMU_SVM_BASE32(mmu);
	//pt->base.compat_va_end = HGSL_IOMMU_SVM_END32(mmu);

	new_pt->base.va_start = hgsl_dev->va_start;
	new_pt->base.va_end = hgsl_dev->va_end;
	DEBUG_HGSL_FV("per process table creation\n");
	LOGI("per process table creation for %u\n", name);

#ifdef CONFIG_PER_PROCESS_PT
	if (is_compat_task()) {
		pt->base.svm_start = HGSL_IOMMU_SVM_BASE32(mmu);
		pt->base.svm_end = HGSL_IOMMU_SVM_END32(mmu);
	} else {
		pt->base.svm_start = HGSL_IOMMU_SVM_BASE64;
		pt->base.svm_end = HGSL_IOMMU_SVM_END64;
	}


	/*
	 * We expect the 64-bit SVM and non-SVM ranges not to overlap so that
	 * va_hint points to VA space at the top of the 64-bit non-SVM range.
	 */
	BUILD_BUG_ON_MSG(!((HGSL_IOMMU_VA_BASE64 >= HGSL_IOMMU_SVM_END64) ||
		(HGSL_IOMMU_SVM_BASE64 >= HGSL_IOMMU_VA_END64)),
		"64-bit SVM and non-SVM ranges should not overlap");
#endif

	/* Set up the hint for 64-bit non-SVM VA on per-process pagetables */
	new_pt->base.va_hint = new_pt->base.va_start;

	/*
	 * For 2 GPU IOMMU, we will be creating single pagetable and get the TTBR0 for the pagetable
	 * and link it to the one of the IOMMU say GPU_0_IOMMU and if second GPU
	 * wants to access the same buffer then it can refer to the same pagetable which was linked
	 * to GPU_0_IOMMU earlier. CP pagetable switch command can use the same
	 * TTBR0(from first GPU IOMMU) to program the context bank register of other GPU IOMMUs.
	 * This is possible because stage2 pagetable will be same for both the GPUs.
	 * Right now iommu_mask is unused, when used we will create two pagetable
	 * for each of the GPU context bank.
	 */
	ret = hgsl_iopgtbl_alloc(&iommu->user_context[HGSL_GPU_0], new_pt);
	if (ret) {
		kfree(new_pt);
		return ERR_PTR(ret);
	}

	hgsl_mmu_pagetable_add(mmu, &new_pt->base);
	return &new_pt->base;
}
#else
static struct hgsl_pagetable *hgsl_iopgtbl_pagetable(struct hgsl_mmu *mmu,
	u32 name, uint32_t iommu_mask)
{
	return NULL;
}
#endif

struct hgsl_pagetable *hgsl_get_pagetable(struct qcom_hgsl *hgsl_dev, unsigned long name)
{
	struct hgsl_pagetable *pt, *found_pt = NULL;
	unsigned long flags;

	spin_lock_irqsave(&hgsl_dev->ptlock, flags);
	list_for_each_entry(pt, &hgsl_dev->pagetable_list, list) {
		if (name == pt->name && kref_get_unless_zero(&pt->refcount)) {
			found_pt = pt;
			break;
		}
	}

	spin_unlock_irqrestore(&hgsl_dev->ptlock, flags);

	if (!found_pt)
		LOGE("Unable to find the pagetable");

	return found_pt;
}

static struct hgsl_pagetable *hgsl_iommu_getpagetable(struct hgsl_mmu *mmu,
			unsigned long name, uint32_t iommu_mask)
{
	struct hgsl_pagetable *pt;
	struct qcom_hgsl *hgsl_dev = HGSL_MMU_DEVICE(mmu);

	/* If we already know the pagetable, return it */
	pt = hgsl_get_pagetable(hgsl_dev, name);
	if (pt)
		return pt;

	pt = hgsl_iopgtbl_pagetable(mmu, name, iommu_mask);

	return pt;
}

static void __iomem *hgsl_iommu_reg(struct hgsl_iommu_context *ctx,
			u32 offset)
{
	struct hgsl_iommu *iommu = HGSL_IOMMU(ctx->hgsldev);

	if (!iommu->cb0_offset) {
		u32 reg = readl_relaxed(ctx->regbase + HGSL_IOMMU_IDR1_OFFSET);

		iommu->pagesize = FIELD_GET(IDR1_PAGESIZE, reg) ? SZ_64K : SZ_4K;

		/*
		 * The number of pages in the global address space or
		 * translation bank address space is 2^(NUMPAGENDXB + 1).
		 */
		iommu->cb0_offset = iommu->pagesize *
				(1 << (FIELD_GET(IDR1_NUMPAGENDXB, reg) + 1));
	}
#if IS_ENABLED(CONFIG_DEBUG_FV)
	DEBUG_HGSL_FV("cb0_offset %u cb_num %u pagesize %u offset %u", iommu->cb0_offset,
			ctx->cb_num, iommu->pagesize, offset);
#endif

	return (void __iomem *) (ctx->regbase + iommu->cb0_offset +
			(ctx->cb_num * iommu->pagesize) + offset);
}

static void HGSL_IOMMU_SET_CTX_REG(struct hgsl_iommu_context *ctx, u32 offset,
			u32 val)
{
	void __iomem *addr = hgsl_iommu_reg(ctx, offset);

	writel_relaxed(val, addr);
	wmb(); // Ensure the write completes before any further memory operations
}

static u32 HGSL_IOMMU_GET_CTX_REG(struct hgsl_iommu_context *ctx, u32 offset)
{
	void __iomem *addr = hgsl_iommu_reg(ctx, offset);
	u32 val = readl_relaxed(addr);

	rmb(); // Ensure the read completes before any further reads

	return val;
}

static void hgsl_iommu_configure_gpu_sctlr(struct hgsl_mmu *mmu,
			unsigned long pf_policy,
			struct hgsl_iommu_context *ctx)
{
	u32 sctlr_val;

	/*
	 * If pagefault policy is GPUHALT_ENABLE,
	 *	 If terminate feature flag is enabled:
	 *	   1) Program CFCFG to 0 to terminate the faulting transaction
	 *	   2) Program HUPCF to 0 (terminate subsequent transactions
	 *		  in the presence of an outstanding fault)
	 *	 Else configure stall:
	 *	   1) Program CFCFG to 1 to enable STALL mode
	 *	   2) Program HUPCF to 0 (Stall subsequent
	 *		  transactions in the presence of an outstanding fault)
	 * else
	 * 1) Program CFCFG to 0 to disable STALL mode (0=Terminate)
	 * 2) Program HUPCF to 1 (Process subsequent transactions
	 *	  independently of any outstanding fault)
	 */

	if (ctx->mask <= 0 && !ctx->regbase)
		return;

	sctlr_val = HGSL_IOMMU_GET_CTX_REG(ctx, HGSL_IOMMU_CTX_SCTLR);
	if (test_bit(HGSL_FT_PAGEFAULT_GPUHALT_ENABLE, &pf_policy)) {
		if (test_bit(HGSL_MMU_PAGEFAULT_TERMINATE, &mmu->features)) {
			sctlr_val &= ~(0x1 << HGSL_IOMMU_SCTLR_CFCFG_SHIFT);
			sctlr_val &= ~(0x1 << HGSL_IOMMU_SCTLR_HUPCF_SHIFT);
		} else {
			sctlr_val |= (0x1 << HGSL_IOMMU_SCTLR_CFCFG_SHIFT);
			sctlr_val &= ~(0x1 << HGSL_IOMMU_SCTLR_HUPCF_SHIFT);
		}
	} else {
		sctlr_val &= ~(0x1 << HGSL_IOMMU_SCTLR_CFCFG_SHIFT);
		sctlr_val |= (0x1 << HGSL_IOMMU_SCTLR_HUPCF_SHIFT);
	}
	HGSL_IOMMU_SET_CTX_REG(ctx, HGSL_IOMMU_CTX_SCTLR, sctlr_val);
}

/* Program the PRR marker and enable it in the ACTLR register */
static void _iommu_context_set_prr(struct hgsl_mmu *mmu,
			struct hgsl_iommu_context *ctx)
{
	struct page *page = hgsl_vbo_zero_page;
	u32 val;

	if (ctx->mask <= 0 && !ctx->regbase)
		return;

	if (ctx->cb_num < 0)
		return;

	/* Quietly return if the context doesn't have a domain */
	if (!ctx->domain)
		return;

	if (!page)
		return;

	writel_relaxed(lower_32_bits(page_to_phys(page)),
			ctx->regbase + HGSL_IOMMU_PRR_CFG_LADDR);

	writel_relaxed(upper_32_bits(page_to_phys(page)),
			ctx->regbase + HGSL_IOMMU_PRR_CFG_UADDR);

	val = HGSL_IOMMU_GET_CTX_REG(ctx, HGSL_IOMMU_CTX_ACTLR);
	val |= FIELD_PREP(HGSL_IOMMU_ACTLR_PRR_ENABLE, 1);
	HGSL_IOMMU_SET_CTX_REG(ctx, HGSL_IOMMU_CTX_ACTLR, val);

	/* Make sure all of the preceding writes have posted */
	wmb();
}

static int hgsl_iommu_start(struct hgsl_mmu *mmu)
{
	struct hgsl_iommu *iommu = &mmu->iommu;

	hgsl_iommu_configure_gpu_sctlr(mmu, mmu->pfpolicy, &(iommu->user_context[HGSL_GPU_0]));

	hgsl_iommu_configure_gpu_sctlr(mmu, mmu->pfpolicy, &(iommu->user_context[HGSL_GPU_1]));

	_iommu_context_set_prr(mmu, &(iommu->user_context[HGSL_GPU_0]));

	_iommu_context_set_prr(mmu, &(iommu->user_context[HGSL_GPU_1]));

	return 0;
}

static void hgsl_iommu_detach_context(struct hgsl_iommu_context *context)
{
	if (!context->domain)
		return;

	iommu_detach_device(context->domain, &context->pdev->dev);
	iommu_domain_free(context->domain);

	context->domain = NULL;

	platform_device_put(context->pdev);

	context->pdev = NULL;
}

static void hgsl_iommu_close(struct hgsl_mmu *mmu)
{
	struct hgsl_iommu *iommu = &mmu->iommu;
	struct adreno_smmu_priv *adreno_smmu;

	if ((iommu->user_context[HGSL_GPU_0].domain)) {
		adreno_smmu = dev_get_drvdata(&(iommu->user_context[HGSL_GPU_0].pdev->dev));
		adreno_smmu->set_ttbr0_cfg(adreno_smmu->cookie, NULL);
		hgsl_iommu_detach_context(&(iommu->user_context[HGSL_GPU_0]));
	}

	if ((iommu->user_context[HGSL_GPU_1].domain)) {
		adreno_smmu = dev_get_drvdata(&(iommu->user_context[HGSL_GPU_1].pdev->dev));
		adreno_smmu->set_ttbr0_cfg(adreno_smmu->cookie, NULL);

		hgsl_iommu_detach_context(&(iommu->user_context[HGSL_GPU_1]));
	}

	if (hgsl_guard_page) {
		__free_page(hgsl_guard_page);
		hgsl_guard_page = NULL;
	}

	kmem_cache_destroy(addr_entry_cache);
	addr_entry_cache = NULL;
}

void hgsl_mmu_set_feature(struct qcom_hgsl *hgsl_dev,
		enum hgsl_mmu_feature feature)
{
	set_bit(feature, &hgsl_dev->mmu.features);
}

#ifdef CONFIG_PER_PROCESS_PT
int hgsl_set_smmu_aperture(struct qcom_hgsl *hgsl_dev,
		struct hgsl_iommu_context *context)
{
	int ret;

	if (!test_bit(HGSL_MMU_SMMU_APERTURE, &hgsl_dev->mmu.features))
		return 0;

	ret = qcom_scm_kgsl_set_smmu_aperture(context->cb_num);
	if (ret == -EBUSY)
		ret = qcom_scm_kgsl_set_smmu_aperture(context->cb_num);

	if (ret)
		/* The aperture needs to be set to use per-process pagetables */
		LOGE("Unable to set the SMMU aperture: ret %d", ret);

	return ret;
}


static void _enable_gpuhtw_llc(struct hgsl_mmu *mmu, struct iommu_domain *domain)
{
	if (!test_bit(HGSL_MMU_LLCC_ENABLE, &mmu->features))
		return;

	if (mmu->subtype == HGSL_IOMMU_SMMU_V500) {
		if (!test_bit(HGSL_MMU_IO_COHERENT, &mmu->features))
			iommu_set_pgtable_quirks(domain, IO_PGTABLE_QUIRK_QCOM_USE_LLC_NWA);
	} else
		iommu_set_pgtable_quirks(domain, IO_PGTABLE_QUIRK_ARM_OUTER_WBWA);
}

/* TO DO: Need to check if this is required for enabling per process PT */
static int hgsl_iommu_set_ttbr0(struct hgsl_iommu_context *context)
{
	struct hgsl_iommu_pt *iommu_pt = NULL;
	struct adreno_smmu_priv *adreno_smmu = dev_get_drvdata(&context->pdev->dev);
	const struct io_pgtable_cfg *cfg = NULL;
	int ret = 0;

	iommu_pt = kzalloc(sizeof(*iommu_pt), GFP_KERNEL);
	if (!iommu_pt)
		return -ENOMEM;

	if (adreno_smmu->cookie)
		cfg = adreno_smmu->get_ttbr1_cfg(adreno_smmu->cookie);

	if (!cfg)
		return -ENODEV;

	iommu_pt->cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	iommu_pt->cfg.tlb = &hgsl_iopgtbl_tlb_ops;
	iommu_pt->ttbr0 = cfg->arm_lpae_s1_cfg.ttbr;

	//TO DO: Need to enable clock before we call SMMU to setup register
	adreno_smmu->set_ttbr0_cfg(adreno_smmu->cookie, &iommu_pt->cfg);
	//TO DO: Need to disable clock before after we call SMMU to setup register

	kfree(iommu_pt);

	return ret;
}
#endif

static int hgsl_iommu_setup_context(struct hgsl_mmu *mmu,
		struct device_node *parent,
		struct hgsl_iommu_context *context, const char *name,
		iommu_fault_handler_t handler)
{
	struct device_node *node = of_find_node_by_name(parent, name);
	struct platform_device *pdev = NULL;
	struct qcom_hgsl *hgsl_dev = HGSL_MMU_DEVICE(mmu);
	int ret = 0;

	if (!node) {
		LOGE("node is NULL");
		return -ENOENT;
	}

	pdev = of_find_device_by_node(node);
	if (!pdev) {
		LOGE("of_find_device_by_node() failed");
		goto out;
	}

	LOGI("cb device name is %s for %s ", dev_name(&pdev->dev), name);

	context->cb_num = -1;
	context->name = name;
	context->hgsldev = hgsl_dev;
	context->pdev = pdev;
	ratelimit_default_init(&context->ratelimit);

#ifdef CONFIG_PER_PROCESS_PT
	/* Set the adreno_smmu priv data for the device */
	dev_set_drvdata(&pdev->dev, &context->adreno_smmu);

	/* Create a new context */
	context->domain = iommu_domain_alloc(&platform_bus_type);
	if (!context->domain) {
		/*FIXME: Put back the pdev here? */
		return -ENODEV;
	}

	ret = iommu_attach_device(context->domain, &context->pdev->dev);
	if (ret) {
		/* FIXME: put back the device here? */
		iommu_domain_free(context->domain);
		context->domain = NULL;
		goto out;
	}
#else
	context->domain = iommu_get_domain_for_dev(&context->pdev->dev);
	if (!context->domain) {
		LOGE("iommu_get_domain_for_dev failed for %s\n", name);
		ret = -ENODEV;
		goto out;
	}

	LOGI("get domain success for name %s", name);

	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64))) {
		dev_err(&pdev->dev, "Failed to set 64-bit DMA mask\n");
		ret = -EIO;
		goto out;
	}

#endif

	iommu_set_fault_handler(context->domain, handler, mmu);

#ifdef CONFIG_PER_PROCESS_PT
	// iommu driver will give virtual context bank number
	context->cb_num = qcom_iommu_get_context_bank_nr(context->domain);

#else
	// TO DO: Hardcode the cb number for this GVM, otherwise this will come from PVM.
	context->cb_num = 8;
#endif

	if (context->cb_num >= 0)
		goto out;

	dev_err(hgsl_dev->dev, "Couldn't get the context bank for %s: %d\n",
			context->name, context->cb_num);

	iommu_detach_device(context->domain, &context->pdev->dev);
	iommu_domain_free(context->domain);
	context->domain = NULL;

out:
	of_node_put(node);
	return ret;
}

/*
 * TO DO: Implement fault handler functionality for both GPUs.
 */
static int hgsl_iommu_default_fault_handler(struct iommu_domain *domain,
	struct device *dev, unsigned long addr, int flags, void *token)
{
#if IS_ENABLED(CONFIG_DEBUG_FV)
	DEBUG_HGSL_FV("Fault has occurred for first GPU");
#endif
	return 0;
}

static int hgsl_iommu_default_fault_handler_second_gpu(struct iommu_domain *domain,
	struct device *dev, unsigned long addr, int flags, void *token)
{
#if IS_ENABLED(CONFIG_DEBUG_FV)
	DEBUG_HGSL_FV("Fault has occurred for second GPU");
#endif
	return 0;
}

fault_handler_t fault_handlers[] = {
	hgsl_iommu_default_fault_handler,
	hgsl_iommu_default_fault_handler_second_gpu
};

int hgsl_iommu_map_using_single_pt(struct hgsl_mem_node *mem_node,
	struct hgsl_iommu_context *context, bool internal)
{
	int ret = 0;
	int attr = 0;
	struct sg_table *sgt = NULL;

	if (IS_ERR_OR_NULL(mem_node)) {
		LOGE("mem_node is NULL");
		ret = -EINVAL;
		goto out;
	}

#if IS_ENABLED(CONFIG_DEBUG_FV)
	DEBUG_HGSL_FV("For mapping, buf size = 0x%x", mem_node->memdesc.size);
#endif

	mutex_lock(&mem_node->mem_node_iommu_info.ptr_hgsl_priv->sgt_lock);
	sgt = hgsl_get_sgt_iommu(&context->pdev->dev, mem_node);
	mutex_unlock(&mem_node->mem_node_iommu_info.ptr_hgsl_priv->sgt_lock);
	if (IS_ERR_OR_NULL(sgt)) {
		LOGE("Invalid sgt!");
		ret = -EINVAL;
		goto out;
	}

	if (internal) {
		ret = dma_map_sgtable(mem_node->mem_node_iommu_info.attach_iommu->dev,
				sgt, DMA_BIDIRECTIONAL, attr);
		if (ret) {
			LOGE("Error in dma_map_sgtable for buf size 0x%x ret=%d", mem_node->memdesc.size, ret);
			goto out_put_sgt;
		}
#if IS_ENABLED(CONFIG_DEBUG_FV)
		atomic_inc_return(&int_map_cnt);
#endif
	}

	if (!sgt->sgl) {
		ret = -EINVAL;
		LOGE("sgl is NULL\n");
		goto out_put_sgt;
	}

	// Device address is provided by sg_dma_address
	mem_node->memdesc.gpuaddr = sg_dma_address(sgt->sgl);

#if IS_ENABLED(CONFIG_DEBUG_FV)
	DEBUG_HGSL_FV("buf size 0x%x, gpuAddr 0x%llx map_cnt %d int_map_cnt %d",
		mem_node->memdesc.size, mem_node->memdesc.gpuaddr,
		atomic_inc_return(&map_cnt), int_map_cnt);
#endif

out_put_sgt:
	if (ret && !IS_ERR_OR_NULL(sgt)) {
		mutex_lock(&mem_node->mem_node_iommu_info.ptr_hgsl_priv->sgt_lock);
		hgsl_put_sgt_iommu(mem_node);
		mutex_unlock(&mem_node->mem_node_iommu_info.ptr_hgsl_priv->sgt_lock);
	}

out:
	return ret;
}

int hgsl_iommu_unmap_using_single_pt(struct hgsl_mem_node *mem_node,
	struct hgsl_iommu_context *context, bool internal)
{
	int attr = 0;
	int ret = 0;

	if (!mem_node) {
		LOGE("Invalid memnode");
		ret = -EINVAL;
		goto out;
	}

#if IS_ENABLED(CONFIG_DEBUG_FV)
	DEBUG_HGSL_FV("For unmapping buffer size 0x%x, gpuAddr is 0x%llx",
			mem_node->memdesc.size, mem_node->memdesc.gpuaddr);
#endif

	if (internal) {
		dma_unmap_sgtable(mem_node->mem_node_iommu_info.attach_iommu->dev,
						mem_node->mem_node_iommu_info.sgt_iommu, DMA_BIDIRECTIONAL, attr);
#if IS_ENABLED(CONFIG_DEBUG_FV)
		atomic_dec_return(&int_map_cnt);
#endif
	}

	mutex_lock(&mem_node->mem_node_iommu_info.ptr_hgsl_priv->sgt_lock);
	hgsl_put_sgt_iommu(mem_node);
	mem_node->memdesc.gpuaddr = 0x0;
	mutex_unlock(&mem_node->mem_node_iommu_info.ptr_hgsl_priv->sgt_lock);

#if IS_ENABLED(CONFIG_DEBUG_FV)
	DEBUG_HGSL_FV("unmap buf cnt %d int_map_cnt %d",
			atomic_dec_return(&map_cnt), int_map_cnt);
#endif

out:
	return ret;
}

static int iommu_probe_user_context(struct qcom_hgsl *hgsl_dev,
			struct device_node *node)
{
	struct hgsl_iommu *iommu = HGSL_IOMMU(hgsl_dev);
	struct hgsl_mmu *mmu = &hgsl_dev->mmu;
	int ret[HGSL_DEVICE_NUM] = {0};
	char node_name[20];
	uint32_t dev_num = HGSL_GPU_0;

	for (dev_num = HGSL_GPU_0; dev_num < HGSL_DEVICE_NUM; dev_num++) {
		memset(&iommu->user_context[dev_num], 0, sizeof(iommu->user_context[dev_num]));
		atomic_set(&iommu->user_context[dev_num].pagetables, 0);

		snprintf(node_name, sizeof(node_name), "gfx3d_user_%d", dev_num);

		ret[dev_num] = hgsl_iommu_setup_context(mmu, node, &iommu->user_context[dev_num],
						node_name, fault_handlers[dev_num]);
		if (ret[dev_num])
			LOGE("hgsl_iommu_setup_context() failed for GPU device %d ret=%d",
					dev_num, ret[dev_num]);
		/*
		 * TO DO: If it will get  used during per process PT enablement
		 * then will keep it otherwise will remove.
		 */
		iommu->user_context[dev_num].mask = 1;
	}

	if (ret[HGSL_GPU_0] && ret[HGSL_GPU_1]) {
		LOGE("hgsl_iommu_setup_context() failed for both GPU ret = %d and %d",
				ret[HGSL_GPU_0], ret[HGSL_GPU_1]);
		goto err;
	}

#ifdef CONFIG_PER_PROCESS_PT
	/*
	 * TO DO : Need to check this
	 * It is problamatic if smmu driver does system suspend before consumer
	 * device (gpu). So smmu driver creates a device_link to act as a
	 * supplier which in turn will ensure correct order during system
	 * suspend. In kgsl, since we don't initialize iommu on the gpu device,
	 * we should create a device_link between kgsl iommu device and gpu
	 * device to maintain a correct suspend order between smmu device and
	 * gpu device.
	 */
	if (!device_link_add(&device->pdev->dev, &iommu->user_context.pdev->dev,
						DL_FLAG_AUTOREMOVE_CONSUMER))
		dev_err(&iommu->user_context.pdev->dev,
						"Unable to create device link to gpu device\n");

	/* TO DO: This is required when we enable per process PT */
	if (hgsl_iommu_set_ttbr0(&(iommu->user_context[HGSL_GPU_0])))
		goto err;

	if (hgsl_iommu_set_ttbr0(&(iommu->user_context[HGSL_GPU_1])))
		goto err;

	/*
	 * TO DO: Adreno SMMU aperture will be already set by PVM.
	 * Need to check durint per process PT enablement.
	 */
	for (dev_num = HGSL_GPU_0; dev_num < HGSL_DEVICE_NUM; dev_num++)
		hgsl_set_smmu_aperture(hgsl_dev, &iommu->user_context[dev_num]);
#endif

	return 0;

err:
	for (dev_num = HGSL_GPU_0; dev_num < HGSL_DEVICE_NUM; dev_num++) {
		if (iommu->user_context[dev_num].domain)
			hgsl_iommu_detach_context(&iommu->user_context[dev_num]);
	}

	return -EIO;
}

#ifdef CONFIG_PER_PROCESS_PT
static void hgsl_iommu_context_clear_fsr(struct hgsl_mmu *mmu,
					struct hgsl_iommu_context *ctx)
{
	unsigned int sctlr_val;

	if (ctx->stalled_on_fault) {
		HGSL_IOMMU_SET_CTX_REG(ctx, HGSL_IOMMU_CTX_FSR, 0xffffffff);
		/*
		 * Re-enable context fault interrupts after clearing
		 * FSR to prevent the interrupt from firing repeatedly
		 */
		sctlr_val = HGSL_IOMMU_GET_CTX_REG(ctx, HGSL_IOMMU_CTX_SCTLR);
		sctlr_val |= (0x1 << HGSL_IOMMU_SCTLR_CFIE_SHIFT);
		HGSL_IOMMU_SET_CTX_REG(ctx, HGSL_IOMMU_CTX_SCTLR, sctlr_val);
		/*
		 * Make sure the above register writes
		 * are not reordered across the barrier
		 * as we use writel_relaxed to write them
		 */
		wmb();
		ctx->stalled_on_fault = false;
	}
}

static void hgsl_iommu_clear_fsr(struct hgsl_mmu *mmu)
{
	struct hgsl_iommu *iommu = &mmu->iommu;

	hgsl_iommu_context_clear_fsr(mmu, &iommu->user_context[HGSL_GPU_0]);
	hgsl_iommu_context_clear_fsr(mmu, &iommu->user_context[HGSL_GPU_1]);
}

static void hgsl_iommu_check_config(struct hgsl_mmu *mmu,
			struct device_node *parent, unsigned int iommu_num)
{
	struct device_node *node = NULL;
	struct device_node *phandle;
	char node_name[20];

	// For each GPU, the IOMMU is different which helps to pick right context bank node
	snprintf(node_name, sizeof(node_name), "gfx3d_user_%d", iommu_num);

	node = of_find_node_by_name(parent, node_name);
	if (!node)
		return;

	phandle = of_parse_phandle(node, "iommus", 0);

	if (phandle) {
		if (of_device_is_compatible(phandle, "qcom,qsmmu-v500"))
			mmu->subtype = HGSL_IOMMU_SMMU_V500;
		if (of_device_is_compatible(phandle, "qcom,adreno-smmu"))
			set_bit(HGSL_MMU_IOPGTABLE, &mmu->features);

		of_node_put(phandle);
}

	of_node_put(node);
}
#endif

int hgsl_iommu_bind(struct platform_device *pdev, struct qcom_hgsl *hgsl_dev)
{
	int ret = 0;
	struct hgsl_iommu *iommu = HGSL_IOMMU(hgsl_dev);
	struct hgsl_mmu *mmu = &hgsl_dev->mmu;
	struct device_node *node = pdev->dev.of_node;
#ifdef CONFIG_PER_PROCESS_PT
	struct resource *res0 = NULL;
	struct resource *res1 = NULL;
#endif

#ifdef CONFIG_PER_PROCESS_PT
	/* Create a kmem cache for the pagetable address objects */
	if (!addr_entry_cache) {
		addr_entry_cache = KMEM_CACHE(hgsl_iommu_addr_entry, 0);
		if (!addr_entry_cache) {
			ret = -ENOMEM;
			goto err;
		}
	}
	/*
	 * Set the SMMU aperture on Gen7 targets to use per-process
	 * pagetables.
	 */
	hgsl_mmu_set_feature(hgsl_dev, HGSL_MMU_SMMU_APERTURE);
	hgsl_mmu_set_feature(hgsl_dev, HGSL_MMU_SUPPORT_VBO);
#endif

#ifdef CONFIG_PER_PROCESS_PT
	// Get the first register pair
	res0 = platform_get_resource(pdev, IORESOURCE_MEM, HGSL_GPU_0);
	if (!res0) {
		dev_err(&pdev->dev, "No memory resource for IOMMU0\n");
		return -ENODEV;
	}

	iommu->user_context[HGSL_GPU_0].regbase = devm_ioremap_resource(&pdev->dev, res0);
	if (!iommu->user_context[HGSL_GPU_0].regbase) {
		dev_err(&pdev->dev, "Couldn't map IOMMU registers for IOMMU0\n");
		return -ENOMEM;
	}

	// Get the second register pair
	res1 = platform_get_resource(pdev, IORESOURCE_MEM, HGSL_GPU_1);
	if (!res1) {
		dev_err(&pdev->dev, "No memory resource for IOMMU1\n");
		ret = -ENODEV;
		goto err_unmap_base0;
	}

	iommu->user_context[HGSL_GPU_1].regbase = devm_ioremap_resource(&pdev->dev, res1);
	if (!iommu->user_context[HGSL_GPU_1].regbase) {
		dev_err(&pdev->dev, "Couldn't map IOMMU registers for IOMMU1\n");
		ret = -ENOMEM;
		goto err_unmap_base0;
	}
#endif
	iommu->pdev = pdev;

	set_bit(HGSL_MMU_PAGED, &mmu->features);

	mmu->type = HGSL_MMU_TYPE_IOMMU;
	mmu->mmu_ops = &hgsl_iommu_ops;

#ifdef CONFIG_PER_PROCESS_PT
	/* Peek at the phandle to set up configuration */
	hgsl_iommu_check_config(mmu, node, HGSL_GPU_0);
	hgsl_iommu_check_config(mmu, node, HGSL_GPU_1);
#endif

	ret = iommu_probe_user_context(hgsl_dev, node);
	if (ret) {
		LOGE("iommu_probe_user_context() failed ret=%d", ret);
		of_platform_depopulate(&pdev->dev);
		goto err;
	}

#ifdef CONFIG_PER_PROCESS_PT
	hgsl_mmu_start(mmu);

	/* Clear FSR here in case it is set from a previous pagefault */
	//hgsl_iommu_clear_fsr(mmu);

	/*
	 * Only support VBOs on MMU500 hardware that supports the PRR
	 * marker register to ignore writes to the zero page
	 */
	if ((mmu->subtype == HGSL_IOMMU_SMMU_V500) &&
				test_bit(HGSL_MMU_SUPPORT_VBO, &mmu->features)) {
		/*
		 * We need to allocate a page because we need a known physical
		 * address to program in the PRR register but the hardware
		 * should intercept accesses to the page before they go to DDR
		 * so this should be mostly just a placeholder
		 */
		hgsl_vbo_zero_page = alloc_page(GFP_KERNEL | __GFP_ZERO |
				__GFP_NORETRY | __GFP_HIGHMEM);
	}
	if (!hgsl_vbo_zero_page)
		clear_bit(HGSL_MMU_SUPPORT_VBO, &mmu->features);
#endif
	return 0;

#ifdef CONFIG_PER_PROCESS_PT
err_unmap_base0:
	devm_iounmap(&pdev->dev, iommu->user_context[HGSL_GPU_0].regbase);
#endif

err:
	kmem_cache_destroy(addr_entry_cache);
	addr_entry_cache = NULL;

	return ret;
}

static struct hgsl_iommu_pt *to_iommu_pt(struct hgsl_pagetable *pagetable)
{
	return container_of(pagetable, struct hgsl_iommu_pt, base);
}

struct hgsl_iommu *to_hgsl_iommu(struct hgsl_pagetable *pt)
{
	return &pt->mmu->iommu;
}

/* hgsl_iommu_get_ttbr0 - Get TTBR0 setting for a pagetable */
static u64 hgsl_iommu_get_ttbr0(struct hgsl_pagetable *pagetable)
{
	struct hgsl_iommu_pt *pt = to_iommu_pt(pagetable);

	/* This will be zero if KGSL_MMU_IOPGTABLE is not enabled */
	return pt->ttbr0;
}

static inline int convert_gsl_to_iommu_memflags(uint32_t *flags)
{
	int iommu_flags = IOMMU_READ | IOMMU_WRITE | IOMMU_NOEXEC;

	// TO DO: Add new flags
	if (0ULL != (*flags & GSL_MEMFLAGS_GPUREADONLY))
		iommu_flags |= ~IOMMU_WRITE;

	if (0ULL != (*flags & GSL_MEMFLAGS_GPUIOCOHERENT))
		iommu_flags |= IOMMU_CACHE;

	return iommu_flags;
}

static void hgsl_iommu_flush_tlb(struct hgsl_mmu *mmu)
{
	struct hgsl_iommu *iommu = &mmu->iommu;

	iommu_flush_iotlb_all(to_iommu_domain(&iommu->user_context[HGSL_GPU_0]));

	if (iommu->user_context[HGSL_GPU_1].domain)
		iommu_flush_iotlb_all(to_iommu_domain(&iommu->user_context[HGSL_GPU_1]));
}

#ifdef CONFIG_PER_PROCESS_PT
static int _iopgtbl_unmap(struct hgsl_iommu_pt *pt, u64 gpuaddr, size_t size)
{
	struct io_pgtable_ops *ops = pt->pgtbl_ops;

	while (size) {
		if ((ops->unmap(ops, gpuaddr, PAGE_SIZE, NULL)) != PAGE_SIZE)
			return -EINVAL;

		gpuaddr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	hgsl_iommu_flush_tlb(pt->base.mmu);
	return 0;
}

static size_t _iopgtbl_map_sg(struct hgsl_iommu_pt *pt, u64 gpuaddr,
			struct sg_table *sgt, int prot)
{
	struct io_pgtable_ops *ops = pt->pgtbl_ops;
	struct scatterlist *sg;
	size_t mapped = 0;
	u64 addr = gpuaddr;
	int ret, i;

	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		size_t size = sg->length;
		phys_addr_t phys = sg_phys(sg);

		while (size) {
			ret = ops->map(ops, addr, phys, PAGE_SIZE, prot, GFP_KERNEL);

			if (ret) {
				_iopgtbl_unmap(pt, gpuaddr, mapped);
				return 0;
			}

			phys += PAGE_SIZE;
			mapped += PAGE_SIZE;
			addr += PAGE_SIZE;
			size -= PAGE_SIZE;
		}
	}

	return mapped;
}

static size_t _iopgtbl_map_page_to_range(struct hgsl_iommu_pt *pt,
			struct page *page, u64 gpuaddr, size_t range, int prot)
{
	struct io_pgtable_ops *ops = pt->pgtbl_ops;
	size_t mapped = 0;
	u64 addr = gpuaddr;
	int ret;

	while (range) {
		ret = ops->map(ops, addr, page_to_phys(page), PAGE_SIZE,
				prot, GFP_KERNEL);
		if (ret) {
			_iopgtbl_unmap(pt, gpuaddr, mapped);
			return 0;
		}

		mapped += PAGE_SIZE;
		addr += PAGE_SIZE;
		range -= PAGE_SIZE;
	}

	return mapped;
}
#else
static int _iopgtbl_unmap(struct hgsl_iommu_pt *pt, u64 gpuaddr, size_t size)
{
	return 0;
}

static size_t _iopgtbl_map_sg(struct hgsl_iommu_pt *pt, u64 gpuaddr,
			struct sg_table *sgt, int prot)
{
	return 0;
}

static size_t _iopgtbl_map_page_to_range(struct hgsl_iommu_pt *pt,
			struct page *page, u64 gpuaddr, size_t range, int prot)
{
	return 0;
}
#endif

/*
 * hgsl_memdesc_footprint - get the size of the mmap region
 * @memdesc - the memdesc
 * The entire memdesc must be mapped. Additionally if the
 * CPU mapping is going to be mirrored, there must be room
 * for the guard page to be mapped so that the address spaces
 * match up.
 */
inline uint64_t
hgsl_memnode_footprint(const struct hgsl_mem_node *mem_node)
{
	if (!(mem_node->mem_node_iommu_info.priv & HGSL_MEMNODE_GUARD_PAGE))
		return mem_node->memdesc.size;

	return PAGE_ALIGN(mem_node->memdesc.size + PAGE_SIZE);
}

static struct page *iommu_get_guard_page(void)
{
	if (!hgsl_guard_page)
		hgsl_guard_page = alloc_page(GFP_KERNEL | __GFP_ZERO |
				__GFP_NORETRY | __GFP_HIGHMEM);

	return hgsl_guard_page;
}

static int hgsl_iopgtbl_map(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *mem_node, bool internal, uint32_t iommu_mask)
{
	// Ignore iommu_mask as we will be using the single pagetable
	// to map with both the GPU IOMMUs.
	struct hgsl_iommu_pt *pt = to_iommu_pt(pagetable);
	size_t mapped, padding;
	int prot;
	struct qcom_hgsl *hgsl_dev = HGSL_MMU_DEVICE(pagetable->mmu);
	struct sg_table *sgt = NULL;

	if (IS_ERR_OR_NULL(mem_node->mem_node_iommu_info.sgt_iommu)) {
		// TO DO dev should be corresponding to the Context bank
		// Need to change here
		sgt = hgsl_get_sgt_iommu(hgsl_dev->dev, mem_node);
		if (IS_ERR_OR_NULL(sgt))
			return -EINVAL;
	}

	/* Get the protection flags for the user context */
	prot = convert_gsl_to_iommu_memflags(&mem_node->flags);

	mapped = _iopgtbl_map_sg(pt, mem_node->memdesc.gpuaddr,
							sgt, prot);
	if (mapped == 0)
		return -ENOMEM;

	padding = hgsl_memnode_footprint(mem_node) - mapped;
	if (padding) {
		struct page *page = iommu_get_guard_page();
		size_t ret;

		if (page)
			ret = _iopgtbl_map_page_to_range(pt, page,
					mem_node->memdesc.gpuaddr + mapped, padding,
					prot & ~IOMMU_WRITE);

		if (!page || !ret) {
			_iopgtbl_unmap(pt, mem_node->memdesc.gpuaddr, mapped);
			return -ENOMEM;
		}
	}

	return 0;
}

static int hgsl_iopgtbl_unmap(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *memnode, bool internal, uint32_t iommu_mask)
{

	hgsl_put_sgt(memnode, internal);

	return _iopgtbl_unmap(to_iommu_pt(pagetable), memnode->memdesc.gpuaddr,
			hgsl_memnode_footprint(memnode));
}

static void hgsl_iommu_destroy_pagetable(struct hgsl_pagetable *pagetable)
{
	struct hgsl_iommu_pt *pt = to_iommu_pt(pagetable);

	if (pt != NULL) {
		free_io_pgtable_ops(pt->pgtbl_ops);
		kfree(pt);
	}
}

static bool hgsl_iommu_addr_in_range(struct hgsl_pagetable *pagetable,
			uint64_t gpuaddr, uint64_t size)
{
	if ((!pagetable) || (gpuaddr == 0)) {
		LOGE("Invalid parameters");
		return false;
	}

	if (gpuaddr >= pagetable->va_start && (gpuaddr + size) <
				pagetable->va_end)
		return true;

	return false;
}

/*
 * hgsl_memnode_get_align - Get alignment flags from a memnode
 * @memnode - the mem node
 * Returns the alignment requested, as power of 2 exponent.
 */
static inline int
hgsl_memnode_get_align(const struct hgsl_mem_node *mem_node)
{
	return FIELD_GET(HGSL_MEMALIGN_MASK, mem_node->flags);
}

static u64 _get_unmapped_area_hint(struct hgsl_pagetable *pagetable,
			u64 bottom, u64 top, u64 size, u64 align)
{
	u64 hint;

	/*
	 * VA fragmentation can be a problem on global and secure pagetables
	 * that are common to all processes, or if we're constrained to a 32-bit
	 * range. Don't use the va_hint in these cases.
	 */
	if (!pagetable->va_hint || !upper_32_bits(top))
		return (u64) -EINVAL;

	/* Satisfy requested alignment */
	hint = ALIGN(pagetable->va_hint, align);

	/*
	 * The va_hint is the highest VA that was allocated in the non-SVM
	 * region. The 64-bit SVM and non-SVM regions do not overlap. So, we
	 * know there is no VA allocated at this gpuaddr. Therefore, we only
	 * need to check whether we have enough space for this allocation.
	 */
	if ((hint + size) > top)
		return (u64) -ENOMEM;

	pagetable->va_hint = hint + size;
	return hint;
}

static uint64_t _get_unmapped_area(struct hgsl_pagetable *pagetable,
			uint64_t bottom, uint64_t top, uint64_t size,
			uint64_t align)
{
	struct rb_node *node;
	uint64_t start;

	/* Check if we can assign a gpuaddr based on the last allocation */
	start = _get_unmapped_area_hint(pagetable, bottom, top, size, align);
	if (!IS_ERR_VALUE(start))
		return start;

	/* Fall back to searching through the range. */
	node = rb_first(&pagetable->rbtree);
	bottom = ALIGN(bottom, align);
	start = bottom;

	while (node != NULL) {
		uint64_t gap;
		struct hgsl_iommu_addr_entry *entry = rb_entry(node,
				struct hgsl_iommu_addr_entry, node);

		/*
		 * Skip any entries that are outside of the range, but make sure
		 * to account for some that might straddle the lower bound
		 */
		if (entry->base < bottom) {
			if (entry->base + entry->size > bottom)
				start = ALIGN(entry->base + entry->size, align);
			node = rb_next(node);
			continue;
		}

		/* Stop if we went over the top */
		if (entry->base >= top)
			break;

		/* Make sure there is a gap to consider */
		if (start < entry->base) {
			gap = entry->base - start;

			if (gap >= size)
				return start;
		}

		/* Stop if there is no more room in the region */
		if (entry->base + entry->size >= top)
			return (uint64_t) -ENOMEM;

		/* Start the next cycle at the end of the current entry */
		start = ALIGN(entry->base + entry->size, align);
		node = rb_next(node);
	}

	if (start + size <= top)
		return start;

	return (uint64_t) -ENOMEM;
}

static int _insert_gpuaddr(struct hgsl_pagetable *pagetable,
			uint64_t gpuaddr, uint64_t size)
{
	struct rb_node **node, *parent = NULL;
	struct hgsl_iommu_addr_entry *new =
			kmem_cache_alloc(addr_entry_cache, GFP_ATOMIC);

	if (new == NULL)
		return -ENOMEM;

	new->base = gpuaddr;
	new->size = size;

	node = &pagetable->rbtree.rb_node;

	while (*node != NULL) {
		struct hgsl_iommu_addr_entry *this;

		parent = *node;
		this = rb_entry(parent, struct hgsl_iommu_addr_entry, node);

		if (new->base < this->base)
			node = &parent->rb_left;
		else if (new->base > this->base)
			node = &parent->rb_right;
		else {
			/* Duplicate entry */
			WARN(1, "duplicate gpuaddr: 0x%llx\n", gpuaddr);
			kmem_cache_free(addr_entry_cache, new);
			return -EEXIST;
		}
	}

	rb_link_node(&new->node, parent, node);
	rb_insert_color(&new->node, &pagetable->rbtree);

	return 0;
}

static int get_gpuaddr(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *mem_node, u64 start, u64 end,
			u64 size, u64 align)
{
	u64 addr = 0;
	int ret = 0;

	spin_lock(&pagetable->lock);
	addr = _get_unmapped_area(pagetable, start, end, size, align);
	if (addr == (u64) -ENOMEM) {
		spin_unlock(&pagetable->lock);
		return -ENOMEM;
	}

	ret = _insert_gpuaddr(pagetable, addr, size);
	spin_unlock(&pagetable->lock);
	if (ret == 0) {
		mem_node->memdesc.gpuaddr = addr;
		mem_node->mem_node_iommu_info.pagetable = pagetable;
	}

	return ret;
}

/*
 * hgsl_memnode_set_align - Set alignment flags of a memnode
 * @memnode - the memnode
 * @align - alignment requested, as a power of 2 exponent.
 */
static inline int
hgsl_memnode_set_align(struct hgsl_mem_node *memnode, unsigned int align)
{
	if (align > 32)
		align = 32;

	memnode->flags &= ~(uint64_t)HGSL_MEMALIGN_MASK;
	memnode->flags |= FIELD_PREP(HGSL_MEMALIGN_MASK, align);
	return 0;
}

static int hgsl_iommu_get_gpuaddr(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *mem_node)
{
	int ret = 0;
	u64 start, end, size, align;

	if (!mem_node) {
		LOGE("Invalid memory node");
		return -EINVAL;
	}

	if (mem_node->flags & GSL_MEMFLAGS_PROTECTED)
		return -EINVAL;

	size = hgsl_memnode_footprint(mem_node);

	/* Validate size to prevent integer overflow */
	if (size == 0 || size > UINT_MAX) {
		LOGE("Invalid memory size requested");
		return -EINVAL;
	}

	if (size >= SZ_2M)
		hgsl_memnode_set_align(mem_node, ilog2(SZ_2M));
	else if (size >= SZ_1M)
		hgsl_memnode_set_align(mem_node, ilog2(SZ_1M));
	else if (size >= SZ_64K)
		hgsl_memnode_set_align(mem_node, ilog2(SZ_64));

	align = max_t(uint64_t, 1 << hgsl_memnode_get_align(mem_node), PAGE_SIZE);

#ifdef CONFIG_PER_PROCESS_PT
	if (memdesc->flags & KGSL_MEMFLAGS_FORCE_32BIT) {
		start = pagetable->compat_va_start;
		end = pagetable->compat_va_end;
	} else {
		start = pagetable->va_start;
		end = pagetable->va_end;
}
#endif
	start = pagetable->va_start;
	end = pagetable->va_end;

	ret = get_gpuaddr(pagetable, mem_node, start, end, size, align);
	/* TO DO if OOM, retry once after flushing lockless_workqueue */
	if (ret == -ENOMEM)
		ret = get_gpuaddr(pagetable, mem_node, start, end, size, align);

	return ret;
}

static struct hgsl_iommu_addr_entry *_find_gpuaddr(
			struct hgsl_pagetable *pagetable, uint64_t gpuaddr)
{
	struct rb_node *node = pagetable->rbtree.rb_node;

	while (node != NULL) {
		struct hgsl_iommu_addr_entry *entry = rb_entry(node,
		struct hgsl_iommu_addr_entry, node);

		if (gpuaddr < entry->base)
			node = node->rb_left;
		else if (gpuaddr > entry->base)
			node = node->rb_right;
		else
			return entry;
	}

	return NULL;
}

static int _remove_gpuaddr(struct hgsl_pagetable *pagetable,
			uint64_t gpuaddr)
{
	struct hgsl_iommu_addr_entry *entry = NULL;

	entry = _find_gpuaddr(pagetable, gpuaddr);

	if (WARN(!entry, "GPU address %llx doesn't exist\n", gpuaddr))
		return -ENOMEM;

	/*
	 * If the hint was based on this entry, adjust it to the end of the
	 * previous entry.
	 */
	if (pagetable->va_hint == (entry->base + entry->size)) {
		struct hgsl_iommu_addr_entry *prev =
				rb_entry_safe(rb_prev(&entry->node),
						struct hgsl_iommu_addr_entry, node);

		pagetable->va_hint = pagetable->va_start;
		if (prev)
			pagetable->va_hint = max_t(u64, prev->base + prev->size,
								pagetable->va_start);
	}

	rb_erase(&entry->node, &pagetable->rbtree);
	kmem_cache_free(addr_entry_cache, entry);
	return 0;
}

static void hgsl_iommu_put_gpuaddr(struct hgsl_mem_node *mem_node)
{
	if (mem_node->mem_node_iommu_info.pagetable == NULL)
		return;

	spin_lock(&mem_node->mem_node_iommu_info.pagetable->lock);

	_remove_gpuaddr(mem_node->mem_node_iommu_info.pagetable, mem_node->memdesc.gpuaddr);

	spin_unlock(&mem_node->mem_node_iommu_info.pagetable->lock);
}

static int hgsl_iommu_get_asid(struct hgsl_pagetable *pt, struct hgsl_context *context)
{
#ifdef CONFIG_PER_PROCESS_PT
	struct hgsl_iommu *iommu = to_hgsl_iommu(pt);
	struct iommu_domain *domain = NULL;

	domain = to_iommu_domain(&iommu->user_context);

	return qcom_iommu_get_asid_nr(domain);
#endif
	/*
	 * TO DO: ASID will be used to register the context which BE is already knowing
	 * so need to check whether to implement this function or not.
	 */
	return 0;
}

static const struct hgsl_mmu_ops hgsl_iommu_ops = {
	.mmu_start = hgsl_iommu_start,
	.mmu_close = hgsl_iommu_close,
#ifdef CONFIG_PER_PROCESS_PT
	.mmu_clear_fsr = hgsl_iommu_clear_fsr,
	.mmu_set_pf_policy = hgsl_iommu_set_pf_policy, // No one calls this API in LA metal
	.mmu_pagefault_resume = hgsl_iommu_pagefault_resume,
#endif
	.mmu_getpagetable = hgsl_iommu_getpagetable,
	.mmu_flush_tlb = hgsl_iommu_flush_tlb,
};

static const struct hgsl_mmu_pt_ops iopgtbl_pt_ops = {
	.mmu_map = hgsl_iopgtbl_map,
	.mmu_unmap = hgsl_iopgtbl_unmap,
	.mmu_destroy_pagetable = hgsl_iommu_destroy_pagetable,
	.get_ttbr0 = hgsl_iommu_get_ttbr0,
	.get_asid = hgsl_iommu_get_asid,
	.get_gpuaddr = hgsl_iommu_get_gpuaddr,
	.put_gpuaddr = hgsl_iommu_put_gpuaddr,
	.addr_in_range = hgsl_iommu_addr_in_range,
};
