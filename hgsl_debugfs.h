/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */
#ifndef __HGSL_DEBUGFS_H
#define __HGSL_DEBUGFS_H

#include <linux/device.h>
#include <linux/platform_device.h>

#ifdef CONFIG_DEBUG_FS
void hgsl_debugfs_init(struct platform_device *pdev);
void hgsl_debugfs_release(struct platform_device *pdev);
void hgsl_debugfs_dump_ctxts(struct hgsl_context **ctxts, int nctxts,
			      struct qcom_hgsl *hgsl, bool atomic);
#else
static inline void hgsl_debugfs_init(struct platform_device *pdev) {}
static inline void hgsl_debugfs_release(struct platform_device *pdev) {}
static inline void hgsl_debugfs_dump_ctxts(struct hgsl_context **ctxts,
					    int nctxts,
					    struct qcom_hgsl *hgsl, bool atomic)
{}
#endif

#endif  /* __HGSL_DEBUGFS_H */
