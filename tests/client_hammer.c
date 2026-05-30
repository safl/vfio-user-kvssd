/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vfio-user-kvssd stress hammer.
 *
 * Drives the device via libvfio-user's tran_sock without QEMU. Hammers the
 * device's state machine the same way the Linux kernel + QEMU vfio-user-pci
 * client would under heavy bind/unbind/FLR/CC.EN churn, so that crashes,
 * wedges, or state-leak bugs reproduce locally in seconds instead of in CI.
 *
 * Modes:
 *   flr        VFIO_USER_DEVICE_RESET in a loop
 *   cc         CC.EN=1 -> wait RDY -> CC.EN=0 -> wait !RDY in a loop
 *   shn        CC.SHN=normal-shutdown -> wait SHST=complete -> CC.EN=0 in a loop
 *   dma        VFIO_USER_DMA_MAP / DMA_UNMAP cycles on varying regions
 *   doorbell   submit many admin commands, ring tail doorbell, drain CQEs
 *   reconnect  close socket -> reopen -> redo VERSION + DMA_MAP in a loop
 *   mayhem     interleave the above
 *   all        run each mode sequentially
 *
 * Usage:
 *   vfu_kvssd_hammer <socket> [--mode MODE] [--iters N] [--seed S] [--quiet]
 */
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "libvfio-user.h"
#include "vfio-user.h"
#include "tran_sock.h"

#include "kvssd_nvme_spec.h"

#define PAGE 4096u
#define NPAGES 32u
#define MEM_SIZE (NPAGES * PAGE)

#define ASQ_IOVA (0u * PAGE)
#define ACQ_IOVA (1u * PAGE)
#define DATA_IOVA (2u * PAGE)

/* Doorbell offsets (DSTRD=0, stride 4). */
#define DB_ASQ_TAIL 0x1000u
#define DB_ACQ_HEAD 0x1004u

#define Q_DEPTH 16u

#define REG_CAP 0x00u
#define REG_VS 0x08u
#define REG_CC 0x14u
#define REG_CSTS 0x1cu
#define REG_AQA 0x24u
#define REG_ASQ 0x28u
#define REG_ACQ 0x30u

#define CC_EN 0x1u
#define CC_SHN_NORMAL (0x1u << 14)
#define CC_SHN_MASK (0x3u << 14)
#define CSTS_RDY 0x1u
#define CSTS_SHST_MASK (0x3u << 2)
#define CSTS_SHST_COMPLETE (0x2u << 2)

struct ctx {
	int sock;
	int mem_fd;
	uint8_t *mem;
	bool verbose;
	unsigned seed;
	unsigned ops;
};

static const char *g_sock_path;

static uint32_t
xrand(struct ctx *c)
{
	c->seed = c->seed * 1103515245u + 12345u;
	return (c->seed >> 16) & 0x7fffu;
}

static int
init_sock(const char *path)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int sock;

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		err(EXIT_FAILURE, "socket");
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		err(EXIT_FAILURE, "connect %s", path);
	}
	return sock;
}

static void
negotiate(int sock)
{
	struct vfio_user_version cv = {.major = LIB_VFIO_USER_MAJOR, .minor = LIB_VFIO_USER_MINOR};
	char caps[256];
	int slen = snprintf(caps, sizeof(caps),
			    "{\"capabilities\":{\"max_msg_fds\":%u,"
			    "\"max_data_xfer_size\":%u}}",
			    8u, 1u << 20);
	struct iovec iov[3] = {0};
	struct vfio_user_header hdr;
	struct vfio_user_version *sv = NULL;
	size_t vlen;

	iov[1].iov_base = &cv;
	iov[1].iov_len = sizeof(cv);
	iov[2].iov_base = caps;
	iov[2].iov_len = slen + 1;

	if (tran_sock_send_iovec(sock, 1, false, VFIO_USER_VERSION, iov, 3, NULL, 0, 0) < 0) {
		err(EXIT_FAILURE, "send version");
	}
	if (tran_sock_recv_alloc(sock, &hdr, true, NULL, (void **)&sv, &vlen) < 0) {
		err(EXIT_FAILURE, "recv version");
	}
	free(sv);
}

