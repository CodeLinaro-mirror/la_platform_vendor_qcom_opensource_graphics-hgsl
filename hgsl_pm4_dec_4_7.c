// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/types.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>

#include "hgsl.h"
#include "hgsl_tcsr.h"
#include "hgsl_memory.h"
#include "hgsl_pm4_dec_4_7.h"
#include "hgsl_snapshot.h"

static int hgsl_pm4_parse_internal(uint64_t gpuaddr, struct hgsl_priv *priv,  size_t sizedwords,
			enum hgsl_pm4_buftype_t type, struct hgsl_pm4_buffers_t *bufs,
			int in_fault, unsigned int parse_depth);

/*
 * Determines if two address ranges overlap and returns
 * an address and size for a combined buffer.
 */
static int hgsl_pm4_ranges_overlap(uint64_t gpuaddr1, uintptr_t hostptr1, uint64_t size1,
			uint64_t gpuaddr2, uintptr_t hostptr2, uint64_t size2,
			uint64_t *gpuaddr, uintptr_t *hostptr, uint64_t *size)
{
	int overlap = 0;
	uint64_t addr_delta = 0;

	if ((gpuaddr1 <= gpuaddr2) && ((gpuaddr1 + size1) > gpuaddr2)) {
		/* Range 2 starts after (or at the same place as) range 1 */
		addr_delta = gpuaddr2 - gpuaddr1;

		overlap = 1;
		*gpuaddr = gpuaddr1;
		*hostptr = hostptr1;

		if (addr_delta + size2 > size1)
			*size = size2 + addr_delta;
		else
			*size = size1;
	} else if ((gpuaddr2 < gpuaddr1) && ((gpuaddr2 + size2) > gpuaddr1)) {
		/* Range 1 starts after range 2 */
		addr_delta = gpuaddr1 - gpuaddr2;
		overlap = 1;
		*gpuaddr = gpuaddr2;
		*hostptr = hostptr2;

		if (addr_delta + size1 > size2)
			*size = size1 + addr_delta;
		else
			*size = size2;
	}

	return overlap;
}

static size_t hgsl_pm4_get_load_state_size(uint32_t ord2)
{
	size_t unitsize_map[/*BlockID*/][2 /*Max Types */] = {
		/* HLSQ_BLOCK_ID_TP_VSTEX */    {4  /*SAMPLERS*/, 16 /*MEMOBJ*/},
		/* HLSQ_BLOCK_ID_TP_HSTEX */    {4  /*SAMPLERS*/, 16 /*MEMOBJ*/},
		/* HLSQ_BLOCK_ID_TP_DSTEX */    {4  /*SAMPLERS*/, 16 /*MEMOBJ*/},
		/* HLSQ_BLOCK_ID_TP_GSTEX */    {4  /*SAMPLERS*/, 16 /*MEMOBJ*/},
		/* HLSQ_BLOCK_ID_TP_FSTEX */    {4  /*SAMPLERS*/, 16 /*MEMOBJ*/},
		/* HLSQ_BLOCK_ID_TP_CSTEX */    {4  /*SAMPLERS*/, 16 /*MEMOBJ*/},
		/* HLSQ_BLOCK_ID_CB_3D */       {0  /*N/A*/, 2 /*CB_SIZE*/},
		/* HLSQ_BLOCK_ID_CB_CS */       {0  /*N/A*/, 2 /*CB_SIZE*/},
		/* HLSQ_BLOCK_ID_SP_VS */       {32  /*INSTR*/, 4 /*CONST*/},
		/* HLSQ_BLOCK_ID_SP_HS */       {32  /*INSTR*/, 4 /*CONST*/},
		/* HLSQ_BLOCK_ID_SP_DS */       {32  /*INSTR*/, 4 /*CONST*/},
		/* HLSQ_BLOCK_ID_SP_GS */       {32  /*INSTR*/, 4 /*CONST*/},
		/* HLSQ_BLOCK_ID_SP_FS */       {32  /*INSTR*/, 4 /*CONST*/},
		/* HLSQ_BLOCK_ID_SP_CS */       {32  /*INSTR*/, 4 /*CONST*/},
		/* HLSQ_BLOCK_ID_UAV_3D */      {4  /*UAV_STRIDE*/, 4 /*UAV_SIZE*/},
		/* HLSQ_BLOCK_ID_UAV_CS */      {4  /*UAV_STRIDE*/, 4 /*UAV_SIZE*/},
	};
	unsigned int max_block_id = ARRAY_SIZE(unitsize_map);
	unsigned int max_type = 1U;
	size_t size_dwords = 0;
	uint32_t statetype = 0;
	uint32_t numunits = 0;
	uint32_t blktype = 0;
	uint32_t statesource = (ord2 & (uint32_t)HLSQ_LOAD_CMD_0_STATESRC_MASK)
				>> HLSQ_LOAD_CMD_0_STATESRC_SHIFT;

	if (statesource == (unsigned int)HLSQ_INDIRECT) {
		statetype = (ord2 & (uint32_t)HLSQ_LOAD_CMD_0_STATETYPE_MASK)
					>> HLSQ_LOAD_CMD_0_STATETYPE_SHIFT;
		numunits = (ord2 & (uint32_t)HLSQ_LOAD_CMD_0_NUMUNITS_MASK)
					>> HLSQ_LOAD_CMD_0_NUMUNITS_SHIFT;
		blktype = (ord2 & (uint32_t)HLSQ_LOAD_CMD_0_STATEBLOCKID_MASK)
					>> HLSQ_LOAD_CMD_0_STATEBLOCKID_SHIFT;

		if ((blktype <= max_block_id) && (statetype <= max_type))
			size_dwords = unitsize_map[blktype][statetype] * numunits;
	}

	return size_dwords;
}

