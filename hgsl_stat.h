/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef __HGSL_STAT_H
#define __HGSL_STAT_H

#include <linux/seq_file.h>

struct qcom_hgsl;
struct hgsl_context;
struct hgsl_stat_query;

/*
 * hgsl_stat_show - render the unfiltered full stat dump used by the
 * debugfs "stat" file.
 *
 * @s:    seq_file to write to
 * @hgsl: device whose contexts, waiting/destroying lists, and other
 *        stat sections are rendered
 */
void hgsl_stat_show(struct seq_file *s, struct qcom_hgsl *hgsl);

/*
 * hgsl_stat_dump_ctxts - dump a set of collected contexts plus the
 * waiting and destroying context lists.
 *
 * Called from syncobj_timer (softirq) after all relevant contexts have been
 * gathered.  Duplicate pointers in @ctxts are silently skipped.
 *
 * @ctxts:   array of context pointers to dump (may contain NULLs / dups)
 * @nctxts:  number of entries in @ctxts
 * @hgsl:    device whose waiting/destroying lists are also printed
 */
void hgsl_stat_dump_ctxts(struct hgsl_context **ctxts, int nctxts,
			  struct qcom_hgsl *hgsl, bool atomic);

/*
 * hgsl_stat_snapshot - allocate a complete stat snapshot for an explicit
 * sysfs query.
 *
 * @buf: receives the allocated snapshot buffer
 * @len: receives the valid byte count in @buf
 * @query: the query to render (must not be NULL)
 *
 * On success, @buf must be released with kvfree().
 */
int hgsl_stat_snapshot(struct qcom_hgsl *hgsl, char **buf, size_t *len,
		       const struct hgsl_stat_query *query);

#endif  /* __HGSL_STAT_H */
