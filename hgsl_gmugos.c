// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include "hgsl.h"
#include "hgsl_snapshot.h"

#define GMUGOS_REG_SET    (0x0)
#define GMUGOS_REG_STATUS (0x4)
#define GMUGOS_REG_CLR    (0x8)
#define GMUGOS_REG_MASK   (0xC)

static irqreturn_t hgsl_gmugos_ts_retire(int num, void *data)
{
	struct hgsl_gmugos_irq *gmugos_irq = data;
	struct hgsl_gmugos *gmugos = container_of(gmugos_irq,
					typeof(*gmugos), irq[gmugos_irq->id]);
	u32 dev_id = hgsl_hnd2id(gmugos->dev_hnd);

	if (dev_id >= HGSL_DEVICE_NUM) {
		pr_warn_ratelimited("Invalid dev handle %u\n",
			gmugos->dev_hnd);
		return IRQ_HANDLED;
	}

	hgsl_retire_common
		(container_of(gmugos, typeof(struct qcom_hgsl),
			gmugos[dev_id]),
		gmugos->dev_hnd);

	return IRQ_HANDLED;
}

static void hgsl_snapshot_dump(struct qcom_hgsl *hgsl_dev, uint32_t *msg_buffer, u32 dev_hnd)
{
	struct hgsl_ipcq_gvm_state_dump_msg msg = {0};
	struct hgsl_context *ctxt = NULL;
	struct hgsl_priv *priv = NULL;
	int ib_idx = 0;
	int ret = 0;
	u32 dev_id = hgsl_hnd2id(dev_hnd);
	struct hgsl_mem_node *mem_node = NULL;
	uint32_t *snapshot_buffer = NULL;
	struct iosys_map buf_vmap = { 0 };
	struct hgsl_snapshot_info_t snapshot_info = {0};
	uint32_t buf_size = 0;
	uint32_t used_size = 0;
	enum gsl_devhandle_t devhandle = hgsl_dev->device_handle[dev_id];
	struct HfiMsgHeader_t *msg_header = (struct HfiMsgHeader_t *)msg_buffer;

	if (msg_header->msg_size_dword * sizeof(uint32_t) !=
		sizeof(struct hgsl_ipcq_gvm_state_dump_msg)) {
		LOGE("size not match header 0x%x", msg_header->msg_size_dword);
		return;
	}

	memcpy(&msg, msg_buffer, msg_header->msg_size_dword * sizeof(uint32_t));
	LOGI("msgid 0x%x type 0x%x size_dw 0x%x dev_id %d ctxtid %d gpuaddr 0x%llx size 0x%x",
		msg_header->msg_id, msg_header->msg_type,
		msg_header->msg_size_dword, dev_id, msg.ctxt_id, msg.gpuaddr[0], msg.size_dw[0]);

	ctxt = hgsl_get_context(hgsl_dev, dev_hnd, msg.ctxt_id);
	if (!ctxt) {
		LOGE("FAILED TO GET ctxt");
		return;
	}

	priv = ctxt->priv;
	if (!priv) {
		LOGE("Invalid hgsl_priv!");
		hgsl_put_context(ctxt);
		return;
	}

	mutex_lock(&priv->lock);
	mem_node = (struct hgsl_mem_node *)hgsl_mem_node_zalloc(priv->dev->cache_flags);
	if (!mem_node) {
		LOGE("FAILED to allocated snapshot memory node");
		goto send_msg;
	}

	ret = hgsl_sharedmem_alloc(priv->dev->dev,
		HGSL_SNAPSHOT_BUFFER_SIZE_IN_BYTES, 0, mem_node);
	if (ret) {
		LOGE("Allocated DMA buffer is invalid");
		goto send_msg;
	}

	dma_buf_begin_cpu_access(mem_node->dma_buf, DMA_BIDIRECTIONAL);
	ret = dma_buf_vmap_unlocked(mem_node->dma_buf, &buf_vmap);
	if (ret) {
		LOGE("failed to map dbq buffer\n");
		goto send_msg;
	}

	snapshot_buffer = (uint32_t *)buf_vmap.vaddr;
	for (ib_idx = 0; ib_idx < msg.ib_num; ib_idx++)
		hgsl_snapshot_parse_ib1(priv, devhandle, msg.gpuaddr[ib_idx],
			msg.size_dw[ib_idx] * sizeof(uint32_t), &snapshot_info);

	hgsl_snapshot_flush_memory(&snapshot_info, snapshot_buffer,
		"state_dump", &used_size);
	buf_size = HGSL_SNAPSHOT_BUFFER_SIZE_IN_BYTES;

send_msg:
	hgsl_hyp_export_memory(&priv->hyp_priv,
		devhandle, mem_node, used_size, buf_size,
		IB1_SNAPSHOT_BUF, msg_header->msg_packet_seq_no);

	if (buf_vmap.vaddr) {
		dma_buf_vunmap_unlocked(mem_node->dma_buf, &buf_vmap);
		dma_buf_end_cpu_access(mem_node->dma_buf, DMA_BIDIRECTIONAL);
	}

	if (mem_node)
		hgsl_sharedmem_free(mem_node);

	mutex_unlock(&priv->lock);
	hgsl_put_context(ctxt);
}

