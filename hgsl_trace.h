/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#if !defined(_HGSL_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _HGSL_TRACE_H

#undef TRACE_SYSTEM
#define TRACE_SYSTEM hgsl
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE hgsl_trace

#include <linux/tracepoint.h>

#include "hgsl_drawobj.h"

DECLARE_EVENT_CLASS(hgsl_hsync_class,
	TP_PROTO(struct hgsl_hsync_fence *hsync, char *fence_name),
	TP_ARGS(hsync, fence_name),
	TP_STRUCT__entry(
		__string(tl_name, hsync->timeline->name)
		__string(fence_name, fence_name)
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, ts)
		__field(u32, last_ts)
	),
	TP_fast_assign(
		__assign_str(tl_name);
		__assign_str(fence_name);
		__entry->devhandle = hsync->timeline->context->devhandle;
		__entry->context_id = hsync->timeline->context->context_id;
		__entry->ts = hsync->ts;
		__entry->last_ts = hsync->timeline->last_ts;
	),
	TP_printk("ctx=[%u:%u] ts=%u last_ts=%u timeline=%s fence=%s",
			__entry->devhandle, __entry->context_id,
			__entry->ts, __entry->last_ts, __get_str(tl_name),
			__get_str(fence_name)
	)
);

DEFINE_EVENT(hgsl_hsync_class, hsync_fence_create,
	TP_PROTO(struct hgsl_hsync_fence *hsync, char *fence_name),
	TP_ARGS(hsync, fence_name)
);

DEFINE_EVENT(hgsl_hsync_class, hsync_fence_signal,
	TP_PROTO(struct hgsl_hsync_fence *hsync, char *fence_name),
	TP_ARGS(hsync, fence_name)
);

DEFINE_EVENT(hgsl_hsync_class, hsync_fence_release_unsignal,
	TP_PROTO(struct hgsl_hsync_fence *hsync, char *fence_name),
	TP_ARGS(hsync, fence_name)
);

DEFINE_EVENT(hgsl_hsync_class, hsync_fence_release,
	TP_PROTO(struct hgsl_hsync_fence *hsync, char *fence_name),
	TP_ARGS(hsync, fence_name)
);

TRACE_EVENT(isync_release,
	TP_PROTO(
		u32 id
	),
	TP_ARGS(
		id
	),
	TP_STRUCT__entry(
		__field(u32, id)
	),
	TP_fast_assign(
		__entry->id = id;
	),
	TP_printk("id=%u",
		__entry->id
	)
);

DECLARE_EVENT_CLASS(hgsl_isync_class,
	TP_PROTO(u32 timeline_id, u64 timestamp),
	TP_ARGS(timeline_id, timestamp),
	TP_STRUCT__entry(
		__field(u32, timeline_id)
		__field(u64, timestamp)
	),
	TP_fast_assign(
		__entry->timeline_id = timeline_id;
		__entry->timestamp = timestamp;
	),
	TP_printk("timeline_id=%u ts=%llu",
		__entry->timeline_id, __entry->timestamp
	)
);

DEFINE_EVENT(hgsl_isync_class, isync_alloc,
	TP_PROTO(u32 timeline_id, u64 timestamp),
	TP_ARGS(timeline_id, timestamp)
);

DEFINE_EVENT(hgsl_isync_class, isync_signal,
	TP_PROTO(u32 timeline_id, u64 timestamp),
	TP_ARGS(timeline_id, timestamp)
);

DECLARE_EVENT_CLASS(hgsl_isync_fence_class,
	TP_PROTO(u32 timeline_id, u64 timestamp, char *fence_name),
	TP_ARGS(timeline_id, timestamp, fence_name),
	TP_STRUCT__entry(
		__field(u32, timeline_id)
		__field(u64, timestamp)
		__string(fence_name, fence_name)
	),
	TP_fast_assign(
		__entry->timeline_id = timeline_id;
		__entry->timestamp = timestamp;
		__assign_str(fence_name);
	),
	TP_printk("timeline_id=%u ts=%llu fence=%s",
		__entry->timeline_id, __entry->timestamp, __get_str(fence_name)
	)
);

