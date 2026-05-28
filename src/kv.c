/* SPDX-License-Identifier: BSD-3-Clause */
#include <stdlib.h>
#include <string.h>

#include "kv.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define KV_INITIAL_ENTRIES 16

int
kv_ns_init(struct kv_ns *ns)
{
	NvmeIdNsKV *id = calloc(1, sizeof(*id));
	if (!id) {
		return -1;
	}

	ns->pairs = calloc(KV_INITIAL_ENTRIES, sizeof(*ns->pairs));
	if (!ns->pairs) {
		free(id);
		return -1;
	}

	ns->entries_allocated = KV_INITIAL_ENTRIES;
	ns->ednek = 0;
	ns->id_ns_kv = id;

	id->nsze = NVME_KV_MAX_NUM_KEYS * (uint64_t)(NVME_KV_MAX_KEY_SIZE + NVME_KV_MAX_VAL_SIZE);
	id->nuse = 0;
	id->nkvf = 1;
	id->kvf[0].mnk = NVME_KV_MAX_NUM_KEYS;
	id->kvf[0].kml = NVME_KV_MAX_KEY_SIZE;
	id->kvf[0].vml = NVME_KV_MAX_VAL_SIZE;

	return 0;
}

void
kv_ns_teardown(struct kv_ns *ns)
{
	free(ns->pairs);
	free(ns->id_ns_kv);
	ns->pairs = NULL;
	ns->id_ns_kv = NULL;
	ns->entries_allocated = 0;
}

static bool
kv_key_match(const NvmeKVCmd *cmd, const struct kv_pair *p)
{
	if (!p->used) {
		return false;
	}
	if (cmd->cdw11.kl != p->kl) {
		return false;
	}
	if (memcmp(&cmd->key, p->key, MIN((size_t)cmd->cdw11.kl, 8u)) != 0) {
		return false;
	}
	if (cmd->cdw11.kl > 8) {
		if (memcmp(&cmd->key_hi, p->key + 8, MIN((size_t)cmd->cdw11.kl - 8, 8u)) != 0) {
			return false;
		}
	}
	return true;
}

static void
kv_key_store(struct kv_pair *p, const NvmeKVCmd *cmd)
{
	memcpy(p->key, &cmd->key, MIN((size_t)cmd->cdw11.kl, 8u));
	if (cmd->cdw11.kl > 8) {
		memcpy(p->key + 8, &cmd->key_hi, MIN((size_t)cmd->cdw11.kl - 8, 8u));
	}
	p->kl = MIN((size_t)cmd->cdw11.kl, 16u);
	p->used = true;
}

uint16_t
kv_store(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma)
{
	int fst_empty = -1;

	if (cmd->cdw11.kl > 16) {
		return NVME_KV_INVALID_KEY_SIZE;
	}
	if (cmd->hbs > NVME_KV_MAX_VAL_SIZE) {
		return NVME_KV_INVALID_VAL_SIZE;
	}

	for (size_t i = 0; i < ns->entries_allocated; i++) {
		if (fst_empty < 0 && !ns->pairs[i].used) {
			fst_empty = (int)i;
		}
		if (kv_key_match(cmd, &ns->pairs[i])) {
			if (cmd->cdw11.ro & NVME_CMD_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS) {
				return NVME_KV_KEY_EXISTS;
			}
			if (dma->h2c(dma->ctx, cmd, ns->pairs[i].val, cmd->hbs)) {
				return NVME_INVALID_FIELD;
			}
			return NVME_SUCCESS;
		}
	}

	if (cmd->cdw11.ro & NVME_CMD_KV_STORE_OPT_DONT_STORE_IF_KEY_NOT_EXISTS) {
		return NVME_KV_KEY_NOT_EXISTS;
	}

	if (fst_empty < 0) {
		size_t new_entries = ns->entries_allocated * 2;
		struct kv_pair *grown = realloc(ns->pairs, new_entries * sizeof(*grown));
		if (!grown) {
			return NVME_CAP_EXCEEDED;
		}
		memset(&grown[ns->entries_allocated], 0,
		       (new_entries - ns->entries_allocated) * sizeof(*grown));
		fst_empty = (int)ns->entries_allocated;
		ns->pairs = grown;
		ns->entries_allocated = new_entries;
	}

	kv_key_store(&ns->pairs[fst_empty], cmd);
	ns->id_ns_kv->nuse += NVME_KV_MAX_KEY_SIZE + NVME_KV_MAX_VAL_SIZE;

	if (dma->h2c(dma->ctx, cmd, ns->pairs[fst_empty].val, cmd->hbs)) {
		return NVME_INVALID_FIELD;
	}
	return NVME_SUCCESS;
}