/* Return updated read/write index after removing/adding message with size msg_size.
 * This function takes care of any alignment we might need to enforce for the
 * read/write index. The caller needs to make sure the occupancy (available space)
 * is greater than aligned msg_size when removing (adding) the message.
 */
uint32_t hgsl_new_idx(struct HfiQueueHeader *header, uint32_t idx, uint32_t msg_size)
{
	unsigned int queue_size = header->dwSize;

	msg_size = ALIGN_DWORD_ADDRESS_4DWORD(msg_size);
	return (idx + msg_size) % queue_size;
}

static inline void hgsl_set_read_idx(void *hdr_addr, uint32_t *readIdx)
{
	memcpy((void *)((char *)hdr_addr + OFFSETOF_IPCQ_HEADER_ELEM(readIdx)), readIdx,
		sizeof(uint32_t));
	dma_wmb();
}

/* Return occupied queue size in DWORD. */
static uint32_t hgsl_get_queue_occupancy(struct HfiQueueHeader *header)
{
	uint32_t read_idx = header->readIdx;
	uint32_t write_idx = header->writeIdx;
	uint32_t queue_size = header->dwSize;

	return (write_idx + queue_size - read_idx) % queue_size;
}

static int hgsl_validate_ipcq_read(struct HfiQueueHeader *header, uintptr_t baseaddr,
	uint32_t rb_offset, struct HfiMsgHeader_t *msg_header)
{
	int ret = GSL_TRUE;
	uint32_t read_idx = header->readIdx;
	struct HfiMsgHeader_t *msg_header_from_ipcq = NULL;
	void *rb_addr = (void *)(char *)baseaddr + rb_offset * 4;
	uint32_t occupied_dwsize = 0;

	/* make sure read_idx is within queue size range */
	if (read_idx > header->dwSize) {
		LOGE("Invalid read request, read_idx is 0x%x", read_idx);
		LOGE("header->dwSize is 0x%x", header->dwSize);
		ret = GSL_FALSE;
		goto out;
	}

	/* make sure enough space for msg header */
	occupied_dwsize = hgsl_get_queue_occupancy(header);
	if (occupied_dwsize < sizeof(struct HfiMsgHeader_t)/sizeof(uint32_t)) {
		LOGE("occupied size 0x%x", occupied_dwsize);
		ret = GSL_FALSE;
		goto out;
	}

	msg_header_from_ipcq = (struct HfiMsgHeader_t *)((char *)rb_addr + read_idx * 4);
	memcpy(msg_header, msg_header_from_ipcq, sizeof(struct HfiMsgHeader_t));
	/* make sure read size is smaller than written size */
	if (occupied_dwsize < msg_header->msg_size_dword) {
		LOGE("Invalid read request, occupied size is 0x%x", occupied_dwsize);
		LOGE("read request size is 0x%x", msg_header->msg_size_dword);
		ret = GSL_FALSE;
		goto out;
	}

	if (msg_header->msg_size_dword == 0) {
		LOGE("Invalid msg size from ipcq");
		ret = GSL_FALSE;
		goto out;
	}

out:
	return ret;
}

static void hgsl_ipcq_copy_msg(void *rb_addr, struct HfiQueueHeader *q_header,
	void *buf, uint32_t msg_size_dword)
{
	uint32_t chunk_size;
	uint32_t read_offset = q_header->readIdx * sizeof(uint32_t);
	void *q_buf = (void *)((char *)rb_addr + read_offset);
	uint32_t size_dword = 0;

	size_dword = msg_size_dword;
	if (size_dword < (q_header->dwSize - q_header->readIdx))
		chunk_size = size_dword;
	else
		chunk_size = q_header->dwSize - q_header->readIdx;

	memcpy(buf, q_buf, chunk_size * sizeof(uint32_t));
	// Wrapping around at index 0
	if (chunk_size < size_dword) {
		// writing from start of queue b/c of address wrapping
		memcpy((void *)((char *)buf + chunk_size * sizeof(uint32_t)), rb_addr,
		(size_dword - chunk_size) * sizeof(uint32_t));
	}
}

