/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2002,2007-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __HGSL_MMU_H
#define __HGSL_MMU_H

#include <linux/platform_device.h>
#include "hgsl.h"
#include "hgsl_utils.h"
#include "hgsl_iommu.h"

// TO DO below macro is not required as its for global memory buffer,
// For FV the global memory buffers will be handled by BE.
//#define MMU_DEFAULT_TTBR0(_d) (hgsl_mmu_pagetable_get_ttbr0((_d)->mmu.defaultpagetable))

/* Mask ASID fields to match 48bit ptbase address*/
#define MMU_SW_PT_BASE(_ttbr0) \
	(_ttbr0 & (BIT_ULL(HGSL_IOMMU_ASID_START_BIT) - 1))

#define HGSL_MMU_DEVICE(_mmu) \
	container_of((_mmu), struct qcom_hgsl, mmu)

/*
 * enum hgsl_ft_pagefault_policy_bits - KGSL pagefault policy bits
 * @HGSL_FT_PAGEFAULT_INT_ENABLE: No longer used, but retained for compatibility
 * @HGSL_FT_PAGEFAULT_GPUHALT_ENABLE: enable GPU halt on pagefaults
 * @HGSL_FT_PAGEFAULT_LOG_ONE_PER_PAGE: log one pagefault per page
 * @HGSL_FT_PAGEFAULT_LOG_ONE_PER_INT: log one pagefault per interrupt
 */
enum {
	HGSL_FT_PAGEFAULT_INT_ENABLE = 0,
	HGSL_FT_PAGEFAULT_GPUHALT_ENABLE = 1,
	HGSL_FT_PAGEFAULT_LOG_ONE_PER_PAGE = 2,
	HGSL_FT_PAGEFAULT_LOG_ONE_PER_INT = 3,
	/* HGSL_FT_PAGEFAULT_MAX_BITS is used to calculate the mask */
	HGSL_FT_PAGEFAULT_MAX_BITS,
};

#define HGSL_FT_PAGEFAULT_MASK GENMASK(HGSL_FT_PAGEFAULT_MAX_BITS - 1, 0)

#define HGSL_FT_PAGEFAULT_DEFAULT_POLICY 0

enum hgsl_mmutype {
	HGSL_MMU_TYPE_IOMMU = 0,
	HGSL_MMU_TYPE_NONE
};

#define HGSL_IOMMU_SMMU_V500 1

/*
 * struct hgsl_mmu - Master definition for KGSL MMU devices
 * @flags: MMU device flags
 * @type: Type of MMU that is attached
 * @subtype: Sub Type of MMU that is attached
 * @defaultpagetable: Default pagetable object for the MMU
 * @securepagetable: Default secure pagetable object for the MMU
 * @mmu_ops: Function pointers for the MMU sub-type
 * @secured: True if the MMU needs to be secured
 * @feature: Static list of MMU features
 */
struct hgsl_mmu {
	unsigned long flags;
	enum hgsl_mmutype type;
	u32 subtype;
	//struct hgsl_pagetable *defaultpagetable;
	//struct hgsl_pagetable *securepagetable;
	const struct hgsl_mmu_ops *mmu_ops;
	bool secured;
	unsigned long features;
	/** @pfpolicy: The current pagefault policy for the device */
	unsigned long pfpolicy;
	/** mmu: Pointer to the IOMMU sub-device */
	struct hgsl_iommu iommu;
};

