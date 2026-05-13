// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "hgsl.h"
#include "hgsl_debugfs.h"
#include "hgsl_dispatch.h"
#include "hgsl_trace.h"

#define DRAWQUEUE_NEXT(_i, _s) (((_i) + 1) % (_s))
/*
 * Number of commands that can be queued in a context before it sleeps
 *
 * Our code that "puts back" a command from the context is much cleaner
 * if we are sure that there will always be enough room in the ringbuffer
 * so restrict the size of the context queue to CONTEXT_DRAWQUEUE_SIZE - 1
 */
static u32 _context_drawqueue_size = CONTEXT_DRAWQUEUE_SIZE - 1;

/* Number of milliseconds to wait for the context queue to clear */
static u32 _context_queue_wait = 10000;

/* Use a kmem cache to speed up allocations for inflight command objects */
static struct kmem_cache *obj_cache;

static void hgsl_syncobj_get_fence_names(struct hgsl_drawobj_sync *syncobj,
	char *buf, size_t buf_size);

/**
 * hgsl_dispatch_requeue_drawobj() - Put a draw objet back on the context
 * queue
 * @ctxt: Pointer to the hgsl context
 * @drawobj: Pointer to the KGSL draw object to requeue
 *
 * Failure to submit a drawobj to the ringbuffer isn't the fault of the drawobj
 * being submitted so if a failure happens, push it back on the head of the
 * context queue to be reconsidered again.
 */
static inline void hgsl_dispatch_requeue_drawobj(
		struct hgsl_context *ctxt,
		struct hgsl_drawobj *drawobj)
{
	u32 prev;

	rt_mutex_lock(&ctxt->drawq_lock);

	prev = ctxt->drawq_head == 0 ? (CONTEXT_DRAWQUEUE_SIZE - 1) :
		(ctxt->drawq_head - 1);

	/*
	 * The maximum queue size always needs to be one less than the size of
	 * the draw queue so there is "room" to put the drawobj back in.
	 */

	WARN_ON(prev == ctxt->drawq_tail);

	ctxt->drawq[prev] = drawobj;
	ctxt->queued++;

	/* Reset the command queue head to reflect the newly requeued change */
	ctxt->drawq_head = prev;
	rt_mutex_unlock(&ctxt->drawq_lock);
}

static bool is_cmdobj(struct hgsl_drawobj *drawobj)
{
	return (drawobj->type & CMDOBJ_TYPE);
}

static DEFINE_RATELIMIT_STATE(_dispatch_warn_rs, 5 * HZ, 3);

static const char *drawobj_type_str(u32 type)
{
	switch (type) {
	case CMDOBJ_TYPE:      return "CMD";
	case MARKEROBJ_TYPE:   return "MARKER";
	case SYNCOBJ_TYPE:     return "SYNC";
	case TIMELINEOBJ_TYPE: return "TL";
	default:               return "?";
	}
}

static void _warn_duplicate_ts(struct hgsl_priv *priv,
				struct hgsl_context *ctxt,
				struct hgsl_drawobj **drawobj,
				u32 count, u32 user_ts)
{
	u32 k;

	if (!__ratelimit(&_dispatch_warn_rs))
		return;

	LOGW("ctx:%d ts %u <= queued_ts %u; batch[%u]:",
	     ctxt->context_id, user_ts, ctxt->queued_ts, count);
	for (k = 0; k < count; k++) {
		struct hgsl_drawobj *obj = drawobj[k];

		LOGW("  [%u] %-7s ts=%u numibs=%u", k,
		     drawobj_type_str(obj->type), obj->timestamp,
		     (obj->type & CMDOBJ_TYPE) ? CMDOBJ(obj)->numibs : 0);
	}
	hgsl_debugfs_dump_ctxts(&ctxt, 1, priv->dev, false);
}

