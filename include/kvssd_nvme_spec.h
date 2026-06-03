/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NVMe spec subset for the vfio-user KV SSD prototype.
 *
 * The KV-specific structures, opcodes and status codes are ported from the
 * SamsungDS/qemu 'for-xnvme' branch (hw/nvme KV support), de-coupled from the
 * QEMU types so they can be reused in a standalone libvfio-user device.
 */
#ifndef KVSSD_NVME_SPEC_H
#define KVSSD_NVME_SPEC_H

#include <stdint.h>

#define KVSSD_PAGE_SIZE 4096u

/* Generic 64-byte submission queue entry (PRP form). */
typedef struct __attribute__((packed)) {
	uint8_t opcode;
	uint8_t flags;
	uint16_t cid;
	uint32_t nsid;
	uint32_t cdw2;
	uint32_t cdw3;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
} NvmeSqe;

/* 16-byte completion queue entry. */
typedef struct __attribute__((packed)) {
	uint32_t cdw0;
	uint32_t cdw1;
	uint16_t sq_head;
	uint16_t sq_id;
	uint16_t cid;
	uint16_t status; /* phase bit in bit 0, status code from bit 1 */
} NvmeCqe;

/*
 * KV command (64 bytes). Layout matches NvmeKVCmd from the QEMU KV patch:
 * key at offset 8, dptr (prp1/prp2) at 24, hbs at 40, cdw11{kl,ro} at 44,
 * key_hi at 56.
 */
typedef struct __attribute__((packed)) {
	uint8_t opcode;
	uint8_t flags;
	uint16_t cid;
	uint32_t nsid;
	uint64_t key;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t hbs; /* host buffer size / value size */
	struct __attribute__((packed)) {
		uint8_t kl; /* key length */
		uint8_t ro; /* retrieve option / store option */
		uint16_t rsvd;
	} cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint64_t key_hi;
} NvmeKVCmd;

/* KV I/O command opcodes. */
enum {
	NVME_CMD_KV_STORE = 0x01,
	NVME_CMD_KV_RETRIEVE = 0x02,
	NVME_CMD_KV_LIST = 0x06,
	NVME_CMD_KV_DELETE = 0x10,
	NVME_CMD_KV_EXIST = 0x14,
};

/* KV store options (cdw11.ro on a store command). */
enum {
	NVME_CMD_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS = 1 << 0,
	NVME_CMD_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS = 1 << 1,
	NVME_CMD_KV_STORE_OPT_COMPRESS = 1 << 2,
};

/*
 * NVMe completion status: (SCT << 8) | SC, in the layout consumed by
 * nvme_post_cqe() which shifts the whole value left by one to land SC in CQE
 * bits 1-8 and SCT in bits 9-11. Generic Command Status (SCT=0) entries leave
 * the upper byte zero; KV Command Specific Status (SCT=1) entries set bit 8
 * so the host driver sees status_code_type=CMDSPEC alongside the KV SC code.
 * Without the SCT bit the host parses these as SCT=0 (Generic) with reserved
 * SC values, which xNVMe's Python KV tests assert against and reject.
 */
enum {
	NVME_SUCCESS = 0x0000,
	NVME_INVALID_OPCODE = 0x0001,
	NVME_INVALID_FIELD = 0x0002,
	NVME_CAP_EXCEEDED = 0x0181,
	NVME_KV_INVALID_VAL_SIZE = 0x0185,
	NVME_KV_INVALID_KEY_SIZE = 0x0186,
	NVME_KV_KEY_NOT_EXISTS = 0x0187,
	NVME_KV_KEY_EXISTS = 0x0189,
};

#define NVME_DNR 0x4000

/* Command set identifiers. */
enum {
	NVME_CSI_NVM = 0x00,
	NVME_CSI_KV = 0x01,
	NVME_CSI_ZONED = 0x02,
};

/* KV namespace limits. Per-instance MAX_NUM_KEYS is configurable at runtime
 * (see --capacity in main); the spec-level hard ceiling is UINT32_MAX. */
#define NVME_KV_HARD_MAX_NUM_KEYS UINT32_MAX
#define NVME_KV_MAX_KEY_SIZE 16
#define NVME_KV_MAX_VAL_SIZE (4 * 1024)

/* KV format descriptor, 16 bytes. */
typedef struct __attribute__((packed)) {
	uint16_t kml; /* key max length */
	uint8_t rsvd2;
	uint8_t fopt;
	uint32_t vml; /* value max length */
	uint32_t mnk; /* max number of keys */
	uint8_t rsvd12[4];
} NvmeKVF;

/* KV-specific Identify Namespace data structure, 4096 bytes. */
typedef struct __attribute__((packed)) {
	uint64_t nsze;
	uint8_t rsvd8[8];
	uint64_t nuse;
	uint8_t nsfeat;
	uint8_t nkvf;
	uint8_t nmic;
	uint8_t rescap;
	uint8_t fpi;
	uint8_t rsvd29[3];
	uint32_t novg;
	uint32_t anagrpid;
	uint8_t rsvd40[3];
	uint8_t nsattr;
	uint16_t nvmsetid;
	uint16_t endgid;
	uint64_t nguid[2];
	uint64_t eui64;
	NvmeKVF kvf[16];
	uint8_t rsvd328[3512];
	uint8_t vs[256];
} NvmeIdNsKV;

_Static_assert(sizeof(NvmeSqe) == 64, "NvmeSqe must be 64 bytes");
_Static_assert(sizeof(NvmeCqe) == 16, "NvmeCqe must be 16 bytes");
_Static_assert(sizeof(NvmeKVCmd) == 64, "NvmeKVCmd must be 64 bytes");
_Static_assert(sizeof(NvmeKVF) == 16, "NvmeKVF must be 16 bytes");
_Static_assert(sizeof(NvmeIdNsKV) == 4096, "NvmeIdNsKV must be 4096 bytes");

#endif /* KVSSD_NVME_SPEC_H */
