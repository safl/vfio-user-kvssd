/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "nvme.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Controller register offsets. */
#define NVME_REG_CAP 0x00
#define NVME_REG_VS 0x08
#define NVME_REG_INTMS 0x0c
#define NVME_REG_INTMC 0x10
#define NVME_REG_CC 0x14
#define NVME_REG_CSTS 0x1c
#define NVME_REG_AQA 0x24
#define NVME_REG_ASQ 0x28
#define NVME_REG_ACQ 0x30
#define NVME_REG_DOORBELL 0x1000

#define NVME_CC_EN 0x1u
#define NVME_CC_SHN_SHIFT 14
#define NVME_CC_SHN_MASK (0x3u << NVME_CC_SHN_SHIFT)
#define NVME_CSTS_RDY 0x1u
#define NVME_CSTS_SHST_MASK (0x3u << 2)
#define NVME_CSTS_SHST_COMPLETE (0x2u << 2) /* shutdown processing complete */

/* Admin command opcodes. */
#define NVME_ADM_DELETE_SQ 0x00
#define NVME_ADM_CREATE_SQ 0x01
#define NVME_ADM_GET_LOG_PAGE 0x02
#define NVME_ADM_DELETE_CQ 0x04
#define NVME_ADM_CREATE_CQ 0x05
#define NVME_ADM_IDENTIFY 0x06
#define NVME_ADM_SET_FEATURES 0x09
#define NVME_ADM_GET_FEATURES 0x0a
#define NVME_ADM_ASYNC_EVENT 0x0c

/* Feature identifiers. */
#define NVME_FEAT_NUM_QUEUES 0x07
#define NVME_FEAT_KV_EDNEK 0x20

/* Identify CNS values. */
#define NVME_ID_CNS_NS 0x00
#define NVME_ID_CNS_CTRL 0x01
#define NVME_ID_CNS_NS_ACTIVE_LIST 0x02
#define NVME_ID_CNS_NS_DESC_LIST 0x03
#define NVME_ID_CNS_CS_NS 0x05
#define NVME_ID_CNS_CS_CTRL 0x06
#define NVME_ID_CNS_NS_INDEPENDENT 0x08
#define NVME_ID_CNS_IO_CMD_SET 0x1c

/* Sentinel: command consumed but no completion should be posted (e.g. AER). */
#define NVME_NO_COMPLETE 0xffffu

#define NVME_STATUS_INVALID_QID 0x0101 /* SCT command-specific, SC 0x01 */

int nvme_trace_enabled;

int
nvme_ctrl_init(struct nvme_ctrl *n, vfu_ctx_t *vfu)
{
	memset(n, 0, sizeof(*n));
	n->vfu = vfu;

	nvme_trace_enabled = getenv("VFU_KVSSD_TRACE") != NULL;

	/*
	 * CAP: MQES=1023, CQR=1, TO=0xf, DSTRD=0,
	 * CSS = NVM command set (bit 0) + I/O command sets supported (bit 6),
	 * so the host can select the KV command set via CC.CSS=110b.
	 */
	n->cap = 0x3ffull;
	n->cap |= (1ull << 16);   /* CQR */
	n->cap |= (0xfull << 24); /* TO */
	n->cap |= (1ull << 37);   /* CSS bit 0: NVM command set */
	n->cap |= (1ull << 43);   /* CSS bit 6: one or more I/O command sets */

	n->vs = 0x00020000; /* NVMe 2.0.0 */

	return kv_ns_init(&n->ns);
}

void
nvme_ctrl_teardown(struct nvme_ctrl *n)
{
	kv_ns_teardown(&n->ns);
}

/* -------------------------------------------------------------------------- */
/* DMA helpers                                                                */
/* -------------------------------------------------------------------------- */

#define SG_AT(base, i) ((dma_sg_t *)((char *)(base) + (size_t)(i) * dma_sg_size()))
#define NVME_MAX_SG 16

/*
 * Copy between a local buffer and an already-built sgl using a local mapping
 * (vfu_sgl_get). This memcpys directly to/from the mmap'd DMA region; it must
 * NOT use vfu_sgl_read/write, which issue server-to-client socket commands and
 * deadlock/reset when called from within a synchronous region-access callback.
 */
