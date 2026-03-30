// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/platform_device.h>
#include <linux/debugfs.h>

#include "hgsl.h"
#include "hgsl_debugfs.h"

static void _ctxt_info_show(struct seq_file *s, struct hgsl_context *ctxt)
{
	struct doorbell_queue *dbq;
	struct doorbell_context_queue *dbcq;
	struct hgsl_hsync_timeline *tl;
	uint32_t ts = 0;

	/*
	 * Contexts without a dispatcher have never submitted GPU work through
	 * the dispatch path.  Print a compact single-line summary so they
	 * don't clutter the output.
	 */
	if (!ctxt->dispatch) {
		seq_printf(s,
			"  ctx=[%u:%u]: { queued_ts=%u, last_ts=%u, in_destroy=%u }\n",
			ctxt->devhandle, ctxt->context_id,
			ctxt->queued_ts, ctxt->last_ts,
			READ_ONCE(ctxt->in_destroy));
		return;
	}

	/* Full detail for dispatching contexts. */
	hgsl_read_timestamp(ctxt, GSL_TIMESTAMP_RETIRED, &ts);

	seq_printf(s, "  ctx=[%u:%u]\n  {\n", ctxt->devhandle, ctxt->context_id);
	seq_printf(s,
		"    is_fe_shadow=%u, in_destroy=%u, ",
		ctxt->is_fe_shadow, READ_ONCE(ctxt->in_destroy));
	seq_printf(s,
		"dbq_info=0x%x, flags=0x%x, tcsr_idx=%d, db_signal=%u;\n",
		ctxt->dbq_info, ctxt->flags, ctxt->tcsr_idx, ctxt->db_signal);
	seq_printf(s,
		"    queued_ts=%u, last_ts=%u, retired_ts=%u, processed=%u;\n",
		ctxt->queued_ts, ctxt->last_ts, ts, ctxt->event_group.processed);
	seq_printf(s,
		"    drawq: head=%u, tail=%u, queued=%d, shadow_ts=%s;\n",
		READ_ONCE(ctxt->drawq_head), READ_ONCE(ctxt->drawq_tail),
		READ_ONCE(ctxt->queued),
		ctxt->shadow_ts ? "enabled" : "disabled");

	dbq = ctxt->dbq;
	if (dbq)
		seq_printf(s,
			"    dbq idx=%u: {\n"
			"      state=%u, tcsr_idx=%d, ibdesc_max_size=%u, seq_num=%d;\n"
			"    }\n",
			dbq->dbq_idx, dbq->state, dbq->tcsr_idx,
			dbq->ibdesc_max_size, atomic_read(&dbq->seq_num));

	dbcq = ctxt->dbcq;
	if (dbcq) {
		struct ctx_queue_header *qhdr =
			(struct ctx_queue_header *)dbcq->queue_header;

		seq_printf(s,
			"    dbcq irq_bit_idx=%d: {\n", dbcq->irq_bit_idx);
		seq_printf(s,
			"      db_signal=%u, queue_size=0x%x, indirect_ib_ts=%u, ",
			dbcq->db_signal, dbcq->queue_size, dbcq->indirect_ib_ts);
		seq_printf(s, "seq_num=%u, dbcq_export_id=%u;\n",
			dbcq->seq_num, ctxt->dbcq_export_id);
		if (qhdr)
			seq_printf(s,
				"      rptr=%u, wptr=%u;\n",
				READ_ONCE(qhdr->readIdx),
				READ_ONCE(qhdr->writeIdx));
		seq_puts(s, "    }\n");
	}

	tl = ctxt->timeline;
	if (tl)
		seq_printf(s,
			"    timeline=%s: {\n"
			"      fcontext=0x%llx, signaled_ts=%u;\n"
			"    }\n",
			tl->name, tl->fence_context, READ_ONCE(tl->last_ts));

	seq_puts(s, "  }\n");
}