DEFINE_EVENT(hgsl_isync_fence_class, isync_fence_alloc,
	TP_PROTO(u32 timeline_id, u64 timestamp, char *fence_name),
	TP_ARGS(timeline_id, timestamp, fence_name)
);

DEFINE_EVENT(hgsl_isync_fence_class, isync_fence_release,
	TP_PROTO(u32 timeline_id, u64 timestamp, char *fence_name),
	TP_ARGS(timeline_id, timestamp, fence_name)
);

TRACE_EVENT(drawobj_timeline,
	TP_PROTO(u32 timeline_id, u64 timepoint
	),
	TP_ARGS(timeline_id, timepoint
	),
	TP_STRUCT__entry(
		__field(u32, timeline_id)
		__field(u64, timepoint)
	),
	TP_fast_assign(
		__entry->timeline_id = timeline_id;
		__entry->timepoint = timepoint;
	),
	TP_printk("timeline_id=%u timepoint=%llu",
		__entry->timeline_id, __entry->timepoint
	)
);

TRACE_EVENT(drawobj_queued,
	TP_PROTO(struct hgsl_drawobj *drawobj, u32 queued),
	TP_ARGS(drawobj, queued),
	TP_STRUCT__entry(
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, drawq_head)
		__field(u32, drawq_tail)
		__field(u32, refcount)
		__field(u32, timestamp)
		__field(u32, queued)
		__field(u32, flags)
	),
	TP_fast_assign(
		__entry->devhandle = drawobj->context->devhandle;
		__entry->context_id = drawobj->context->context_id;
		__entry->drawq_head = drawobj->context->drawq_head;
		__entry->drawq_tail = drawobj->context->drawq_tail;
		__entry->refcount = kref_read(&drawobj->context->kref);
		__entry->timestamp = drawobj->timestamp;
		__entry->queued = queued;
		__entry->flags = drawobj->flags;
	),
	TP_printk("ctx=[%u:%u] drawq[%u-%u] refcount=%u ts=%u queued=%u flags=%s",
			__entry->devhandle, __entry->context_id, __entry->drawq_head,
			__entry->drawq_tail, __entry->refcount, __entry->timestamp,
			__entry->queued,
			__entry->flags ? __print_flags(
						__entry->flags, "|",
						{ HGSL_DRAWOBJ_MARKER, "MARKER" },
						{ HGSL_DRAWOBJ_CTX_SWITCH, "CTX_SWITCH" },
						{ HGSL_DRAWOBJ_SYNC, "SYNC" },
						{ HGSL_DRAWOBJ_END_OF_FRAME, "EOF" },
						{ HGSL_DRAWOBJ_SUBMIT_IB_LIST, "IB_LIST" }
						) : "none"
	)
);

DECLARE_EVENT_CLASS(hgsl_drawobj_class,
	TP_PROTO(struct hgsl_drawobj *drawobj),
	TP_ARGS(drawobj),
	TP_STRUCT__entry(
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, drawq_head)
		__field(u32, drawq_tail)
		__field(u32, refcount)
		__field(u32, type)
		__field(u32, timestamp)
	),
	TP_fast_assign(
		__entry->devhandle = drawobj->context->devhandle;
		__entry->context_id = drawobj->context->context_id;
		__entry->drawq_head = drawobj->context->drawq_head;
		__entry->drawq_tail = drawobj->context->drawq_tail;
		__entry->refcount = kref_read(&drawobj->context->kref);
		__entry->type = drawobj->type;
		__entry->timestamp = drawobj->timestamp;
	),
	TP_printk("ctx=[%u:%u] drawq[%u-%u] refcount=%u timestamp=%u",
		__entry->devhandle, __entry->context_id, __entry->drawq_head,
		__entry->drawq_tail, __entry->refcount, __entry->type,
        __entry->timestamp)
);

DEFINE_EVENT(hgsl_drawobj_class, drawobj_submitted,
	TP_PROTO(struct hgsl_drawobj *drawobj),
	TP_ARGS(drawobj)
);

DEFINE_EVENT(hgsl_drawobj_class, drawobj_retired,
	TP_PROTO(struct hgsl_drawobj *drawobj),
	TP_ARGS(drawobj)
);

DEFINE_EVENT(hgsl_drawobj_class, drawobj_destroy,
	TP_PROTO(struct hgsl_drawobj *drawobj),
	TP_ARGS(drawobj)
);

