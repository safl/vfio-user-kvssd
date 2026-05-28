/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Unit test for the ported KV handlers. Exercises the logic directly via a
 * mock kv_dma (a fake host buffer), no libvfio-user / QEMU required.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "kv.h"

struct host {
	uint8_t buf[NVME_KV_MAX_VAL_SIZE];
	size_t len;
};

static int
mock_h2c(void *ctx, const NvmeKVCmd *cmd, void *dst, size_t len)
{
	struct host *h = ctx;
	(void)cmd;
	memcpy(dst, h->buf, len);
	return 0;
}

static int
mock_c2h(void *ctx, const NvmeKVCmd *cmd, const void *src, size_t len)
{
	struct host *h = ctx;
	(void)cmd;
	memcpy(h->buf, src, len);
	h->len = len;
	return 0;
}

static NvmeKVCmd
mk_cmd(uint8_t opcode, const char *key, uint8_t kl, uint32_t hbs, uint8_t ro)
{
	NvmeKVCmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = opcode;
	cmd.hbs = hbs;
	cmd.cdw11.kl = kl;
	cmd.cdw11.ro = ro;
	memcpy(&cmd.key, key, kl > 8 ? 8 : kl);
	if (kl > 8) {
		memcpy(&cmd.key_hi, key + 8, kl - 8);
	}
	return cmd;
}

int
main(void)
{
	struct kv_ns ns;
	struct host host;
	struct kv_dma dma = {.h2c = mock_h2c, .c2h = mock_c2h, .ctx = &host};
	NvmeKVCmd cmd;
	uint16_t st;

	assert(kv_ns_init(&ns) == 0);

	const char *key = "hello";
	const char *val = "world-value-payload";
	uint8_t kl = 5;
	uint32_t vlen = (uint32_t)strlen(val) + 1;

	/* store */
	memset(&host, 0, sizeof(host));
	memcpy(host.buf, val, vlen);
	cmd = mk_cmd(NVME_CMD_KV_STORE, key, kl, vlen, 0);
	st = kv_store(&ns, &cmd, &dma);
	assert(st == NVME_SUCCESS);

	/* exist: present and absent */
	cmd = mk_cmd(NVME_CMD_KV_EXIST, key, kl, 0, 0);
	assert(kv_exist(&ns, &cmd, &dma) == NVME_SUCCESS);
	cmd = mk_cmd(NVME_CMD_KV_EXIST, "nope", 4, 0, 0);
	assert(kv_exist(&ns, &cmd, &dma) == NVME_KV_KEY_NOT_EXISTS);

	/* retrieve and compare */
	memset(&host, 0, sizeof(host));
	cmd = mk_cmd(NVME_CMD_KV_RETRIEVE, key, kl, vlen, 0);
	st = kv_retrieve(&ns, &cmd, &dma);
	assert(st == NVME_SUCCESS);
	assert(memcmp(host.buf, val, vlen) == 0);

	/* store-if-not-exists on an existing key must fail */
	memset(&host, 0, sizeof(host));
	memcpy(host.buf, val, vlen);
	cmd = mk_cmd(NVME_CMD_KV_STORE, key, kl, vlen,
		     NVME_CMD_KV_STORE_OPT_DONT_STORE_IF_KEY_EXISTS);
	assert(kv_store(&ns, &cmd, &dma) == NVME_KV_KEY_EXISTS);

	/* list returns at least one key (nrk in first u16) */
	memset(&host, 0, sizeof(host));
	cmd = mk_cmd(NVME_CMD_KV_LIST, "", 0, sizeof(host.buf), 0);
	st = kv_list(&ns, &cmd, &dma);
	assert(st == NVME_SUCCESS);
	uint16_t nrk;
	memcpy(&nrk, host.buf, sizeof(nrk));
	assert(nrk >= 1);

	/* delete then retrieve must miss */
	cmd = mk_cmd(NVME_CMD_KV_DELETE, key, kl, 0, 0);
	assert(kv_delete(&ns, &cmd, &dma) == NVME_SUCCESS);
	cmd = mk_cmd(NVME_CMD_KV_RETRIEVE, key, kl, vlen, 0);
	assert(kv_retrieve(&ns, &cmd, &dma) == NVME_KV_KEY_NOT_EXISTS);

	/* 16-byte key round-trip (exercises key_hi path) */
	memset(&host, 0, sizeof(host));
	memcpy(host.buf, "X", 1);
	cmd = mk_cmd(NVME_CMD_KV_STORE, "0123456789abcdef", 16, 1, 0);
	assert(kv_store(&ns, &cmd, &dma) == NVME_SUCCESS);
	cmd = mk_cmd(NVME_CMD_KV_EXIST, "0123456789abcdef", 16, 0, 0);
	assert(kv_exist(&ns, &cmd, &dma) == NVME_SUCCESS);
	cmd = mk_cmd(NVME_CMD_KV_EXIST, "0123456789abcdeX", 16, 0, 0);
	assert(kv_exist(&ns, &cmd, &dma) == NVME_KV_KEY_NOT_EXISTS);

	kv_ns_teardown(&ns);
	printf("kv_test: all assertions passed\n");
	return 0;
}