static bool _check_context_queue(struct hgsl_context *ctxt, u32 count)
{
	/*
	 * No lock is needed here.  This function is the condition argument of
	 * wait_event_interruptible_timeout().  The wait_event mechanism
	 * guarantees that ctxt->queued is read fresh on every evaluation:
	 *
	 *  - wake_up_all() includes an smp_mb() before waking waiters, so all
	 *    writes to queued that precede the wake_up_all() are visible to the
	 *    woken thread.
	 *  - prepare_to_wait_event() (called at the top of each wait_event loop
	 *    iteration) provides a barrier that ensures the condition is
	 *    re-evaluated with up-to-date values after each wake-up.
	 *
	 * A stale read on the very first evaluation is harmless: the dispatcher
	 * calls wake_up_all() after every modification to queued, so the waiter
	 * will be re-evaluated promptly with a fresh value.
	 *
	 * Taking any lock here would corrupt the TASK_INTERRUPTIBLE state that
	 * wait_event sets: rt_mutex_lock() sets the task to TASK_UNINTERRUPTIBLE
	 * while waiting for the lock, then restores TASK_RUNNING, causing
	 * schedule_timeout() to return immediately without sleeping.
	 */
	return (ctxt->queued + count) < _context_drawqueue_size;
}

static void _pop_drawobj(struct hgsl_context *ctxt)
{
	ctxt->drawq_head = DRAWQUEUE_NEXT(ctxt->drawq_head,
		CONTEXT_DRAWQUEUE_SIZE);
	ctxt->queued--;
}

static void hgsl_syncobj_get_fence_names(struct hgsl_drawobj_sync *syncobj,
	char *buf, size_t buf_size)
{
	int i, j;
	size_t len = 0;

	buf[0] = '\0';
	if (!syncobj->synclist)
		return;
	for (i = 0; i < syncobj->numsyncs; i++) {
		struct hgsl_drawobj_sync_event *event = &syncobj->synclist[i];

		if (event->type == HGSL_CMD_SYNCPOINT_TYPE_FENCE) {
			struct event_fence_info *info = event->priv;

			for (j = 0; info && j < info->num_fences; j++)
				len += scnprintf(buf + len, buf_size - len,
					"%s%s", len ? "," : "",
					info->fences[j].name);
		} else if (event->type == HGSL_CMD_SYNCPOINT_TYPE_TIMELINE) {
			struct event_timeline_info *info = event->priv;

			for (j = 0; info && info[j].timeline; j++)
				len += scnprintf(buf + len, buf_size - len,
					"%stl_%u:%llu", len ? "," : "",
					info[j].timeline, info[j].seqno);
		}
	}
}

static int _retire_syncobj(struct qcom_hgsl *hgsl,
	struct hgsl_drawobj_sync *syncobj, struct hgsl_context *ctxt)
{

	if (!hgsl_drawobj_events_pending(syncobj)) {
		hgsl_trace(trace_syncobj_retired,
			hgsl_syncobj_get_fence_names, syncobj, syncobj);
		_pop_drawobj(ctxt);
		hgsl_drawobj_destroy(DRAWOBJ(syncobj));
		return 0;
	}

	/*
	 * If we got here, there are pending events for sync object.
	 * Start the canary timer if it hasn't been started already.
	 * The syncobj remains in the queue and will be checked again
	 * when events complete.
	 */
	if (!syncobj->timeout_jiffies) {
		syncobj->timeout_jiffies = jiffies + msecs_to_jiffies(5000);
		mod_timer(&syncobj->timer, syncobj->timeout_jiffies);
	}

	return 1;
}

static bool _marker_expired(struct hgsl_drawobj_cmd *markerobj)
{
	struct hgsl_drawobj *drawobj = DRAWOBJ(markerobj);
	bool expired;

	if (!(drawobj->flags & HGSL_DRAWOBJ_MARKER) ||
		!drawobj->context->shadow_ts)
		return false;

	if (!hgsl_context_get(drawobj->context))
		return true;

	expired = hgsl_ts32_ge(drawobj->context->event_group.processed,
			markerobj->marker_timestamp);

	hgsl_put_context(drawobj->context);

	return expired;
}

