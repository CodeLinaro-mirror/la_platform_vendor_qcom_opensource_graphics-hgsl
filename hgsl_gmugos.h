/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __HGSL_GMUGOS_H
#define __HGSL_GMUGOS_H

#include <linux/device.h>
#include <linux/irqreturn.h>
#include <linux/platform_device.h>

#define HGSL_GMUGOS_NODE_NAME    "hgsl_gmugos"
#define HGSL_GMUGOS_IRQ_NUM      (8)
#define HGSL_GMUGOS_NAME_LEN     (64)
#define HGSL_GMUGOS_WQ_NAME_LEN  (100)

#define GMUGOS_IRQ_MASK TCSR_DEST_IRQ_MASK_0
#define RGSGOS_IRQ_MASK TCSR_DEST_IRQ_MASK_1

struct hgsl_gmugos_irq {
	char name[HGSL_GMUGOS_NAME_LEN];
	struct regmap *regmap;
	u32 id;
	s32 num;
	struct work_struct irq_work;
	struct workqueue_struct *irq_workqueue;
};

struct hgsl_gmugos {
	struct hgsl_gmugos_irq irq[HGSL_GMUGOS_IRQ_NUM];
	uint32_t activated_irq[HGSL_DEVICE_NUM][HGSL_GMUGOS_IRQ_NUM];
	u32 dev_hnd;
};

int hgsl_init_gmugos(struct platform_device *pdev, uint32_t devhandle,
				u32 irq_idx, u32 mask_bits);
void hgsl_gmugos_irq_trigger(struct hgsl_gmugos *gmugos,
				u32 irq_idx, u32 bit_id);
void hgsl_gmugos_irq_enable(struct hgsl_gmugos_irq *gmugos_irq,
				u32 mask_bits);
void hgsl_gmugos_irq_free(struct hgsl_gmugos_irq *irq);

#endif  /* __HGSL_GMUGOS_H */

