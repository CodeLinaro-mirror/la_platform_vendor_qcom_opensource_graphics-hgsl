// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "hgsl.h"
#include "hgsl_stat.h"
#include "hgsl_sysfs.h"

struct hgsl_sysfs_client_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj,
				struct hgsl_sysfs_client_attr *attr,
				char *buf);
	ssize_t (*store)(struct kobject *kobj,
				struct hgsl_sysfs_client_attr *attr,
				const char *buf,
			ssize_t count);
};

struct hgsl_sysfs_client_malloc_attr {
	struct hgsl_sysfs_client_attr attr;
	ssize_t (*show)(struct hgsl_priv *priv, char *buf);
};

static ssize_t hgsl_sysfs_client_malloc_show(struct hgsl_priv *priv,
						char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			atomic64_read(&priv->total_mem_size));
}

static ssize_t hgsl_sysfs_client_show(struct kobject *kobj,
		struct hgsl_sysfs_client_attr *_attr, char *buf)
{
	struct hgsl_sysfs_client_malloc_attr *client_attrib =
		container_of(_attr, struct hgsl_sysfs_client_malloc_attr, attr);
	struct hgsl_priv *priv =
		container_of(kobj, struct hgsl_priv, sysfs_client);

	return client_attrib->show(priv, buf);
}

static struct hgsl_sysfs_client_malloc_attr hgsl_sysfs_client_malloc_attr = {
	.attr = __ATTR(mem_alloc, 0444, hgsl_sysfs_client_show, NULL),
	.show = hgsl_sysfs_client_malloc_show,
};

static struct attribute *malloc_entry_attrs[] = {
	&hgsl_sysfs_client_malloc_attr.attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(malloc_entry);

static ssize_t hgsl_malloc_sysfs_show(struct kobject *kobj,
		struct attribute *_attr, char *buf)
{
	struct hgsl_sysfs_client_attr *pattrib =
		container_of(_attr, struct hgsl_sysfs_client_attr, attr);

	return pattrib->show(kobj, pattrib, buf);
}

static const struct sysfs_ops hgsl_malloc_sysfs_ops = {
	.show = hgsl_malloc_sysfs_show,
};

static struct kobj_type hgsl_sysfs_client_type = {
	.sysfs_ops = &hgsl_malloc_sysfs_ops,
	.default_groups = malloc_entry_groups,
};

int hgsl_sysfs_client_init(struct hgsl_priv *priv)
{
	int ret = 0;
	struct qcom_hgsl *hgsl = priv->dev;
	char name[9];

	snprintf(name, sizeof(name), "%d", priv->pid);

	ret = kobject_init_and_add(&priv->sysfs_client,
			&hgsl_sysfs_client_type,
			hgsl->clients_sysfs,
			name);

	if (unlikely(ret != 0))
		pr_warn("Create sysfs proc node failed.\n");

	return ret;
}

void hgsl_sysfs_client_release(struct hgsl_priv *priv)
{
	kobject_put(&priv->sysfs_client);
}

/* Global GPU memory usage */
static ssize_t total_mem_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qcom_hgsl *hgsl = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%lld\n",
			atomic64_read(&hgsl->total_mem_size));
}

static DEVICE_ATTR_RO(total_mem);

/*
 * stat_filter_token_len - length of the run of non-whitespace bytes
 * starting at @s (possibly zero if @s itself is whitespace or NUL).
 */
static size_t stat_filter_token_len(const char *s)
{
	size_t len = 0;

	while (s[len] && !isspace((unsigned char)s[len]))
		len++;

	return len;
}

/*
 * stat_filter_parse_uint_token - strictly parse an exact @len-byte
 * decimal token (no sign, no embedded/trailing characters) into @out.
 *
 * Unlike sscanf()/vsscanf() %u, kstrtouint() reports overflow as
 * -ERANGE instead of silently truncating the parsed value to 32 bits,
 * so out-of-range input is rejected rather than aliased to some
 * smaller in-range value.
 */
static int stat_filter_parse_uint_token(const char *tok, size_t len,
					 unsigned int *out)
{
	char buf[32];

	/*
	 * Reject empty tokens (e.g. the whitespace right after "ctxt="
	 * in "ctxt= 1:2"), a leading sign (decimal fields here have
	 * none), and tokens too long to hold any valid u32 -- copying
	 * into a bounded stack buffer before calling kstrtouint() also
	 * means @tok need not be NUL-terminated at a token boundary.
	 */
	if (len == 0 || len >= sizeof(buf) || tok[0] == '+' || tok[0] == '-')
		return -EINVAL;

	memcpy(buf, tok, len);
	buf[len] = '\0';

	return kstrtouint(buf, 10, out) ? -EINVAL : 0;
}

/*
 * stat_filter_parse_pid_token - strictly parse an exact @len-byte
 * signed decimal token (no embedded/trailing characters, no leading
 * '+') into @out, rejecting negative and overflowing values.
 *
 * kstrtoint() reports overflow as -ERANGE rather than truncating, so
 * e.g. "-4294967296" (which truncates to 0 under a naive 32-bit parse)
 * is rejected instead of being silently accepted as "pid=0".
 */