struct hgsl_mmu_ops {
	void (*mmu_close)(struct hgsl_mmu *mmu);
	int (*mmu_start)(struct hgsl_mmu *mmu);
	uint64_t (*mmu_get_current_ttbr0)(struct hgsl_mmu *mmu, struct hgsl_context *context);
	void (*mmu_pagefault_resume)(struct hgsl_mmu *mmu, bool terminate);
	void (*mmu_clear_fsr)(struct hgsl_mmu *mmu);
	void (*mmu_enable_clk)(struct hgsl_mmu *mmu);
	void (*mmu_disable_clk)(struct hgsl_mmu *mmu);
	int (*mmu_set_pf_policy)(struct hgsl_mmu *mmu, unsigned long pf_policy);
	int (*mmu_init_pt)(struct hgsl_mmu *mmu, struct hgsl_pagetable *pt);
	struct hgsl_pagetable * (*mmu_getpagetable)(struct hgsl_mmu *mmu,
			unsigned long name, uint32_t iommu_mask);
	void (*mmu_map_global)(struct hgsl_mmu *mmu,
			struct hgsl_mem_node *memnode, u32 padding);
	void (*mmu_flush_tlb)(struct hgsl_mmu *mmu);
};

struct hgsl_mmu_pt_ops {
	int (*mmu_map)(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *mem_node, bool internal, uint32_t iommu_mask);
	int (*mmu_unmap)(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *memnode, bool internal, uint32_t iommu_mask);
	void (*mmu_destroy_pagetable)(struct hgsl_pagetable *pt);
	u64 (*get_ttbr0)(struct hgsl_pagetable *pt);
	int (*get_context_bank)(struct hgsl_pagetable *pt, struct hgsl_context *context);
	int (*get_asid)(struct hgsl_pagetable *pt, struct hgsl_context *context);
	int (*get_gpuaddr)(struct hgsl_pagetable *pt,
			struct hgsl_mem_node *memnode);
	void (*put_gpuaddr)(struct hgsl_mem_node *memnode);
	uint64_t (*find_svm_region)(struct hgsl_pagetable *pt, uint64_t start,
			uint64_t end, uint64_t size, uint64_t align);
	int (*set_svm_region)(struct hgsl_pagetable *pt,
			uint64_t gpuaddr, uint64_t size);
	int (*svm_range)(struct hgsl_pagetable *pt, uint64_t *lo, uint64_t *hi,
			uint64_t memflags);
	bool (*addr_in_range)(struct hgsl_pagetable *pagetable,
			uint64_t gpuaddr, uint64_t size);
};

enum hgsl_mmu_feature {
	/* @HGSL_MMU_64BIT: Use 64 bit virtual address space */
	HGSL_MMU_64BIT,
	/* @HGSL_MMU_PAGED: Support paged memory */
	HGSL_MMU_PAGED,
	/*
	 * @HGSL_MMU_NEED_GUARD_PAGE: Set if a guard page is needed for each
	 * mapped region
	 */
	HGSL_MMU_NEED_GUARD_PAGE,
	/** @HGSL_MMU_IO_COHERENT: Set if a device supports I/O coherency */
	HGSL_MMU_IO_COHERENT,
	/** @HGSL_MMU_LLC_ENABLE: Set if LLC is activated for the target */
	HGSL_MMU_LLCC_ENABLE,
	/** @HGSL_MMU_SMMU_APERTURE: Set the SMMU aperture */
	HGSL_MMU_SMMU_APERTURE,
	/*
	 * @HGSL_MMU_IOPGTABLE: Set if the qcom,adreno-smmu implementation is
	 * available. Implies split address space and per-process pagetables
	 */
	HGSL_MMU_IOPGTABLE,
	/* @HGSL_MMU_SUPPORT_VBO: Non-secure VBOs are supported */
	HGSL_MMU_SUPPORT_VBO,
	/* @HGSL_MMU_PAGEFAULT_TERMINATE: Set to make pagefaults fatal */
	HGSL_MMU_PAGEFAULT_TERMINATE,
};

#define HGSL_IOMMU(d) (&((d)->mmu.iommu))

int __init hgsl_mmu_init(void);
void hgsl_mmu_exit(void);

int hgsl_mmu_start(struct hgsl_mmu *mmu);

void hgsl_print_global_pt_entries(struct seq_file *s);
void hgsl_mmu_putpagetable(struct hgsl_pagetable *pagetable);