static int
dma_map(int sock, int fd, uint64_t iova, uint64_t size, uint64_t offset)
{
	struct vfio_user_dma_map m = {
		.argsz = sizeof(m),
		.addr = iova,
		.size = size,
		.offset = offset,
		.flags = VFIO_USER_F_DMA_REGION_READ | VFIO_USER_F_DMA_REGION_WRITE,
	};
	struct iovec iov[2] = {0};

	iov[1].iov_base = &m;
	iov[1].iov_len = sizeof(m);
	return tran_sock_msg_iovec(sock, 2, VFIO_USER_DMA_MAP, iov, 2, &fd, 1, NULL, NULL, 0, NULL,
				   0);
}

static int
dma_unmap(int sock, uint64_t iova, uint64_t size)
{
	struct vfio_user_dma_unmap u = {
		.argsz = sizeof(u),
		.addr = iova,
		.size = size,
		.flags = 0,
	};
	struct iovec iov[2] = {0};

	iov[1].iov_base = &u;
	iov[1].iov_len = sizeof(u);
	return tran_sock_msg_iovec(sock, 3, VFIO_USER_DMA_UNMAP, iov, 2, NULL, 0, NULL, &u,
				   sizeof(u), NULL, 0);
}

static int
access_region(int sock, int region, bool is_write, uint64_t offset, void *data, size_t len)
{
	static int msg_id = 0xf00f;
	struct vfio_user_region_access req = {
		.offset = offset,
		.region = region,
		.count = len,
	};
	struct iovec iov[3] = {
		[1] = {.iov_base = &req, .iov_len = sizeof(req)},
		[2] = {.iov_base = data, .iov_len = len},
	};
	struct vfio_user_region_access *resp;
	size_t nr_iov, resp_len;
	int op, ret;

	if (is_write) {
		op = VFIO_USER_REGION_WRITE;
		nr_iov = 3;
		resp_len = sizeof(*resp);
	} else {
		op = VFIO_USER_REGION_READ;
		nr_iov = 2;
		resp_len = sizeof(*resp) + len;
	}
	resp = calloc(1, resp_len);
	if (!resp) {
		err(EXIT_FAILURE, "calloc");
	}
	ret = tran_sock_msg_iovec(sock, msg_id--, op, iov, nr_iov, NULL, 0, NULL, resp, resp_len,
				  NULL, 0);
	if (ret != 0) {
		free(resp);
		return ret;
	}
	if (!is_write) {
		memcpy(data, (char *)resp + sizeof(*resp), len);
	}
	free(resp);
	return 0;
}

static uint32_t
reg_rd32(int sock, uint64_t off)
{
	uint32_t v = 0;
	if (access_region(sock, VFU_PCI_DEV_BAR0_REGION_IDX, false, off, &v, 4) != 0) {
		errx(EXIT_FAILURE, "reg read %#lx", (unsigned long)off);
	}
	return v;
}

static void
reg_wr32(int sock, uint64_t off, uint32_t v)
{
	if (access_region(sock, VFU_PCI_DEV_BAR0_REGION_IDX, true, off, &v, 4) != 0) {
		errx(EXIT_FAILURE, "reg write %#lx", (unsigned long)off);
	}
}

static void
reg_wr64(int sock, uint64_t off, uint64_t v)
{
	if (access_region(sock, VFU_PCI_DEV_BAR0_REGION_IDX, true, off, &v, 8) != 0) {
		errx(EXIT_FAILURE, "reg write64 %#lx", (unsigned long)off);
	}
}

static int
device_reset(int sock)
{
	struct iovec iov[1] = {0};
	return tran_sock_msg_iovec(sock, 4, VFIO_USER_DEVICE_RESET, iov, 1, NULL, 0, NULL, NULL, 0,
				   NULL, 0);
}

/* Register N eventfds for the MSI-X IRQ index, starting at 'start'. The
 * device's vfu_irq_trigger() will signal these from then on. */
static int
set_irqs_msix(int sock, int *eventfds, unsigned start, unsigned n)
{
	struct vfio_irq_set req = {
		.argsz = sizeof(req),
		.flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = VFIO_PCI_MSIX_IRQ_INDEX,
		.start = start,
		.count = n,
	};
	struct iovec iov[2] = {[1] = {.iov_base = &req, .iov_len = sizeof(req)}};
	return tran_sock_msg_iovec(sock, 5, VFIO_USER_DEVICE_SET_IRQS, iov, 2, eventfds,
				   (int)n, NULL, NULL, 0, NULL, 0);
}

