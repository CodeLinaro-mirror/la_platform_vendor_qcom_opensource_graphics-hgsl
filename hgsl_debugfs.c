// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/platform_device.h>
#include <linux/debugfs.h>

#include "hgsl.h"
#include "hgsl_dispatch.h"
#include "hgsl_debugfs.h"

/*
 * hgsl_debugfs_printf / hgsl_debugfs_puts
 *
 * Output routing is determined solely by @s:
 *   s == NULL  -> softirq / timer context: use LOGE()
 *   s != NULL  -> process context / debugfs read: use seq_printf/seq_puts
 *
 * This lets _ctxt_info_show and _hgsl_stat_show share a single
 * implementation without any intermediate buffer.
 */
#define hgsl_debugfs_printf(s, fmt, ...) \
	do { \
		if ((s) == NULL) \
			LOGE(fmt, ##__VA_ARGS__); \
		else \
			seq_printf(s, fmt, ##__VA_ARGS__); \
	} while (0)

#define hgsl_debugfs_puts(s, str) \
	do { \
		if ((s) == NULL) \
			LOGE("%s", str); \
		else \
			seq_puts(s, str); \
	} while (0)

/*
 * _ctxt_info_show - print one context's details.
 *
 * @s:      seq_file to write to (ignored when atomic=true)
 * @atomic: true  -> softirq context: use LOGW, read shadow_ts directly
 *          false -> process context: use seq_printf, call hgsl_read_timestamp
 */
static void _ctxt_info_show(struct seq_file *s, struct hgsl_context *ctxt,
			     bool atomic)
{
	struct doorbell_queue *dbq;
	struct doorbell_context_queue *dbcq;
	struct hgsl_hsync_timeline *tl;
	u32 ts = 0;

	if (!ctxt->dispatch) {
		hgsl_debugfs_printf(s,
			"  ctx=[%u:%u]: queued_ts=%u, last_ts=%u, in_destroy=%u\n",
			ctxt->devhandle, ctxt->context_id,
			ctxt->queued_ts, ctxt->last_ts,
			READ_ONCE(ctxt->in_destroy));
		return;
	}

	if (atomic) {
		/* Avoid sleeping in softirq: read shadow_ts directly */
		if (ctxt->shadow_ts)
			ts = get_context_retired_ts(ctxt);
	} else {
		hgsl_read_timestamp(ctxt, GSL_TIMESTAMP_RETIRED, &ts);
	}

	hgsl_debugfs_printf(s, "  ctx=[%u:%u] {\n",
		ctxt->devhandle, ctxt->context_id);
	hgsl_debugfs_printf(s,
		"    is_fe_shadow=%u, in_destroy=%u, is_killed=%u, dbq_info=0x%x, flags=0x%x, tcsr_idx=%d, db_signal=%u;\n",
		ctxt->is_fe_shadow, READ_ONCE(ctxt->in_destroy), ctxt->is_killed,
		ctxt->dbq_info, ctxt->flags, ctxt->tcsr_idx, ctxt->db_signal);
	{
		char submitted_ts_str[32];

		if (list_empty(&ctxt->dispatch->drawobj_list))
			strscpy(submitted_ts_str, "all-submitted",
				sizeof(submitted_ts_str));
		else
			snprintf(submitted_ts_str, sizeof(submitted_ts_str),
				"%u",
				list_last_entry(&ctxt->dispatch->drawobj_list,
					struct cmd_obj,
					node)->drawobj->timestamp);
		hgsl_debugfs_printf(s,
			"    queued_ts=%u, submitted_ts=%s, last_ts=%u, retired_ts=%u, processed=%u;\n",
			ctxt->queued_ts, submitted_ts_str,
			ctxt->last_ts, ts, ctxt->event_group.processed);
	}
	hgsl_debugfs_printf(s,
		"    drawq: head=%u, tail=%u, queued=%d, shadow_ts=%s;\n",
		READ_ONCE(ctxt->drawq_head), READ_ONCE(ctxt->drawq_tail),
		READ_ONCE(ctxt->queued),
		ctxt->shadow_ts ? "enabled" : "disabled");

	dbq = ctxt->dbq;
	if (dbq)
		hgsl_debugfs_printf(s,
			"    dbq idx=%u: state=%u, tcsr_idx=%d, ibdesc_max_size=%u, seq_num=%d;\n",
			dbq->dbq_idx, dbq->state, dbq->tcsr_idx,
			dbq->ibdesc_max_size, atomic_read(&dbq->seq_num));

	dbcq = ctxt->dbcq;
	if (dbcq) {
		struct ctx_queue_header *qhdr =
			(struct ctx_queue_header *)dbcq->queue_header;

		hgsl_debugfs_printf(s,
			"    dbcq irq_bit_idx=%d: db_signal=%u, queue_size=0x%x, indirect_ib_ts=%u, seq_num=%u, dbcq_export_id=%u;\n",
			dbcq->irq_bit_idx,
			dbcq->db_signal, dbcq->queue_size, dbcq->indirect_ib_ts,
			dbcq->seq_num, ctxt->dbcq_export_id);
		if (qhdr)
			hgsl_debugfs_printf(s, "    dbcq rptr=%u, wptr=%u;\n",
				READ_ONCE(qhdr->readIdx),
				READ_ONCE(qhdr->writeIdx));
	}

	tl = ctxt->timeline;
	if (tl)
		hgsl_debugfs_printf(s,
			"    timeline=%s: fcontext=0x%llx, signaled_ts=%u;\n",
			tl->name, tl->fence_context, READ_ONCE(tl->last_ts));

	hgsl_debugfs_puts(s, "  }\n");
}

/* Per-client snapshot collected in the !atomic path. */
struct hgsl_client_info {
	pid_t    pid;
	char     comm[RPC_CLIENT_NAME_SIZE];
	int64_t  alloc_mem;    /* hgsl-owned, non-protected */
	int64_t  extern_mem;   /* externally imported/mapped, non-protected */
	int64_t  prot_mem;     /* protected (alloc or extern) */
};

/* Resolve pid → process name (safe in both process and softirq context). */
static void _get_comm(pid_t pid, char *comm)
{
	struct task_struct *task;

	strscpy(comm, "<unknown>", TASK_COMM_LEN);
	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (task)
		__get_task_comm(comm, TASK_COMM_LEN, task);
	rcu_read_unlock();
}

/* Print active isync timelines. */
static void _show_isyncs(struct seq_file *s, struct qcom_hgsl *hgsl)
{
	struct hgsl_isync_timeline *cur;
	uint32_t idr;
	int found = 0;

	spin_lock(&hgsl->isync_timeline_lock);
	idr_for_each_entry(&hgsl->isync_timeline_idr, cur, idr) {
		if (!found) {
			hgsl_debugfs_puts(s, "ACTIVE ISYNCS:\n");
			found = 1;
		}
		hgsl_debugfs_printf(s,
			"  %s: t_context=0x%llx, signaled_ts=%u, flags=0x%x, 64bit=%u\n",
			cur->name, cur->context, cur->last_ts,
			cur->flags, cur->is64bits);
	}
	spin_unlock(&hgsl->isync_timeline_lock);
	if (!found)
		hgsl_debugfs_puts(s, "ACTIVE ISYNCS: null\n");
}

/* Print contexts that are blocked waiting for a timestamp. */
static void _show_waiting_contexts(struct seq_file *s,
				    struct qcom_hgsl *hgsl)
{
	struct hgsl_active_wait *wait;
	int found = 0;

	spin_lock(&hgsl->active_wait_lock);
	list_for_each_entry(wait, &hgsl->active_wait_list, head) {
		if (!found) {
			hgsl_debugfs_puts(s, "ACTIVE WAITING CONTEXTS:\n");
			found = 1;
		}
		hgsl_debugfs_printf(s,
			"  context_id=%u, wait_timestamp=%u\n",
			wait->ctxt->context_id, wait->timestamp);
	}
	spin_unlock(&hgsl->active_wait_lock);
	if (!found)
		hgsl_debugfs_puts(s, "ACTIVE WAITING CONTEXTS: null\n");
}

/*
 * Collect per-client memory stats into @clients (max @max entries).
 * Returns the number of clients found.  Must be called in process context.
 */
static int _collect_clients(struct qcom_hgsl *hgsl,
			     struct hgsl_client_info *clients, int max)
{
	struct hgsl_priv *priv;
	int npids = 0;

	mutex_lock(&hgsl->mutex);
	list_for_each_entry(priv, &hgsl->active_list, node) {
		if (npids >= max)
			break;
		clients[npids].pid        = priv->pid;
		strscpy(clients[npids].comm, priv->hyp_priv.client_name,
			sizeof(clients[npids].comm));
		clients[npids].alloc_mem  = atomic64_read(&priv->alloc_mem_size);
		clients[npids].extern_mem = atomic64_read(&priv->extern_mem_size);
		clients[npids].prot_mem   = atomic64_read(&priv->prot_mem_size);
		npids++;
	}
	mutex_unlock(&hgsl->mutex);
	return npids;
}

/*
 * Print all active contexts.
 *
 * If @clients is available (process context, alloc succeeded): contexts are
 * grouped by owning client with memory totals.
 * Otherwise (atomic or alloc failure): flat list with pid/name per context.
 */
static void _show_contexts(struct seq_file *s, struct qcom_hgsl *hgsl,
			    bool atomic)
{
	struct hgsl_client_info *clients = NULL;
	struct hgsl_context *ctxt;
	int i, dev_hnd;

	if (!atomic)
		clients = hgsl_zalloc(HGSL_CONTEXT_NUM * sizeof(*clients));

	if (clients) {
		int p, npids = _collect_clients(hgsl, clients, HGSL_CONTEXT_NUM);

		hgsl_debugfs_printf(s, "\nTotal (%d) active clients\n{\n",
				    npids);
		for (p = 0; p < npids; p++) {
			hgsl_debugfs_printf(s,
				"\n  client-%d-[%s]: total=%lldKB  alloc=%lldKB  extern=%lldKB  (protected=%lldKB)\n",
				clients[p].pid, clients[p].comm,
				(clients[p].alloc_mem + clients[p].extern_mem) >> 10,
				clients[p].alloc_mem >> 10,
				clients[p].extern_mem >> 10,
				clients[p].prot_mem >> 10);
			for (dev_hnd = GSL_HANDLE_DEV0;
			     dev_hnd < (HGSL_DEVICE_NUM + 1); dev_hnd++) {
				for (i = 0; i < HGSL_CONTEXT_NUM; i++) {
					ctxt = hgsl_get_context(hgsl,
							dev_hnd, i);
					if (!ctxt)
						continue;
					if (ctxt->pid == clients[p].pid)
						_ctxt_info_show(s, ctxt,
								atomic);
					hgsl_put_context(ctxt);
				}
			}
		}
		hgsl_free(clients);
	} else {
		hgsl_debugfs_printf(s, "\nACTIVE CONTEXTS:\n{\n");
		for (dev_hnd = GSL_HANDLE_DEV0;
		     dev_hnd < (HGSL_DEVICE_NUM + 1); dev_hnd++) {
			for (i = 0; i < HGSL_CONTEXT_NUM; i++) {
				char comm[TASK_COMM_LEN];

				ctxt = hgsl_get_context(hgsl, dev_hnd, i);
				if (!ctxt)
					continue;
				_get_comm(ctxt->pid, comm);
				hgsl_debugfs_printf(s, "  pid=%d [%s]\n",
						    ctxt->pid, comm);
				_ctxt_info_show(s, ctxt, atomic);
				hgsl_put_context(ctxt);
			}
		}
	}
	hgsl_debugfs_puts(s, "}\n");
}

/* Print contexts that are in the process of being destroyed. */
static void _show_destroying_contexts(struct seq_file *s,
				       struct qcom_hgsl *hgsl, bool atomic)
{
	struct hgsl_context *ctxt;
	bool locked;
	int found = 0;

	locked = atomic ? mutex_trylock(&hgsl->destroying_ctx_list_lock)
			: (mutex_lock(&hgsl->destroying_ctx_list_lock), true);
	if (!locked) {
		hgsl_debugfs_puts(s, "DESTROYING CONTEXTS: (skipped — lock busy)\n");
		return;
	}

	list_for_each_entry(ctxt, &hgsl->destroying_ctx_list, node) {
		if (!hgsl_context_get(ctxt))
			continue;
		if (!found) {
			hgsl_debugfs_puts(s, "DESTROYING CONTEXTS:\n");
			found = 1;
		}
		_ctxt_info_show(s, ctxt, atomic);
		hgsl_put_context(ctxt);
	}
	mutex_unlock(&hgsl->destroying_ctx_list_lock);

	if (!found)
		hgsl_debugfs_puts(s, "DESTROYING CONTEXTS: null\n");
}

static void _show_mmu_state(struct seq_file *s, struct qcom_hgsl *hgsl)
{
	struct hgsl_iommu *iommu = &hgsl->mmu.iommu;
	int i;

	if (hgsl_mmu_get_mmutype(hgsl) == HGSL_MMU_TYPE_NONE)
		return;

	hgsl_debugfs_puts(s, "MMU:\n");
	for (i = 0; i < HGSL_DEVICE_NUM; i++) {
		struct hgsl_iommu_context *ctx = &iommu->user_context[i];

		hgsl_debugfs_printf(s, "  dev%d: stalled=%s  pagetables=%d\n",
				    i,
				    ctx->stalled_on_fault ? "YES" : "no",
				    atomic_read(&ctx->pagetables));
	}
}

/*
 * _hgsl_stat_show - common stat implementation.
 *
 * @s:      seq_file to write to (ignored when atomic=true)
 * @atomic: true  -> softirq context (timer deadlock dump)
 *          false -> process context (debugfs read)
 */
static void _hgsl_stat_show(struct seq_file *s, struct qcom_hgsl *hgsl,
			     bool atomic)
{
	s64 alloc = atomic64_read(&hgsl->alloc_mem_size);
	s64 ext   = atomic64_read(&hgsl->extern_mem_size);
	s64 prot  = atomic64_read(&hgsl->prot_mem_size);

	hgsl_debugfs_printf(s, "DEVICE INFO:\n"
		"{ chip_id=0x%x, fv_on=%d, use_single_pt=%d, db_off=%d,\n"
		"  default_iocoherency=%d, skip_cache_ops=%d;\n"
		"  mem: total=%lldKB  alloc=%lldKB  extern=%lldKB  (protected=%lldKB); }\n",
		hgsl->chip_id, hgsl->fv_on, hgsl->use_single_pt, hgsl->db_off,
		hgsl->cache_flags.default_iocoherency, hgsl->cache_flags.skip_cache_ops,
		(alloc + ext) >> 10,
		alloc >> 10, ext >> 10, prot >> 10);

	_show_mmu_state(s, hgsl);
	_show_isyncs(s, hgsl);
	_show_waiting_contexts(s, hgsl);
	_show_destroying_contexts(s, hgsl, atomic);
	_show_contexts(s, hgsl, atomic);
}

/*
 * hgsl_debugfs_dump_ctxts - dump a set of collected contexts plus the
 * waiting and destroying context lists.
 *
 * Called from syncobj_timer (softirq) after all relevant contexts have been
 * gathered.  Duplicate pointers in @ctxts are silently skipped.
 *
 * @ctxts:   array of context pointers to dump (may contain NULLs / dups)
 * @nctxts:  number of entries in @ctxts
 * @hgsl:    device whose waiting/destroying lists are also printed
 */
void hgsl_debugfs_dump_ctxts(struct hgsl_context **ctxts, int nctxts,
			      struct qcom_hgsl *hgsl, bool atomic)
{
	int i, j;

	for (i = 0; i < nctxts; i++) {
		bool dup = false;

		if (!ctxts[i])
			continue;
		for (j = 0; j < i; j++) {
			if (ctxts[j] == ctxts[i]) {
				dup = true;
				break;
			}
		}
		if (!dup)
			_ctxt_info_show(NULL, ctxts[i], atomic);
	}

	_show_waiting_contexts(NULL, hgsl);
	_show_destroying_contexts(NULL, hgsl, atomic);
}

static int hgsl_stat_show(struct seq_file *s, void *unused)
{
	_hgsl_stat_show(s, s->private, false);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hgsl_stat);

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
					       root, hgsl, &hgsl_stat_fops)))
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
