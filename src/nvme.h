/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Minimal NVMe controller front-end for the vfio-user KV SSD prototype.
 *
 * This owns the controller register block, the admin/IO queue engine and the
 * data-transfer plumbing that the QEMU hw/nvme model provided for free. The KV
 * command set is layered on top (see kv.{h,c}).
 */
#ifndef KVSSD_NVME_H
#define KVSSD_NVME_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include <libvfio-user.h>

#include "kv.h"
#include "kvssd_nvme_spec.h"

/* Total queues including the admin queue at index 0. */
#define NVME_MAX_QUEUES 64
#define NVME_BAR0_SIZE 0x4000 /* registers + doorbells */

/* MSI-X: one vector per queue. Table + PBA live in BAR4. */
#define NVME_NUM_IRQS NVME_MAX_QUEUES
#define NVME_MSIX_BAR 4
#define NVME_MSIX_TABLE_OFFSET 0
#define NVME_MSIX_PBA_OFFSET 1024 /* NVME_NUM_IRQS * 16 */
#define NVME_MSIX_BAR_SIZE 4096

struct nvme_cq {
	uint64_t dma;  /* guest physical base address */
	uint32_t size; /* number of entries */
	uint16_t qid;
	uint16_t head; /* updated by the CQ head doorbell */
	uint16_t tail; /* next slot the controller will write */
	uint8_t phase; /* phase tag the controller writes */
	int irq;       /* interrupt vector, -1 if none */
	bool enabled;
};

struct nvme_sq {
	uint64_t dma;
	uint32_t size;
	uint16_t qid;
	uint16_t cqid;
	uint16_t head; /* next SQE the controller will fetch */
	uint16_t tail; /* updated by the SQ tail doorbell */
	bool enabled;
};

struct nvme_ctrl {
	vfu_ctx_t *vfu;

	/* Controller register block (register semantics, host byte order). */
	uint64_t cap;
	uint32_t vs;
	uint32_t intms;
	uint32_t intmc;
	uint32_t cc;
	uint32_t csts;
	uint32_t aqa;
	uint64_t asq;
	uint64_t acq;

	struct nvme_sq sq[NVME_MAX_QUEUES];
	struct nvme_cq cq[NVME_MAX_QUEUES];

	/* Backing store for the MSI-X table + PBA (BAR4). The actual interrupt
	 * routing is set up by the client via VFIO_USER_DEVICE_SET_IRQS. */
	uint8_t msix_region[NVME_MSIX_BAR_SIZE];

	struct kv_ns ns;
};

int
nvme_ctrl_init(struct nvme_ctrl *n, vfu_ctx_t *vfu);
void
nvme_ctrl_teardown(struct nvme_ctrl *n);

/* BAR0 access callback (matches vfu_region_access_cb_t). */
ssize_t
nvme_bar0_access(vfu_ctx_t *vfu, char *buf, size_t count, loff_t offset, bool is_write);

/* MSI-X table/PBA (BAR4) access callback. */
ssize_t
nvme_msix_access(vfu_ctx_t *vfu, char *buf, size_t count, loff_t offset, bool is_write);

/*
 * Move 'len' bytes between a host buffer described by prp1/prp2 and a local
 * buffer. 'to_host' selects controller-to-host (write) vs host-to-controller
 * (read). Returns 0 on success, -1 on failure.
 */
int
nvme_prp_xfer(vfu_ctx_t *vfu, uint64_t prp1, uint64_t prp2, void *buf, size_t len, bool to_host);

/* Dispatch a KV I/O command to the ported handlers. Returns an NVMe status. */
uint16_t
nvme_io_kv(struct nvme_ctrl *n, const NvmeSqe *sqe);

#endif /* KVSSD_NVME_H */