static int
nvme_sgl_copy(vfu_ctx_t *vfu, dma_sg_t *sgl, int nsg, void *buf, size_t len, bool to_host)
{
	struct iovec iov[NVME_MAX_SG];
	size_t done = 0;

	if (nsg <= 0 || nsg > NVME_MAX_SG) {
		return -1;
	}
	if (vfu_sgl_get(vfu, sgl, iov, (size_t)nsg, 0) != 0) {
		return -1;
	}
	for (int i = 0; i < nsg && done < len; i++) {
		size_t c = iov[i].iov_len;
		if (done + c > len) {
			c = len - done;
		}
		if (to_host) {
			memcpy(iov[i].iov_base, (char *)buf + done, c);
		} else {
			memcpy((char *)buf + done, iov[i].iov_base, c);
		}
		done += c;
	}
	vfu_sgl_put(vfu, sgl, iov, (size_t)nsg);
	return (done == len) ? 0 : -1;
}

/* Transfer a contiguous guest physical range (e.g. a queue entry). */
static int
nvme_gpa_xfer(vfu_ctx_t *vfu, uint64_t gpa, void *buf, size_t len, bool to_host)
{
	const size_t max_sg = 8;
	int prot = to_host ? PROT_WRITE : PROT_READ;
	dma_sg_t *sgl;
	int r, rc = -1;

	if (len == 0) {
		return 0;
	}
	sgl = malloc(dma_sg_size() * max_sg);
	if (!sgl) {
		return -1;
	}
	r = vfu_addr_to_sgl(vfu, (vfu_dma_addr_t)(uintptr_t)gpa, len, sgl, max_sg, prot);
	if (r <= 0) {
		goto out;
	}
	rc = nvme_sgl_copy(vfu, sgl, r, buf, len, to_host);
out:
	free(sgl);
	return rc;
}

int
nvme_prp_xfer(vfu_ctx_t *vfu, uint64_t prp1, uint64_t prp2, void *buf, size_t len, bool to_host)
{
	const size_t ps = KVSSD_PAGE_SIZE;
	const size_t max_sg = 16;
	int prot = to_host ? PROT_WRITE : PROT_READ;
	dma_sg_t *sgl;
	size_t nsg = 0;
	int r, rc = -1;

	if (len == 0) {
		return 0;
	}
	sgl = malloc(dma_sg_size() * max_sg);
	if (!sgl) {
		return -1;
	}

	size_t off = (size_t)(prp1 & (ps - 1));
	size_t first = MIN(len, ps - off);
	r = vfu_addr_to_sgl(vfu, (vfu_dma_addr_t)(uintptr_t)prp1, first, SG_AT(sgl, nsg),
			    max_sg - nsg, prot);
	if (r <= 0) {
		NVME_TRACE("prp_xfer: addr_to_sgl(prp1=0x%llx len=%zu) failed r=%d",
			   (unsigned long long)prp1, first, r);
		goto out;
	}
	nsg += (size_t)r;

	size_t remaining = len - first;
	if (remaining) {
		if (remaining > ps) {
			/* TODO: PRP lists / chaining. Prototype transfers (KV
			 * values and identify pages) are at most two pages; the
			 * controller advertises MDTS accordingly. */
			NVME_TRACE("prp_xfer: UNSUPPORTED >2-page transfer len=%zu (needs PRP list)",
				   len);
			goto out;
		}
		r = vfu_addr_to_sgl(vfu, (vfu_dma_addr_t)(uintptr_t)prp2, remaining,
				    SG_AT(sgl, nsg), max_sg - nsg, prot);
		if (r <= 0) {
			goto out;
		}
		nsg += (size_t)r;
	}

	rc = nvme_sgl_copy(vfu, sgl, (int)nsg, buf, len, to_host);
out:
	free(sgl);
	return rc;
}

/* -------------------------------------------------------------------------- */
/* Completion posting                                                         */
/* -------------------------------------------------------------------------- */

