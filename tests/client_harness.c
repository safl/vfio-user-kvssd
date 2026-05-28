/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NVMe-aware vfio-user client harness.
 *
 * Drives the kvssd device over a vfio-user socket without QEMU: negotiates the
 * protocol, maps a shared "guest memory" region, enables the controller,
 * issues Identify, creates an IO queue pair, and round-trips a KV store/
 * retrieve. The device processes doorbell writes synchronously, so completions
 * are in shared memory by the time the REGION_WRITE reply returns.
 *
 * Usage: kvssd-harness <socket-path>
 */
#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <linux/pci_regs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "libvfio-user.h"
#include "vfio-user.h"
#include "tran_sock.h"

#include "kvssd_nvme_spec.h"

#define PAGE 4096u
#define NPAGES 16
#define MEM_SIZE (NPAGES * PAGE)

#define ASQ_IOVA (0u * PAGE)
#define ACQ_IOVA (1u * PAGE)
#define IOSQ_IOVA (2u * PAGE)
#define IOCQ_IOVA (3u * PAGE)
#define DATA_IOVA (4u * PAGE)
#define VAL_IOVA (5u * PAGE)

#define Q_DEPTH 8

/* Doorbell offsets (DSTRD=0, stride 4). */
#define DB_ASQ_TAIL 0x1000u
#define DB_ACQ_HEAD 0x1004u
#define DB_IOSQ_TAIL 0x1008u
#define DB_IOCQ_HEAD 0x100cu

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
	struct vfio_user_version cversion = {
		.major = LIB_VFIO_USER_MAJOR,
		.minor = LIB_VFIO_USER_MINOR,
	};
	char caps[256];
	int slen = snprintf(caps, sizeof(caps),
			    "{\"capabilities\":{\"max_msg_fds\":%u,"
			    "\"max_data_xfer_size\":%u}}",
			    8u, 1u << 20);
	struct iovec iov[3] = {0};
	struct vfio_user_header hdr;
	struct vfio_user_version *sv = NULL;
	size_t vlen;

	iov[1].iov_base = &cversion;
	iov[1].iov_len = sizeof(cversion);
	iov[2].iov_base = caps;
	iov[2].iov_len = slen + 1;

	if (tran_sock_send_iovec(sock, 1, false, VFIO_USER_VERSION, iov, 3, NULL, 0, 0) <
	    0) {
		err(EXIT_FAILURE, "send version");
	}
	if (tran_sock_recv_alloc(sock, &hdr, true, NULL, (void **)&sv, &vlen) < 0) {
		err(EXIT_FAILURE, "recv version");
	}
	free(sv);
}