DECLARE_EVENT_CLASS(hgsl_syncobj_class,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj, char *fence_names),
	TP_ARGS(syncobj, fence_names),
	TP_STRUCT__entry(
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, drawq_head)
		__field(u32, drawq_tail)
		__field(u32, refcount)
		__field(u32, numsyncs)
		__field(uintptr_t, syncobj)
		__string(fence_names, fence_names)
	),
	TP_fast_assign(
		__entry->devhandle = syncobj->base.context->devhandle;
		__entry->context_id = syncobj->base.context->context_id;
		__entry->drawq_head = syncobj->base.context->drawq_head;
		__entry->drawq_tail = syncobj->base.context->drawq_tail;
		__entry->refcount = kref_read(&syncobj->base.context->kref);
		__entry->numsyncs = syncobj->numsyncs;
		__entry->syncobj = (uintptr_t)syncobj;
		__assign_str(fence_names);
	),
	TP_printk("ctx=[%u:%u] drawq[%u-%u] refcount=%u numsyncs=%u syncobj=0x%llx fence_names=%s",
		__entry->devhandle, __entry->context_id, __entry->drawq_head,
		__entry->drawq_tail, __entry->refcount, __entry->numsyncs,
		__entry->syncobj, __get_str(fence_names))
);

DEFINE_EVENT(hgsl_syncobj_class, syncobj_queued,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj, char *fence_names),
	TP_ARGS(syncobj, fence_names)
);

DEFINE_EVENT(hgsl_syncobj_class, syncobj_retired,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj, char *fence_names),
	TP_ARGS(syncobj, fence_names)
);

DECLARE_EVENT_CLASS(syncpoint_timestamp_class,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj,
		struct hgsl_context *context,
		u32 timestamp),
	TP_ARGS(syncobj, context, timestamp),
	TP_STRUCT__entry(
		__field(u32, syncobj_devhandle)
		__field(u32, syncobj_context_id)
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, timestamp)
	),
	TP_fast_assign(
		__entry->syncobj_devhandle = syncobj->base.context->devhandle;
		__entry->syncobj_context_id = syncobj->base.context->context_id;
		__entry->devhandle = context->devhandle;
		__entry->context_id = context->context_id;
		__entry->timestamp = timestamp;
	),
	TP_printk("ctx=[%u:%u] sync ctx=[%u:%u] ts=%u",
		__entry->syncobj_devhandle, __entry->syncobj_context_id,
		__entry->devhandle, __entry->context_id,
		__entry->timestamp)
);

DEFINE_EVENT(syncpoint_timestamp_class, syncpoint_timestamp,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj,
		struct hgsl_context *context,
		u32 timestamp),
	TP_ARGS(syncobj, context, timestamp)
);

DEFINE_EVENT(syncpoint_timestamp_class, syncpoint_timestamp_expire,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj,
		struct hgsl_context *context,
		u32 timestamp),
	TP_ARGS(syncobj, context, timestamp)
);

DECLARE_EVENT_CLASS(syncpoint_fence_class,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj, char *name),
	TP_ARGS(syncobj, name),
	TP_STRUCT__entry(
		__string(fence_name, name)
		__field(u32, syncobj_devhandle)
		__field(u32, syncobj_context_id)
		__field(uintptr_t, syncobj)
	),
	TP_fast_assign(
		__entry->syncobj_devhandle = syncobj->base.context->devhandle;
		__entry->syncobj_context_id = syncobj->base.context->context_id;
		__entry->syncobj = (uintptr_t)syncobj;
		__assign_str(fence_name);
	),
	TP_printk("ctx=[%u:%u] syncobj=0x%llx fence=%s",
		__entry->syncobj_devhandle, __entry->syncobj_context_id,
		__entry->syncobj, __get_str(fence_name))
);

DEFINE_EVENT(syncpoint_fence_class, syncpoint_fence,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj, char *name),
	TP_ARGS(syncobj, name)
);

DEFINE_EVENT(syncpoint_fence_class, syncpoint_fence_expire,
	TP_PROTO(struct hgsl_drawobj_sync *syncobj, char *name),
	TP_ARGS(syncobj, name)
);

