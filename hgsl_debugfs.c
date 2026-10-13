// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/platform_device.h>
#include <linux/debugfs.h>

#include "hgsl.h"
#include "hgsl_debugfs.h"
#include "hgsl_stat.h"

#define hgsl_debugfs_printf(s, fmt, ...) seq_printf((s), fmt, ##__VA_ARGS__)
#define hgsl_debugfs_puts(s, str) seq_puts((s), str)

static int hgsl_stat_debugfs_show(struct seq_file *s, void *unused)
{
	hgsl_stat_show(s, s->private);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hgsl_stat_debugfs);

/* Map GET_MEMTYPE(flags) index → human-readable name. */
static const char *hgsl_memtype_name(unsigned int idx)
{
	static const char * const names[] = {
		"any",                       /* 0x00 */
		"framebuffer",               /* 0x01 */
		"renderbuffer",              /* 0x02 */
		"arraybuffer",               /* 0x03 */
		"elementarraybuffer",        /* 0x04 */
		"vertexarraybuffer",         /* 0x05 */
		"texture",                   /* 0x06 */
		"surface",                   /* 0x07 */
		"eglsurface",                /* 0x08 */
		"gl",                        /* 0x09 */
		"cl",                        /* 0x0a */
		"cl_buffer_map",             /* 0x0b */
		"cl_buffer_unmap",           /* 0x0c */
		"cl_image_map",              /* 0x0d */
		"cl_image_unmap",            /* 0x0e */
		"cl_kernel_stack",           /* 0x0f */
		"cmds",                      /* 0x10 */
		"2d",                        /* 0x11 */
		"egl_image",                 /* 0x12 */
		"egl_shadow",                /* 0x13 */
		"multisample",               /* 0x14 */
		"2d_ext",                    /* 0x15 */
		"3d_ext",                    /* 0x16 */
		"unknown", "unknown", "unknown", "unknown", "unknown",
		"unknown", "unknown", "unknown", "unknown",          /* 0x17-0x1f */
		"vk_any",                    /* 0x20 */
		"vk_instance",               /* 0x21 */
		"vk_physicaldevice",         /* 0x22 */
		"vk_device",                 /* 0x23 */
		"vk_queue",                  /* 0x24 */
		"vk_cmdbuffer",              /* 0x25 */
		"vk_devicememory",           /* 0x26 */
		"vk_buffer",                 /* 0x27 */
		"vk_bufferview",             /* 0x28 */
		"vk_image",                  /* 0x29 */
		"vk_imageview",              /* 0x2a */
		"vk_shadermodule",           /* 0x2b */
		"vk_pipeline",               /* 0x2c */
		"vk_pipelinecache",          /* 0x2d */
		"vk_pipelinelayout",         /* 0x2e */
		"vk_sampler",                /* 0x2f */
		"vk_samplerycbcrconversionkhr", /* 0x30 */
		"vk_descriptorset",          /* 0x31 */
		"vk_descriptorsetlayout",    /* 0x32 */
		"vk_descriptorpool",         /* 0x33 */
		"vk_fence",                  /* 0x34 */
		"vk_semaphore",              /* 0x35 */
		"vk_event",                  /* 0x36 */
		"vk_querypool",              /* 0x37 */
		"vk_framebuffer",            /* 0x38 */
		"vk_renderpass",             /* 0x39 */
		"vk_program",                /* 0x3a */
		"vk_commandpool",            /* 0x3b */
		"vk_surfacekhr",             /* 0x3c */
		"vk_swapchainkhr",           /* 0x3d */
		"vk_descriptorupdatetemplate", /* 0x3e */
		"vk_deferredoperationkhr",   /* 0x3f */
		"vk_privatedataslotext",     /* 0x40 */
		"vk_debug_utils",            /* 0x41 */
		"vk_tensor",                 /* 0x42 */
		"vk_tensorview",             /* 0x43 */
		"vk_mlpipeline",             /* 0x44 */
		"vk_acceleration_structure", /* 0x45 */
	};

	if (idx < ARRAY_SIZE(names))
		return names[idx];
	return "unknown";
}