int hgsl_mmu_map(struct qcom_hgsl *hgsl_dev, struct hgsl_pagetable *pagetable,
			 struct hgsl_mem_node *memnode, bool internal, u32 dev_hnd);
int hgsl_mmu_unmap(struct qcom_hgsl *hgsl_dev, struct hgsl_pagetable *pagetable,
				struct hgsl_mem_node *memnode, bool internal, u32 dev_hnd);
unsigned int hgsl_mmu_log_fault_addr(struct hgsl_mmu *mmu,
			u64 ttbr0, uint64_t addr);
bool hgsl_mmu_gpuaddr_in_range(struct hgsl_pagetable *pt, uint64_t gpuaddr,
			uint64_t size);

int hgsl_mmu_get_region(struct hgsl_pagetable *pagetable,
			uint64_t gpuaddr, uint64_t size);

int hgsl_mmu_find_region(struct hgsl_pagetable *pagetable,
			uint64_t region_start, uint64_t region_end,
			uint64_t *gpuaddr, uint64_t size, unsigned int align);

uint64_t hgsl_mmu_find_svm_region(struct hgsl_pagetable *pagetable,
			uint64_t start, uint64_t end, uint64_t size,
			uint64_t alignment);

int hgsl_mmu_set_svm_region(struct hgsl_pagetable *pagetable, uint64_t gpuaddr,
			uint64_t size);

void hgsl_mmu_detach_pagetable(struct hgsl_pagetable *pagetable);

int hgsl_mmu_svm_range(struct hgsl_pagetable *pagetable,
			uint64_t *lo, uint64_t *hi, uint64_t memflags);

struct hgsl_pagetable *hgsl_get_pagetable(struct qcom_hgsl *hgsl_dev, unsigned long name);

/*
 * Static inline functions of MMU that simply call the SMMU specific
 * function using a function pointer. These functions can be thought
 * of as wrappers around the actual function
 */

#define MMU_OP_VALID(_mmu, _field) \
	(((_mmu) != NULL) && \
	 ((_mmu)->mmu_ops != NULL) && \
	 ((_mmu)->mmu_ops->_field != NULL))

#define PT_OP_VALID(_pt, _field) \
	(((_pt) != NULL) && \
	 ((_pt)->pt_ops != NULL) && \
	 ((_pt)->pt_ops->_field != NULL))

enum hgsl_mmutype hgsl_mmu_get_mmutype(struct qcom_hgsl *hgsl_dev);

/*
 * hgsl_mmu_get_gpuaddr - Assign a GPU address to the memnode
 * @pagetable: GPU pagetable to assign the address in
 * @memnode: mem node to assign the memory to
 *
 * Return: 0 on success or negative on failure
 */
static inline int hgsl_mmu_get_gpuaddr(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *memnode, bool use_single_pt)
{
	int ret = 0;

	/*
	 * For single global PT, the GPU address is managed by the IOMMU driver.
	*/
	if (use_single_pt)
		goto out;

	if ((!memnode) || (!pagetable)) {
		LOGW("invalid mem node or pagetable");
		ret = -EINVAL;
		goto out;
	}

	if (PT_OP_VALID(pagetable, get_gpuaddr))
		ret = pagetable->pt_ops->get_gpuaddr(pagetable, memnode);
	else
		ret = -ENOMEM;

out:
	return ret;
}

/*
 * hgsl_mmu_put_gpuaddr - Remove a GPU address from a pagetable
 * @pagetable: Pagetable to release the memory from
 * @memnode: Memory node containing the GPU address to free
 *
 * Release a GPU address in the MMU virtual address space.
 */
static inline int hgsl_mmu_put_gpuaddr(struct hgsl_pagetable *pagetable,
			struct hgsl_mem_node *memnode, bool use_single_pt)
{
	int ret = 0;

	if (use_single_pt)
		goto out;

	if (!memnode || !pagetable) {
		LOGW("invalid mem node or pagetable");
		ret = -EINVAL;
		goto out;
	}