static int stat_filter_parse_pid_token(const char *tok, size_t len, int *out)
{
	char buf[32];
	int val;

	if (len == 0 || len >= sizeof(buf) || tok[0] == '+')
		return -EINVAL;

	memcpy(buf, tok, len);
	buf[len] = '\0';

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;
	if (val < 0)
		return -EINVAL;

	*out = val;
	return 0;
}

/*
 * stat_filter_parse - parse a complete stat_filter command into @query.
 *
 * Accepted forms (exactly, ignoring the trailing newline sysfs writes
 * normally supply):
 *
 *   all summary
 *   all full
 *   pid=<non-negative decimal> summary
 *   pid=<non-negative decimal> full
 *   ctxt=<decimal devhandle>:<decimal context_id> summary
 *   ctxt=<decimal devhandle>:<decimal context_id> full
 *
 * The selector and the trailing "summary"/"full" token must be
 * separated by real whitespace (one or more spaces/tabs); that
 * separator, and any trailing whitespace/newline after the format
 * token, may be arbitrary in length. No whitespace is permitted
 * anywhere inside "pid=<...>" or on either side of the ":" in
 * "ctxt=<...>:<...>" -- those numeric fields are each parsed as a
 * single unbroken token via kstrtouint()/kstrtoint(), which also
 * rejects overflowing values (-ERANGE) instead of the truncate-and-
 * silently-accept behavior sscanf()/vsscanf() has for out-of-range
 * %u/%d input.
 *
 * @query is only assigned on success (0 return). Any other input,
 * including the former incremental commands, missing/extra tokens,
 * out-of-range or overflowing values, whitespace inside a numeric
 * field, or tokens concatenated without a real whitespace separator
 * (e.g. "allsummary", "pid=5full", "ctxt=1:2full"), returns -EINVAL
 * and leaves @query untouched.
 */
static int stat_filter_parse(const char *buf, struct hgsl_stat_query *query)
{
	struct hgsl_stat_query next = { };
	char format[8];
	const char *rest;
	int prefix_len = 0;
	int format_len = 0;
	bool detail;

	if (!strncmp(buf, "all", 3)) {
		next.scope = HGSL_STAT_SCOPE_ALL;
		prefix_len = 3;
	} else if (!strncmp(buf, "pid=", 4)) {
		const char *tok = buf + 4;
		size_t tok_len = stat_filter_token_len(tok);
		int pid;

		if (stat_filter_parse_pid_token(tok, tok_len, &pid))
			return -EINVAL;

		next.scope = HGSL_STAT_SCOPE_PID;
		next.pid = pid;
		prefix_len = 4 + (int)tok_len;
	} else if (!strncmp(buf, "ctxt=", 5)) {
		const char *tok = buf + 5;
		size_t tok_len = stat_filter_token_len(tok);
		const char *colon = tok_len ? memchr(tok, ':', tok_len) : NULL;
		unsigned int devhandle, context_id;
		size_t dev_len, ctx_len;

		if (!colon)
			return -EINVAL;

		dev_len = colon - tok;
		ctx_len = tok_len - dev_len - 1;

		if (stat_filter_parse_uint_token(tok, dev_len, &devhandle) ||
		    stat_filter_parse_uint_token(colon + 1, ctx_len, &context_id))
			return -EINVAL;

		if (devhandle < GSL_HANDLE_DEV0 || devhandle > HGSL_DEVICE_NUM ||
		    context_id >= HGSL_CONTEXT_NUM)
			return -EINVAL;

		next.scope = HGSL_STAT_SCOPE_CONTEXT;
		next.devhandle = devhandle;
		next.context_id = context_id;
		prefix_len = 5 + (int)tok_len;
	} else {
		return -EINVAL;
	}

	/*
	 * sscanf()/vsscanf() treat a literal space in the format string as
	 * "zero or more whitespace characters in the input," so appending
	 * " %s" directly to the scans above would let "allsummary",
	 * "pid=5full", and "ctxt=1:2full" (no separating whitespace at all)
	 * parse successfully. Require a real whitespace separator here
	 * explicitly instead of relying on sscanf() to enforce it.
	 */
	if (!isspace((unsigned char)buf[prefix_len]))
		return -EINVAL;

	rest = buf + prefix_len;
	while (isspace((unsigned char)*rest))
		rest++;

	if (sscanf(rest, "%7s%n", format, &format_len) != 1)
		return -EINVAL;

	/* Reject any further non-whitespace (trailing) token. */
	rest += format_len;
	while (isspace((unsigned char)*rest))
		rest++;
	if (*rest != '\0')
		return -EINVAL;

	if (!strcmp(format, "full"))
		detail = true;
	else if (!strcmp(format, "summary"))
		detail = false;
	else
		return -EINVAL;

	next.detail = detail;
	*query = next;
	return 0;
}