/* Print per-buffer table for one rb-tree, with a separate protected section. */
static void _show_mem_pool(struct seq_file *s, struct rb_root *root,
			   const char *label)
{
	struct rb_node *rb;
	struct hgsl_mem_node *mn;
	bool header_printed = false;

	for (rb = rb_first(root); rb; rb = rb_next(rb)) {
		mn = rb_entry(rb, struct hgsl_mem_node, mem_rb_node);
		if (!header_printed) {
			hgsl_debugfs_printf(s, "  %s:\n", label);
			hgsl_debugfs_printf(s, "  %-18s %12s  %-10s %-24s\n",
					    "iova", "size", "flags", "memtype");
			header_printed = true;
		}
		hgsl_debugfs_printf(s, "  0x%016llx 0x%010llx  0x%08x %-24s%s\n",
				    mn->memdesc.gpuaddr,
				    mn->memdesc.size64,
				    mn->flags,
				    hgsl_memtype_name(GET_MEMTYPE(mn->flags)),
				    (mn->flags & GSL_MEMFLAGS_PROTECTED) ? " [P]" : "");
	}
}

static void _show_client_mem(struct seq_file *s, struct hgsl_priv *priv)
{
	hgsl_debugfs_printf(s, "client-%d-[%s]\n",
			    priv->pid, priv->hyp_priv.client_name);

	mutex_lock(&priv->lock);
	_show_mem_pool(s, &priv->mem_allocated, "allocated");
	_show_mem_pool(s, &priv->mem_mapped,    "extern/mapped");
	mutex_unlock(&priv->lock);
}

static int hgsl_mem_info_show(struct seq_file *s, void *unused)
{
	struct qcom_hgsl *hgsl = s->private;
	struct hgsl_priv *priv;
	s64 total_alloc = 0, total_ext = 0, total_prot = 0;

	mutex_lock(&hgsl->mutex);

	hgsl_debugfs_puts(s, "SUMMARY:\n");
	list_for_each_entry(priv, &hgsl->active_list, node) {
		s64 alloc = atomic64_read(&priv->alloc_mem_size);
		s64 ext   = atomic64_read(&priv->extern_mem_size);
		s64 prot  = atomic64_read(&priv->prot_mem_size);
		int dev;

		hgsl_debugfs_printf(s, "  client-%d-[%s]:\n    total=%lldKB  alloc=%lldKB  extern=%lldKB  (protected=%lldKB)\n",
			   priv->pid, priv->hyp_priv.client_name,
			   (alloc + ext) >> 10,
			   alloc >> 10, ext >> 10, prot >> 10);
		for (dev = 0; dev < HGSL_DEVICE_NUM; dev++) {
			struct hgsl_pagetable *pt = priv->pagetable[dev];

			if (!pt)
				continue;
			hgsl_debugfs_printf(s,
				   "    pagetable[dev%d]: entries=%d  mapped=%ldKB  peak=%ldKB  fault=0x%016llx\n",
				   dev,
				   atomic_read(&pt->stats.entries),
				   atomic_long_read(&pt->stats.mapped) >> 10,
				   atomic_long_read(&pt->stats.max_mapped) >> 10,
				   pt->fault_addr);
		}
		total_alloc += alloc;
		total_ext   += ext;
		total_prot  += prot;
	}
	hgsl_debugfs_printf(s, "  TOTAL: total=%lldKB  alloc=%lldKB  extern=%lldKB  (protected=%lldKB)\n\n",
		   (total_alloc + total_ext) >> 10,
		   total_alloc >> 10, total_ext >> 10, total_prot >> 10);

	list_for_each_entry(priv, &hgsl->active_list, node) {
		_show_client_mem(s, priv);
		hgsl_debugfs_puts(s, "\n");
	}

	mutex_unlock(&hgsl->mutex);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hgsl_mem_info);

void hgsl_debugfs_init(struct platform_device *pdev)
{
	struct qcom_hgsl *hgsl = platform_get_drvdata(pdev);
	struct dentry *root;

	root = debugfs_create_dir("hgsl", NULL);
	if (IS_ERR_OR_NULL(root)) {
		LOGW("Unable to create debugfs dir for hgsl");
		return;
	}

	if (IS_ERR_OR_NULL(debugfs_create_file("stat", 0444,
					       root, hgsl, &hgsl_stat_debugfs_fops)))
		LOGW("Unable to create debugfs stat");

	if (IS_ERR_OR_NULL(debugfs_create_file("mem_info", 0444,
					       root, hgsl, &hgsl_mem_info_fops)))
		LOGW("Unable to create debugfs mem_info");

	hgsl->debugfs = root;
}

void hgsl_debugfs_release(struct platform_device *pdev)
{
	struct qcom_hgsl *hgsl = platform_get_drvdata(pdev);

	debugfs_remove_recursive(hgsl->debugfs);
}
