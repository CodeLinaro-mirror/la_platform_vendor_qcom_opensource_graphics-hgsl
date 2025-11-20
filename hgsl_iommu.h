/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2012-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __HGSL_IOMMU_H
#define __HGSL_IOMMU_H

#include <linux/adreno-smmu-priv.h>
#include <linux/io-pgtable.h>
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include "hgsl.h"

/*
 * These defines control the address range for allocations that
 * are mapped into all pagetables.
 */
#define HGSL_IOMMU_GLOBAL_MEM_SIZE      (20  * SZ_1M)
#define HGSL_IOMMU_GLOBAL_MEM_BASE32    0xf8000000
#define HGSL_IOMMU_GLOBAL_MEM_BASE64    \
    (HGSL_MEMSTORE_TOKEN_ADDRESS - HGSL_IOMMU_GLOBAL_MEM_SIZE)

/*
 * This is a dummy token address that we use to identify memstore when the user
 * wants to map it. mmap() uses a unsigned long for the offset so we need a 32
 * bit value that works with all sized apps. We chose a value that was purposely
 * unmapped so if you increase the global memory size make sure it doesn't
 * conflict
 */

#define HGSL_MEMSTORE_TOKEN_ADDRESS     (HGSL_IOMMU_SECURE_BASE32 - SZ_4K)

#define HGSL_IOMMU_GLOBAL_MEM_BASE(__mmu)   \
        (test_bit(HGSL_MMU_64BIT, &(__mmu)->features) ? \
            HGSL_IOMMU_GLOBAL_MEM_BASE64 : HGSL_IOMMU_GLOBAL_MEM_BASE32)

#define HGSL_IOMMU_SVM_BASE32(__mmu)    \
    (ADRENO_DEVICE(HGSL_MMU_DEVICE(__mmu))->uche_gmem_base + \
            ADRENO_DEVICE(HGSL_MMU_DEVICE(__mmu))->gpucore->gmem_size)

#define HGSL_IOMMU_SVM_END32(__mmu) \
    (test_bit(HGSL_MMU_64BIT, &(__mmu)->features) ? \
            (test_bit(HGSL_MMU_IOPGTABLE, &(__mmu)->features) ? \
             HGSL_MEMSTORE_TOKEN_ADDRESS : \
             HGSL_IOMMU_GLOBAL_MEM_BASE64) : \
    (0xC0000000 - SZ_16M))

/*
 * Limit secure size to 256MB for 32bit kernels.
 */
#define HGSL_IOMMU_SECURE_SIZE32 SZ_256M
#define HGSL_IOMMU_SECURE_BASE32        \
    (HGSL_IOMMU_SECURE_BASE64 - HGSL_IOMMU_SECURE_SIZE32)
#define HGSL_IOMMU_SECURE_END32 HGSL_IOMMU_SECURE_BASE64

#define HGSL_IOMMU_SECURE_BASE64        0x100000000ULL
#define HGSL_IOMMU_SECURE_END64 \
    (HGSL_IOMMU_SECURE_BASE64 + HGSL_IOMMU_SECURE_SIZE64)

#define HGSL_IOMMU_MAX_SECURE_SIZE 0xFFFFF000

#define HGSL_IOMMU_SECURE_SIZE64        \
    (HGSL_IOMMU_MAX_SECURE_SIZE - HGSL_IOMMU_SECURE_SIZE32)

#define HGSL_IOMMU_SECURE_BASE(_mmu) (test_bit(HGSL_MMU_64BIT, \
                    &(_mmu)->features) ? HGSL_IOMMU_SECURE_BASE64 : \
                    HGSL_IOMMU_SECURE_BASE32)
#define HGSL_IOMMU_SECURE_END(_mmu) (test_bit(HGSL_MMU_64BIT, \
                    &(_mmu)->features) ? HGSL_IOMMU_SECURE_END64 : \
                    HGSL_IOMMU_SECURE_END32)