enum hgsl_add_buffer_state_t hgsl_pm4_add_buffer(struct hgsl_pm4_buffers_t *bufs, uint64_t gpuaddr,
	unsigned int *va_ptr, size_t sizedwords, enum hgsl_pm4_buftype_t type,
	struct hgsl_pm4_bufdesc_t **desc)
{
	enum hgsl_add_buffer_state_t state = HGSL_BUFFER_ERROR;
	struct gsl_memdesc_t tmp_desc = {0};
	size_t size = sizedwords * sizeof(uint32_t);
	unsigned int i = 0U;
	int error_printed = GSL_FALSE;
	struct hgsl_pm4_bufdesc_t *pdesc = NULL;
	uint64_t combined_addr = (uint64_t)0;
	uintptr_t combined_hostaddr = 0;
	uint64_t combined_size = 0;

	if (!bufs || !size)
		goto out;

	memset(&tmp_desc, 0, sizeof(tmp_desc));
	tmp_desc.hostptr = va_ptr;
	tmp_desc.size = size;
	tmp_desc.gpuaddr = gpuaddr;

	/* Check if we've already tracked this buffer */
	state = HGSL_BUFFER_ADDED;
	for (i = 0U; i < bufs->num_buffers; i++) {
		pdesc = &bufs->buffers[i];
		if ((bufs->buffers[i].memdesc.hostptr == va_ptr)
		   && (bufs->buffers[i].type == type)
		   && hgsl_pm4_ranges_overlap(gpuaddr, (uintptr_t)tmp_desc.hostptr, size,
				pdesc->memdesc.gpuaddr, (uintptr_t)pdesc->memdesc.hostptr,
				pdesc->memdesc.size, &combined_addr, &combined_hostaddr,
				&combined_size)) {
			state = HGSL_BUFFER_EXISTS;
			if (pdesc->memdesc.size < combined_size) {
				if (pdesc->copy_ptr != NULL) {
					LOGD("hostptr has already been obtained");
					state = HGSL_BUFFER_ADDED;
					break;
				}

				pdesc->memdesc.hostptr = (void *)combined_hostaddr;
				pdesc->memdesc.gpuaddr = combined_addr;
				pdesc->memdesc.size = combined_size;
				state = HGSL_BUFFER_UPDATED;
			}

			if (desc != NULL)
				*desc = pdesc;
			break;
		}
	}