static void
nvme_post_cqe(struct nvme_ctrl *n, uint16_t cqid, uint16_t sqid, uint16_t sq_head, uint16_t cid,
	      uint32_t cdw0, uint16_t status)
{
	struct nvme_cq *cq = &n->cq[cqid];
	NvmeCqe cqe;

	if (!cq->enabled) {
		return;
	}

	memset(&cqe, 0, sizeof(cqe));
	cqe.cdw0 = cdw0;
	cqe.sq_head = sq_head;
	cqe.sq_id = sqid;
	cqe.cid = cid;
	cqe.status = (uint16_t)((status << 1) | (cq->phase & 1));

	nvme_gpa_xfer(n->vfu, cq->dma + (uint64_t)cq->tail * sizeof(NvmeCqe), &cqe, sizeof(cqe),
		      true);

	cq->tail++;
	if (cq->tail == cq->size) {
		cq->tail = 0;
		cq->phase ^= 1;
	}

	/*
	 * Raise the queue's MSI-X interrupt. vfu_irq_trigger only writes an
	 * eventfd (no socket command), so it is safe from within this
	 * synchronous region-access callback. If the client has not configured
	 * the vector (e.g. the polling harness), it is a harmless no-op.
	 */
	if (cq->irq >= 0) {
		vfu_irq_trigger(n->vfu, (uint32_t)cq->irq);
	}
}

/* -------------------------------------------------------------------------- */
/* Admin commands                                                             */
/* -------------------------------------------------------------------------- */

static void
nvme_fill_id_ctrl(uint8_t *buf)
{
	memset(buf, 0, KVSSD_PAGE_SIZE);

	buf[0x00] = 0x1d; /* VID */
	buf[0x01] = 0x1d;
	memcpy(buf + 0x04, "KVSSD0000000000000001", 20); /* SN  */
	memcpy(buf + 0x18, "vfio-user-kvssd", 15);       /* MN  */
	memcpy(buf + 0x40, "0.1     ", 8);               /* FR  */
	buf[0x4d] = 0x01;                                /* MDTS = 2 pages */
	buf[0x50] = 0x00;                                /* VER 2.0.0 */
	buf[0x51] = 0x00;
	buf[0x52] = 0x02;
	buf[0x53] = 0x00;
	buf[0x200] = 0x66; /* SQES: min/max 64B */
	buf[0x201] = 0x44; /* CQES: min/max 16B */
	buf[0x204] = 0x01; /* NN = 1 */
}

static uint16_t
nvme_admin_identify(struct nvme_ctrl *n, const NvmeSqe *sqe)
{
	uint8_t cns = sqe->cdw10 & 0xff;
	uint8_t csi = (sqe->cdw11 >> 24) & 0xff;
	uint8_t buf[KVSSD_PAGE_SIZE];

	NVME_TRACE("identify: cns=0x%02x csi=0x%02x nsid=%u", cns, csi, sqe->nsid);

	memset(buf, 0, sizeof(buf));

	switch (cns) {
	case NVME_ID_CNS_CTRL:
		nvme_fill_id_ctrl(buf);
		break;
	case NVME_ID_CNS_NS:
		/* NVM Identify Namespace for a KV namespace: zeros. */
		break;
	case NVME_ID_CNS_NS_ACTIVE_LIST: {
		uint32_t *list = (uint32_t *)buf;
		list[0] = 1; /* nsid 1 */
		break;
	}
	case NVME_ID_CNS_NS_DESC_LIST:
		buf[0] = 0x04;        /* NIDT = CSI */
		buf[1] = 0x01;        /* NIDL = 1   */
		buf[4] = NVME_CSI_KV; /* CSI value  */
		break;
	case NVME_ID_CNS_CS_NS:
		if (csi == NVME_CSI_KV) {
			memcpy(buf, n->ns.id_ns_kv, sizeof(NvmeIdNsKV));
		}
		break;
	case NVME_ID_CNS_CS_CTRL:
		/* KV: command-set specific Identify Controller is empty. */
		break;
	case NVME_ID_CNS_NS_INDEPENDENT:
		/* Command-set independent Identify Namespace (NvmeIdNsIndependent):
		 * nmic@1, nstat@14. nstat bit0 (NRDY) = namespace ready. The
		 * guest kernel uses this to enumerate a ns whose CSI it does not
		 * attach as a block device (it exposes /dev/ngXnY instead). */
		buf[14] = 0x01;
		break;
	case NVME_ID_CNS_IO_CMD_SET:
		/* I/O Command Set data structure: byte 0 is the command-set bit
		 * vector for combination 0. Advertise NVM (bit 0). KV is driven
		 * via the generic char device, not a kernel-attached command
		 * set, so it is intentionally not advertised here (matches the
		 * SamsungDS/qemu for-xnvme behaviour). */
		buf[0] = (uint8_t)(1u << NVME_CSI_NVM);
		break;
	default:
		return NVME_INVALID_FIELD | NVME_DNR;
	}

	if (nvme_prp_xfer(n->vfu, sqe->prp1, sqe->prp2, buf, sizeof(buf), true)) {
		return NVME_INVALID_FIELD | NVME_DNR;
	}
	return NVME_SUCCESS;
}