static void hgsl_ipcq_msg_handler(struct qcom_hgsl *hgsl_dev, uint32_t *msg_buffer, u32 dev_hnd)
{
	struct HfiMsgHeader_t *msg_header = (struct HfiMsgHeader_t *)msg_buffer;

	switch (msg_header->msg_id) {
	case GSL_IPCQ_SNAPSHOT_DUMP: {
		hgsl_snapshot_dump(hgsl_dev, msg_buffer, dev_hnd);
	} break;

	default:
		LOGE("Invalid message id %d", msg_header->msg_id);
	}
}

static int hgsl_read_msg_from_ipcq(struct qcom_hgsl *hgsl_dev,
	struct HfiQueueHeader *cached_qheader, uintptr_t baseaddr, uint32_t dwSize,
	uint32_t *msg_buffer, uint32_t rb_offset, uint32_t hdr_offset, u32 dev_hnd)
{
	int ret = 0;
	uint32_t write_idx = cached_qheader->writeIdx;
	uint32_t read_idx = cached_qheader->readIdx;
	struct HfiMsgHeader_t msg_header = {0};
	void *rb_addr = (void *)(baseaddr + rb_offset * sizeof(uint32_t));
	uint32_t msg_dwsize = 0;
	int is_read_valid = GSL_FALSE;
	struct HfiQueueHeader *ipcq_header = (struct HfiQueueHeader *)(baseaddr + hdr_offset);

	/* Since read_idx/write_idx, as well as queue sizes are all 4DWORD aligned
	 * we should be able to read the cached_qheader (1 DWORD) without wrapping around
	 * the queue
	 */
	if (read_idx >= dwSize || write_idx >= dwSize) {
		LOGE("Invalid read idx 0x%x, write idx 0x%x", read_idx, write_idx);
		LOGE("ipcq overall dwsize is 0x%x", dwSize);
		ret = -EINVAL;
		goto out;
	}

	LOGD("read idx is %d, write idx is %d ", read_idx, write_idx);
	LOGD("rboffset is 0x%x rb addr is %p", rb_offset, rb_addr);
	is_read_valid = hgsl_validate_ipcq_read(cached_qheader, baseaddr,
		rb_offset, &msg_header);
	if (is_read_valid != GSL_TRUE) {
		LOGE("Invalid read operation!");
		ret = -EINVAL;
		goto out;
	}

	hgsl_ipcq_copy_msg(rb_addr, cached_qheader, (void *)msg_buffer, msg_header.msg_size_dword);
	msg_buffer[0] = *(uint32_t *)&msg_header;
	msg_dwsize = ALIGN_DWORD_ADDRESS_4DWORD(msg_header.msg_size_dword);
	LOGD("msg size 0x%x", msg_header.msg_size_dword);
	read_idx = hgsl_new_idx(cached_qheader, read_idx, msg_dwsize);
	hgsl_set_read_idx((void *)ipcq_header, &read_idx);
	cached_qheader->readIdx = read_idx;

out:
	return ret;
}

