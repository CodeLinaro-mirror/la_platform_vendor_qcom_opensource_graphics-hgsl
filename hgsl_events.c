// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/debugfs.h>
#include <linux/spinlock.h>

#include "hgsl.h"
#include "hgsl_trace.h"
#include "hgsl_dispatch.h"

/**
 * enum hgsl_event_results - result codes passed to an event callback when the
 * event is retired or cancelled
 * @HGSL_EVENT_RETIRED: The timestamp associated with the event retired
 * successflly
 * @HGSL_EVENT_CANCELLED: The event was cancelled before the event was fired
 */
enum hgsl_event_results {
	HGSL_EVENT_RETIRED = 1,
	HGSL_EVENT_CANCELLED = 2,
};

/*
 * Define an kmem cache for the event structures since we allocate and free them
 * so frequently
 */
static struct kmem_cache *events_cache;

static inline void signal_event(struct hgsl_event *event, int result)
{
	list_del(&event->node);
	event->result = result;

	trace_retire_event_signal(event);
	kthread_queue_work(event->hgsl->events_worker, &event->work);
}

/**
 * _hgsl_event_worker() - Work handler for processing GPU event callbacks
 * @work: Pointer to the kthread_work for the event
 *
 * Each event callback has its own kthread_work struct and is run on a event specific
 * worker thread.  This is the worker that queues up the event callback function.
 */
static void _hgsl_event_worker(struct kthread_work *work)
{
	struct hgsl_event *event = container_of(work, struct hgsl_event, work);

	event->func(event->hgsl, event->group, event->priv, event->result);

	trace_retire_event_cbfunc(event);
	hgsl_put_context(event->context);
	kmem_cache_free(events_cache, event);
}

static void _process_event_group(struct qcom_hgsl *hgsl,
		struct hgsl_event_group *group,
		bool flush)
{
	struct hgsl_event *event, *tmp;
	struct hgsl_context *ctxt;
	u32 retired;

	if (group == NULL)
		return;

	ctxt = group->context;

	/*
	 * Sanity check to be sure that we aren't racing with the context
	 * getting destroyed
	 */
	if (WARN_ON(!hgsl_context_get(ctxt)))
		return;

	spin_lock(&group->lock);

	group->readtimestamp(group->context, GSL_TIMESTAMP_RETIRED,
		&retired);

	if (!flush && hgsl_ts32_ge(group->processed, retired))
		goto out;

	list_for_each_entry_safe(event, tmp, &group->events, node) {
		if (hgsl_ts32_ge(retired, event->timestamp))
			signal_event(event, HGSL_EVENT_RETIRED);
		else if (flush)
			signal_event(event, HGSL_EVENT_CANCELLED);
	}

	group->processed = retired;
out:
	spin_unlock(&group->lock);
	hgsl_put_context(ctxt);
}

/**
 * hgsl_process_event_group() - Handle all the retired events in a group
 * @hgsl: Pointer to a HGSL device
 * @group: Pointer to a GPU events group to process
 */
void hgsl_process_event_group(struct qcom_hgsl *hgsl,
		struct hgsl_event_group *group)
{
	_process_event_group(hgsl, group, false);
}

/**
 * hgsl_flush_event_group() - flush all the events in a group by retiring the
 * ones can be retired and cancelling the ones that are pending
 * @hgsl: Pointer to a HGSL device
 * @group: Pointer to a GPU events group to process
 */
void hgsl_flush_event_group(struct qcom_hgsl *hgsl,
		struct hgsl_event_group *group)
{
	_process_event_group(hgsl, group, true);
}

/**
 * hgsl_cancel_events_timestamp() - Cancel pending events for a given timestamp
 * @hgsl: Pointer to a HGSL device
 * @group: Ponter to the GPU event group that owns the event
 * @timestamp: Registered expiry timestamp for the event
 */
void hgsl_cancel_events_timestamp(struct qcom_hgsl *hgsl,
		struct hgsl_event_group *group, u32 timestamp)
{
	struct hgsl_event *event, *tmp;

	spin_lock(&group->lock);

	list_for_each_entry_safe(event, tmp, &group->events, node) {
		if (timestamp == event->timestamp)
			signal_event(event, HGSL_EVENT_CANCELLED);
	}

	spin_unlock(&group->lock);
}

/**
 * hgsl_cancel_event() - Cancel a specific event from a group
 * @hgsl: Pointer to a HGSL device
 * @group: Pointer to the group that contains the events
 * @timestamp: Registered expiry timestamp for the event
 * @func: Registered callback for the function
 * @priv: Registered priv data for the function
 */
void hgsl_cancel_event(struct qcom_hgsl *hgsl,
		struct hgsl_event_group *group, u32 timestamp,
		hgsl_event_func func, void *priv)
{
	struct hgsl_event *event, *tmp;

	spin_lock(&group->lock);

	list_for_each_entry_safe(event, tmp, &group->events, node) {
		if (timestamp == event->timestamp && func == event->func &&
			event->priv == priv) {
			signal_event(event, HGSL_EVENT_CANCELLED);
			break;
		}
	}

	spin_unlock(&group->lock);
}