#define HGSL_IOMMU_SECURE_SIZE(_mmu) (test_bit(HGSL_MMU_64BIT, \
                    &(_mmu)->features) ? HGSL_IOMMU_MAX_SECURE_SIZE : \
                    HGSL_IOMMU_SECURE_SIZE32)

/* The CPU supports 39 bit addresses */
#define HGSL_IOMMU_SVM_BASE64           0x1000000000ULL
#define HGSL_IOMMU_SVM_END64            0x4000000000ULL
#define HGSL_IOMMU_VA_BASE64            0x4000000000ULL
#define HGSL_IOMMU_VA_END64             0x8000000000ULL

/* Global SMMU register offsets */
#define HGSL_IOMMU_PRR_CFG_LADDR        0x6008
#define HGSL_IOMMU_PRR_CFG_UADDR        0x600c

/* Register offsets */
#define HGSL_IOMMU_CTX_SCTLR            0x10000
#define HGSL_IOMMU_CTX_ACTLR            0x10004

#define HGSL_IOMMU_CTX_TTBR0            0x10020
#define HGSL_IOMMU_CTX_CONTEXTIDR       0x10034
#define HGSL_IOMMU_CTX_FSR                      0x10058

#define HGSL_IOMMU_CTX_TLBIALL          0x10618
#define HGSL_IOMMU_CTX_RESUME           0x10008

#define HGSL_IOMMU_CTX_FSYNR0           0x10068
#define HGSL_IOMMU_CTX_FSYNR1           0x1006c

#define HGSL_IOMMU_CTX_TLBSYNC          0x107f0
#define HGSL_IOMMU_CTX_TLBSTATUS        0x107f4

#define DMA_ATTR_DELAYED_UNMAP          (1UL << 17)

/* TLBSTATUS register fields */
#define HGSL_IOMMU_CTX_TLBSTATUS_SACTIVE BIT(0)

/* SCTLR fields */
#define HGSL_IOMMU_SCTLR_HUPCF_SHIFT            8
#define HGSL_IOMMU_SCTLR_CFCFG_SHIFT            7
#define HGSL_IOMMU_SCTLR_CFIE_SHIFT             6

#define HGSL_IOMMU_ACTLR_PRR_ENABLE             BIT(5)

/* FSR fields */
#define HGSL_IOMMU_FSR_SS_SHIFT         30

/* ASID field in TTBR register */
#define HGSL_IOMMU_ASID_START_BIT       48

/* offset at which a nop command is placed in setstate */
#define HGSL_IOMMU_SETSTATE_NOP_OFFSET  1024

/*
 * Alignment hint, passed as the power of 2 exponent.
 * i.e 4k (2^12) would be 12, 64k (2^16)would be 16.
 */
#define HGSL_MEMALIGN_MASK              0x00FF0000
#define HGSL_MEMALIGN_SHIFT             16

typedef int (*fault_handler_t)(struct iommu_domain *, struct device *, unsigned long, int, void *);

struct hgsl_pagetable;

/*
 * struct hgsl_iommu_context - Structure holding data about an iommu context
 * bank
 * @pdev: pointer to the iommu context's platform device
 * @name: context name
 * @id: The id of the context, used for deciding how it is used.
 * @cb_num: The hardware context bank number, used for calculating register offsets.
 * @kgsldev: The kgsl device that uses this context.
 * @stalled_on_fault: Flag when set indicates that this iommu device is stalled
 * on a page fault
 */
struct hgsl_iommu_context {
	struct platform_device *pdev;
	void __iomem *regbase;
	const char *name;
	int cb_num;
	struct qcom_hgsl *hgsldev;
	bool stalled_on_fault;
	/* * ratelimit: Ratelimit state for the context */
	struct ratelimit_state ratelimit;
	struct iommu_domain *domain;
	struct adreno_smmu_priv adreno_smmu;
	int mask;
	atomic_t pagetables;
};