/* Only retire the timestamp. The drawobj will be destroyed later. */
static void _retire_timestamp_only(struct hgsl_drawobj *drawobj)
{
	struct qcom_hgsl *hgsl = drawobj->priv->dev;
	struct hgsl_context *ctxt = drawobj->context;

	/*
	 * shadow_ts must be available for local marker retirement.
	 * When shadow_ts is NULL the marker is sent to the GPU instead of
	 * being retired locally, so this function is never reached.
	 */
	if (!ctxt || !ctxt->shadow_ts)
		return;

	set_context_shadow_ts(ctxt, GSL_TIMESTAMP_CONSUMED,
		drawobj->timestamp);
	set_context_shadow_ts(ctxt, GSL_TIMESTAMP_RETIRED,
		drawobj->timestamp);

	/* Retire pending GPU events for the object */
	hgsl_process_event_group(hgsl, &ctxt->event_group);
}

static void _retire_timestamp(struct hgsl_drawobj *drawobj)
{
	_retire_timestamp_only(drawobj);

	hgsl_drawobj_destroy(drawobj);
}

static int _retire_markerobj(struct qcom_hgsl *hgsl,
	struct hgsl_drawobj_cmd *cmdobj,
	struct hgsl_context *ctxt)
{
	if (_marker_expired(cmdobj)) {
		set_bit(CMDOBJ_MARKER_EXPIRED, &cmdobj->priv);
		_pop_drawobj(ctxt);
		_retire_timestamp(DRAWOBJ(cmdobj));
		return 0;
	}

	/*
	 * If the marker isn't expired but the SKIP bit
	 * is set then there are real commands following
	 * this one in the queue. This means that we
	 * need to dispatch the command so that we can
	 * keep the timestamp accounting correct. If
	 * skip isn't set then we block this queue
	 * until the dependent timestamp expires
	 */

	return test_bit(CMDOBJ_SKIP, &cmdobj->priv) ? 1 : -EAGAIN;
}

static int _retire_timelineobj(struct hgsl_drawobj *drawobj,
		struct hgsl_context *ctxt)
{
	_pop_drawobj(ctxt);
	hgsl_drawobj_destroy(drawobj);
	return 0;
}

/**
 * sendcmd() - Send a drawobj to the GPU hardware
 * @dispatcher: Pointer to the adreno dispatcher struct
 * @drawobj: Pointer to the KGSL drawobj being sent
 *
 * Send a KGSL drawobj to the GPU hardware
 */
static int _sendcmd(struct qcom_hgsl *hgsl,
	struct hgsl_drawobj *drawobj)
{
	struct hgsl_context *ctxt = drawobj->context;
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;
	int ret;
	struct cmd_obj *obj;

	obj = HGSL_ZALLOC_CACHED(obj_cache, struct cmd_obj, GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	ret = hgsl_issue_drawobj(hgsl, drawobj);
	if (ret) {
		HGSL_FREE_CACHED(obj_cache, obj);
		return ret;
	}

	if (is_cmdobj(drawobj)) {
		struct hgsl_drawobj_cmd *cmdobj = CMDOBJ(drawobj);

		/* If this MARKER object is already retired, we can destroy it here */
		if ((test_bit(CMDOBJ_MARKER_EXPIRED, &cmdobj->priv))) {
			HGSL_FREE_CACHED(obj_cache, obj);
			hgsl_drawobj_destroy(drawobj);
			return 0;
		}
	}

	obj->drawobj = drawobj;
	list_add_tail(&obj->node, &dispatch->drawobj_list);
	trace_drawobj_submitted(drawobj);
	return 0;
}

/*
 * Retires all expired marker and sync objs from the context
 * queue and returns one of the below
 * a) next drawobj that needs to be sent to ringbuffer
 * b) -EAGAIN for syncobj with syncpoints pending.
 * c) -EAGAIN for markerobj whose marker timestamp has not expired yet.
 * c) NULL for no commands remaining in drawq.
 */
static struct hgsl_drawobj *_process_drawqueue_get_next_drawobj(
	struct qcom_hgsl *hgsl, struct hgsl_context *ctxt)
{
	struct hgsl_drawobj *drawobj;
	u32 i = ctxt->drawq_head;
	struct hgsl_drawobj_cmd *cmdobj;
	int ret = 0;

	if (ctxt->drawq_head == ctxt->drawq_tail)
		return NULL;