static void hgsl_gmugos_handle_ipcq_msg(struct work_struct *work)
{
	struct hgsl_gmugos_irq *gmugos_irq = container_of(work, struct hgsl_gmugos_irq, irq_work);
	struct hgsl_gmugos *gmugos = NULL;
	struct qcom_hgsl *hgsl_dev = NULL;
	struct hgsl_mem_node *mem_node = NULL;
	struct HfiQueueHeader *header = NULL;
	u32 dev_id = 0;
	uint32_t *msg_buffer = NULL;
	struct hgsl_ipcq_data_t *data_ptr = NULL;
	int ret = 0;

	if (!gmugos_irq) {
		LOGE("Invalid gmugos_irq!");
		goto exit;
	}

	gmugos = container_of(gmugos_irq, typeof(*gmugos), irq[gmugos_irq->id]);
	if (!gmugos) {
		LOGE("Invalid gmugos!");
		goto exit;
	}

	// this dev_id is different from the gsl_deviceid_t
	dev_id = hgsl_hnd2id(gmugos->dev_hnd);
	hgsl_dev = (struct qcom_hgsl *)container_of
		(gmugos, typeof(struct qcom_hgsl), gmugos[dev_id]);
	if (!hgsl_dev) {
		LOGE("Invalid hgsl_dev! dev_id %d", dev_id);
		goto exit;
	}

	mutex_lock(&hgsl_dev->mutex);
	data_ptr = &(hgsl_dev->ipcq_datas[dev_id][HGSL_IPCQ_RGS_IDX]);
	msg_buffer = (uint32_t *)(&(hgsl_dev->hgsl_ipcq_msgbuf[dev_id][HGSL_IPCQ_RGS_IDX]));
	mem_node = hgsl_dev->ipcq_memnode[dev_id][HGSL_IPCQ_RGS_IDX];
	if (!mem_node) {
		LOGE("Invalid mem_node!");
		goto out;
	}

	header = (struct HfiQueueHeader *)((uint32_t *)mem_node->vmapping +
		hgsl_dev->ipcq_settings[dev_id].PKMD2HGSL_queue_offset * sizeof(uint32_t));
	if (!header) {
		LOGE("Invalid header!");
		goto out;
	}

	if (header->status == 0) {
		LOGE("IPCQ is not active!!");
		goto out;
	}

	data_ptr->header.writeIdx = header->writeIdx;
	LOGD("readIdx %d, writeIdx %d", data_ptr->header.readIdx, data_ptr->header.writeIdx);
	while (data_ptr->header.readIdx != data_ptr->header.writeIdx) {
		ret = hgsl_read_msg_from_ipcq(hgsl_dev, &(data_ptr->header), data_ptr->baseaddr,
			data_ptr->header.dwSize, msg_buffer,
			hgsl_dev->ipcq_settings[dev_id].PKMD2HGSL_rb_offset,
			hgsl_dev->ipcq_settings[dev_id].PKMD2HGSL_queue_offset, gmugos->dev_hnd);
		if (ret)
			break;

		hgsl_ipcq_msg_handler(hgsl_dev, msg_buffer, gmugos->dev_hnd);
		data_ptr->header.writeIdx = header->writeIdx;
	}

out:
	mutex_unlock(&hgsl_dev->mutex);
exit:
	return;
}

static irqreturn_t hgsl_gmugos_isr(int num, void *data)
{
	struct hgsl_gmugos_irq *gmugos_irq = data;
	u32 val;

	val = hgsl_regmap_read(gmugos_irq->regmap, GMUGOS_REG_STATUS);
	hgsl_regmap_write(gmugos_irq->regmap, GMUGOS_REG_CLR, val);
	if (val & RGSGOS_IRQ_MASK) {
		// call new bottom half handler here
		queue_work(gmugos_irq->irq_workqueue, &gmugos_irq->irq_work);

		return (val == RGSGOS_IRQ_MASK) ? IRQ_HANDLED : IRQ_WAKE_THREAD;
	}

	return IRQ_WAKE_THREAD;
}

static int hgsl_gmugos_request_irq(struct platform_device *pdev, const  char *name,
		irq_handler_t handler, irq_handler_t thread_fn, void *data)
{
	int ret, num = platform_get_irq_byname(pdev, name);

	if (num < 0)
		return num;

	ret = devm_request_threaded_irq(&pdev->dev, num, handler,
		thread_fn, IRQF_TRIGGER_HIGH | IRQF_SHARED, name, data);

	if (ret) {
		dev_err(&pdev->dev, "Unable to get interrupt %s: %d\n",
			name, ret);
		return ret;
	}

	disable_irq(num);
	return num;
}

static int hgsl_syscon_node_regmap(struct hgsl_gmugos *gmugos,
				u32 dev_id, u32 irq_idx)
{
	char syscon_name[HGSL_GMUGOS_NAME_LEN];
	struct regmap *regmap;
	int ret = 0;

	if (gmugos->irq[irq_idx].regmap)
		goto out;

	snprintf(syscon_name, sizeof(syscon_name),
			"qcom,hgsl-gmugos%u-ipc%u", dev_id, irq_idx);
	regmap = syscon_regmap_lookup_by_compatible(syscon_name);
	if (IS_ERR_OR_NULL(regmap)) {
		LOGE("failed to regmap node\n");
		ret = regmap ? PTR_ERR(regmap) : -EINVAL;
		goto out;
	}

	gmugos->irq[irq_idx].regmap = regmap;
out:
	return ret;
}

void hgsl_gmugos_irq_enable(
	struct hgsl_gmugos_irq *gmugos_irq,
	u32 mask_bits)
{
	u32 val;

	if (!gmugos_irq->num) {
		LOGW("Invalid gmugos irq, ignore");
		return;
	}

	/* Clear pending IRQs */
	hgsl_regmap_write(gmugos_irq->regmap, GMUGOS_REG_CLR,
			mask_bits);

	/* Mask needed IRQs */
	val = hgsl_regmap_read(gmugos_irq->regmap, GMUGOS_REG_MASK);
	hgsl_regmap_write(gmugos_irq->regmap, GMUGOS_REG_MASK,
			val | mask_bits);
}