/*
 * struct hgsl_iommu - Structure holding iommu data for kgsl driver
 * @regbase: Virtual address of the IOMMU register base
 * @regstart: Physical address of the iommu registers
 * @regsize: Length of the iommu register region.
 * @setstate: Scratch GPU memory for IOMMU operations
 * @clk_enable_count: The ref count of clock enable calls
 * @clks: Array of pointers to IOMMU clocks
 * @smmu_info: smmu info used in a5xx preemption
 */
struct hgsl_iommu {
	/* * @user_context: Container for the user iommu context for IOMMU */
	struct hgsl_iommu_context user_context[HGSL_DEVICE_NUM];
	struct hgsl_mem_node *setstate;

	struct hgsl_mem_node *smmu_info;
	/* @pdev: Pointer to the platform device for the IOMMU device */
	struct platform_device *pdev;
	/*
	 * @ppt_active: Set when the first per process pagetable is created.
	 * This is used to warn when global buffers are created that might not
	 * be mapped in all contexts
	 */
	bool ppt_active;
	/* @cb0_offset: Offset of context bank 0 from iommu register base */
	u32 cb0_offset;
	/* @pagesize: Size of each context bank register space */
	u32 pagesize;
	/* @cx_gdsc: CX GDSC handle in case the IOMMU needs it */
	struct regulator *cx_gdsc;
};

struct hgsl_pagetable {
	spinlock_t lock;
	struct kref refcount;
	struct list_head list;
	unsigned int name;
	struct kobject *kobj;
	struct work_struct destroy_ws;

	struct {
	    atomic_t entries;
	    atomic_long_t mapped;
	    atomic_long_t max_mapped;
	} stats;
	const struct hgsl_mmu_pt_ops *pt_ops;
	uint64_t fault_addr;
	struct hgsl_mmu *mmu;
	/* @rbtree: all buffers mapped into the pagetable, indexed by gpuaddr */
	struct rb_root rbtree;
	/* @va_start: Start of virtual range used in this pagetable */
	u64 va_start;
	/* @va_end: End of virtual range */
	u64 va_end;
	/*
	 * @svm_start: Start of shared virtual memory range. Addresses in this
	 * range are also valid in the process's CPU address space.
	 */
	u64 svm_start;
	/* @svm_end: end of 32 bit compatible range */
	u64 svm_end;
	/*
	 * @compat_va_start - Start of the "compat" virtual address range for
	 * forced 32 bit allocations
	 */
	u64 compat_va_start;
	/*
	 * @compat_va_end - End of the "compat" virtual address range for
	 * forced 32 bit allocations
	 */
	u64 compat_va_end;
	/* @va_hint: Virtual address hint for 64-bit non-SVM allocations */
	u64 va_hint;
	u64 global_base;
};

/*
 * struct hgsl_iommu_pt - Iommu pagetable structure private to kgsl driver
 * @domain: Pointer to the iommu domain that contains the iommu pagetable
 * @ttbr0: register value to set when using this pagetable
 */
struct hgsl_iommu_pt {
	struct hgsl_pagetable base;
	u64 ttbr0;

	struct io_pgtable_ops *pgtbl_ops;
	struct io_pgtable_cfg cfg;
};

struct hgsl_iommu *to_hgsl_iommu(struct hgsl_pagetable *pt);

/*
 * hgsl_set_smmu_aperture - set SMMU Aperture for user context
 * @device: A GPU device handle
 *
 * Return: 0 on success or negative on failure.
 */
int hgsl_set_smmu_aperture(struct qcom_hgsl *device,
	struct hgsl_iommu_context *context);

int hgsl_iommu_bind(struct platform_device *pdev, struct qcom_hgsl *hgsl_dev);
void hgsl_destroy_pagetable(struct kref *kref);
inline uint64_t hgsl_memnode_footprint(const struct hgsl_mem_node *mem_node);
void _deferred_destroy(struct work_struct *ws);
int hgsl_iommu_map_using_single_pt(struct hgsl_mem_node *mem_node,
	struct hgsl_iommu_context *context, bool internal);
int hgsl_iommu_unmap_using_single_pt(struct hgsl_mem_node *mem_node,
	struct hgsl_iommu_context *context, bool internal);

#endif