/* Bring CC.EN=1 with AQA/ASQ/ACQ programmed; wait CSTS.RDY=1. */
static void
controller_enable(int sock)
{
	uint32_t aqa = (uint32_t)((Q_DEPTH - 1) | ((Q_DEPTH - 1) << 16));
	uint32_t cc = (4u << 20) | (6u << 16) | CC_EN; /* CC.IOSQES=6, IOCQES=4, EN=1 */

	reg_wr32(sock, REG_AQA, aqa);
	reg_wr64(sock, REG_ASQ, ASQ_IOVA);
	reg_wr64(sock, REG_ACQ, ACQ_IOVA);
	reg_wr32(sock, REG_CC, cc);
	if (!(reg_rd32(sock, REG_CSTS) & CSTS_RDY)) {
		errx(EXIT_FAILURE, "enable: CSTS.RDY did not go high");
	}
}

static void
controller_disable(int sock)
{
	reg_wr32(sock, REG_CC, 0);
	if (reg_rd32(sock, REG_CSTS) & CSTS_RDY) {
		errx(EXIT_FAILURE, "disable: CSTS.RDY did not go low");
	}
}

/* Issue identify-controller; assert CQE is good. The cheapest "is the device
 * alive and processing commands?" round-trip we have. */
static void
sanity_identify(struct ctx *c, uint16_t cid)
{
	NvmeSqe *asq = (NvmeSqe *)(c->mem + ASQ_IOVA);
	NvmeCqe *acq = (NvmeCqe *)(c->mem + ACQ_IOVA);

	memset(asq, 0, sizeof(*asq));
	memset(acq, 0, sizeof(*acq));
	asq[0].opcode = 0x06;
	asq[0].cid = cid;
	asq[0].prp1 = DATA_IOVA;
	asq[0].cdw10 = 0x01; /* CNS=controller */
	reg_wr32(c->sock, DB_ASQ_TAIL, 1);
	if ((acq[0].status & 1) != 1 || (acq[0].status >> 1) != 0) {
		errx(EXIT_FAILURE, "sanity: bad CQE status %#x after identify",
		     acq[0].status);
	}
	if (acq[0].cid != cid) {
		errx(EXIT_FAILURE, "sanity: cid mismatch %u != %u", acq[0].cid, cid);
	}
	reg_wr32(c->sock, DB_ACQ_HEAD, 1);
}

/* --- modes --- */

static void
mode_flr(struct ctx *c, unsigned iters)
{
	for (unsigned i = 0; i < iters; i++) {
		if (device_reset(c->sock) != 0) {
			errx(EXIT_FAILURE, "flr[%u]: VFIO_USER_DEVICE_RESET failed", i);
		}
		c->ops++;
	}
}

static void
mode_cc(struct ctx *c, unsigned iters)
{
	for (unsigned i = 0; i < iters; i++) {
		controller_enable(c->sock);
		controller_disable(c->sock);
		c->ops += 2;
	}
}

static void
mode_shn(struct ctx *c, unsigned iters)
{
	for (unsigned i = 0; i < iters; i++) {
		uint32_t cc;

		controller_enable(c->sock);
		/* request normal shutdown: leave EN=1, set SHN=01 */
		cc = (4u << 20) | (6u << 16) | CC_EN | CC_SHN_NORMAL;
		reg_wr32(c->sock, REG_CC, cc);
		if ((reg_rd32(c->sock, REG_CSTS) & CSTS_SHST_MASK) != CSTS_SHST_COMPLETE) {
			errx(EXIT_FAILURE, "shn[%u]: CSTS.SHST did not reach complete", i);
		}
		controller_disable(c->sock);
		c->ops += 3;
	}
}