	if (PT_OP_VALID(pagetable, put_gpuaddr))
		pagetable->pt_ops->put_gpuaddr(memnode);

out:
	return ret;
}

#ifdef CONFIG_PER_PROCESS_PT
static inline u64 hgsl_mmu_get_current_ttbr0(struct hgsl_mmu *mmu, struct hgsl_context *context)
{
	if (MMU_OP_VALID(mmu, mmu_get_current_ttbr0))
		return mmu->mmu_ops->mmu_get_current_ttbr0(mmu, context);

	return 0;
}
#endif

static inline struct hgsl_pagetable *hgsl_mmu_getpagetable(struct hgsl_mmu *mmu,
			unsigned long pid, uint32_t iommu_mask)
{
	if (MMU_OP_VALID(mmu, mmu_getpagetable)) {
		LOGD("get page table for the new process pid %lu\n", pid);
		return mmu->mmu_ops->mmu_getpagetable(mmu, pid, iommu_mask);
	}

	return NULL;
}

static inline int hgsl_mmu_set_pagefault_policy(struct hgsl_mmu *mmu,
					unsigned long pf_policy)
{
	if (MMU_OP_VALID(mmu, mmu_set_pf_policy))
		return mmu->mmu_ops->mmu_set_pf_policy(mmu, pf_policy);

	return 0;
}

static inline void hgsl_mmu_pagefault_resume(struct hgsl_mmu *mmu,
	bool terminate)
{
	if (MMU_OP_VALID(mmu, mmu_pagefault_resume))
		return mmu->mmu_ops->mmu_pagefault_resume(mmu, terminate);
}

static inline bool hgsl_mmu_is_perprocess(struct hgsl_mmu *mmu)
{
	return test_bit(HGSL_MMU_IOPGTABLE, &mmu->features);
}

inline u64
hgsl_mmu_pagetable_get_ttbr0(struct hgsl_pagetable *pagetable);

#ifdef CONFIG_PER_PROCESS_PT
static inline void hgsl_mmu_flush_tlb(struct hgsl_mmu *mmu)
{
	if (!test_bit(HGSL_MMU_IOPGTABLE, &mmu->features))
		return;

	if (MMU_OP_VALID(mmu, mmu_flush_tlb))
		return mmu->mmu_ops->mmu_flush_tlb(mmu);
}

/*
 * hgsl_mmu_map_global - Map a memdesc as a global buffer
 * @device: A KGSL GPU device handle
 * @memdesc: Pointer to a GPU memory descriptor
 * @padding: Any padding to add to the end of the VA allotment (in bytes)
 *
 * Map a buffer as globally accessible in all pagetable contexts
 */
void hgsl_mmu_map_global(struct hgsl_device *device,
		struct hgsl_memdesc *memdesc, u32 padding);

/*
 * hgsl_mmu_pagetable_get_context_bank - Return the context bank number
 * @pagetable: A handle to a given pagetable
 * @context : LPAC or User context
 * This function will find the context number of the given pagetable
 * Return: The context bank number the pagetable is attached to or
 * negative error on failure.
 */

int hgsl_mmu_pagetable_get_context_bank(struct hgsl_pagetable *pagetable,
			struct hgsl_context *context);
#endif


/*
 * hgsl_mmu_pagetable_get_asid - Return the ASID number
 * @pagetable: A handle to a given pagetable
 * @context: LPAC or GC context
 * This function will find the ASID number of the given pagetable
 * Return: The ASID number the pagetable is attached to or
 * negative error on failure.
 */
int hgsl_mmu_pagetable_get_asid(struct hgsl_pagetable *pagetable,
			struct hgsl_context *context);

void hgsl_mmu_pagetable_init(struct hgsl_mmu *mmu,
			struct hgsl_pagetable *pagetable, u32 name);

void hgsl_mmu_pagetable_add(struct hgsl_mmu *mmu, struct hgsl_pagetable *pagetable);
#endif /* __HGSL_MMU_H */