static uint16_t
nvme_admin_set_features(struct nvme_ctrl *n, const NvmeSqe *sqe, uint32_t *cdw0)
{
	uint8_t fid = sqe->cdw10 & 0xff;

	switch (fid) {
	case NVME_FEAT_NUM_QUEUES: {
		uint16_t nsqr = sqe->cdw11 & 0xffff;
		uint16_t ncqr = (sqe->cdw11 >> 16) & 0xffff;
		uint16_t max0 = NVME_MAX_QUEUES - 2; /* 0-based count of IO queues */
		uint16_t gs = MIN(nsqr, max0);
		uint16_t gc = MIN(ncqr, max0);
		*cdw0 = ((uint32_t)gc << 16) | gs;
		return NVME_SUCCESS;
	}
	case NVME_FEAT_KV_EDNEK:
		n->ns.ednek = sqe->cdw11;
		return NVME_SUCCESS;
	default:
		return NVME_SUCCESS; /* accept silently */
	}
}

static uint16_t
nvme_admin_get_features(struct nvme_ctrl *n, const NvmeSqe *sqe, uint32_t *cdw0)
{
	uint8_t fid = sqe->cdw10 & 0xff;

	switch (fid) {
	case NVME_FEAT_NUM_QUEUES: {
		uint16_t max0 = NVME_MAX_QUEUES - 2;
		*cdw0 = ((uint32_t)max0 << 16) | max0;
		return NVME_SUCCESS;
	}
	case NVME_FEAT_KV_EDNEK:
		*cdw0 = n->ns.ednek;
		return NVME_SUCCESS;
	default:
		*cdw0 = 0;
		return NVME_SUCCESS;
	}
}

static uint16_t
nvme_admin_create_cq(struct nvme_ctrl *n, const NvmeSqe *sqe)
{
	uint16_t qid = sqe->cdw10 & 0xffff;
	uint16_t qsize = (sqe->cdw10 >> 16) & 0xffff; /* 0-based */
	uint16_t iv = (sqe->cdw11 >> 16) & 0xffff;
	struct nvme_cq *cq;

	if (qid == 0 || qid >= NVME_MAX_QUEUES) {
		return NVME_STATUS_INVALID_QID | NVME_DNR;
	}
	cq = &n->cq[qid];
	memset(cq, 0, sizeof(*cq));
	cq->dma = sqe->prp1;
	cq->size = (uint32_t)qsize + 1;
	cq->qid = qid;
	cq->phase = 1;
	cq->irq = (int)iv; /* recorded; INTx used regardless for now */
	cq->enabled = true;
	NVME_TRACE("create_cq qid=%u size=%u iv=%u dma=0x%llx", qid, cq->size, iv,
		   (unsigned long long)cq->dma);
	return NVME_SUCCESS;
}

static uint16_t
nvme_admin_create_sq(struct nvme_ctrl *n, const NvmeSqe *sqe)
{
	uint16_t qid = sqe->cdw10 & 0xffff;
	uint16_t qsize = (sqe->cdw10 >> 16) & 0xffff;
	uint16_t cqid = (sqe->cdw11 >> 16) & 0xffff;
	struct nvme_sq *sq;

	if (qid == 0 || qid >= NVME_MAX_QUEUES || cqid >= NVME_MAX_QUEUES ||
	    !n->cq[cqid].enabled) {
		return NVME_STATUS_INVALID_QID | NVME_DNR;
	}
	sq = &n->sq[qid];
	memset(sq, 0, sizeof(*sq));
	sq->dma = sqe->prp1;
	sq->size = (uint32_t)qsize + 1;
	sq->qid = qid;
	sq->cqid = cqid;
	sq->enabled = true;
	NVME_TRACE("create_sq qid=%u size=%u cqid=%u dma=0x%llx", qid, sq->size, cqid,
		   (unsigned long long)sq->dma);
	return NVME_SUCCESS;
}

