// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/spinlock.h>
#include <linux/regmap.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/fs_struct.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "hgsl_tcsr.h"
#include "hgsl_snapshot.h"
#include "hgsl_pm4_dec_4_7.h"
#include "hgsl_hyp.h"

static void hgsl_snapshot_add_gpu_obj(struct hgsl_snapshot_info_t *snapshot,
	struct hgsl_pm4_bufdesc_t *cur_buf, enum hgsl_snapshot_gpu_obj_type_t type);

void *hgsl_get_va_from_gpuaddr(struct hgsl_priv *priv, uint64_t gpuaddr, uint32_t ib_size)
{
	void *ret = NULL;
	int status = 0;
	struct hgsl_mem_node *ib_mem_node = NULL;

	mutex_lock(&priv->lock);
	ib_mem_node = hgsl_mem_find_node_locked(&priv->mem_allocated, gpuaddr, ib_size, false);

	if (!ib_mem_node) {
		LOGE("FAILED to find snapshot ib");
		goto out;
	}

	if (!ib_mem_node->kva_map.vaddr) {
		dma_buf_begin_cpu_access(ib_mem_node->dma_buf, DMA_FROM_DEVICE);
		status = dma_buf_vmap_unlocked(ib_mem_node->dma_buf, &(ib_mem_node->kva_map));
		if (status) {
			LOGE("failed to map ib memnode buffer\n");
			goto out;
		}
	}

	ret = (void *)((uintptr_t)ib_mem_node->kva_map.vaddr
		+ (gpuaddr - ib_mem_node->memdesc.gpuaddr));
out:

	mutex_unlock(&priv->lock);
	return ret;
}

void hgsl_snapshot_parse_ib1(struct hgsl_priv *priv, enum gsl_devhandle_t devhandle,
	uint64_t gpuaddr, uint32_t ib_size, struct hgsl_snapshot_info_t *snapshot)
{
	size_t pkt_size = 0;
	uint32_t ib_size_dword = ib_size >> 2;
	uint32_t *va_ptr = NULL;
	unsigned int buf_idx = 0;
	enum hgsl_snapshot_gpu_obj_type_t objtype = SNAP_GPU_OBJ_INVALID;
	struct hgsl_pm4_buffers_t *bufs = NULL;

	va_ptr = hgsl_get_va_from_gpuaddr(priv, gpuaddr, ib_size);
	if (!va_ptr) {
		LOGE("FAILED to get va from gpuaddr 0x%x", gpuaddr);
		goto out;
	}

	bufs = (struct hgsl_pm4_buffers_t *)(hgsl_malloc(sizeof(struct hgsl_pm4_buffers_t)));
	if (!bufs) {
		LOGE("FAILED to malloc pm4 buffer");
		goto out;
	}

	if (pkt_is_type4(va_ptr[0])) {
		pkt_size = type4_pkt_size(va_ptr[0]);
		LOGI("cmd type 4 value 0x%x", va_ptr[0]);
	} else if (pkt_is_type7(va_ptr[0])) {
		LOGI("cmd type 7 value 0x%x", va_ptr[0]);
		pkt_size = GET_PM4_TYPE7_COUNT(va_ptr[0]);
	} else {
		LOGE("unknown type 0x%x", va_ptr[0]);
		goto out;
	}

	hgsl_pm4_parse(gpuaddr, priv, ib_size_dword, HGSL_BUFTYPE_IB1, bufs, 1);
	if (bufs->num_buffers > 0) {
		snapshot->header.magic_ver = HGSL_SNAPSHOT_HEADER_MAGIC_VER;
	} else {
		LOGE("No valid buffer added");
		goto out;
	}

	for (buf_idx = 0; buf_idx < bufs->num_buffers; buf_idx++) {
		switch (bufs->buffers[buf_idx].type) {
		case HGSL_BUFTYPE_IB1:
			objtype = SNAP_GPU_OBJ_IB;
			break;
		case HGSL_BUFTYPE_IB2:
			objtype = SNAP_GPU_OBJ_IB;
			break;
		case HGSL_BUFTYPE_IB3:
			objtype = SNAP_GPU_OBJ_IB;
			break;
		case HGSL_BUFTYPE_DRAW_STATE:
			objtype = SNAP_GPU_OBJ_DRAW_STATE;
			break;
		case HGSL_BUFTYPE_SHADER:
			objtype = SNAP_GPU_OBJ_SHADER;
			break;
		default:
			objtype = SNAP_GPU_OBJ_GENERIC;
			break;
		}

		/* This will transfer ownership of the copy_ptr to the snapshot */
		hgsl_snapshot_add_gpu_obj(snapshot, &(bufs->buffers[buf_idx]),
			objtype);
	}

out:
	hgsl_free(bufs);
}

static void hgsl_snapshot_add_gpu_obj(struct hgsl_snapshot_info_t *snapshot,
	struct hgsl_pm4_bufdesc_t *cur_buf, enum hgsl_snapshot_gpu_obj_type_t type)
{
	struct hgsl_snapshot_gpu_obj_node_t *obj_node = NULL;
	void *dataptr = NULL;