uint16_t
kv_retrieve(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma)
{
	if (cmd->cdw11.kl > 16) {
		return NVME_INVALID_FIELD;
	}
	for (size_t i = 0; i < ns->entries_allocated; i++) {
		if (kv_key_match(cmd, &ns->pairs[i])) {
			size_t len = MIN((size_t)cmd->hbs, (size_t)NVME_KV_MAX_VAL_SIZE);
			if (dma->c2h(dma->ctx, cmd, ns->pairs[i].val, len)) {
				return NVME_INVALID_FIELD;
			}
			return NVME_SUCCESS;
		}
	}
	return NVME_KV_KEY_NOT_EXISTS;
}

uint16_t
kv_delete(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma)
{
	(void)dma;
	if (cmd->cdw11.kl > 16) {
		return NVME_INVALID_FIELD;
	}
	for (size_t i = 0; i < ns->entries_allocated; i++) {
		if (kv_key_match(cmd, &ns->pairs[i])) {
			memset(&ns->pairs[i], 0, sizeof(ns->pairs[i]));
			ns->id_ns_kv->nuse -= NVME_KV_MAX_KEY_SIZE + NVME_KV_MAX_VAL_SIZE;
			return NVME_SUCCESS;
		}
	}
	if (ns->ednek & 0x01) {
		return NVME_KV_KEY_NOT_EXISTS;
	}
	return NVME_SUCCESS;
}

uint16_t
kv_exist(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma)
{
	(void)dma;
	if (cmd->cdw11.kl > 16) {
		return NVME_INVALID_FIELD;
	}
	for (size_t i = 0; i < ns->entries_allocated; i++) {
		if (kv_key_match(cmd, &ns->pairs[i])) {
			return NVME_SUCCESS;
		}
	}
	return NVME_KV_KEY_NOT_EXISTS;
}

uint16_t
kv_list(struct kv_ns *ns, const NvmeKVCmd *cmd, struct kv_dma *dma)
{
	const size_t kls_size = sizeof(uint16_t);
	const size_t rds_hdr = sizeof(uint32_t);
	size_t rds_size = rds_hdr;
	uint8_t *rds = calloc(1, rds_hdr);
	uint16_t *nrk;
	size_t first_entry = 0;
	uint16_t status;

	if (cmd->cdw11.kl > 16) {
		free(rds);
		return NVME_INVALID_FIELD;
	}
	if (!rds) {
		return NVME_CAP_EXCEEDED;
	}
	nrk = (uint16_t *)rds;

	for (size_t i = 0; i < ns->entries_allocated; i++) {
		if (kv_key_match(cmd, &ns->pairs[i])) {
			first_entry = i;
			break;
		}
	}

	for (size_t i = first_entry; i < ns->entries_allocated; i++) {
		if (!ns->pairs[i].used) {
			continue;
		}

		size_t kds_wo_pad = kls_size + ns->pairs[i].kl;
		size_t kds = kds_wo_pad + (kds_wo_pad % 4);
		uint8_t *grown = realloc(rds, rds_size + kds);
		if (!grown) {
			free(rds);
			return NVME_CAP_EXCEEDED;
		}
		rds = grown;
		nrk = (uint16_t *)rds;

		if (rds_size + kds > cmd->hbs) {
			break; /* host buffer full */
		}
		if (kds_wo_pad < kds) {
			memset(rds + rds_size + kds_wo_pad, 0, kds - kds_wo_pad);
		}

		uint8_t *kds_ptr = rds + rds_size;
		rds_size += kds;
		uint16_t kl16 = (uint16_t)ns->pairs[i].kl;
		memcpy(kds_ptr, &kl16, kls_size);
		memcpy(kds_ptr + kls_size, ns->pairs[i].key, ns->pairs[i].kl);
		*nrk = *nrk + 1;
	}

	status = dma->c2h(dma->ctx, cmd, rds, rds_size) ? NVME_INVALID_FIELD : NVME_SUCCESS;
	free(rds);
	return status;
}
