// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __HGSL_PM4_DEC_4_7_H_
#define __HGSL_PM4_DEC_4_7_H_

#define HGSL_PM4_MAX_BUFFERS_TRACKED 4096
#define HGSL_PM4_MAX_PARSE_DEPTH     5

/* Draw via indirect data */
#define CP_DRAW_INDIRECT            0x28
/* Draw using an Index Buffer via indirect data */
#define CP_DRAW_INDX_INDIRECT       0x29
/* Load high level sequencer command for VS|DS|GS|HS */
#define CP_LOAD_STATE_VS            0x32
/* Load high level sequencer command for PS|CS */
#define CP_LOAD_STATE_PS            0X34
/* Draw using index buffer */
#define CP_DRAW_INDX_OFFSET         0x38
/* Load a buffer with pre-fetch enabled */
#define CP_INDIRECT_BUFFER_PFE      0x3F
/* (A4x) save PM4 stream pointers to execute upon a visible draw */
#define CP_SET_DRAW_STATE           0x43

#define CP_TYPE4_PKT	(4 << 28)
#define CP_TYPE7_PKT	(7 << 28)

#define type4_pkt_size(pkt) ((pkt) & 0x7F)

#define pkt_is_type4(pkt) (((pkt) & 0xF0000000) == CP_TYPE4_PKT)

static const uint32_t Pm4Type4HeaderCountMask      = 0x0000007F;
static const uint32_t Pm4Type4HeaderCountShift;

#define GET_PM4_TYPE(x) ((x & 0xF0000000) >> 28)
#define GET_PM4_TYPE7_OPCODE(x) ((x & 0x007F0000) >> 16)
#define GET_PM4_TYPE7_COUNT(x) ((((x) & 0x00007FFF) >> 0x0U) + 1U)
#define GET_PM4_TYPE4_COUNT(x) ((((x) & Pm4Type4HeaderCountMask) >> Pm4Type4HeaderCountShift) + 1U)
#define pkt_is_type7(pkt) (((pkt) & 0xF0000000) == CP_TYPE7_PKT)

#define HLSQ_LOAD_CMD_0_STATESRC_MASK            (0x00030000)
#define HLSQ_LOAD_CMD_0_STATESRC_SHIFT           (0x00000010)
#define HLSQ_LOAD_CMD_0_STATETYPE_MASK           (0x0000c000)
#define HLSQ_LOAD_CMD_0_STATETYPE_SHIFT          (0x0000000e)
#define HLSQ_LOAD_CMD_0_NUMUNITS_MASK            (0xffc00000)
#define HLSQ_LOAD_CMD_0_NUMUNITS_SHIFT           (0x00000016)
#define HLSQ_LOAD_CMD_0_STATEBLOCKID_MASK        (0x003c0000)
#define HLSQ_LOAD_CMD_0_STATEBLOCKID_SHIFT       (0x00000012)
#define HLSQ_LOAD_CMD_1_EXTSRCADDRLO_MASK        (0xfffffff0)

#define HGSL_PM4_DEC_MAX_VBOS                                (32)
#define HGSL_PM4_DEC_MAX_VSTREAMS                            (16)

#define HGSL_PM4_MAX_PARSE_DEPTH                 5

enum HLSQ_STATESRC_ENUM {
	HLSQ_DIRECT                              = 0x0,
	HLSQ_INDIRECT_BINDLESS                   = 0x1,
	HLSQ_INDIRECT                            = 0x2,
	HLSQ_INDIRECT_CB                         = 0x3,
	HLSQ_STATESRC_ENUM_MIN_VALUE             = 0x0,
	HLSQ_STATESRC_ENUM_MAX_VALUE             = 0x3,
};

enum hgsl_pm4_buftype_t {
	HGSL_BUFTYPE_RINGBUFFER,
	HGSL_BUFTYPE_IB1,
	HGSL_BUFTYPE_IB2,
	HGSL_BUFTYPE_IB3,
	HGSL_BUFTYPE_DRAW_STATE,
	HGSL_BUFTYPE_SHADER,
	HGSL_BUFTYPE_OTHER,
};

struct hgsl_pm4_bufdesc_t {
	struct gsl_memdesc_t      memdesc;
	enum hgsl_pm4_buftype_t type;
	void *copy_ptr;
};

struct hgsl_pm4_buffers_t {
	struct hgsl_pm4_bufdesc_t buffers[HGSL_PM4_MAX_BUFFERS_TRACKED];
	unsigned int num_buffers;
};

int hgsl_pm4_parse(uint64_t gpuaddr, struct hgsl_priv *priv, size_t sizedwords,
	enum hgsl_pm4_buftype_t type, struct hgsl_pm4_buffers_t *bufs, int in_fault);
void *hgsl_pm4_get_buff_ptr(struct hgsl_pm4_bufdesc_t *buf);

#endif /* __HGSL_PM4_DEC_4_7_H_ */