static int hgsl_stat_show(struct seq_file *s, void *unused)
{
	struct qcom_hgsl *hgsl = s->private;
	struct hgsl_isync_timeline *cur;
	struct hgsl_context *ctxt;
	uint32_t idr;
	int i = 0, found = 0, dev_hnd, p, npids = 0;
	struct hgsl_client_info {
		pid_t    pid;
		int64_t  alloc_mem;
		int64_t  mapped_mem;
	} *clients;
	struct hgsl_priv *priv;
	struct hgsl_active_wait *wait;

	clients = hgsl_zalloc(HGSL_CONTEXT_NUM * sizeof(*clients));
	if (!clients)
		return -ENOMEM;

	seq_printf(s, "DEVICE INFO:\n"
		"{ default_iocoherency=%d, db_off=%d, total_mem_size=%lld; }\n",
		hgsl->cache_flags.default_iocoherency, hgsl->db_off,
		atomic64_read(&hgsl->total_mem_size));

	seq_printf(s, "\n%s\n%s\n", "ACTIVE ISYNCS:", "{");
	spin_lock(&hgsl->isync_timeline_lock);
	idr_for_each_entry(&hgsl->isync_timeline_idr, cur, idr) {
		seq_printf(s,
			"    %s: { t_context=0x%llx, signaled_ts=%u, flags=0x%x, 64bit=%u; }\n",
			cur->name, cur->context, cur->last_ts, cur->flags, cur->is64bits);
		found = 1;
	}
	spin_unlock(&hgsl->isync_timeline_lock);
	if (!found)
		seq_puts(s, "  null\n");
	seq_puts(s, "}\n");

	found = 0;
	seq_printf(s, "\n%s\n%s\n", "ACTIVE WAITING CONTEXTS:", "{");
	spin_lock(&hgsl->active_wait_lock);
	list_for_each_entry(wait, &hgsl->active_wait_list, head) {
		seq_printf(s, "    { context_id=%u, wait_timestamp=%u; }\n",
			wait->ctxt->context_id, wait->timestamp);
		found = 1;
	}
	spin_unlock(&hgsl->active_wait_lock);
	if (!found)
		seq_puts(s, "  null\n");
	seq_puts(s, "}\n");

	/*
	 * Collect the ordered list of active clients from hgsl->active_list.
	 * While holding hgsl->mutex (which keeps priv alive), also snapshot
	 * the per-client memory totals.
	 */
	mutex_lock(&hgsl->mutex);
	list_for_each_entry(priv, &hgsl->active_list, node) {
		struct rb_node *rb;
		int64_t mapped = 0;

		if (npids >= HGSL_CONTEXT_NUM)
			break;

		clients[npids].pid       = priv->pid;
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

	/* Print contexts grouped by owning pid. */
	seq_printf(s, "\n%s (%d) %s\n%s", "Total", npids, "active clients", "{");
	for (p = 0; p < npids; p++) {
		char comm[TASK_COMM_LEN] = "<unknown>";
		struct task_struct *task;
		bool header_printed = false;

		rcu_read_lock();
		task = pid_task(find_vpid(clients[p].pid), PIDTYPE_PID);
		if (task)
			get_task_comm(comm, task);
		rcu_read_unlock();

		for (dev_hnd = GSL_HANDLE_DEV0; dev_hnd < (HGSL_DEVICE_NUM + 1); dev_hnd++) {
			for (i = 0; i < HGSL_CONTEXT_NUM; i++) {
				ctxt = hgsl_get_context(hgsl, dev_hnd, i);
				if (!ctxt)
					continue;
				if (ctxt->pid != clients[p].pid) {
					hgsl_put_context(ctxt);
					continue;
				}
				if (!header_printed) {
					seq_printf(s,
						"\n  client-%d-[%s]: alloc=%lld KB, mapped=%lld KB\n",
						clients[p].pid, comm,
						clients[p].alloc_mem >> 10,
						clients[p].mapped_mem >> 10);
					header_printed = true;
				}
				_ctxt_info_show(s, ctxt);
				hgsl_put_context(ctxt);
			}
		}
	}
	seq_puts(s, "}\n");

	hgsl_free(clients);

	mutex_lock(&hgsl->destroying_ctx_list_lock);
	if (!list_empty(&hgsl->destroying_ctx_list)) {
		seq_printf(s, "\n%s\n", "Destroying Context:");
		list_for_each_entry(ctxt, &hgsl->destroying_ctx_list, node) {
			if (!hgsl_context_get(ctxt))
				continue;
			_ctxt_info_show(s, ctxt);
			hgsl_put_context(ctxt);
		}
	}
	mutex_unlock(&hgsl->destroying_ctx_list_lock);

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