static void
mode_dma(struct ctx *c, unsigned iters)
{
	/* Map a tiny scratch region in addition to the main one. Cycle it. */
	int aux_fd;
	char tmpl[] = "/tmp/vfu-hammer-aux.XXXXXX";
	const uint64_t aux_size = PAGE * 4;
	const uint64_t aux_base = MEM_SIZE; /* placed above the main region */

	aux_fd = mkstemp(tmpl);
	if (aux_fd == -1) {
		err(EXIT_FAILURE, "mkstemp");
	}
	if (ftruncate(aux_fd, aux_size) == -1) {
		err(EXIT_FAILURE, "ftruncate aux");
	}
	unlink(tmpl);

	for (unsigned i = 0; i < iters; i++) {
		uint64_t off = (xrand(c) % 4u) * PAGE; /* not used by device but exercises bookkeeping */
		(void)off;
		if (dma_map(c->sock, aux_fd, aux_base, aux_size, 0) != 0) {
			errx(EXIT_FAILURE, "dma[%u]: map failed", i);
		}
		if (dma_unmap(c->sock, aux_base, aux_size) != 0) {
			errx(EXIT_FAILURE, "dma[%u]: unmap failed", i);
		}
		c->ops += 2;
	}
	close(aux_fd);
}

static void
mode_doorbell(struct ctx *c, unsigned iters)
{
	NvmeSqe *asq = (NvmeSqe *)(c->mem + ASQ_IOVA);
	NvmeCqe *acq = (NvmeCqe *)(c->mem + ACQ_IOVA);
	uint16_t phase = 1;
	uint16_t head = 0;
	uint16_t tail = 0;
	uint16_t cid = 100;

	controller_enable(c->sock);
	memset(acq, 0, Q_DEPTH * sizeof(*acq));

	for (unsigned i = 0; i < iters; i++) {
		/* Fill ASQ to Q_DEPTH-1 entries (leave one slot to keep tail!=head). */
		unsigned batch = (xrand(c) % (Q_DEPTH - 1)) + 1;
		for (unsigned k = 0; k < batch; k++) {
			memset(&asq[tail], 0, sizeof(asq[tail]));
			asq[tail].opcode = 0x06;
			asq[tail].cid = cid++;
			asq[tail].prp1 = DATA_IOVA;
			asq[tail].cdw10 = 0x01;
			tail = (tail + 1) % Q_DEPTH;
		}
		reg_wr32(c->sock, DB_ASQ_TAIL, tail);
		/* Drain CQEs in phase. */
		while (head != tail) {
			if ((acq[head].status & 1) != phase) {
				errx(EXIT_FAILURE,
				     "doorbell[%u]: cqe[%u].phase=%u expected %u",
				     i, head, acq[head].status & 1, phase);
			}
			if ((acq[head].status >> 1) != 0) {
				errx(EXIT_FAILURE, "doorbell[%u]: status=%#x", i,
				     acq[head].status);
			}
			head = (head + 1) % Q_DEPTH;
			if (head == 0) {
				phase ^= 1;
			}
		}
		reg_wr32(c->sock, DB_ACQ_HEAD, head);
		c->ops += batch;
	}
	controller_disable(c->sock);
}

static void
mode_reconnect(struct ctx *c, unsigned iters)
{
	for (unsigned i = 0; i < iters; i++) {
		close(c->sock);
		c->sock = init_sock(g_sock_path);
		negotiate(c->sock);
		if (dma_map(c->sock, c->mem_fd, 0, MEM_SIZE, 0) != 0) {
			errx(EXIT_FAILURE, "reconnect[%u]: dma_map failed", i);
		}
		c->ops += 1;
	}
}

/* Register one MSI-X eventfd (for admin CQ, IRQ=0), enable the controller,
 * fire an admin command, read() the eventfd to confirm delivery, then FLR and
 * reconfigure. Exercises vfu_irq_trigger across reset cycles -- the path that
 * Linux + QEMU actually drives but the protocol-only modes don't reach. */
static void
mode_msix(struct ctx *c, unsigned iters)
{
	int efd = eventfd(0, EFD_NONBLOCK);
	if (efd < 0) {
		err(EXIT_FAILURE, "eventfd");
	}
	int fds[1] = {efd};

	for (unsigned i = 0; i < iters; i++) {
		if (set_irqs_msix(c->sock, fds, 0, 1) != 0) {
			errx(EXIT_FAILURE, "msix[%u]: SET_IRQS failed", i);
		}
		/* Drain any pending counter from a previous cycle. */
		uint64_t drain;
		while (read(efd, &drain, 8) == 8) {
		}
		controller_enable(c->sock);
		sanity_identify(c, (uint16_t)(0xe000 + (i & 0xfff)));
		/* Admin completion should have signalled the eventfd. */
		uint64_t val = 0;
		ssize_t r = read(efd, &val, 8);
		if (r != 8 || val == 0) {
			errx(EXIT_FAILURE, "msix[%u]: eventfd not signaled (r=%zd val=%llu)",
			     i, r, (unsigned long long)val);
		}
		controller_disable(c->sock);
		if (device_reset(c->sock) != 0) {
			errx(EXIT_FAILURE, "msix[%u]: device_reset failed", i);
		}
		c->ops += 3;
	}
	close(efd);
}

