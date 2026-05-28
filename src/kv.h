/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * In-memory KV namespace and the KV command handlers.
 *
 * The handlers are ported from the SamsungDS/qemu KV patch but de-coupled from
 * QEMU: data movement to/from the host is delegated through 'struct kv_dma' so
 * the logic can be exercised without a real vfio-user client.
 */
#ifndef KVSSD_KV_H
#define KVSSD_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvssd_nvme_spec.h"

struct kv_pair {
	uint8_t key[16];
	size_t kl;
	uint8_t val[NVME_KV_MAX_VAL_SIZE];
	bool used;
};

struct kv_ns {
	struct kv_pair *pairs;
	size_t entries_allocated;
	NvmeIdNsKV *id_ns_kv;
	uint32_t ednek; /* Error on Delete of Non-Existent Key */
};

/*
 * Host data movement, implemented by the controller front-end over the PRP
 * pointers in the command. 'len' bytes are moved; return 0 on success or a
 * negative errno on DMA failure.
 */
struct kv_dma {
	int (*h2c)(void *ctx, const NvmeKVCmd *cmd, void *dst, size_t len);
	int (*c2h)(void *ctx, const NvmeKVCmd *cmd, const void *src, size_t len);
	void *ctx;
};

int
kv_ns_init(struct kv_ns *ns);
void
kv_ns_teardown(struct kv_ns *ns);

/* Each handler returns an NVMe status code (without DNR/phase). */
uint16_t
kv_store(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma);
uint16_t
kv_retrieve(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma);
uint16_t
kv_delete(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma);
uint16_t
kv_exist(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma);
uint16_t
kv_list(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma);

#endif /* KVSSD_KV_H */