	/* If state is still "ADDED" we didn't find the buffer and must add it */
	if (state == HGSL_BUFFER_ADDED) {
		if (bufs->num_buffers < (unsigned int)HGSL_PM4_MAX_BUFFERS_TRACKED) {
			pdesc = &bufs->buffers[bufs->num_buffers];

			memcpy(&pdesc->memdesc, &tmp_desc, sizeof(pdesc->memdesc));
			pdesc->type = type;
			pdesc->copy_ptr = NULL;
			if (desc != NULL)
				*desc = pdesc;
			else
				LOGI("Input pointer desc is NULL");

			bufs->num_buffers++;
			error_printed = GSL_FALSE;
		} else {
			if (error_printed == GSL_FALSE) {
				error_printed = GSL_TRUE;
				LOGE("More buffers (%d) referenced than allocated to track (%d)",
						bufs->num_buffers, HGSL_PM4_MAX_BUFFERS_TRACKED);
			}
			state = HGSL_BUFFER_ERROR;
		}
	}

out:
	return state;
}

void *hgsl_pm4_get_buff_ptr(struct hgsl_pm4_bufdesc_t *buf)
{
	void *ptr = buf->copy_ptr;

	if (!buf->memdesc.size || !buf->memdesc.hostptr)
		return NULL;

	if (!ptr) {
		buf->copy_ptr = hgsl_malloc(buf->memdesc.size);
		if (buf->copy_ptr) {
			memcpy(buf->copy_ptr, buf->memdesc.hostptr, buf->memdesc.size);
			ptr = buf->copy_ptr;
		} else {
			LOGE("PM4 parse: alloc of local buffer failed");
		}
	} else {
		LOGD("Buffer already allocated");
	}

	return ptr;
}

static size_t hgsl_pm4_parse_type4(uint32_t *vaddr)
{
	return GET_PM4_TYPE4_COUNT(vaddr[0]) - 1;
}

static uint32_t hgsl_pm4_parse_type7(unsigned int *vaddr, struct hgsl_priv *priv,
	enum hgsl_pm4_buftype_t type, struct hgsl_pm4_buffers_t *bufs,
	int in_fault, int parse_depth)
{
	uint32_t count = 0;
	enum hgsl_pm4_buftype_t new_type = HGSL_BUFTYPE_OTHER;
	uint64_t new_gpuaddr = 0;
	uint32_t opcode = 0;
	size_t size = 0;

	if (!vaddr)
		goto out;

	opcode = GET_PM4_TYPE7_OPCODE(vaddr[0]);
	/* skip the header */
	count = GET_PM4_TYPE7_COUNT(vaddr[0]) - 1;

	/* Increment pointer to payload */
	vaddr++;
	switch (opcode) {
	case CP_LOAD_STATE_VS:
	case CP_LOAD_STATE_PS:
		new_gpuaddr = vaddr[2]; // Upper bits first shifted in next statement
		new_gpuaddr = (new_gpuaddr << 32) |
			(vaddr[1] & HLSQ_LOAD_CMD_1_EXTSRCADDRLO_MASK);
		size = hgsl_pm4_get_load_state_size(vaddr[0]);
		if (size)
			hgsl_pm4_add_buffer(bufs, new_gpuaddr, vaddr, size,
				HGSL_BUFTYPE_SHADER, NULL);
		break;
	case CP_INDIRECT_BUFFER_PFE:
		size = vaddr[2] & 0xFFFFF;
		new_gpuaddr = vaddr[1];
		new_gpuaddr = (new_gpuaddr << 32) | vaddr[0];
		switch (type) {
		case HGSL_BUFTYPE_RINGBUFFER:
			new_type = HGSL_BUFTYPE_IB1;
			break;
		case HGSL_BUFTYPE_IB1:
			new_type = HGSL_BUFTYPE_IB2;
			break;
		case HGSL_BUFTYPE_IB2:
			new_type = HGSL_BUFTYPE_IB3;
			break;
		default:
			new_type = HGSL_BUFTYPE_DRAW_STATE;
			break;
		}
		hgsl_pm4_parse_internal(new_gpuaddr, priv, size, new_type, bufs,
			in_fault, parse_depth);
		break;
	case CP_SET_DRAW_STATE:
		{
			unsigned int grp = 0;

			for (grp = 0; (grp * 3 + 1) < count; grp++) {
				uint32_t control_word = vaddr[grp * 3];
				// capture the DRAW_STATE if the group is not being reset
				if (!(0x20000 & control_word) && !(0x40000 & control_word)) {
					size = control_word & 0xFFFF;
					new_gpuaddr = vaddr[grp * 3 + 2];
					new_gpuaddr = new_gpuaddr << 32 |
						vaddr[grp * 3 + 1];

					hgsl_pm4_parse_internal(new_gpuaddr, priv, size,
						HGSL_BUFTYPE_DRAW_STATE, bufs,
						in_fault, parse_depth);
				}
			}
		}
		break;
	/*
	 * For all drawing types take the buffers pointed to by the regs
	 * and "finalize" them into the buffer list
	 */
	case CP_DRAW_INDX_OFFSET:
	case CP_DRAW_INDX_INDIRECT:
	case CP_DRAW_INDIRECT:
	default:
		LOGD("Default opcode is called with val=%x", opcode);
		break;
	}

out:
	return count;
}

