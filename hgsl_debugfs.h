/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 */
#ifndef __HGSL_DEBUGFS_H
#define __HGSL_DEBUGFS_H

#include <linux/device.h>
#include <linux/platform_device.h>

#ifdef CONFIG_DEBUG_FS
void hgsl_debugfs_init(struct platform_device *pdev);
void hgsl_debugfs_release(struct platform_device *pdev);
#else
static inline void hgsl_debugfs_init(struct platform_device *pdev) {}
static inline void hgsl_debugfs_release(struct platform_device *pdev) {}
#endif

#endif  /* __HGSL_DEBUGFS_H */
