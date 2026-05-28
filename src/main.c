/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <linux/pci_regs.h>

#include <libvfio-user.h>
#include <pci_caps/msix.h>

#include "nvme.h"

static volatile sig_atomic_t g_stop;

static void
on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

#ifndef VFU_KVSSD_VERSION
#define VFU_KVSSD_VERSION "unknown"
#endif

static void
usage(FILE *f, const char *argv0)
{
	fprintf(f, "usage: %s -s <socket-path>\n", argv0);
	fprintf(f, "\n");
	fprintf(f, "Emulate an NVMe Key-Value SSD over vfio-user.\n");
	fprintf(f, "\n");
	fprintf(f, "Options:\n");
	fprintf(f, "  -s, --socket <path>  vfio-user UNIX socket to listen on (required)\n");
	fprintf(f, "  -h, --help           show this help and exit\n");
	fprintf(f, "  -V, --version        print version and exit\n");
}

/*
 * Minimal DMA (un)register callbacks. libvfio-user only mmaps fd-backed DMA
 * regions locally (so vfu_sgl_read/write can memcpy) when an unregister
 * callback is provided; the bodies themselves can be empty.
 */
static void
vfu_log_cb(vfu_ctx_t *vfu, int level, const char *msg)
{
	(void)vfu;
	fprintf(stderr, "kvssd[vfu:%d] %s\n", level, msg);
}

static void
dma_register(vfu_ctx_t *vfu, vfu_dma_info_t *info)
{
	(void)vfu;
	(void)info;
}

static void
dma_unregister(vfu_ctx_t *vfu, vfu_dma_info_t *info)
{
	(void)vfu;
	(void)info;
}

int
main(int argc, char **argv)
{
	const char *sock = NULL;
	struct nvme_ctrl ctrl;
	vfu_ctx_t *vfu;
	uint8_t *cfg;
	int opt, ret;

	static const struct option long_opts[] = {
		{ "socket",  required_argument, NULL, 's' },
		{ "help",    no_argument,       NULL, 'h' },
		{ "version", no_argument,       NULL, 'V' },
		{ 0, 0, 0, 0 },
	};

	while ((opt = getopt_long(argc, argv, "s:hV", long_opts, NULL)) != -1) {
		switch (opt) {
		case 's':
			sock = optarg;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		case 'V':
			printf("vfu_kvssd %s\n", VFU_KVSSD_VERSION);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}
	if (!sock) {
		usage(stderr, argv[0]);
		return 2;
	}

	vfu = vfu_create_ctx(VFU_TRANS_SOCK, sock, 0, &ctrl, VFU_DEV_TYPE_PCI);
	if (!vfu) {
		perror("vfu_create_ctx");
		return 1;
	}

	vfu_setup_log(vfu, vfu_log_cb, LOG_DEBUG);

	if (vfu_pci_init(vfu, VFU_PCI_TYPE_EXPRESS, 0, 0) < 0) {
		perror("vfu_pci_init");
		goto err;
	}
	vfu_pci_set_id(vfu, 0x1d1d, 0x1f1f, 0, 0);

	/* PCI class code: mass storage / NVM / NVMe programming interface. */
	cfg = (uint8_t *)vfu_pci_get_config_space(vfu);
	cfg[0x08] = 0x00; /* revision id */
	cfg[0x09] = 0x02; /* prog-if: NVMe */
	cfg[0x0a] = 0x08; /* subclass: NVM */
	cfg[0x0b] = 0x01; /* class: mass storage */

	if (vfu_setup_region(vfu, VFU_PCI_DEV_BAR0_REGION_IDX, NVME_BAR0_SIZE,
			     nvme_bar0_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
			     NULL, 0, -1, 0) < 0) {
		perror("vfu_setup_region BAR0");
		goto err;
	}

	/* BAR4 holds the MSI-X table + PBA. */
	if (vfu_setup_region(vfu, VFU_PCI_DEV_BAR4_REGION_IDX, NVME_MSIX_BAR_SIZE,
			     nvme_msix_access, VFU_REGION_FLAG_RW | VFU_REGION_FLAG_MEM,
			     NULL, 0, -1, 0) < 0) {
		perror("vfu_setup_region BAR4");
		goto err;
	}

	struct msixcap msix = {0};
	msix.hdr.id = PCI_CAP_ID_MSIX;
	msix.hdr.next = 0;
	msix.mxc.ts = NVME_NUM_IRQS - 1; /* table size, 0-based */
	/* The 'to'/'pbao' fields are bits [31:3] of the Table/PBA Offset+BIR
	 * register, i.e. the byte offset >> 3 (low 3 bits hold the BIR). */
	msix.mtab.tbir = NVME_MSIX_BAR;
	msix.mtab.to = NVME_MSIX_TABLE_OFFSET >> 3;
	msix.mpba.pbir = NVME_MSIX_BAR;
	msix.mpba.pbao = NVME_MSIX_PBA_OFFSET >> 3;
	if (vfu_pci_add_capability(vfu, 0, 0, &msix) < 0) {
		perror("vfu_pci_add_capability MSI-X");
		goto err;
	}

	if (vfu_setup_device_dma(vfu, LIBVFIO_USER_MAX_DMA_REGIONS, dma_register,
				 dma_unregister) < 0) {
		perror("vfu_setup_device_dma");
		goto err;
	}

	if (vfu_setup_device_nr_irqs(vfu, VFU_DEV_MSIX_IRQ, NVME_NUM_IRQS) < 0) {
		perror("vfu_setup_device_nr_irqs");
		goto err;
	}

	if (nvme_ctrl_init(&ctrl, vfu) < 0) {
		fprintf(stderr, "nvme_ctrl_init failed\n");
		goto err;
	}

	if (vfu_realize_ctx(vfu) < 0) {
		perror("vfu_realize_ctx");
		goto err_ctrl;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	printf("kvssd: listening on %s\n", sock);
	fflush(stdout);

	while (!g_stop) {
		ret = vfu_attach_ctx(vfu);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			perror("vfu_attach_ctx");
			break;
		}
		printf("kvssd: client attached\n");
		fflush(stdout);

		do {
			ret = vfu_run_ctx(vfu);
		} while (ret >= 0 && !g_stop);

		if (ret < 0 && errno == ENOTCONN) {
			printf("kvssd: client detached\n");
			fflush(stdout);
			continue;
		}
		if (ret < 0 && errno != EINTR) {
			perror("vfu_run_ctx");
			break;
		}
	}

	nvme_ctrl_teardown(&ctrl);
	vfu_destroy_ctx(vfu);
	return 0;

err_ctrl:
	nvme_ctrl_teardown(&ctrl);
err:
	vfu_destroy_ctx(vfu);
	return 1;
}
