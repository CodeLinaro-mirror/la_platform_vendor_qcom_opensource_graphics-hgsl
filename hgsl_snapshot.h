// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __HGSL_SNAPSHOT_H_
#define __HGSL_SNAPSHOT_H_

#include "hgsl.h"
#include "hgsl_memory.h"
#include "hgsl_pm4_dec_4_7.h"
#pragma pack(push, 1)

enum hgsl_snapshot_gpu_obj_type_t {
	SNAP_GPU_OBJ_SHADER = 0x1,
	SNAP_GPU_OBJ_IB = 0x2,
	SNAP_GPU_OBJ_GENERIC = 0x3,
	SNAP_GPU_OBJ_DRAW_STATE = 0x4,
	SNAP_GPU_OBJ_INVALID = 0x5,
};

enum hgsl_add_buffer_state_t {
	HGSL_BUFFER_ADDED,
	HGSL_BUFFER_UPDATED,
	HGSL_BUFFER_EXISTS,
	HGSL_BUFFER_ERROR
};

struct hgsl_snapshot_sec_header_t {
	uint16_t magic;
	uint16_t id_ver;
	uint32_t size_bytes; /* including the header */
};

struct hgsl_snapshot_gpu_obj_base_t {
	uint32_t type;
	uint64_t gpuaddr;
	void *hostptr;
	uint64_t size_dwords;
};

struct hgsl_snapshot_gpu_obj_node_t {
	struct hgsl_snapshot_gpu_obj_base_t base_info;
	int is_allocated;
	void *objdata;
	struct hgsl_snapshot_gpu_obj_node_t *next;
};

struct hgsl_snapshot_header_t {
	uint32_t magic_ver;
	uint32_t gpuid;
	uint32_t chipid;
};

struct hgsl_snapshot_info_t {
	struct hgsl_snapshot_header_t          header;
	struct hgsl_snapshot_gpu_obj_node_t    *gpu_objs_head;
};

#pragma pack(pop)

#define HGSL_SNAPSHOT_HEADER_MAGIC      (0x504DU)
#define HGSL_SNAPSHOT_HEADER_VERSION    (0x0002U) /* versions the headers */
#define HGSL_SNAPSHOT_HEADER_MAGIC_VER  ((HGSL_SNAPSHOT_HEADER_MAGIC << 16U) | \
	HGSL_SNAPSHOT_HEADER_VERSION)

#define HGSL_SNAP_GPU_OBJ_SECID         (0x0B)
#define HGSL_SNAP_GPU_OBJ_VERSION       (0x02)

#define HGSL_SNAPSHOT_SECTION_MAGIC     (0xABCDU)
#define HGSL_SNAPSHOT_MAKE_SECTION_ID(id, ver) \
	(((id) << 8) | (ver))

#define HGSL_SNAPSHOT_BUFFER_SIZE_IN_BYTES       (1 << 21) // 2 MB

int hgsl_snapshot_init(void);
void *hgsl_get_va_from_gpuaddr(struct hgsl_priv *priv, uint64_t gpuaddr, uint32_t ib_size);
void hgsl_snapshot_parse_ib1(struct hgsl_priv *priv, enum gsl_devhandle_t devhandle,
	uint64_t gpuaddr, uint32_t ib_size, struct hgsl_snapshot_info_t *snapshot);
void hgsl_snapshot_flush_memory(struct hgsl_snapshot_info_t *snapshot,
	unsigned int *snapshot_buffer, const char *const postfix, uint32_t *used_size);

#endif /* __HGSL_SNAPSHOT_H_ */