/* Probe PCI config space the way Linux does on bind: read whole header, BAR
 * sizing via the writable-mask trick (write all-ones, read back, restore),
 * toggle PCI_COMMAND bus-master/memory bits. None of this should wedge or
 * corrupt the device. */
static void
mode_config(struct ctx *c, unsigned iters)
{
	uint8_t cfg[256];
	uint16_t cmd_save;

	if (access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, false, 0, cfg, sizeof(cfg)) != 0) {
		errx(EXIT_FAILURE, "config: read cfg space failed");
	}
	cmd_save = (uint16_t)(cfg[PCI_COMMAND] | (cfg[PCI_COMMAND + 1] << 8));

	for (unsigned i = 0; i < iters; i++) {
		/* Read full config header. */
		(void)access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, false, 0, cfg,
				    sizeof(cfg));
		/* BAR sizing trick on BAR0: write all-ones, read back, restore. */
		uint32_t bar_save = cfg[PCI_BASE_ADDRESS_0] |
				    (cfg[PCI_BASE_ADDRESS_0 + 1] << 8) |
				    (cfg[PCI_BASE_ADDRESS_0 + 2] << 16) |
				    (cfg[PCI_BASE_ADDRESS_0 + 3] << 24);
		uint32_t ones = 0xffffffffu;
		(void)access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, true, PCI_BASE_ADDRESS_0,
				    &ones, 4);
		uint32_t mask = 0;
		(void)access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, false, PCI_BASE_ADDRESS_0,
				    &mask, 4);
		(void)access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, true, PCI_BASE_ADDRESS_0,
				    &bar_save, 4);
		/* Toggle bus master + memory enable bits in PCI_COMMAND. */
		uint16_t cmd = (uint16_t)(cmd_save | PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY);
		(void)access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, true, PCI_COMMAND, &cmd,
				    2);
		cmd = (uint16_t)(cmd_save & ~(PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY));
		(void)access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, true, PCI_COMMAND, &cmd,
				    2);
		(void)access_region(c->sock, VFU_PCI_DEV_CFG_REGION_IDX, true, PCI_COMMAND,
				    &cmd_save, 2);
		c->ops += 6;
	}
	/* Sanity. */
	controller_enable(c->sock);
	sanity_identify(c, 0xc0fe);
	controller_disable(c->sock);
}

/* Write garbage to read-only registers, past the end of the BAR0 register
 * area, and at misaligned offsets. Device must survive and still serve a
 * sanity command afterwards. */
static void
mode_garbage(struct ctx *c, unsigned iters)
{
	for (unsigned i = 0; i < iters; i++) {
		uint64_t junk64 = 0xdeadbeefdeadbeefULL;
		uint32_t junk32 = 0xdeadbeefU;
		uint8_t junk8 = 0x5a;

		/* RO register (CAP). */
		(void)access_region(c->sock, VFU_PCI_DEV_BAR0_REGION_IDX, true, REG_CAP, &junk64,
				    8);
		/* VS (RO). */
		(void)access_region(c->sock, VFU_PCI_DEV_BAR0_REGION_IDX, true, REG_VS, &junk32,
				    4);
		/* Past the end of doorbell region. */
		(void)access_region(c->sock, VFU_PCI_DEV_BAR0_REGION_IDX, true, 0x1f00, &junk32,
				    4);
		/* Misaligned: 1-byte write to CC. */
		(void)access_region(c->sock, VFU_PCI_DEV_BAR0_REGION_IDX, true, REG_CC + 1, &junk8,
				    1);
		/* Read out-of-range. */
		(void)access_region(c->sock, VFU_PCI_DEV_BAR0_REGION_IDX, false, 0x1f00, &junk32,
				    4);
		c->ops += 5;
	}
	/* Sanity after all that nonsense. */
	controller_enable(c->sock);
	sanity_identify(c, 0xdeae);
	controller_disable(c->sock);
}