	for (i = ctxt->drawq_head; i != ctxt->drawq_tail;
			i = DRAWQUEUE_NEXT(i, CONTEXT_DRAWQUEUE_SIZE)) {

		drawobj = ctxt->drawq[i];

		if (!drawobj)
			return NULL;

		switch (drawobj->type) {
		case CMDOBJ_TYPE:
			cmdobj = CMDOBJ(drawobj);
			return drawobj;
		case SYNCOBJ_TYPE:
			ret = _retire_syncobj(hgsl, SYNCOBJ(drawobj), ctxt);
			if (ret == 1)
				return NULL;
			break;
		case MARKEROBJ_TYPE:
			ret = _retire_markerobj(hgsl, CMDOBJ(drawobj), ctxt);
			/* Special case where marker needs to be sent to GPU */
			if (ret == 1)
				return drawobj;
			break;
		case BINDOBJ_TYPE:
			/* TODO: */
			LOGW("BINDOBJ is not supported");
			break;
		case TIMELINEOBJ_TYPE:
			ret = _retire_timelineobj(drawobj, ctxt);
			break;
		default:
			ret = -EINVAL;
			break;
		}

		if (ret)
			return ERR_PTR(ret);
	}

	return NULL;
}

/**
 * hgsl_dispatch_sendcmds() - Send commands from a context to the GPU
 * @hgsl: Pointer to the hgsl device
 * @ctxt: Pointer to the adreno context to dispatch commands from
 *
 * Dequeue and send a burst of commands from the specified context to the GPU
 * Returns postive if the context needs to be put back on the pending queue
 * 0 if the context is empty or detached and negative on error
 */
static int hgsl_dispatch_sendcmds(struct qcom_hgsl *hgsl,
		struct hgsl_context *ctxt)
{
	int count = 0;
	int ret = 0;

	while (1) {
		struct hgsl_drawobj *drawobj;

		rt_mutex_lock(&ctxt->drawq_lock);
		drawobj = _process_drawqueue_get_next_drawobj(hgsl, ctxt);

		/*
		 * _process_drawqueue_get_next_drawobj returns -EAGAIN if the current
		 * drawobj has pending sync points so no more to do here.
		 * When the sync points are satisfied then the context will get
		 * reqeueued
		 */
		if (IS_ERR_OR_NULL(drawobj)) {
			if (IS_ERR(drawobj))
				ret = PTR_ERR(drawobj);
			rt_mutex_unlock(&ctxt->drawq_lock);
			break;
		}
		_pop_drawobj(ctxt);
		rt_mutex_unlock(&ctxt->drawq_lock);

		ret = _sendcmd(hgsl, drawobj);

		/* On error from _sendcmd() try to requeue the cmdobj. */
		if (ret) {
			/*
			 * TODO: -ENOENT which means that the context has been
			 * destroyed and there will be no more deliveries from
			 * here, then destroy the cmdobj.
			 */
			if (ret == -ENOENT)
				hgsl_drawobj_destroy(drawobj);
			else {
				/*
				 * If we couldn't put it on dispatch queue
				 * then return it to the context queue
				 */
				hgsl_dispatch_requeue_drawobj(
					ctxt, drawobj);
			}
			break;
		}
		count++;
	}

	/*
	 * Wake up any snoozing threads if we have consumed any real commands
	 * or marker commands and we have room in the context queue.
	 *
	 * This is a best-effort hint: a missed wake-up is benign because the
	 * sleeping thread will be woken by the next dispatch cycle or by the
	 * _context_queue_wait timeout.  A spurious wake-up is also harmless
	 * because the woken thread re-evaluates the condition under drawq_lock
	 * before proceeding.
	 */
	if (ctxt->wait_cnt > 0 &&
	    (ctxt->queued + 1) < _context_drawqueue_size)
		wake_up_all(&ctxt->drawq_wq);

	if (!ret)
		ret = count;

	/* Return error or the number of commands queued */
	return ret;
}

static int _check_for_room_in_context_drawq(
	struct hgsl_context *ctxt, u32 count)
{
	int ret = 0;