static int hgsl_pm4_parse_internal(uint64_t gpuaddr, struct hgsl_priv *priv,  size_t sizedwords,
	enum hgsl_pm4_buftype_t type, struct hgsl_pm4_buffers_t *bufs,
	int in_fault, unsigned int parse_depth)
{
	int status = GSL_SUCCESS;
	/* dwords left in the current command buffer */
	size_t dwords_left = sizedwords;
	enum hgsl_add_buffer_state_t bufadd_state = HGSL_BUFFER_ERROR;
	struct hgsl_pm4_bufdesc_t *pdesc = NULL;
	unsigned int *cpy_ptr = NULL;
	unsigned int *vaddr = hgsl_get_va_from_gpuaddr(priv, gpuaddr, sizedwords << 2);

	if (!vaddr) {
		LOGE("FAILED to get vaddr gpu addr 0x%llx size = %d", gpuaddr, sizedwords << 2);
		status = -EINVAL;
		goto out;
	}

	if (HGSL_PM4_MAX_PARSE_DEPTH <= ++parse_depth) {
		LOGE("pm4 commands are invalid, depth=%d", parse_depth);
		status = -EINVAL;
		goto out;
	}

	bufadd_state = hgsl_pm4_add_buffer(bufs, gpuaddr, vaddr, sizedwords, type, &pdesc);
	if ((bufadd_state == HGSL_BUFFER_ADDED || bufadd_state == HGSL_BUFFER_UPDATED) && pdesc)
		cpy_ptr = (unsigned int *)hgsl_pm4_get_buff_ptr(pdesc);


	if (cpy_ptr) {
		while (dwords_left && (status == GSL_SUCCESS)) {
			size_t count = 0;

			switch (GET_PM4_TYPE(cpy_ptr[0])) {
			case 4:
			count = hgsl_pm4_parse_type4(cpy_ptr) + 1;
			break;
			case 7:
			count = hgsl_pm4_parse_type7(cpy_ptr, priv, type, bufs,
						in_fault, parse_depth) + 1;
			break;
			default:
			count = sizedwords + 1; /* Error out */
			break;
			}

			if (count == 0) {
				status = -EINVAL;
				break;
			} else if (count <= dwords_left) {
				dwords_left -= count;
				cpy_ptr += count;
			} else {
				status = -EINVAL;
				dwords_left = 0;
			}
		}
	} else {
		LOGE("Copied ptr is NULL from hgsl_pm4_get_buff_ptr");
	}

out:

	return status;
}

int hgsl_pm4_parse(uint64_t gpuaddr, struct hgsl_priv *priv, size_t sizedwords,
	enum hgsl_pm4_buftype_t type, struct hgsl_pm4_buffers_t *bufs, int in_fault)
{
	return hgsl_pm4_parse_internal(gpuaddr, priv, sizedwords, type,
			bufs, in_fault, 0);
}
