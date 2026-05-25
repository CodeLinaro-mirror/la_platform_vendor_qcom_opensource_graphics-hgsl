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
		"    is_fe_shadow=%u, in_destroy=%u, dbq_info=0x%x, flags=0x%x, tcsr_idx=%d, db_signal=%u;\n",
		ctxt->is_fe_shadow, READ_ONCE(ctxt->in_destroy),
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
	int64_t  alloc_mem;
	int64_t  mapped_mem;
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
		struct rb_node *rb;
		int64_t mapped = 0;

		if (npids >= max)
			break;
		clients[npids].pid      = priv->pid;
		clients[npids].alloc_mem = atomic64_read(&priv->total_mem_size);
		mutex_lock(&priv->lock);
		for (rb = rb_first(&priv->mem_mapped); rb; rb = rb_next(rb)) {
			struct hgsl_mem_node *mn =
				rb_entry(rb, struct hgsl_mem_node, mem_rb_node);
			mapped += mn->memdesc.size;
		}
		mutex_unlock(&priv->lock);
		clients[npids].mapped_mem = mapped;
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
			char comm[TASK_COMM_LEN];

			_get_comm(clients[p].pid, comm);
			hgsl_debugfs_printf(s,
				"\n  client-%d-[%s]: alloc=%lld KB, mapped=%lld KB\n",
				clients[p].pid, comm,
				clients[p].alloc_mem >> 10,
				clients[p].mapped_mem >> 10);
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
	if (!locked)
		return;

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
	if (!found)
		hgsl_debugfs_puts(s, "DESTROYING CONTEXTS: null\n");
	mutex_unlock(&hgsl->destroying_ctx_list_lock);
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
	hgsl_debugfs_printf(s, "DEVICE INFO:\n"
		"{ default_iocoherency=%d, skip_cache_ops=%d, db_off=%d, total_mem_size=%lld; }\n",
		hgsl->cache_flags.default_iocoherency, hgsl->cache_flags.skip_cache_ops,
		hgsl->db_off, atomic64_read(&hgsl->total_mem_size));

	_show_isyncs(s, hgsl);
	_show_waiting_contexts(s, hgsl);
	_show_contexts(s, hgsl, atomic);
	_show_destroying_contexts(s, hgsl, atomic);
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