void hgsl_gmugos_irq_trigger(struct hgsl_gmugos *gmugos,
				u32 irq_idx, u32 bit_id)
{
	if (irq_idx >= HGSL_GMUGOS_IRQ_NUM ||
		!gmugos->irq[irq_idx].num) {
		LOGE("Invalid irq index %u", irq_idx);
		return;
	}

	hgsl_regmap_write(gmugos->irq[irq_idx].regmap,
		GMUGOS_REG_SET, BIT(bit_id));
}

void hgsl_gmugos_irq_free(struct hgsl_gmugos_irq *irq)
{
	if (!irq->num)
		return;

	/* Disable and free IRQ */
	disable_irq(irq->num);
	if (irq->irq_workqueue) {
		flush_workqueue(irq->irq_workqueue);
		destroy_workqueue(irq->irq_workqueue);
		irq->irq_workqueue = NULL;
	}

	free_irq(irq->num, irq);
	irq->num = 0;
}

int hgsl_init_gmugos(struct platform_device *pdev, uint32_t devhandle,
	u32 irq_idx, u32 mask_bits)
{
	struct qcom_hgsl *hgsl = platform_get_drvdata(pdev);
	struct hgsl_gmugos *gmugos;
	struct hgsl_gmugos_irq *gmugos_irq;
	char *name;
	int ret = 0;
	u32 dev_id = hgsl_hnd2id(devhandle);
	int irq_num = 0;

	if (dev_id >= HGSL_DEVICE_NUM) {
		LOGE("Invalid dev handle %u for dev_id %u\n", devhandle, dev_id);
		return -EFAULT;
	}

	if (irq_idx >= HGSL_GMUGOS_IRQ_NUM) {
		LOGE("Invalid irq index %u for devhandle %u\n", irq_idx, devhandle);
		ret = -EFAULT;
		return ret;
	}

	mutex_lock(&hgsl->mutex);
	gmugos = &hgsl->gmugos[dev_id];
	if ((gmugos->activated_irq[dev_id][irq_idx] & mask_bits) == mask_bits) {
		LOGD("mask bit 0x%x already enabled, id %d idx %d", mask_bits, dev_id, irq_idx);
		goto out;
	}

	ret = hgsl_syscon_node_regmap(gmugos, dev_id, irq_idx);
	if (ret)
		goto out;

	gmugos->dev_hnd = devhandle;
	gmugos_irq = &gmugos->irq[irq_idx];
	name = gmugos_irq->name;
	snprintf(name, sizeof(gmugos_irq->name), "hgsl_gmugos%u_irq%u",
			dev_id, irq_idx);

	if (mask_bits & RGSGOS_IRQ_MASK) {
		if (!gmugos_irq->irq_workqueue) {
			gmugos_irq->irq_workqueue =
				alloc_ordered_workqueue("%s_workqueue", WQ_MEM_RECLAIM, name);
			if (!gmugos_irq->irq_workqueue) {
				LOGE("FAILED to allocated %s wq", name);
				ret = -ENOMEM;
				goto out;
			} else {
				INIT_WORK(&gmugos_irq->irq_work, hgsl_gmugos_handle_ipcq_msg);
			}
		}
	} else if (mask_bits & GMUGOS_IRQ_MASK) {
		LOGI("Enable GMUGOS mask, name %s", name);
	} else {
		LOGE("invalid maskbit provided, 0x%x", mask_bits);
		ret = -EFAULT;
		goto out;
	}

	if (gmugos->activated_irq[dev_id][irq_idx])
		goto add_mask;

	irq_num = hgsl_gmugos_request_irq(pdev, name, hgsl_gmugos_isr,
				hgsl_gmugos_ts_retire, gmugos_irq);
	if (irq_num < 0) {
		dev_err(&pdev->dev, "failed to request gmugos irq %s, irq %u\n",
			name, irq_num);
		goto out;
	}

	gmugos_irq->id = irq_idx;
	gmugos_irq->num = irq_num;
	/* Enable IRQ */
	enable_irq(gmugos_irq->num);
add_mask:
	hgsl_gmugos_irq_enable(gmugos_irq, mask_bits);
	gmugos->activated_irq[dev_id][irq_idx] |= mask_bits;
out:
	mutex_unlock(&hgsl->mutex);
	return ret;
}