	/*
	 * There is always a possibility that dispatcher may end up pushing
	 * the last popped draw object back to the context drawqueue. Hence,
	 * we can only queue up to _context_drawqueue_size - 1 here to make
	 * sure we never let drawqueue->queued exceed _context_drawqueue_size.
	 */
	/*
	 * Use a loop (not an if) so the condition is re-checked under
	 * drawq_lock after every wake-up.  A single wake_up_all() can wake
	 * multiple waiters simultaneously; without the re-check, two waiters
	 * could both observe free slots, both proceed, and together overflow
	 * the queue — triggering the WARN_ON in hgsl_dispatch_requeue_drawobj
	 * and corrupting the circular buffer.
	 */
	while ((ctxt->queued + count) > (_context_drawqueue_size - 1) && !ret) {
		trace_ctxt_sleep(ctxt);
		ctxt->wait_cnt++;
		rt_mutex_unlock(&ctxt->drawq_lock);

		if (wait_event_interruptible_timeout(ctxt->drawq_wq,
			_check_context_queue(ctxt, count),
			msecs_to_jiffies(_context_queue_wait)) <= 0) {
			/* No room in drawqueue, drop it, let user decide. */
			LOGW("No room in queue for context: %u", ctxt->context_id);
			ret = -EAGAIN;
		}

		rt_mutex_lock(&ctxt->drawq_lock);
		ctxt->wait_cnt--;
		trace_ctxt_wake(ctxt);
	}

	return ret;
}

static void _queue_drawobj(struct hgsl_context *ctxt,
	struct hgsl_drawobj *drawobj)
{
	/* Put the command into the queue */
	ctxt->drawq[ctxt->drawq_tail] = drawobj;
	ctxt->drawq_tail =
		(ctxt->drawq_tail + 1) % CONTEXT_DRAWQUEUE_SIZE;
	ctxt->queued++;

	trace_drawobj_queued(drawobj, ctxt->queued);
}

static int _queue_cmdobj(struct hgsl_context *ctxt,
	struct hgsl_drawobj_cmd *cmdobj, uint32_t *timestamp,
	u32 user_ts)
{
	struct hgsl_drawobj *drawobj = DRAWOBJ(cmdobj);
	u32 j;
	int ret;

	ret = hgsl_db_next_timestamp(ctxt, &user_ts);
	if (ret)
		return ret;

	*timestamp = user_ts;
	drawobj->timestamp = *timestamp;

	/*
	 * If this is a real command then we need to force any markers
	 * queued before it to dispatch to keep time linear - set the
	 * skip bit so the commands get NOPed.
	 */
	j = ctxt->drawq_head;

	while (j != ctxt->drawq_tail) {
		if (ctxt->drawq[j]->type == MARKEROBJ_TYPE) {
			struct hgsl_drawobj_cmd *markerobj =
				CMDOBJ(ctxt->drawq[j]);

			set_bit(CMDOBJ_SKIP, &markerobj->priv);
		}

		j = DRAWQUEUE_NEXT(j, CONTEXT_DRAWQUEUE_SIZE);
	}

	ctxt->queued_ts = *timestamp;

	_queue_drawobj(ctxt, drawobj);
	return 0;
}

static void _queue_syncobj(struct hgsl_context *ctxt,
	struct hgsl_drawobj_sync *syncobj, uint32_t *timestamp)
{
	struct hgsl_drawobj *drawobj = DRAWOBJ(syncobj);

	hgsl_trace(trace_syncobj_queued,
		hgsl_syncobj_get_fence_names, syncobj, syncobj);

	*timestamp = 0;
	drawobj->timestamp = 0;