/* Submit one admin command, drain its completion from the proper CQ slot
 * given the current ring head + phase. Used by stress modes that pump many
 * commands per enable. Returns the CQE status field. */
static uint16_t
submit_drain_admin(struct ctx *c, NvmeSqe *asq, NvmeCqe *acq, uint16_t *head_io,
		   uint16_t *phase_io, const NvmeSqe *cmd_template)
{
	uint16_t head = *head_io;
	uint16_t phase = *phase_io;
	uint16_t tail = (uint16_t)((head + 1u) % Q_DEPTH);

	asq[head] = *cmd_template;
	reg_wr32(c->sock, DB_ASQ_TAIL, tail);
	uint16_t status = acq[head].status;
	if ((status & 1) != phase) {
		errx(EXIT_FAILURE, "submit_drain: cqe[%u].phase=%u expected %u status=%#x",
		     head, status & 1, phase, status);
	}
	*head_io = tail;
	if (tail == 0) {
		*phase_io ^= 1;
	}
	reg_wr32(c->sock, DB_ACQ_HEAD, tail);
	return status;
}

/* Submit admin commands that point PRP1 at unmapped IOVAs. Device must either
 * post a non-zero status CQE or fail gracefully -- never crash. */
static void
mode_bad_dma(struct ctx *c, unsigned iters)
{
	NvmeSqe *asq = (NvmeSqe *)(c->mem + ASQ_IOVA);
	NvmeCqe *acq = (NvmeCqe *)(c->mem + ACQ_IOVA);
	uint16_t head = 0, phase = 1;
	NvmeSqe cmd = {0};

	controller_enable(c->sock);
	memset(acq, 0, Q_DEPTH * sizeof(*acq));
	for (unsigned i = 0; i < iters; i++) {
		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = 0x06;
		cmd.cid = (uint16_t)(0xb000 + (i & 0xfff));
		cmd.prp1 = 0xdeadbeef0000ULL + ((uint64_t)i * 0x1000);
		cmd.cdw10 = 0x01;
		(void)submit_drain_admin(c, asq, acq, &head, &phase, &cmd);
		c->ops++;
	}
	controller_disable(c->sock);
}

/* Ring doorbells at addresses for queues that do not exist, and ask the device
 * to create IO queues with junk PRP1 / cqid. Device must reject without
 * crashing. */
static void
mode_bad_queue(struct ctx *c, unsigned iters)
{
	NvmeSqe *asq = (NvmeSqe *)(c->mem + ASQ_IOVA);
	NvmeCqe *acq = (NvmeCqe *)(c->mem + ACQ_IOVA);
	uint16_t head = 0, phase = 1;
	NvmeSqe cmd = {0};

	controller_enable(c->sock);
	memset(acq, 0, Q_DEPTH * sizeof(*acq));
	for (unsigned i = 0; i < iters; i++) {
		uint32_t v = 1;
		/* Doorbell for a queue ID well past NVME_MAX_QUEUES. */
		(void)access_region(c->sock, VFU_PCI_DEV_BAR0_REGION_IDX, true, 0x1500, &v, 4);

		/* Create IO CQ with PRP1 pointing nowhere. */
		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = 0x05;
		cmd.cid = (uint16_t)(0xb100 + (i & 0xfff));
		cmd.prp1 = 0xfeedfeed000ULL;
		cmd.cdw10 = (15u << 16) | 1u;
		cmd.cdw11 = 1u;
		(void)submit_drain_admin(c, asq, acq, &head, &phase, &cmd);

		/* Create IO SQ referencing a CQ that does not exist. */
		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = 0x01;
		cmd.cid = (uint16_t)(0xb200 + (i & 0xfff));
		cmd.prp1 = ASQ_IOVA;
		cmd.cdw10 = (15u << 16) | 1u;
		cmd.cdw11 = (63u << 16) | 1u; /* cqid 63 - never created */
		(void)submit_drain_admin(c, asq, acq, &head, &phase, &cmd);
		c->ops += 3;
	}
	controller_disable(c->sock);
}

/* Map several small ancillary DMA regions and unmap them in jumbled order
 * while admin commands are in flight on the long-lived main region. Probes
 * libvfio-user's region bookkeeping and the device's address-translation
 * stability under churn. */