static ssize_t stat_filter_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct qcom_hgsl *hgsl = dev_get_drvdata(dev);
	struct hgsl_stat_state *state = &hgsl->stat;
	const char *format;
	ssize_t ret;

	mutex_lock(&state->lock);
	format = state->query.detail ? "full" : "summary";
	switch (state->query.scope) {
	case HGSL_STAT_SCOPE_PID:
		ret = scnprintf(buf, PAGE_SIZE, "pid=%d %s\n",
				state->query.pid, format);
		break;
	case HGSL_STAT_SCOPE_CONTEXT:
		ret = scnprintf(buf, PAGE_SIZE, "ctxt=%u:%u %s\n",
				state->query.devhandle, state->query.context_id,
				format);
		break;
	case HGSL_STAT_SCOPE_ALL:
	default:
		ret = scnprintf(buf, PAGE_SIZE, "all %s\n", format);
		break;
	}
	mutex_unlock(&state->lock);

	return ret;
}

static ssize_t stat_filter_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct qcom_hgsl *hgsl = dev_get_drvdata(dev);
	struct hgsl_stat_state *state = &hgsl->stat;
	struct hgsl_stat_query next;
	int ret;

	ret = stat_filter_parse(buf, &next);
	if (ret)
		return ret;

	mutex_lock(&state->lock);
	state->query = next;
	mutex_unlock(&state->lock);

	return count;
}

static DEVICE_ATTR_RW(stat_filter);

static ssize_t stat_read(struct file *file, struct kobject *kobj,
			 struct bin_attribute *attr, char *buf, loff_t offset,
			 size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct qcom_hgsl *hgsl = dev_get_drvdata(dev);
	struct hgsl_stat_state *state = &hgsl->stat;
	char *snapshot = NULL;
	size_t snapshot_size = 0;
	size_t copy_size;
	ssize_t ret;

	if (offset < 0)
		return -EINVAL;

	mutex_lock(&state->lock);
	if (!offset) {
		ret = hgsl_stat_snapshot(hgsl, &snapshot, &snapshot_size,
					  &state->query);
		if (ret)
			goto out;

		kvfree(state->snapshot);
		state->snapshot = snapshot;
		state->snapshot_size = snapshot_size;
	}

	if ((u64)offset >= state->snapshot_size) {
		ret = 0;
		goto out;
	}

	copy_size = min(count, state->snapshot_size - (size_t)offset);
	memcpy(buf, state->snapshot + (size_t)offset, copy_size);
	ret = copy_size;
out:
	mutex_unlock(&state->lock);
	return ret;
}

static BIN_ATTR_RO(stat, 0);

static const struct attribute *_attrs[] = {
	&dev_attr_total_mem.attr,
	NULL,
};

int hgsl_sysfs_init(struct platform_device *pdev)
{
	struct qcom_hgsl *hgsl = platform_get_drvdata(pdev);
	int ret;

	hgsl->stat.query.scope = HGSL_STAT_SCOPE_ALL;
	hgsl->stat.query.detail = true;

	hgsl->clients_sysfs = kobject_create_and_add("clients",
					&hgsl->class_dev->kobj);

	if (hgsl->clients_sysfs == NULL) {
		ret = -EINVAL;
		goto exit;
	}

	ret = sysfs_create_files(&hgsl->class_dev->kobj, _attrs);
	if (ret) {
		kobject_put(hgsl->clients_sysfs);
		hgsl->clients_sysfs = NULL;
		goto exit;
	}

	ret = sysfs_create_file(&hgsl->class_dev->kobj,
				&dev_attr_stat_filter.attr);
	if (ret) {
		dev_warn(hgsl->dev, "Failed to create stat_filter: %d\n", ret);
		ret = 0;
		goto notify;
	}

	ret = sysfs_create_bin_file(&hgsl->class_dev->kobj, &bin_attr_stat);
	if (ret) {
		dev_warn(hgsl->dev, "Failed to create stat: %d\n", ret);
		sysfs_remove_file(&hgsl->class_dev->kobj,
				  &dev_attr_stat_filter.attr);
		ret = 0;
	}

notify:
	/* Notify userspace */
	kobject_uevent(&hgsl->dev->kobj, KOBJ_ADD);

exit:
	return ret;
}


void hgsl_sysfs_release(struct platform_device *pdev)
{
	struct qcom_hgsl *hgsl;

	hgsl = platform_get_drvdata(pdev);

	sysfs_remove_bin_file(&hgsl->class_dev->kobj, &bin_attr_stat);
	sysfs_remove_file(&hgsl->class_dev->kobj, &dev_attr_stat_filter.attr);
	sysfs_remove_files(&hgsl->class_dev->kobj, _attrs);
	mutex_lock(&hgsl->stat.lock);
	kvfree(hgsl->stat.snapshot);
	hgsl->stat.snapshot = NULL;
	hgsl->stat.snapshot_size = 0;
	mutex_unlock(&hgsl->stat.lock);
	if (hgsl->clients_sysfs) {
		kobject_put(hgsl->clients_sysfs);
		hgsl->clients_sysfs = NULL;
	}
}