static int hgsl_client_mem_show(struct seq_file *s, void *unused)
{
	struct hgsl_priv *priv = s->private;
	struct hgsl_mem_node *tmp = NULL;
	struct rb_node *rb = NULL;

	mutex_lock(&priv->lock);
	if (RB_EMPTY_ROOT(&priv->mem_allocated)) {
		seq_printf(s, "No entries exist for allocated memory");
		goto out;
	}

	seq_printf(s, "%16s %16s %10s %10s\n",
				"gpuaddr", "size", "flags", "type");

	for (rb = rb_first(&priv->mem_allocated); rb; rb = rb_next(rb)) {
		tmp = rb_entry(rb, struct hgsl_mem_node, mem_rb_node);
		seq_printf(s, "0x%llx 0x%16llx %10x %10d\n",
		tmp->memdesc.gpuaddr,
		tmp->memdesc.size,
		tmp->flags,
		tmp->memtype
		);
	}
out:
	mutex_unlock(&priv->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hgsl_client_mem);

static int hgsl_client_mem_mapped_show(struct seq_file *s, void *unused)
{
	struct hgsl_priv *priv = s->private;
	struct hgsl_mem_node *tmp = NULL;
	struct rb_node *rb = NULL;

	mutex_lock(&priv->lock);
	if (RB_EMPTY_ROOT(&priv->mem_mapped)) {
		seq_printf(s, "No entries exist for mapped memory");
		goto out;
	}

	seq_printf(s, "%16s %16s %10s %10s\n",
				"gpuaddr", "size", "flags", "type");

	for (rb = rb_first(&priv->mem_mapped); rb; rb = rb_next(rb)) {
		tmp = rb_entry(rb, struct hgsl_mem_node, mem_rb_node);
		seq_printf(s, "0x%llx %16llx %10x %10d\n",
					tmp->memdesc.gpuaddr,
					tmp->memdesc.size,
					tmp->flags,
					tmp->memtype
					);
	}
out:
	mutex_unlock(&priv->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hgsl_client_mem_mapped);

static int hgsl_client_memtype_show(struct seq_file *s, void *unused)
{
	struct hgsl_priv *priv = s->private;
	struct hgsl_mem_node *tmp = NULL;
	struct rb_node *rb = NULL;
	int i;
	int memtype;

	static struct {
		char *name;
		size_t size;
	} gpu_mem_types[] = {
		{"any", 0},
		{"framebuffer", 0},
		{"renderbbuffer", 0},
		{"arraybuffer", 0},
		{"elementarraybuffer", 0},
		{"vertexarraybuffer", 0},
		{"texture", 0},
		{"surface", 0},
		{"eglsurface", 0},
		{"gl", 0},
		{"cl", 0},
		{"cl_buffer_map", 0},
		{"cl_buffer_unmap", 0},
		{"cl_image_map", 0},
		{"cl_image_unmap", 0},
		{"cl_kernel_stack", 0},
		{"cmds", 0},
		{"2d", 0},
		{"egl_image", 0},
		{"egl_shadow", 0},
		{"multisample", 0},
		{"2d_ext", 0},
		{"3d_ext", 0}, /* 0x16 */
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"vk_any", 0}, /* 0x20 */
		{"vk_instance", 0},
		{"vk_physicaldevice", 0},
		{"vk_device", 0},
		{"vk_queue", 0},
		{"vk_cmdbuffer", 0},
		{"vk_devicememory", 0},
		{"vk_buffer", 0},
		{"vk_bufferview", 0},
		{"vk_image", 0},
		{"vk_imageview", 0},
		{"vk_shadermodule", 0},
		{"vk_pipeline", 0},
		{"vk_pipelinecache", 0},
		{"vk_pipelinelayout", 0},
		{"vk_sampler", 0},
		{"vk_samplerycbcrconversionkhr", 0}, /* 0x30 */
		{"vk_descriptorset", 0},
		{"vk_descriptorsetlayout", 0},
		{"vk_descriptorpool", 0},
		{"vk_fence", 0},
		{"vk_semaphore", 0},
		{"vk_event", 0},
		{"vk_querypool", 0},
		{"vk_framebuffer", 0},
		{"vk_renderpass", 0},
		{"vk_program", 0},
		{"vk_commandpool", 0},
		{"vk_surfacekhr", 0},
		{"vk_swapchainkhr", 0},
		{"vk_descriptorupdatetemplate", 0},
		{"vk_deferredoperationkhr", 0},
		{"vk_privatedataslotext", 0}, /* 0x40 */
		{"vk_debug_utils", 0},
		{"vk_tensor", 0},
		{"vk_tensorview", 0},
		{"vk_mlpipeline", 0},
		{"vk_acceleration_structure", 0},
	};

	for (i = 0; i < ARRAY_SIZE(gpu_mem_types); i++)
		gpu_mem_types[i].size = 0;

	mutex_lock(&priv->lock);
	for (rb = rb_first(&priv->mem_allocated); rb; rb = rb_next(rb)) {
		tmp = rb_entry(rb, struct hgsl_mem_node, mem_rb_node);
		memtype = GET_MEMTYPE(tmp->flags);
		if (memtype < ARRAY_SIZE(gpu_mem_types))
			gpu_mem_types[memtype].size += tmp->memdesc.size;
	}
	mutex_unlock(&priv->lock);

	seq_printf(s, "%16s %16s\n", "type", "size");
	for (i = 0; i < ARRAY_SIZE(gpu_mem_types); i++) {
		if (gpu_mem_types[i].size != 0)
			seq_printf(s, "%16s %16d\n",
				gpu_mem_types[i].name,
				gpu_mem_types[i].size);
}


	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hgsl_client_memtype);

static int hgsl_client_mem_mapped_type_show(struct seq_file *s, void *unused)
{
	struct hgsl_priv *priv = s->private;
	struct hgsl_mem_node *tmp = NULL;
	struct rb_node *rb = NULL;
	int i;
	int memtype;

	static struct {
			char *name;
			size_t size;
	} gpu_mem_types[] = {
		{"any", 0},
		{"framebuffer", 0},
		{"renderbbuffer", 0},
		{"arraybuffer", 0},
		{"elementarraybuffer", 0},
		{"vertexarraybuffer", 0},
		{"texture", 0},
		{"surface", 0},
		{"eglsurface", 0},
		{"gl", 0},
		{"cl", 0},
		{"cl_buffer_map", 0},
		{"cl_buffer_unmap", 0},
		{"cl_image_map", 0},
		{"cl_image_unmap", 0},
		{"cl_kernel_stack", 0},
		{"cmds", 0},
		{"2d", 0},
		{"egl_image", 0},
		{"egl_shadow", 0},
		{"multisample", 0},
		{"2d_ext", 0},
		{"3d_ext", 0}, /* 0x16 */
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"unknown_type", 0},
		{"vk_any", 0}, /* 0x20 */
		{"vk_instance", 0},
		{"vk_physicaldevice", 0},
		{"vk_device", 0},
		{"vk_queue", 0},
		{"vk_cmdbuffer", 0},
		{"vk_devicememory", 0},
		{"vk_buffer", 0},
		{"vk_bufferview", 0},
		{"vk_image", 0},
		{"vk_imageview", 0},
		{"vk_shadermodule", 0},
		{"vk_pipeline", 0},
		{"vk_pipelinecache", 0},
		{"vk_pipelinelayout", 0},
		{"vk_sampler", 0},
		{"vk_samplerycbcrconversionkhr", 0}, /* 0x30 */
		{"vk_descriptorset", 0},
		{"vk_descriptorsetlayout", 0},
		{"vk_descriptorpool", 0},
		{"vk_fence", 0},
		{"vk_semaphore", 0},
		{"vk_event", 0},
		{"vk_querypool", 0},
		{"vk_framebuffer", 0},
		{"vk_renderpass", 0},
		{"vk_program", 0},
		{"vk_commandpool", 0},
		{"vk_surfacekhr", 0},
		{"vk_swapchainkhr", 0},
		{"vk_descriptorupdatetemplate", 0},
		{"vk_deferredoperationkhr", 0},
		{"vk_privatedataslotext", 0}, /* 0x40 */
		{"vk_debug_utils", 0},
		{"vk_tensor", 0},
		{"vk_tensorview", 0},
		{"vk_mlpipeline", 0},
		{"vk_acceleration_structure", 0},
	};

	for (i = 0; i < ARRAY_SIZE(gpu_mem_types); i++)
		gpu_mem_types[i].size = 0;

	mutex_lock(&priv->lock);
	for (rb = rb_first(&priv->mem_mapped); rb; rb = rb_next(rb)) {
		tmp = rb_entry(rb, struct hgsl_mem_node, mem_rb_node);
		memtype = GET_MEMTYPE(tmp->flags);
		if (memtype < ARRAY_SIZE(gpu_mem_types))
			gpu_mem_types[memtype].size += tmp->memdesc.size;
	}
	mutex_unlock(&priv->lock);

	seq_printf(s, "%16s %16s\n", "type", "size");
	for (i = 0; i < ARRAY_SIZE(gpu_mem_types); i++) {
		if (gpu_mem_types[i].size != 0)
			seq_printf(s, "%16s %16d\n",
				gpu_mem_types[i].name,
				gpu_mem_types[i].size);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(hgsl_client_mem_mapped_type);

int hgsl_debugfs_client_init(struct hgsl_priv *priv)
{
	struct qcom_hgsl *hgsl = priv->dev;
	unsigned char name[16];
	struct dentry *ret;

	snprintf(name, sizeof(name), "%d", priv->pid);
	ret = debugfs_create_dir(name,
				hgsl->clients_debugfs);
	if (IS_ERR_OR_NULL(ret)) {
		LOGW("Create debugfs proc node failed.");
		priv->debugfs_client = NULL;
		return ret ? PTR_ERR(ret) : -EINVAL;
	} else
		priv->debugfs_client = ret;

	priv->debugfs_mem = debugfs_create_file("mem", 0444,
			priv->debugfs_client,
			priv,
			&hgsl_client_mem_fops);

	priv->debugfs_memtype = debugfs_create_file("obj_types", 0444,
			priv->debugfs_client,
			priv,
			&hgsl_client_memtype_fops);

	priv->debugfs_mem_mapped = debugfs_create_file("mem_mapped", 0444,
					priv->debugfs_client,
					priv,
					&hgsl_client_mem_mapped_fops);

	priv->debugfs_mem_mapped_type = debugfs_create_file("mem_mapped_obj_types", 0444,
					priv->debugfs_client,
					priv,
					&hgsl_client_mem_mapped_type_fops);

	return 0;
}

void hgsl_debugfs_client_release(struct hgsl_priv *priv)
{
	debugfs_remove_recursive(priv->debugfs_client);
}

static void events_debugfs_print_group(struct seq_file *s,
		struct hgsl_event_group *group)
{
	struct hgsl_event *event;
	struct hgsl_context *ctxt = container_of(group,
		struct hgsl_context, event_group);
	u32 retired;

	if (WARN_ON(!hgsl_context_get(ctxt)))
		return;

	/* Sanity check if the group is inintalized */
	if (WARN_ON(ctxt != group->context)) {
		hgsl_put_context(ctxt);
		return;
	}

	/*
	 * Read the retired timestamp before taking the spinlock.
	 * group->readtimestamp is hgsl_read_timestamp(), which may fall
	 * back to a hyp IPC call that can sleep, so it must not be
	 * called while holding the spinlock.
	 */
	if (group->readtimestamp(ctxt, GSL_TIMESTAMP_RETIRED, &retired))
		retired = group->processed;

	spin_lock(&group->lock);
	seq_printf(s, "%s: last=%d\n", group->name,
		group->processed);
	list_for_each_entry(event, &group->events, node) {
		seq_printf(s, "\t%u:%u age=%lums func=%ps [retired=%u]\n",
			ctxt->context_id, event->timestamp,
			jiffies_to_msecs(get_jiffies_64() - event->created),
			event->func, retired);
	}
	spin_unlock(&group->lock);
	hgsl_put_context(ctxt);
}

static int events_show(struct seq_file *s, void *unused)
{
	struct qcom_hgsl *hgsl = s->private;
	struct hgsl_event_group *group;

	seq_puts(s, "event groups:\n");
	seq_puts(s, "--------------\n");

	read_lock(&hgsl->event_groups_lock);
	list_for_each_entry(group, &hgsl->event_groups, node) {
		events_debugfs_print_group(s, group);
		seq_puts(s, "\n");
	}
	read_unlock(&hgsl->event_groups_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(events);

void hgsl_debugfs_events_init(struct qcom_hgsl *hgsl)
{
	struct dentry *dentry;

	dentry = debugfs_create_file("events", 0444, hgsl->debugfs,
		hgsl, &events_fops);
	if (IS_ERR_OR_NULL(dentry))
		LOGW("Unable to create debugfs for events");
}

void hgsl_debugfs_init(struct platform_device *pdev)
{
	struct qcom_hgsl *hgsl = platform_get_drvdata(pdev);
	struct dentry *root;

	root = debugfs_create_dir("hgsl", NULL);
	if (IS_ERR_OR_NULL(root)) {
		LOGW("Unable to create debugfs dir for hgsl");
		return;
	}
	hgsl->clients_debugfs = debugfs_create_dir("clients", root);
	if (IS_ERR_OR_NULL(hgsl->clients_debugfs)) {
		LOGW("Unable to create debugfs dir for clients");
		hgsl->clients_debugfs = NULL;
		return;
	}

	hgsl->debugfs_stat = debugfs_create_file("stat", 0444,
		root, hgsl, &hgsl_stat_fops);
	if (IS_ERR_OR_NULL(hgsl->debugfs_stat)) {
		LOGW("Unable to create debugfs state");
		hgsl->debugfs_stat = NULL;
	}

	hgsl->debugfs = root;
	hgsl_debugfs_events_init(hgsl);
}

void hgsl_debugfs_release(struct platform_device *pdev)
{
	struct qcom_hgsl *hgsl = platform_get_drvdata(pdev);

	debugfs_remove_recursive(hgsl->debugfs);
}