DECLARE_EVENT_CLASS(event_class,
	TP_PROTO(struct hgsl_event *event),
	TP_ARGS(event),
	TP_STRUCT__entry(
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, timestamp)
		__field(u64, created)
		__field(int, result)
	),
	TP_fast_assign(
		__entry->devhandle = event->context->devhandle;
		__entry->context_id = event->context->context_id;
		__entry->timestamp = event->timestamp;
		__entry->created = event->created;
		__entry->result = event->result;
	),
	TP_printk("ctx=[%u:%u] ts=%u age=%lums result=%d",
		__entry->devhandle, __entry->context_id, __entry->timestamp,
		jiffies_to_msecs(get_jiffies_64() - __entry->created),
		__entry->result)
);

DEFINE_EVENT(event_class, retire_event_signal,
	TP_PROTO(struct hgsl_event *event),
	TP_ARGS(event)
);

DEFINE_EVENT(event_class, retire_event_cbfunc,
	TP_PROTO(struct hgsl_event *event),
	TP_ARGS(event)
);

DECLARE_EVENT_CLASS(hgsl_ctxt_class,
	TP_PROTO(struct hgsl_context *ctxt),
	TP_ARGS(ctxt),
	TP_STRUCT__entry(
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, refcount)
	),
	TP_fast_assign(
		__entry->devhandle = ctxt->devhandle;
		__entry->context_id = ctxt->context_id;
		__entry->refcount = kref_read(&ctxt->kref);
	),
	TP_printk("ctx=[%u:%u] refcount=%u", __entry->devhandle,
		__entry->context_id, __entry->refcount)
);

DEFINE_EVENT(hgsl_ctxt_class, ctxt_sleep,
	TP_PROTO(struct hgsl_context *ctxt),
	TP_ARGS(ctxt)
);

DEFINE_EVENT(hgsl_ctxt_class, ctxt_wake,
	TP_PROTO(struct hgsl_context *ctxt),
	TP_ARGS(ctxt)
);

DEFINE_EVENT(hgsl_ctxt_class, dispatch_queue_context,
	TP_PROTO(struct hgsl_context *ctxt),
	TP_ARGS(ctxt)
);

DEFINE_EVENT(hgsl_ctxt_class, ctxt_detach_drawobjs,
	TP_PROTO(struct hgsl_context *ctxt),
	TP_ARGS(ctxt)
);

DEFINE_EVENT(hgsl_ctxt_class, ctxt_release,
	TP_PROTO(struct hgsl_context *ctxt),
	TP_ARGS(ctxt)
);

TRACE_EVENT(hgsl_aux_command,
	TP_PROTO(u32 devhandle, u32 context_id, u32 numcmds,
		u32 flags, u32 timestamp
	),
	TP_ARGS(devhandle, context_id, numcmds, flags, timestamp
	),
	TP_STRUCT__entry(
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, numcmds)
		__field(u32, flags)
		__field(u32, timestamp)
	),
	TP_fast_assign(
		__entry->devhandle = devhandle;
		__entry->context_id = context_id;
		__entry->numcmds = numcmds;
		__entry->flags = flags;
		__entry->timestamp = timestamp;
	),
	TP_printk("context=[%u:%u] numcmds=%u flags=0x%x timestamp=%u",
		__entry->devhandle, __entry->context_id, __entry->numcmds,
		__entry->flags, __entry->timestamp
	)
);

TRACE_EVENT(next_timestamp,
	TP_PROTO(struct hgsl_context *ctxt, u32 timestamp, int ret
	),
	TP_ARGS(ctxt, timestamp, ret
	),
	TP_STRUCT__entry(
		__field(u32, devhandle)
		__field(u32, context_id)
		__field(u32, timestamp)
		__field(int, ret)
	),
	TP_fast_assign(
		__entry->devhandle = ctxt->devhandle;
		__entry->context_id = ctxt->context_id;
		__entry->timestamp = timestamp;
		__entry->ret = ret;
	),
	TP_printk("context=[%u:%u] timestamp=%u ret=%d",
		__entry->devhandle, __entry->context_id,
		__entry->timestamp, __entry->ret
	)
);

#endif /* _HGSL_TRACE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