static void
map_dma(int sock, int fd)
{
	struct vfio_user_dma_map m = {
		.argsz = sizeof(m),
		.addr = 0,
		.size = MEM_SIZE,
		.offset = 0,
		.flags = VFIO_USER_F_DMA_REGION_READ | VFIO_USER_F_DMA_REGION_WRITE,
	};
	struct iovec iov[2] = {0};

	iov[1].iov_base = &m;
	iov[1].iov_len = sizeof(m);
	if (tran_sock_msg_iovec(sock, 2, VFIO_USER_DMA_MAP, iov, 2, &fd, 1, NULL, NULL, 0,
				NULL, 0) < 0) {
		err(EXIT_FAILURE, "DMA_MAP");
	}
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
	ret = tran_sock_msg_iovec(sock, msg_id--, op, iov, nr_iov, NULL, 0, NULL, resp,
				  resp_len, NULL, 0);
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

static void check_cqe(const NvmeCqe *cqe, uint16_t expect_cid, const char *what);

/* Walk the PCI capability list and verify the MSI-X capability is present. */
static void
check_msix_cap(int sock)
{
	uint8_t cfg[256];
	uint8_t pos;

	if (access_region(sock, VFU_PCI_DEV_CFG_REGION_IDX, false, 0, cfg, sizeof(cfg)) !=
	    0) {
		errx(EXIT_FAILURE, "read PCI config space");
	}
	pos = cfg[0x34]; /* capabilities pointer */
	while (pos != 0 && pos < sizeof(cfg) - 1) {
		uint8_t id = cfg[pos];
		if (id == PCI_CAP_ID_MSIX) {
			uint16_t mc = (uint16_t)(cfg[pos + 2] | (cfg[pos + 3] << 8));
			printf("  ok: MSI-X capability (table size %u)\n",
			       (mc & 0x7ff) + 1);
			return;
		}
		pos = cfg[pos + 1];
	}
	errx(EXIT_FAILURE, "MSI-X capability not found in config space");
}

/* Submit a single Identify on the admin queue and validate completion. */
static void
admin_identify(int sock, NvmeSqe *asq, NvmeCqe *acq, uint16_t slot, uint16_t cid,
	       uint32_t cns, const char *what)
{
	memset(&asq[slot], 0, sizeof(asq[slot]));
	asq[slot].opcode = 0x06;
	asq[slot].cid = cid;
	asq[slot].nsid = 1;
	asq[slot].prp1 = DATA_IOVA;
	asq[slot].cdw10 = cns;
	reg_wr32(sock, DB_ASQ_TAIL, slot + 1);
	check_cqe(&acq[slot], cid, what);
	reg_wr32(sock, DB_ACQ_HEAD, slot + 1);
}

static void
check_cqe(const NvmeCqe *cqe, uint16_t expect_cid, const char *what)
{
	uint16_t sc = (uint16_t)(cqe->status >> 1);
	if ((cqe->status & 1) != 1) {
		errx(EXIT_FAILURE, "%s: phase bit not set (status=%#x)", what, cqe->status);
	}
	if (sc != 0) {
		errx(EXIT_FAILURE, "%s: non-zero status %#x", what, sc);
	}
	if (cqe->cid != expect_cid) {
		errx(EXIT_FAILURE, "%s: cid mismatch %u != %u", what, cqe->cid, expect_cid);
	}
	printf("  ok: %s (cid=%u)\n", what, cqe->cid);
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <socket-path>\n", argv[0]);
		return 2;
	}

	int sock = init_sock(argv[1]);
	negotiate(sock);

	char tmpl[] = "/tmp/kvssd-harness.XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd == -1) {
		err(EXIT_FAILURE, "mkstemp");
	}
	if (ftruncate(fd, MEM_SIZE) == -1) {
		err(EXIT_FAILURE, "ftruncate");
	}
	unlink(tmpl);
	uint8_t *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		err(EXIT_FAILURE, "mmap");
	}
	map_dma(sock, fd);
	printf("connected, DMA mapped (%u bytes)\n", MEM_SIZE);

	/* Enable the controller. */
	reg_wr32(sock, 0x24, (uint32_t)((Q_DEPTH - 1) | ((Q_DEPTH - 1) << 16))); /* AQA */
	reg_wr64(sock, 0x28, ASQ_IOVA);                                         /* ASQ */
	reg_wr64(sock, 0x30, ACQ_IOVA);                                         /* ACQ */
	reg_wr32(sock, 0x14, (4u << 20) | (6u << 16) | 1u);                     /* CC.EN */
	if (!(reg_rd32(sock, 0x1c) & 1u)) {
		errx(EXIT_FAILURE, "controller did not become ready (CSTS.RDY)");
	}
	printf("controller enabled (CSTS.RDY)\n");

	check_msix_cap(sock);

	NvmeSqe *asq = (NvmeSqe *)(mem + ASQ_IOVA);
	NvmeCqe *acq = (NvmeCqe *)(mem + ACQ_IOVA);

	/* Identify Controller (CNS 0x01). */
	memset(&asq[0], 0, sizeof(asq[0]));
	asq[0].opcode = 0x06;
	asq[0].cid = 1;
	asq[0].prp1 = DATA_IOVA;
	asq[0].cdw10 = 0x01;
	reg_wr32(sock, DB_ASQ_TAIL, 1);
	check_cqe(&acq[0], 1, "identify-controller");
	reg_wr32(sock, DB_ACQ_HEAD, 1);
	if (memcmp(mem + DATA_IOVA + 0x18, "vfio-user-kvssd", 15) != 0) {
		errx(EXIT_FAILURE, "identify MN mismatch");
	}
	printf("  identify MN = \"vfio-user-kvssd\"\n");

	/* Create IO CQ (qid 1). */
	memset(&asq[1], 0, sizeof(asq[1]));
	asq[1].opcode = 0x05;
	asq[1].cid = 2;
	asq[1].prp1 = IOCQ_IOVA;
	asq[1].cdw10 = ((uint32_t)(Q_DEPTH - 1) << 16) | 1u;
	asq[1].cdw11 = 1u; /* PC */
	reg_wr32(sock, DB_ASQ_TAIL, 2);
	check_cqe(&acq[1], 2, "create-io-cq");
	reg_wr32(sock, DB_ACQ_HEAD, 2);

	/* Create IO SQ (qid 1 -> cqid 1). */
	memset(&asq[2], 0, sizeof(asq[2]));
	asq[2].opcode = 0x01;
	asq[2].cid = 3;
	asq[2].prp1 = IOSQ_IOVA;
	asq[2].cdw10 = ((uint32_t)(Q_DEPTH - 1) << 16) | 1u;
	asq[2].cdw11 = (1u << 16) | 1u; /* cqid 1, PC */
	reg_wr32(sock, DB_ASQ_TAIL, 3);
	check_cqe(&acq[2], 3, "create-io-sq");
	reg_wr32(sock, DB_ACQ_HEAD, 3);

	/* Command-set-independent Identify Namespace (CNS 0x08): nstat ready. */
	memset(mem + DATA_IOVA, 0, PAGE);
	admin_identify(sock, asq, acq, 3, 4, 0x08, "identify-cs-ind-ns");
	if (mem[DATA_IOVA + 14] != 0x01) {
		errx(EXIT_FAILURE, "cs-ind-ns: nstat not ready (%#x)", mem[DATA_IOVA + 14]);
	}
	printf("  cs-ind-ns nstat = ready\n");

	/* I/O Command Set data structure (CNS 0x1c): advertises NVM (bit 0). */
	memset(mem + DATA_IOVA, 0, PAGE);
	admin_identify(sock, asq, acq, 4, 5, 0x1c, "identify-io-cmd-set");
	if (!(mem[DATA_IOVA] & 0x01)) {
		errx(EXIT_FAILURE, "io-cmd-set: NVM bit not set (%#x)", mem[DATA_IOVA]);
	}
	printf("  io-cmd-set advertises NVM\n");

	NvmeKVCmd *iosq = (NvmeKVCmd *)(mem + IOSQ_IOVA);
	NvmeCqe *iocq = (NvmeCqe *)(mem + IOCQ_IOVA);
	const char *key = "abc";
	const char *val = "hello-kv-value";
	uint32_t vlen = (uint32_t)strlen(val) + 1;

	/* KV store. */
	memset(mem + VAL_IOVA, 0, PAGE);
	memcpy(mem + VAL_IOVA, val, vlen);
	memset(&iosq[0], 0, sizeof(iosq[0]));
	iosq[0].opcode = NVME_CMD_KV_STORE;
	iosq[0].cid = 10;
	iosq[0].nsid = 1;
	iosq[0].prp1 = VAL_IOVA;
	iosq[0].hbs = vlen;
	iosq[0].cdw11.kl = (uint8_t)strlen(key);
	memcpy(&iosq[0].key, key, strlen(key));
	reg_wr32(sock, DB_IOSQ_TAIL, 1);
	check_cqe(&iocq[0], 10, "kv-store");
	reg_wr32(sock, DB_IOCQ_HEAD, 1);

	/* KV retrieve into a cleared buffer and compare. */
	memset(mem + VAL_IOVA, 0, PAGE);
	memset(&iosq[1], 0, sizeof(iosq[1]));
	iosq[1].opcode = NVME_CMD_KV_RETRIEVE;
	iosq[1].cid = 11;
	iosq[1].nsid = 1;
	iosq[1].prp1 = VAL_IOVA;
	iosq[1].hbs = PAGE;
	iosq[1].cdw11.kl = (uint8_t)strlen(key);
	memcpy(&iosq[1].key, key, strlen(key));
	reg_wr32(sock, DB_IOSQ_TAIL, 2);
	check_cqe(&iocq[1], 11, "kv-retrieve");
	reg_wr32(sock, DB_IOCQ_HEAD, 2);

	if (memcmp(mem + VAL_IOVA, val, vlen) != 0) {
		errx(EXIT_FAILURE, "kv-retrieve: value mismatch (got \"%s\")",
		     (char *)(mem + VAL_IOVA));
	}
	printf("  retrieved value = \"%s\"\n", (char *)(mem + VAL_IOVA));

	printf("HARNESS PASSED\n");
	munmap(mem, MEM_SIZE);
	close(fd);
	close(sock);
	return 0;
}