static void
mode_dma_overlap(struct ctx *c, unsigned iters)
{
	enum { N = 8 };
	int aux_fds[N];
	uint64_t aux_base[N];

	for (unsigned k = 0; k < N; k++) {
		char path[] = "/tmp/vfu-hammer-overlap-XXXXXX";
		aux_fds[k] = mkstemp(path);
		if (aux_fds[k] < 0) {
			err(EXIT_FAILURE, "mkstemp");
		}
		if (ftruncate(aux_fds[k], PAGE * 4) < 0) {
			err(EXIT_FAILURE, "ftruncate");
		}
		unlink(path);
		aux_base[k] = MEM_SIZE + (uint64_t)(k + 1) * (PAGE * 8);
	}

	controller_enable(c->sock);
	NvmeSqe *asq = (NvmeSqe *)(c->mem + ASQ_IOVA);
	NvmeCqe *acq = (NvmeCqe *)(c->mem + ACQ_IOVA);
	memset(acq, 0, Q_DEPTH * sizeof(*acq));
	uint16_t head = 0, phase = 1;
	NvmeSqe cmd;

	for (unsigned i = 0; i < iters; i++) {
		/* Map all aux regions. */
		for (unsigned k = 0; k < N; k++) {
			if (dma_map(c->sock, aux_fds[k], aux_base[k], PAGE * 4, 0) != 0) {
				errx(EXIT_FAILURE, "dma_overlap[%u]: map %u failed", i, k);
			}
		}
		/* Identify-controller round-trip using the main DATA_IOVA region. */
		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = 0x06;
		cmd.cid = (uint16_t)(0xb300 + (i & 0xfff));
		cmd.prp1 = DATA_IOVA;
		cmd.cdw10 = 0x01;
		uint16_t st = submit_drain_admin(c, asq, acq, &head, &phase, &cmd);
		if ((st >> 1) != 0) {
			errx(EXIT_FAILURE, "dma_overlap[%u]: sanity status=%#x", i, st >> 1);
		}
		/* Unmap in reverse order with random gaps. */
		for (unsigned k = 0; k < N; k++) {
			unsigned idx = (xrand(c) + k) % N;
			(void)dma_unmap(c->sock, aux_base[idx], PAGE * 4);
		}
		/* Re-unmap the remaining (already-unmapped) regions -- many should
		 * fail with -EINVAL but the device must not crash. */
		for (unsigned k = 0; k < N; k++) {
			(void)dma_unmap(c->sock, aux_base[k], PAGE * 4);
		}
		c->ops += N * 2 + 1;
	}
	controller_disable(c->sock);
	for (unsigned k = 0; k < N; k++) {
		close(aux_fds[k]);
	}
}

static void
mode_mayhem(struct ctx *c, unsigned iters)
{
	for (unsigned i = 0; i < iters; i++) {
		unsigned pick = xrand(c) % 10;
		switch (pick) {
		case 0:
			mode_flr(c, 4);
			break;
		case 1:
			mode_cc(c, 1);
			break;
		case 2:
			mode_shn(c, 1);
			break;
		case 3:
			mode_dma(c, 4);
			break;
		case 4:
			mode_doorbell(c, 1);
			break;
		case 5:
			mode_msix(c, 1);
			break;
		case 6:
			mode_config(c, 2);
			break;
		case 7:
			mode_garbage(c, 2);
			break;
		case 8:
			mode_bad_dma(c, 2);
			break;
		case 9:
			mode_bad_queue(c, 1);
			break;
		}
		/* Every few iterations, sanity-check that the device still responds. */
		if ((i & 0x7) == 0x7) {
			controller_enable(c->sock);
			sanity_identify(c, (uint16_t)(0xa000 + (i & 0xfff)));
			controller_disable(c->sock);
		}
	}
}

/* --- harness --- */

struct mode_def {
	const char *name;
	void (*fn)(struct ctx *, unsigned);
	const char *desc;
};