	_queue_drawobj(ctxt, drawobj);
}

static int _queue_markerobj(struct hgsl_context *ctxt,
	struct hgsl_drawobj_cmd *markerobj, u32 *timestamp,
	u32 user_ts)
{
	struct hgsl_drawobj *drawobj = DRAWOBJ(markerobj);
	int ret;

	ret = hgsl_db_next_timestamp(ctxt, &user_ts);
	if (ret)
		return ret;

	*timestamp = user_ts;
	drawobj->timestamp = *timestamp;

	if (ctxt->shadow_ts) {
		/*
		 * See if we can fastpath this thing - if nothing is queued
		 * and nothing is inflight retire without bothering the GPU.
		 */
		if (!ctxt->queued &&
			hgsl_ts32_ge(ctxt->event_group.processed,
				ctxt->queued_ts)) {
			ctxt->queued_ts = drawobj->timestamp;
			_retire_timestamp(drawobj);
			return 1;
		}
	} else {
		/*
		 * Currently, there is no way to set retire ts if it's hyp case,
		 * set SKIP flag to issue this marker obj directly, this can help
		 * us to bookkeeping timestamp correctly.
		 */
		set_bit(CMDOBJ_SKIP, &markerobj->priv);
	}

	/*
	 * Remember the last queued timestamp - the marker will block
	 * until that timestamp is expired (unless another command
	 * comes along and forces the marker to execute)
	 */
	markerobj->marker_timestamp = ctxt->queued_ts;
	ctxt->queued_ts = drawobj->timestamp;

	_queue_drawobj(ctxt, drawobj);
	return 0;
}

static void _queue_timelineobj(struct hgsl_context *ctxt,
		struct hgsl_drawobj *drawobj)
{
	/*
	 * This drawobj is not submitted to the GPU so use a timestamp of 0.
	 * Update the timestamp through a subsequent marker to keep userspace
	 * happy.
	 */
	drawobj->timestamp = 0;