static uint16_t
nvme_admin_delete_cq(struct nvme_ctrl *n, const NvmeSqe *sqe)
{
	uint16_t qid = sqe->cdw10 & 0xffff;

	if (qid == 0 || qid >= NVME_MAX_QUEUES) {
		return NVME_STATUS_INVALID_QID | NVME_DNR;
	}
	n->cq[qid].enabled = false;
	return NVME_SUCCESS;
}

static uint16_t
nvme_admin_delete_sq(struct nvme_ctrl *n, const NvmeSqe *sqe)
{
	uint16_t qid = sqe->cdw10 & 0xffff;

	if (qid == 0 || qid >= NVME_MAX_QUEUES) {
		return NVME_STATUS_INVALID_QID | NVME_DNR;
	}
	n->sq[qid].enabled = false;
	return NVME_SUCCESS;
}

static uint16_t
nvme_admin_cmd(struct nvme_ctrl *n, const NvmeSqe *sqe, uint32_t *cdw0)
{
	*cdw0 = 0;

	switch (sqe->opcode) {
	case NVME_ADM_IDENTIFY:
		return nvme_admin_identify(n, sqe);
	case NVME_ADM_SET_FEATURES:
		return nvme_admin_set_features(n, sqe, cdw0);
	case NVME_ADM_GET_FEATURES:
		return nvme_admin_get_features(n, sqe, cdw0);
	case NVME_ADM_CREATE_CQ:
		return nvme_admin_create_cq(n, sqe);
	case NVME_ADM_CREATE_SQ:
		return nvme_admin_create_sq(n, sqe);
	case NVME_ADM_DELETE_CQ:
		return nvme_admin_delete_cq(n, sqe);
	case NVME_ADM_DELETE_SQ:
		return nvme_admin_delete_sq(n, sqe);
	case NVME_ADM_GET_LOG_PAGE:
		/* Stub: return a zeroed log page. */
		return NVME_SUCCESS;
	case NVME_ADM_ASYNC_EVENT:
		/* Leave outstanding; do not complete. */
		return NVME_NO_COMPLETE;
	default:
		return NVME_INVALID_OPCODE | NVME_DNR;
	}
}

/* -------------------------------------------------------------------------- */
/* KV I/O dispatch                                                            */
/* -------------------------------------------------------------------------- */

static int
kvdma_h2c(void *ctx, const NvmeKVCmd *cmd, void *dst, size_t len)
{
	struct nvme_ctrl *n = ctx;
	return nvme_prp_xfer(n->vfu, cmd->prp1, cmd->prp2, dst, len, false);
}

static int
kvdma_c2h(void *ctx, const NvmeKVCmd *cmd, const void *src, size_t len)
{
	struct nvme_ctrl *n = ctx;
	return nvme_prp_xfer(n->vfu, cmd->prp1, cmd->prp2, (void *)src, len, true);
}

uint16_t
nvme_io_kv(struct nvme_ctrl *n, const NvmeSqe *sqe)
{
	const NvmeKVCmd *cmd = (const NvmeKVCmd *)sqe;
	struct kv_dma dma = {.h2c = kvdma_h2c, .c2h = kvdma_c2h, .ctx = n};

	switch (cmd->opcode) {
	case NVME_CMD_KV_STORE:
		return kv_store(&n->ns, cmd, &dma);
	case NVME_CMD_KV_RETRIEVE:
		return kv_retrieve(&n->ns, cmd, &dma);
	case NVME_CMD_KV_DELETE:
		return kv_delete(&n->ns, cmd, &dma);
	case NVME_CMD_KV_EXIST:
		return kv_exist(&n->ns, cmd, &dma);
	case NVME_CMD_KV_LIST:
		return kv_list(&n->ns, cmd, &dma);
	default:
		return NVME_INVALID_OPCODE | NVME_DNR;
	}
}

/* -------------------------------------------------------------------------- */
/* Queue engine                                                               */
/* -------------------------------------------------------------------------- */