int hgsl_add_event(struct hgsl_priv *hgsl_priv, struct hgsl_event_group *group,
		u32 timestamp, hgsl_event_func func, void *priv)
{
	struct qcom_hgsl *hgsl = hgsl_priv->dev;
	u32 queued;
	struct hgsl_context *ctxt = group->context;
	struct hgsl_event *event;

	if (!func)
		return -EINVAL;

	/*
	 * If the caller is creating their own timestamps, let them schedule
	 * events in the future. Otherwise only allow timestamps that have been
	 * queued.
	 */
	if (!ctxt || !(ctxt->flags & GSL_CONTEXT_FLAG_USER_GENERATED_TS)) {
		group->readtimestamp(group->context, GSL_TIMESTAMP_QUEUED,
			&queued);
		if (hgsl_ts32_ge(timestamp, queued) > 0)
			return -EINVAL;
	}

	if (hgsl_ts32_ge(group->processed, timestamp)) {
		/* Already retired, cb directly */
		func(hgsl, group, priv, HGSL_EVENT_RETIRED);
		return 0;
	}

	event = kmem_cache_alloc(events_cache, GFP_KERNEL);
	if (event == NULL)
		return -ENOMEM;

	/* Get a reference to the context while the event is active */
	if (!hgsl_context_get(ctxt)) {
		kmem_cache_free(events_cache, event);
		return -ENOENT;
	}

	event->hgsl = hgsl;
	event->context = ctxt;
	event->timestamp = timestamp;
	event->priv = priv;
	event->func = func;
	event->created = get_jiffies_64();
	event->group = group;

	kthread_init_work(&event->work, _hgsl_event_worker);

	spin_lock(&group->lock);
	/* Recheck under lock in case it retired while we were allocating */
	if (hgsl_ts32_ge(group->processed, timestamp)) {
		event->result = HGSL_EVENT_RETIRED;
		kthread_queue_work(hgsl->events_worker, &event->work);
		spin_unlock(&group->lock);
		return 0;
	}

	/* Add the event to the group list */
	list_add_tail(&event->node, &group->events);

	spin_unlock(&group->lock);

	return 0;
}

void hgsl_process_event_groups(struct qcom_hgsl *hgsl)
{
	struct hgsl_event_group *group;

	read_lock(&hgsl->event_groups_lock);
	list_for_each_entry(group, &hgsl->event_groups, node)
		_process_event_group(hgsl, group, false);
	read_unlock(&hgsl->event_groups_lock);
}

void hgsl_del_event_group(struct qcom_hgsl *hgsl,
		struct hgsl_event_group *group)
{
	/* Check if the group is uninintalized */
	if (!group->context)
		return;

	/* Make sure that all the events have been deleted from the list */
	WARN_ON(!list_empty(&group->events));

	write_lock(&hgsl->event_groups_lock);
	list_del(&group->node);
	write_unlock(&hgsl->event_groups_lock);
}

void hgsl_add_event_group(struct qcom_hgsl *hgsl,
		struct hgsl_event_group *group, struct hgsl_context *ctxt,
		readtimestamp_func readtimestamp,
		void *priv, const char *fmt, ...)
{
	va_list args;

	WARN_ON(readtimestamp == NULL);

	spin_lock_init(&group->lock);
	INIT_LIST_HEAD(&group->events);

	group->context = ctxt;
	group->readtimestamp = readtimestamp;
	group->priv = priv;

	if (fmt) {
		va_start(args, fmt);
		vsnprintf(group->name, sizeof(group->name), fmt, args);
		va_end(args);
	}

	write_lock(&hgsl->event_groups_lock);
	list_add_tail(&group->node, &hgsl->event_groups);
	write_unlock(&hgsl->event_groups_lock);
}

void hgsl_events_deinit(struct qcom_hgsl *hgsl)
{
	struct hgsl_event_group *group, *tmp;

	hgsl_process_event_groups(hgsl);

	write_lock(&hgsl->event_groups_lock);
	list_for_each_entry_safe(group, tmp, &hgsl->event_groups, node) {
		WARN_ON(!list_empty(&group->events));
		list_del(&group->node);
	}
	write_unlock(&hgsl->event_groups_lock);

	if (!IS_ERR_OR_NULL(hgsl->events_worker)) {
		kthread_flush_worker(hgsl->events_worker);
		kthread_destroy_worker(hgsl->events_worker);
		hgsl->events_worker = NULL;
	}

	if (hgsl->lockless_wq) {
		flush_workqueue(hgsl->lockless_wq);
		destroy_workqueue(hgsl->lockless_wq);
		hgsl->lockless_wq = NULL;
	}

	if (!IS_ERR_OR_NULL(events_cache)) {
		kmem_cache_destroy(events_cache);
		events_cache = NULL;
	}
}

int hgsl_events_init(struct qcom_hgsl *hgsl)
{
	struct kthread_worker *events_worker;
	int ret = 0;

	events_cache = KMEM_CACHE(hgsl_event, 0);
	if (IS_ERR_OR_NULL(events_cache)) {
		LOGE("Failed to allocate events_cache.\n");
		return -ENOMEM;
	}

	/*
	 * The lockless workqueue is used to perform work which doesn't need to
	 * take the mutex
	 */
	hgsl->lockless_wq = alloc_workqueue("hgsl-lockless-work",
		WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!hgsl->lockless_wq) {
		ret = -ENOMEM;
		LOGE("Failed to allocate lockless workqueue.\n");
		goto err;
	}

	events_worker = kthread_create_worker(0, "hgsl-events");
	if (IS_ERR_OR_NULL(events_worker)) {
		destroy_workqueue(hgsl->lockless_wq);
		hgsl->lockless_wq = NULL;
		ret = events_worker ? PTR_ERR(events_worker) : -ENOMEM;
		LOGE("Failed to create events worker, ret=%d\n", ret);
		goto err;
	}
	sched_set_fifo(events_worker->task);

	INIT_LIST_HEAD(&hgsl->event_groups);
	rwlock_init(&hgsl->event_groups_lock);
	hgsl->events_worker = events_worker;

	return 0;
err:
	kmem_cache_destroy(events_cache);
	events_cache = NULL;
	return ret;
}