	dataptr = hgsl_pm4_get_buff_ptr(cur_buf);
	if (!dataptr) {
		LOGE("GOT NULL data ptr from buffer");
		goto out;
	}

	if (snapshot->gpu_objs_head) {
		obj_node = snapshot->gpu_objs_head;
		while (obj_node->next) {
			if ((obj_node->base_info.gpuaddr == cur_buf->memdesc.gpuaddr)
				&& (obj_node->base_info.hostptr == cur_buf->memdesc.hostptr)) {
				hgsl_free(dataptr);
				dataptr = NULL;
				goto out;
			}

			obj_node = obj_node->next;
		}

		obj_node->next = (struct hgsl_snapshot_gpu_obj_node_t *)
			(hgsl_malloc(sizeof(struct hgsl_snapshot_gpu_obj_node_t)));
		obj_node = obj_node->next;
	} else {
		snapshot->gpu_objs_head = (struct hgsl_snapshot_gpu_obj_node_t *)
			(hgsl_malloc(sizeof(struct hgsl_snapshot_gpu_obj_node_t)));
		obj_node = snapshot->gpu_objs_head;
	}

	if (!obj_node) {
		LOGE("Unable to allocate memory for GPU object in snapshot");
		goto out;
	}


	obj_node->next = NULL;
	obj_node->base_info.gpuaddr = (uint64_t)cur_buf->memdesc.gpuaddr;
	obj_node->base_info.hostptr = cur_buf->memdesc.hostptr;
	obj_node->base_info.size_dwords = (uint64_t)(cur_buf->memdesc.size / sizeof(uint32_t));
	obj_node->base_info.type = (uint32_t)type;
	obj_node->objdata = (uint32_t *)dataptr;
	if (cur_buf->copy_ptr == dataptr)
		obj_node->is_allocated = GSL_TRUE;
	else
		obj_node->is_allocated = GSL_FALSE;

out:
	return;
}

void hgsl_snapshot_flush_memory(struct hgsl_snapshot_info_t *snapshot,
	unsigned int *snapshot_buffer, const char *const postfix, uint32_t *used_size)
{
	char *snapshot_buffer_wptr = (char *)snapshot_buffer;
	size_t snapshot_buffer_wsize = 0;
	struct hgsl_snapshot_sec_header_t section_header = {0};
	struct hgsl_snapshot_gpu_obj_node_t *tmp_node = NULL;
	struct hgsl_snapshot_gpu_obj_node_t *node = NULL;
	int unused_size = HGSL_SNAPSHOT_BUFFER_SIZE_IN_BYTES;
	int node_info_size = 0;

	if (snapshot->gpu_objs_head != NULL) {
		node = snapshot->gpu_objs_head;
		section_header.magic = HGSL_SNAPSHOT_SECTION_MAGIC;
		section_header.id_ver = HGSL_SNAPSHOT_MAKE_SECTION_ID(HGSL_SNAP_GPU_OBJ_SECID,
			HGSL_SNAP_GPU_OBJ_VERSION);

		while (node) {
			node_info_size = sizeof(section_header) +
				sizeof(struct hgsl_snapshot_gpu_obj_base_t) +
				sizeof(uint32_t) * (uint32_t)node->base_info.size_dwords;
			if (unused_size > node_info_size) {
				section_header.size_bytes =
					(uint32_t)(sizeof(struct hgsl_snapshot_gpu_obj_base_t)
					+ ((uint32_t)node->base_info.size_dwords * sizeof(uint32_t))
					+ sizeof(section_header));
				snapshot_buffer_wsize = sizeof(section_header);
				memcpy((void *)snapshot_buffer_wptr, (void *)(&section_header),
					snapshot_buffer_wsize);
				snapshot_buffer_wptr += snapshot_buffer_wsize;

				snapshot_buffer_wsize = sizeof(struct hgsl_snapshot_gpu_obj_base_t);
				memcpy((void *)snapshot_buffer_wptr, (void *)(&node->base_info),
					snapshot_buffer_wsize);
				snapshot_buffer_wptr += snapshot_buffer_wsize;

				snapshot_buffer_wsize = sizeof(uint32_t) *
					(uint32_t)node->base_info.size_dwords;
				memcpy((void *)snapshot_buffer_wptr, (void *)(node->objdata),
					snapshot_buffer_wsize);
				snapshot_buffer_wptr += snapshot_buffer_wsize;

				unused_size -= node_info_size;
			} else {
				LOGE("Snapshot buffer overflow");
			}

			hgsl_free(node->objdata);
			node->objdata = NULL;
			tmp_node = node->next;
			hgsl_free(node);
			node = tmp_node;
		}

		snapshot->gpu_objs_head = NULL;
		*used_size = HGSL_SNAPSHOT_BUFFER_SIZE_IN_BYTES - unused_size;
		LOGD("snapshto used size 0x%x", *used_size);
	}
}