static void
nvme_process_sq(struct nvme_ctrl *n, uint16_t qid)
{
	struct nvme_sq *sq = &n->sq[qid];

	if (!sq->enabled) {
		return;
	}

	while (sq->head != sq->tail) {
		NvmeSqe sqe;
		uint32_t cdw0 = 0;
		uint16_t status;

		if (nvme_gpa_xfer(n->vfu, sq->dma + (uint64_t)sq->head * sizeof(NvmeSqe), &sqe,
				  sizeof(sqe), false)) {
			return; /* DMA failure; cannot make progress */
		}
		sq->head = (uint16_t)((sq->head + 1) % sq->size);

		status = (qid == 0) ? nvme_admin_cmd(n, &sqe, &cdw0) : nvme_io_kv(n, &sqe);
		NVME_TRACE("%s q%u opc=0x%02x cid=%u -> status=0x%04x", qid == 0 ? "adm" : "io", qid,
			   sqe.opcode, sqe.cid, status);
		if (status == NVME_NO_COMPLETE) {
			continue;
		}
		nvme_post_cqe(n, sq->cqid, qid, sq->head, sqe.cid, cdw0, status);
	}
}

static void
nvme_enable(struct nvme_ctrl *n)
{
	struct nvme_sq *asq = &n->sq[0];
	struct nvme_cq *acq = &n->cq[0];

	memset(asq, 0, sizeof(*asq));
	memset(acq, 0, sizeof(*acq));

	asq->dma = n->asq;
	asq->size = (n->aqa & 0xfff) + 1; /* ASQS, 0-based */
	asq->qid = 0;
	asq->cqid = 0;
	asq->enabled = true;

	acq->dma = n->acq;
	acq->size = ((n->aqa >> 16) & 0xfff) + 1; /* ACQS, 0-based */
	acq->qid = 0;
	acq->phase = 1;
	acq->irq = 0;
	acq->enabled = true;

	/* Fresh enable: ready, shutdown status cleared. */
	n->csts = NVME_CSTS_RDY;

	NVME_TRACE("enable: asq=0x%llx acq=0x%llx asqs=%u acqs=%u -> csts=0x%x",
		   (unsigned long long)n->asq, (unsigned long long)n->acq, asq->size, acq->size,
		   n->csts);
}

static void
nvme_disable(struct nvme_ctrl *n)
{
	for (int i = 0; i < NVME_MAX_QUEUES; i++) {
		n->sq[i].enabled = false;
		n->cq[i].enabled = false;
	}
	n->csts &= ~NVME_CSTS_RDY;

	NVME_TRACE("disable: all queues down -> csts=0x%x", n->csts);
}

void
nvme_ctrl_reset(struct nvme_ctrl *n)
{
	NVME_TRACE("reset: cc=0x%x csts=0x%x -> clear", n->cc, n->csts);

	nvme_disable(n);
	n->cc = 0;
	n->csts = 0;
	n->aqa = 0;
	n->asq = 0;
	n->acq = 0;
	n->intms = 0;
	n->intmc = 0;
}

/* -------------------------------------------------------------------------- */
/* Register access                                                            */
/* -------------------------------------------------------------------------- */

static uint64_t
rd_le(const char *b, size_t n)
{
	uint64_t v = 0;
	for (size_t i = 0; i < n; i++) {
		v |= (uint64_t)(uint8_t)b[i] << (8 * i);
	}
	return v;
}

static void
wr_le(char *b, size_t n, uint64_t v)
{
	for (size_t i = 0; i < n; i++) {
		b[i] = (char)((v >> (8 * i)) & 0xff);
	}
}

static void
nvme_reg_read(struct nvme_ctrl *n, size_t offset, char *buf, size_t count)
{
	uint8_t regs[0x40];

	memset(regs, 0, sizeof(regs));
	wr_le((char *)regs + NVME_REG_CAP, 8, n->cap);
	wr_le((char *)regs + NVME_REG_VS, 4, n->vs);
	wr_le((char *)regs + NVME_REG_INTMS, 4, n->intms);
	wr_le((char *)regs + NVME_REG_INTMC, 4, n->intmc);
	wr_le((char *)regs + NVME_REG_CC, 4, n->cc);
	wr_le((char *)regs + NVME_REG_CSTS, 4, n->csts);
	wr_le((char *)regs + NVME_REG_AQA, 4, n->aqa);
	wr_le((char *)regs + NVME_REG_ASQ, 8, n->asq);
	wr_le((char *)regs + NVME_REG_ACQ, 8, n->acq);

	if (offset + count <= sizeof(regs)) {
		memcpy(buf, regs + offset, count);
	} else {
		memset(buf, 0, count);
	}
}