static const struct mode_def MODES[] = {
	{"flr", mode_flr, "VFIO_USER_DEVICE_RESET in a loop"},
	{"cc", mode_cc, "CC.EN enable/disable cycles"},
	{"shn", mode_shn, "CC.SHN normal-shutdown cycles"},
	{"dma", mode_dma, "DMA_MAP/UNMAP cycles on an aux region"},
	{"doorbell", mode_doorbell, "fill ASQ and ring doorbell repeatedly"},
	{"reconnect", mode_reconnect, "close + reopen socket cycles"},
	{"msix", mode_msix, "SET_IRQS + admin cmd + read() eventfd across FLR"},
	{"config", mode_config, "PCI config-space probe + BAR sizing + command toggles"},
	{"garbage", mode_garbage, "RO-reg / out-of-range / misaligned BAR0 writes"},
	{"bad_dma", mode_bad_dma, "admin commands with unmapped PRP1"},
	{"bad_queue", mode_bad_queue, "doorbell out-of-range, create-queue with junk"},
	{"dma_overlap", mode_dma_overlap, "many aux DMA regions mapped+unmapped during commands"},
	{"mayhem", mode_mayhem, "interleave all modes with periodic sanity"},
};

static const struct mode_def *
find_mode(const char *name)
{
	for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
		if (strcmp(MODES[i].name, name) == 0) {
			return &MODES[i];
		}
	}
	return NULL;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s <socket> [--mode MODE] [--iters N] [--seed S] [--quiet]\n"
		"\nmodes:\n",
		prog);
	for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
		fprintf(stderr, "  %-10s %s\n", MODES[i].name, MODES[i].desc);
	}
	fprintf(stderr, "  %-10s run every mode sequentially\n", "all");
}

int
main(int argc, char **argv)
{
	const char *mode_name = "all";
	unsigned iters = 200;
	unsigned seed = 1;
	bool quiet = false;
	struct ctx c = {0};
	int opt;

	static const struct option longopts[] = {
		{"mode", required_argument, 0, 'm'},
		{"iters", required_argument, 0, 'i'},
		{"seed", required_argument, 0, 's'},
		{"quiet", no_argument, 0, 'q'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0},
	};

	while ((opt = getopt_long(argc, argv, "m:i:s:qh", longopts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			mode_name = optarg;
			break;
		case 'i':
			iters = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 's':
			seed = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'q':
			quiet = true;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 2;
	}
	g_sock_path = argv[optind];
	c.seed = seed;
	c.verbose = !quiet;

	c.sock = init_sock(g_sock_path);
	negotiate(c.sock);

	char tmpl[] = "/tmp/vfu-hammer-main.XXXXXX";
	c.mem_fd = mkstemp(tmpl);
	if (c.mem_fd == -1) {
		err(EXIT_FAILURE, "mkstemp");
	}
	if (ftruncate(c.mem_fd, MEM_SIZE) == -1) {
		err(EXIT_FAILURE, "ftruncate");
	}
	unlink(tmpl);
	c.mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, c.mem_fd, 0);
	if (c.mem == MAP_FAILED) {
		err(EXIT_FAILURE, "mmap");
	}
	if (dma_map(c.sock, c.mem_fd, 0, MEM_SIZE, 0) != 0) {
		err(EXIT_FAILURE, "initial DMA_MAP");
	}

	if (c.verbose) {
		printf("hammer: socket=%s mode=%s iters=%u seed=%u\n",
		       g_sock_path, mode_name, iters, seed);
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	if (strcmp(mode_name, "all") == 0) {
		for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
			if (c.verbose) {
				printf("[%s] %u iters\n", MODES[i].name, iters);
			}
			MODES[i].fn(&c, iters);
			/* Sanity round-trip between modes. */
			controller_enable(c.sock);
			sanity_identify(&c, (uint16_t)(0xb000 + i));
			controller_disable(c.sock);
		}
	} else {
		const struct mode_def *m = find_mode(mode_name);
		if (!m) {
			fprintf(stderr, "unknown mode '%s'\n", mode_name);
			usage(argv[0]);
			return 2;
		}
		if (c.verbose) {
			printf("[%s] %u iters\n", m->name, iters);
		}
		m->fn(&c, iters);
		/* Final sanity. */
		controller_enable(c.sock);
		sanity_identify(&c, 0xc001);
		controller_disable(c.sock);
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("HAMMER PASSED ops=%u in %.2fs (%.0f ops/s)\n", c.ops, dt,
	       dt > 0 ? c.ops / dt : 0.0);

	munmap(c.mem, MEM_SIZE);
	close(c.mem_fd);
	close(c.sock);
	return 0;
}
