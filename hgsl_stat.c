// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "hgsl.h"
#include "hgsl_dispatch.h"
#include "hgsl_stat.h"

/* A NULL seq_file preserves the softirq/timer LOGE() dump path. */
#define hgsl_stat_printf(s, fmt, ...) \
	do { \
		if ((s) == NULL) \
			LOGE(fmt, ##__VA_ARGS__); \
		else \
			seq_printf(s, fmt, ##__VA_ARGS__); \
	} while (0)

#define hgsl_stat_puts(s, str) \
	do { \
		if ((s) == NULL) \
			LOGE("%s", str); \
		else \
			seq_puts(s, str); \
	} while (0)

static bool _ctxt_matches_query(struct hgsl_context *ctxt,
				const struct hgsl_stat_query *query)
{
	switch (query->scope) {
	case HGSL_STAT_SCOPE_PID:
		return ctxt->pid == query->pid;
	case HGSL_STAT_SCOPE_CONTEXT:
		return ctxt->devhandle == query->devhandle &&
			ctxt->context_id == query->context_id;
	case HGSL_STAT_SCOPE_ALL:
	default:
		return true;
	}
}

static void _ctxt_info_show(struct seq_file *s,
			    struct hgsl_context *ctxt, bool atomic,
			    bool summary)
{
	struct doorbell_queue *dbq;
	struct doorbell_context_queue *dbcq;
	struct hgsl_hsync_timeline *tl;
	u32 ts = 0;

	/* Preserve the original behavior for non-dispatch contexts. */
	if (!ctxt->dispatch && !summary) {
		hgsl_stat_printf(s,
			"  ctx=[%u:%u]: queued_ts=%u, last_ts=%u, in_destroy=%u\n",
			ctxt->devhandle, ctxt->context_id,
			ctxt->queued_ts, ctxt->last_ts,
			READ_ONCE(ctxt->in_destroy));
		return;
	}

	if (atomic) {
		if (ctxt->shadow_ts)
			ts = get_context_retired_ts(ctxt);
	} else {
		hgsl_read_timestamp(ctxt, GSL_TIMESTAMP_RETIRED, &ts);
	}

	if (summary) {
		hgsl_stat_printf(s,
			"  ctx=[%u:%u] pid=%d queued_ts=%u last_ts=%u retired_ts=%u in_destroy=%u\n",
			ctxt->devhandle, ctxt->context_id, ctxt->pid,
			ctxt->queued_ts, ctxt->last_ts, ts,
			READ_ONCE(ctxt->in_destroy));
		return;
	}

	hgsl_stat_printf(s, "  ctx=[%u:%u] {\n",
		ctxt->devhandle, ctxt->context_id);
	hgsl_stat_printf(s,
		"    is_fe_shadow=%u, in_destroy=%u, dbq_info=0x%x, flags=0x%x, tcsr_idx=%d, db_signal=%u;\n",
		ctxt->is_fe_shadow, READ_ONCE(ctxt->in_destroy),
		ctxt->dbq_info, ctxt->flags, ctxt->tcsr_idx, ctxt->db_signal);
	{
		char submitted_ts_str[32];

		/* rt_mutex_trylock() cannot be used from timer/softirq context. */
		if (atomic) {
			strscpy(submitted_ts_str, "unavailable",
				sizeof(submitted_ts_str));
		} else if (!rt_mutex_trylock(&ctxt->dispatch_lock)) {
			strscpy(submitted_ts_str, "busy",
				sizeof(submitted_ts_str));
		} else {
			struct hgsl_dispatch_context *dispatch = ctxt->dispatch;

			if (!dispatch || !rt_mutex_trylock(&dispatch->mutex)) {
				strscpy(submitted_ts_str, "busy",
					sizeof(submitted_ts_str));
			} else {
				if (list_empty(&dispatch->drawobj_list))
					strscpy(submitted_ts_str, "all-submitted",
						sizeof(submitted_ts_str));
				else
					snprintf(submitted_ts_str,
						sizeof(submitted_ts_str), "%u",
						list_last_entry(&dispatch->drawobj_list,
							struct cmd_obj, node)->drawobj->timestamp);
				rt_mutex_unlock(&dispatch->mutex);
			}
			rt_mutex_unlock(&ctxt->dispatch_lock);
		}
		hgsl_stat_printf(s,
			"    queued_ts=%u, submitted_ts=%s, last_ts=%u, retired_ts=%u, processed=%u;\n",
			ctxt->queued_ts, submitted_ts_str,
			ctxt->last_ts, ts, ctxt->event_group.processed);
	}
	hgsl_stat_printf(s,
		"    drawq: head=%u, tail=%u, queued=%d, shadow_ts=%s;\n",
		READ_ONCE(ctxt->drawq_head), READ_ONCE(ctxt->drawq_tail),
		READ_ONCE(ctxt->queued),
		ctxt->shadow_ts ? "enabled" : "disabled");

	dbq = ctxt->dbq;
	if (dbq)
		hgsl_stat_printf(s,
			"    dbq idx=%u: state=%u, tcsr_idx=%d, ibdesc_max_size=%u, seq_num=%d;\n",
			dbq->dbq_idx, dbq->state, dbq->tcsr_idx,
			dbq->ibdesc_max_size, atomic_read(&dbq->seq_num));

	dbcq = ctxt->dbcq;
	if (dbcq) {
		struct ctx_queue_header *qhdr =
			(struct ctx_queue_header *)dbcq->queue_header;

		hgsl_stat_printf(s,
			"    dbcq irq_bit_idx=%d: db_signal=%u, queue_size=0x%x, indirect_ib_ts=%u, seq_num=%u, dbcq_export_id=%u;\n",
			dbcq->irq_bit_idx, dbcq->db_signal, dbcq->queue_size,
			dbcq->indirect_ib_ts, dbcq->seq_num, ctxt->dbcq_export_id);
		if (qhdr)
			hgsl_stat_printf(s, "    dbcq rptr=%u, wptr=%u;\n",
				READ_ONCE(qhdr->readIdx), READ_ONCE(qhdr->writeIdx));
	}

	tl = ctxt->timeline;
	if (tl)
		hgsl_stat_printf(s,
			"    timeline=%s: fcontext=0x%llx, signaled_ts=%u;\n",
			tl->name, tl->fence_context, READ_ONCE(tl->last_ts));

	hgsl_stat_puts(s, "  }\n");
}

struct hgsl_client_info {
	pid_t    pid;
	char     comm[RPC_CLIENT_NAME_SIZE];
	int64_t  alloc_mem;
	int64_t  extern_mem;
	int64_t  prot_mem;
};

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

static void _show_isyncs(struct seq_file *s,
			 struct qcom_hgsl *hgsl)
{
	struct hgsl_isync_timeline *cur;
	uint32_t idr;
	int found = 0;

	spin_lock(&hgsl->isync_timeline_lock);
	idr_for_each_entry(&hgsl->isync_timeline_idr, cur, idr) {
		if (!found) {
			hgsl_stat_puts(s, "ACTIVE ISYNCS:\n");
			found = 1;
		}
		hgsl_stat_printf(s,
			"  %s: t_context=0x%llx, signaled_ts=%llu, flags=0x%x, 64bit=%u\n",
			cur->name, cur->context, cur->last_ts,
			cur->flags, cur->is64bits);
	}
	spin_unlock(&hgsl->isync_timeline_lock);
	if (!found)
		hgsl_stat_puts(s, "ACTIVE ISYNCS: null\n");
}

static void _show_waiting_contexts(struct seq_file *s,
				   struct qcom_hgsl *hgsl,
				   const struct hgsl_stat_query *query,
				   bool atomic)
{
	struct hgsl_active_wait *wait;
	int found = 0;

	if (atomic) {
		hgsl_stat_puts(s, "ACTIVE WAITING CONTEXTS: unavailable\n");
		return;
	}

	spin_lock(&hgsl->active_wait_lock);
	list_for_each_entry(wait, &hgsl->active_wait_list, head) {
		if (!_ctxt_matches_query(wait->ctxt, query))
			continue;
		if (!found) {
			hgsl_stat_puts(s, "ACTIVE WAITING CONTEXTS:\n");
			found = 1;
		}
		hgsl_stat_printf(s, "  context_id=%u, wait_timestamp=%u\n",
			wait->ctxt->context_id, wait->timestamp);
	}
	spin_unlock(&hgsl->active_wait_lock);
	if (!found)
		hgsl_stat_puts(s, "ACTIVE WAITING CONTEXTS: null\n");
}

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

static void _show_query_contexts(struct seq_file *s,
				 struct qcom_hgsl *hgsl, bool atomic,
				 const struct hgsl_stat_query *query)
{
	struct hgsl_context *ctxt;
	int i, dev_hnd;

	if (query->scope == HGSL_STAT_SCOPE_CONTEXT) {
		ctxt = hgsl_get_context(hgsl, query->devhandle, query->context_id);
		if (!ctxt)
			return;

		if (_ctxt_matches_query(ctxt, query))
			_ctxt_info_show(s, ctxt, atomic, !query->detail);
		hgsl_put_context(ctxt);
		return;
	}

	for (dev_hnd = GSL_HANDLE_DEV0;
	     dev_hnd < (HGSL_DEVICE_NUM + 1); dev_hnd++) {
		for (i = 0; i < HGSL_CONTEXT_NUM; i++) {
			ctxt = hgsl_get_context(hgsl, dev_hnd, i);
			if (!ctxt)
				continue;
			if (_ctxt_matches_query(ctxt, query))
				_ctxt_info_show(s, ctxt, atomic,
						!query->detail);
			hgsl_put_context(ctxt);
		}
	}
}

static void _show_contexts(struct seq_file *s,
			   struct qcom_hgsl *hgsl, bool atomic,
			   const struct hgsl_stat_query *query)
{
	struct hgsl_client_info *clients = NULL;
	struct hgsl_context *ctxt;
	int i, dev_hnd;

	if (query->scope != HGSL_STAT_SCOPE_ALL || !query->detail) {
		hgsl_stat_puts(s, "\nACTIVE CONTEXTS:\n{\n");
		_show_query_contexts(s, hgsl, atomic, query);
		hgsl_stat_puts(s, "}\n");
		return;
	}

	if (!atomic)
		clients = hgsl_zalloc(HGSL_CONTEXT_NUM * sizeof(*clients));

	if (clients) {
		int p, npids = _collect_clients(hgsl, clients, HGSL_CONTEXT_NUM);

		hgsl_stat_printf(s, "\nTotal (%d) active clients\n{\n", npids);
		for (p = 0; p < npids; p++) {
			hgsl_stat_printf(s,
				"\n  client-%d-[%s]: total=%lldKB  alloc=%lldKB  extern=%lldKB  (protected=%lldKB)\n",
				clients[p].pid, clients[p].comm,
				(clients[p].alloc_mem + clients[p].extern_mem) >> 10,
				clients[p].alloc_mem >> 10,
				clients[p].extern_mem >> 10,
				clients[p].prot_mem >> 10);
			for (dev_hnd = GSL_HANDLE_DEV0;
			     dev_hnd < (HGSL_DEVICE_NUM + 1); dev_hnd++) {
				for (i = 0; i < HGSL_CONTEXT_NUM; i++) {
					ctxt = hgsl_get_context(hgsl, dev_hnd, i);
					if (!ctxt)
						continue;
					if (ctxt->pid == clients[p].pid)
						_ctxt_info_show(s, ctxt, atomic, false);
					hgsl_put_context(ctxt);
				}
			}
		}
		hgsl_free(clients);
	} else {
		hgsl_stat_puts(s, "\nACTIVE CONTEXTS:\n{\n");
		for (dev_hnd = GSL_HANDLE_DEV0;
		     dev_hnd < (HGSL_DEVICE_NUM + 1); dev_hnd++) {
			for (i = 0; i < HGSL_CONTEXT_NUM; i++) {
				char comm[TASK_COMM_LEN];

				ctxt = hgsl_get_context(hgsl, dev_hnd, i);
				if (!ctxt)
					continue;
				_get_comm(ctxt->pid, comm);
				hgsl_stat_printf(s, "  pid=%d [%s]\n",
						ctxt->pid, comm);
				_ctxt_info_show(s, ctxt, atomic, false);
				hgsl_put_context(ctxt);
			}
		}
	}
	hgsl_stat_puts(s, "}\n");
}

static void _show_destroying_contexts(struct seq_file *s,
				      struct qcom_hgsl *hgsl, bool atomic,
				      const struct hgsl_stat_query *query)
{
	struct hgsl_context *ctxt;
	bool locked;
	int found = 0;

	locked = atomic ? mutex_trylock(&hgsl->destroying_ctx_list_lock)
			: (mutex_lock(&hgsl->destroying_ctx_list_lock), true);
	if (!locked) {
		hgsl_stat_puts(s, "DESTROYING CONTEXTS: (skipped - lock busy)\n");
		return;
	}

	list_for_each_entry(ctxt, &hgsl->destroying_ctx_list, node) {
		if (!hgsl_context_get(ctxt))
			continue;
		if (_ctxt_matches_query(ctxt, query)) {
			if (!found) {
				hgsl_stat_puts(s, "DESTROYING CONTEXTS:\n");
				found = 1;
			}
			_ctxt_info_show(s, ctxt, atomic, !query->detail);
		}
		hgsl_put_context(ctxt);
	}
	mutex_unlock(&hgsl->destroying_ctx_list_lock);

	if (!found)
		hgsl_stat_puts(s, "DESTROYING CONTEXTS: null\n");
}

static void _show_mmu_state(struct seq_file *s,
			    struct qcom_hgsl *hgsl)
{
	struct hgsl_iommu *iommu = &hgsl->mmu.iommu;
	int i;

	if (hgsl_mmu_get_mmutype(hgsl) == HGSL_MMU_TYPE_NONE)
		return;

	hgsl_stat_puts(s, "MMU:\n");
	for (i = 0; i < HGSL_DEVICE_NUM; i++) {
		struct hgsl_iommu_context *ctx = &iommu->user_context[i];

		hgsl_stat_printf(s, "  dev%d: stalled=%s  pagetables=%d\n",
			i, ctx->stalled_on_fault ? "YES" : "no",
			atomic_read(&ctx->pagetables));
	}
}

static void _hgsl_stat_show_query(struct seq_file *s,
				  struct qcom_hgsl *hgsl, bool atomic,
				  const struct hgsl_stat_query *query)
{
	s64 alloc = atomic64_read(&hgsl->alloc_mem_size);
	s64 ext = atomic64_read(&hgsl->extern_mem_size);
	s64 prot = atomic64_read(&hgsl->prot_mem_size);

	hgsl_stat_printf(s, "DEVICE INFO:\n"
		"{ chip_id=0x%x, fv_on=%d, use_single_pt=%d, db_off=%d,\n"
		"  default_iocoherency=%d, skip_cache_ops=%d;\n"
		"  mem: total=%lldKB  alloc=%lldKB  extern=%lldKB  (protected=%lldKB); }\n",
		hgsl->chip_id, hgsl->fv_on, hgsl->use_single_pt, hgsl->db_off,
		hgsl->cache_flags.default_iocoherency,
		hgsl->cache_flags.skip_cache_ops,
		(alloc + ext) >> 10, alloc >> 10, ext >> 10, prot >> 10);

	/* Isync timelines are global and cannot be associated with a context. */
	_show_mmu_state(s, hgsl);
	if (query->scope == HGSL_STAT_SCOPE_ALL)
		_show_isyncs(s, hgsl);
	_show_waiting_contexts(s, hgsl, query, atomic);
	_show_destroying_contexts(s, hgsl, atomic, query);
	_show_contexts(s, hgsl, atomic, query);
}

void hgsl_stat_show(struct seq_file *s, struct qcom_hgsl *hgsl)
{
	const struct hgsl_stat_query query = {
		.scope = HGSL_STAT_SCOPE_ALL,
		.detail = true,
	};

	_hgsl_stat_show_query(s, hgsl, false, &query);
}

void hgsl_stat_dump_ctxts(struct hgsl_context **ctxts, int nctxts,
			  struct qcom_hgsl *hgsl, bool atomic)
{
	const struct hgsl_stat_query query = {
		.scope = HGSL_STAT_SCOPE_ALL,
		.detail = true,
	};
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
			_ctxt_info_show(NULL, ctxts[i], atomic, false);
	}

	_show_waiting_contexts(NULL, hgsl, &query, atomic);
	_show_destroying_contexts(NULL, hgsl, atomic, &query);
}

int hgsl_stat_snapshot(struct qcom_hgsl *hgsl, char **buf,
			size_t *len,
			const struct hgsl_stat_query *query)
{
	size_t size = PAGE_SIZE;

	for (;;) {
		/*
		 * Use seq_file only as an in-memory writer. The output helpers below
		 * are limited to seq_printf(), seq_puts(), and seq_has_overflowed(),
		 * which only use the zeroed buffer state initialized here.
		 */
		struct seq_file s = { };
		char *snapshot;

		snapshot = kvmalloc(size, GFP_KERNEL);
		if (!snapshot)
			return -ENOMEM;

		s.buf = snapshot;
		s.size = size;
		_hgsl_stat_show_query(&s, hgsl, false, query);
		if (!seq_has_overflowed(&s)) {
			*buf = snapshot;
			*len = s.count;
			return 0;
		}

		kvfree(snapshot);
		if (size > SIZE_MAX / 2)
			return -EOVERFLOW;
		size *= 2;
	}
}