static void
nvme_reg_write(struct nvme_ctrl *n, size_t offset, const char *buf, size_t count)
{
	uint64_t v = rd_le(buf, count);

	switch (offset) {
	case NVME_REG_INTMS:
		n->intms = (uint32_t)v;
		break;
	case NVME_REG_INTMC:
		n->intmc = (uint32_t)v;
		break;
	case NVME_REG_CC: {
		uint32_t old = n->cc;
		uint32_t shn = (uint32_t)v & NVME_CC_SHN_MASK;
		n->cc = (uint32_t)v;
		NVME_TRACE("CC write: 0x%x -> 0x%x (en %u->%u shn=%u)", old, n->cc,
			   old & NVME_CC_EN, n->cc & NVME_CC_EN, shn >> NVME_CC_SHN_SHIFT);
		if ((n->cc & NVME_CC_EN) && !(old & NVME_CC_EN)) {
			nvme_enable(n);
		} else if (!(n->cc & NVME_CC_EN) && (old & NVME_CC_EN)) {
			nvme_disable(n);
		}
		/*
		 * Shutdown notification (CC.SHN != 0): tear the queues down and report
		 * SHST=complete so the host's controller-shutdown wait returns instead
		 * of timing out (~10s) and aborting -- a stuck shutdown wedges the
		 * controller across the driver unbind that the tests do every case.
		 */
		if (shn) {
			nvme_disable(n);
			n->csts = (n->csts & ~NVME_CSTS_SHST_MASK) | NVME_CSTS_SHST_COMPLETE;
			NVME_TRACE("shutdown: SHST=complete (csts=0x%x)", n->csts);
		}
		break;
	}
	case NVME_REG_AQA:
		n->aqa = (uint32_t)v;
		break;
	case NVME_REG_ASQ:
		n->asq = (count == 8) ? v : ((n->asq & ~0xffffffffull) | (v & 0xffffffff));
		break;
	case NVME_REG_ASQ + 4:
		n->asq = (n->asq & 0xffffffffull) | ((v & 0xffffffff) << 32);
		break;
	case NVME_REG_ACQ:
		n->acq = (count == 8) ? v : ((n->acq & ~0xffffffffull) | (v & 0xffffffff));
		break;
	case NVME_REG_ACQ + 4:
		n->acq = (n->acq & 0xffffffffull) | ((v & 0xffffffff) << 32);
		break;
	default:
		break;
	}
}

ssize_t
nvme_bar0_access(vfu_ctx_t *vfu, char *buf, size_t count, loff_t offset, bool is_write)
{
	struct nvme_ctrl *n = vfu_get_private(vfu);

	if (offset >= NVME_REG_DOORBELL) {
		if (is_write) {
			uint32_t val = (uint32_t)rd_le(buf, count);
			size_t idx = ((size_t)offset - NVME_REG_DOORBELL) / 4;
			uint16_t qid = (uint16_t)(idx / 2);
			bool is_cq = idx & 1;

			if (qid < NVME_MAX_QUEUES) {
				if (is_cq) {
					n->cq[qid].head = (uint16_t)val;
				} else {
					n->sq[qid].tail = (uint16_t)val;
					nvme_process_sq(n, qid);
				}
			}
		} else {
			memset(buf, 0, count);
		}
		return (ssize_t)count;
	}

	if (is_write) {
		nvme_reg_write(n, (size_t)offset, buf, count);
	} else {
		nvme_reg_read(n, (size_t)offset, buf, count);
	}
	return (ssize_t)count;
}

ssize_t
nvme_msix_access(vfu_ctx_t *vfu, char *buf, size_t count, loff_t offset, bool is_write)
{
	struct nvme_ctrl *n = vfu_get_private(vfu);

	if ((size_t)offset + count > sizeof(n->msix_region)) {
		return -1;
	}
	if (is_write) {
		memcpy(n->msix_region + offset, buf, count);
	} else {
		memcpy(buf, n->msix_region + offset, count);
	}
	return (ssize_t)count;
}