	_queue_drawobj(ctxt, drawobj);
}

static void _retire_drawobjs(struct hgsl_context *ctxt)
{
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;
	struct cmd_obj *obj, *tmp;

	list_for_each_entry_safe(obj, tmp, &dispatch->drawobj_list,
			node) {
		struct hgsl_drawobj *drawobj = obj->drawobj;

		if (!hgsl_ts32_ge(ctxt->event_group.processed,
				drawobj->timestamp))
			continue;

		hgsl_drawobj_destroy(drawobj);
		list_del_init(&obj->node);
		HGSL_FREE_CACHED(obj_cache, obj);
	}
}

static void _warn_reclaim_pending(struct hgsl_context *ctxt)
{
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;

	if (!__ratelimit(&_dispatch_warn_rs))
		return;

	LOGE("ctx:%u [queued_ts=%u processed=%u retired_ts=%u] drawobjs pending",
	     ctxt->context_id, ctxt->queued_ts,
	     ctxt->event_group.processed,
	     get_context_retired_ts(ctxt));
	hgsl_debugfs_dump_ctxts(&ctxt, 1, dispatch->hgsl, false);
}

void hgsl_reclaim_drawobjs(struct hgsl_context *ctxt)
{
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;
	unsigned long deadline = jiffies + msecs_to_jiffies(60000);

	rt_mutex_lock(&dispatch->mutex);
	while (unlikely(!list_empty(&dispatch->drawobj_list))) {
		hgsl_process_event_group(dispatch->hgsl, &ctxt->event_group);
		_retire_drawobjs(ctxt);
		rt_mutex_unlock(&dispatch->mutex);

		if (time_after(jiffies, deadline))
			_warn_reclaim_pending(ctxt);

		usleep_range(100, 1000);
		rt_mutex_lock(&dispatch->mutex);
	}
	rt_mutex_unlock(&dispatch->mutex);
}

void hgsl_dispatch_ctxt_schedule(struct hgsl_context *ctxt)
{
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;

	kthread_queue_work(dispatch->worker, &dispatch->work);
}

/*
 * hgsl_dispatch_ctxt_issuecmds - Send pending commands for a context.
 *
 * MUST only be called from hgsl_dispatch_ctxt_work(), which clears
 * dispatch->pending before acquiring the mutex.  Calling this function
 * from any other site would leave the pending flag set, causing
 * hgsl_dispatch_queue_context() in the -EAGAIN retry path to see
 * pending=1 and drop the new reference without scheduling, leaving
 * the context permanently stuck with no retry scheduled.
 */
static void hgsl_dispatch_ctxt_issuecmds(struct hgsl_context *ctxt)
{
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;
	struct qcom_hgsl *hgsl = dispatch->hgsl;
	int ret;

	ret = hgsl_dispatch_sendcmds(hgsl, ctxt);

	/*
	 * If the dispatch queue is full then requeue the job via the
	 * standard queue path, which handles the pending flag and
	 * reference counting.  Otherwise the context either successfully
	 * submitted to the GPU or an unexpected error occurred.
	 */
	if (ret == -EINTR || ret == -EAGAIN)
		hgsl_dispatch_queue_context(ctxt);
	else if (ret < 0)
		/* An unexpected error occurred, drop the job */
		LOGE("sendcmds failed=%d", ret);

	hgsl_put_context(ctxt);
}

static void hgsl_dispatch_ctxt_work(struct kthread_work *work)
{
	struct hgsl_dispatch_context *dispatch =
			container_of(work, struct hgsl_dispatch_context, work);
	struct hgsl_context *ctxt = dispatch->ctxt;

	/*
	 * Clear the pending flag before acquiring the mutex so that
	 * concurrent callers can schedule a fresh work item if new
	 * commands arrive while we are waiting for or holding the lock.
	 */
	atomic_set(&dispatch->pending, 0);

	rt_mutex_lock(&dispatch->mutex);
	_retire_drawobjs(ctxt);
	hgsl_dispatch_ctxt_issuecmds(ctxt);
	rt_mutex_unlock(&dispatch->mutex);
}

int hgsl_dispatch_queue_context(struct hgsl_context *ctxt)
{
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;

	if (!dispatch || !hgsl_context_get(ctxt))
		return 0;

	/*
	 * Use dispatch->pending as a flag (0 = idle, 1 = scheduled).
	 * If a work item is already pending, release the extra reference
	 * immediately rather than accumulating it for the worker to drain.
	 * The pending work item will pick up any new draw objects that were
	 * added to the queue before this call.
	 */
	if (atomic_cmpxchg(&dispatch->pending, 0, 1) != 0) {
		hgsl_put_context(ctxt);
		return 0;
	}

	trace_dispatch_queue_context(ctxt);
	hgsl_dispatch_ctxt_schedule(ctxt);
	return 0;
}

int hgsl_dispatch_queue_cmds(
	struct hgsl_priv *priv, struct hgsl_context *ctxt,
	struct hgsl_drawobj *drawobj[],
	u32 count, u32 *timestamp)
{
	int ret = 0;
	u32 i, user_ts;

	/*
	 * There is always a possibility that dispatcher may end up pushing
	 * the last popped draw object back to the context drawq. Hence,
	 * we can only queue up to _context_drawqueue_size - 1 here to make
	 * sure we never let drawq->queued exceed _context_drawqueue_size.
	 */
	if (!ctxt || !count || count > _context_drawqueue_size - 1) {
		LOGE("drawobj number %u or invalid context", count);
		return -EINVAL;
	}

	rt_mutex_lock(&ctxt->drawq_lock);

	if (unlikely(READ_ONCE(ctxt->in_destroy))) {
		LOGW("ctx:%d is in destroy, skip dispatch.", ctxt->context_id);
		ret = -EINVAL;
		goto out;
	}

	ret = _check_for_room_in_context_drawq(ctxt, count);
	if (ret)
		goto out;

	user_ts = *timestamp;
	/*
	 * If there is only one drawobj in the array and it is of
	 * type SYNCOBJ_TYPE, skip comparing user_ts as it can be 0
	 */
	if (!(count == 1 && drawobj[0]->type == SYNCOBJ_TYPE) &&
		(ctxt->flags & GSL_CONTEXT_FLAG_CLIENT_GENERATED_TS)) {
		/*
		 * User specified timestamps need to be greater than the last
		 * issued timestamp in the context
		 */
		if (hgsl_ts32_ge(ctxt->queued_ts, user_ts)) {
			_warn_duplicate_ts(priv, ctxt, drawobj, count, user_ts);
			ret = -ERANGE;
			goto out;
		}
	}

	for (i = 0; i < count; i++) {
		switch (drawobj[i]->type) {
		case MARKEROBJ_TYPE:
			ret = _queue_markerobj(ctxt, CMDOBJ(drawobj[i]),
					timestamp, user_ts);
			if (ret == 1) {
				ret = 0;
				goto out;
			} else if (ret)
				goto out;
			break;
		case CMDOBJ_TYPE:
			ret = _queue_cmdobj(ctxt, CMDOBJ(drawobj[i]),
						timestamp, user_ts);
			if (ret)
				goto out;
			break;
		case SYNCOBJ_TYPE:
			_queue_syncobj(ctxt, SYNCOBJ(drawobj[i]),
						timestamp);
			break;
		case BINDOBJ_TYPE:
			/* TODO: */
			LOGW("BINDOBJ is not supported");
			break;
		case TIMELINEOBJ_TYPE:
			_queue_timelineobj(ctxt, drawobj[i]);
			break;
		default:
			ret = -EINVAL;
			goto out;
		}
	}
	rt_mutex_unlock(&ctxt->drawq_lock);

	return hgsl_dispatch_queue_context(ctxt);
out:
	rt_mutex_unlock(&ctxt->drawq_lock);
	return ret;
}

int hgsl_dispatch_ctxt_init(struct qcom_hgsl *hgsl,
	struct hgsl_context *ctxt)
{
	int ret = 0;
	struct hgsl_dispatch_context *dispatch;

	rt_mutex_lock(&ctxt->dispatch_lock);
	if (ctxt->dispatch)
		goto out;

	dispatch = hgsl_zalloc(sizeof(*dispatch));
	if (IS_ERR_OR_NULL(dispatch)) {
		ret = -ENOMEM;
		goto out;
	}

	dispatch->worker = kthread_create_worker(0, "hgsl_dp_%u_%u",
		ctxt->devhandle, ctxt->context_id);
	if (IS_ERR(dispatch->worker)) {
		ret = PTR_ERR(dispatch->worker);
		hgsl_free(dispatch);
		goto out;
	}

	rt_mutex_init(&dispatch->mutex);
	kthread_init_work(&dispatch->work, hgsl_dispatch_ctxt_work);

	sched_set_fifo(dispatch->worker->task);

	INIT_LIST_HEAD(&dispatch->drawobj_list);
	atomic_set(&dispatch->pending, 0);

	dispatch->hgsl = hgsl;
	dispatch->ctxt = ctxt;
	ctxt->dispatch = dispatch;

out:
	rt_mutex_unlock(&ctxt->dispatch_lock);
	return ret;
}

void hgsl_dispatch_ctxt_deinit(struct qcom_hgsl *hgsl,
	struct hgsl_context *ctxt)
{
	struct hgsl_dispatch_context *dispatch = ctxt->dispatch;

	/* Remove the event group from the list */
	hgsl_del_event_group(hgsl, &ctxt->event_group);

	rt_mutex_lock(&ctxt->dispatch_lock);
	if (!dispatch)
		goto out;

	if (!IS_ERR_OR_NULL(dispatch->worker))
		kthread_destroy_worker(dispatch->worker);

	hgsl_free(dispatch);
	ctxt->dispatch = NULL;

out:
	rt_mutex_unlock(&ctxt->dispatch_lock);
}

void hgsl_dispatch_deinit(struct qcom_hgsl *hgsl)
{
	hgsl_events_deinit(hgsl);
	hgsl_drawobjs_deinit();

	if (!IS_ERR_OR_NULL(obj_cache)) {
		kmem_cache_destroy(obj_cache);
		obj_cache = NULL;
	}
}

int hgsl_dispatch_init(struct qcom_hgsl *hgsl)
{
	int ret;

	obj_cache = KMEM_CACHE(cmd_obj, SLAB_HWCACHE_ALIGN);
	if (IS_ERR_OR_NULL(obj_cache)) {
		LOGW("Failed to allocate obj_cache, falling back to kzalloc.\n");
		obj_cache = NULL;
	}

	/* best-effort: cache creation failures fall back to kzalloc */
	hgsl_drawobjs_init();

	/* Set up the GPU events for the device */
	ret = hgsl_events_init(hgsl);
	if (ret) {
		LOGE("events init failed, ret %d\n", ret);
		goto err;
	}

	return 0;
err:
	hgsl_drawobjs_deinit();
	if (!IS_ERR_OR_NULL(obj_cache)) {
		kmem_cache_destroy(obj_cache);
		obj_cache = NULL;
	}
	return ret;
}
