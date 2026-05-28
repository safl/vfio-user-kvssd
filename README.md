<p align="center">
  <img src="vuk.png" alt="vuk - vfio-user-kvssd" width="480">
</p>

# vfio-user-kvssd

[![ci](https://github.com/safl/vfio-user-kvssd/actions/workflows/ci.yml/badge.svg)](https://github.com/safl/vfio-user-kvssd/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)
[![built with Zig](https://img.shields.io/badge/built%20with-Zig%200.16.0-f7a41d.svg)](https://ziglang.org)
[![static musl](https://img.shields.io/badge/static%20musl-x86__64%20%7C%20aarch64-blue.svg)](https://github.com/safl/vfio-user-kvssd/releases)

`vfu_kvssd` is a userspace process that emulates an **NVMe Key-Value (KV) SSD**
and attaches to **stock upstream QEMU (>= 10.1)** over a vfio-user socket. It
lets xNVMe exercise the NVMe KV command set without the old `SamsungDS/qemu`
fork. KV is the only NVMe feature still missing from upstream QEMU.

It is C, built with **Zig** as the only toolchain, and ships as a single static
x86_64/arm64 binary with no runtime dependencies.

## Quick start

```sh
# Build the device (needs Zig 0.16.x, nothing else).
zig build

# Run it on a socket.
zig-out/bin/vfu_kvssd --socket /tmp/vfu_kvssd.sock

# Or drive it end-to-end without QEMU (build, start, store/retrieve over KV):
tests/run_harness.sh zig-out/bin
```

Static, dependency-free binaries (cross-compiled from any host):

```sh
zig build -Dtarget=x86_64-linux-musl  -Dstatic=true -Doptimize=ReleaseSafe
zig build -Dtarget=aarch64-linux-musl -Dstatic=true -Doptimize=ReleaseSafe
```

## Attach to a QEMU guest

Needs QEMU >= 10.1 (Fedora 44 / Ubuntu 26.04 ship 10.2; Debian 13 and Ubuntu
24.04 do not). The guest RAM must be a **shared** memory backend so the device
can mmap it for DMA.

```sh
# 1. Start the device.
vfu_kvssd --socket /tmp/vfu_kvssd.sock

# 2. Boot the guest with the device attached as a vfio-user client.
qemu-system-x86_64 \
  -machine q35,accel=kvm,memory-backend=mem \
  -object memory-backend-memfd,id=mem,size=4G,share=on \
  -cpu host -smp 4 \
  <your guest kernel/disk options> \
  -device vfio-user-pci,socket=/tmp/vfu_kvssd.sock

# 3. In the guest the KV namespace appears as a generic char device, e.g.
#    /dev/ng0n1, ready for xNVMe KV tools (kvs, xnvme_tests_kvs).
```

The automated guest bring-up + xNVMe KV tests run under cijoe in CI; see
`cijoe/` and `HANDOFF.md`.

## KV command set

I/O opcodes: STORE `0x01`, RETRIEVE `0x02`, LIST `0x06`, DELETE `0x10`,
EXIST `0x14`. Limits: key <= 16 bytes, value <= 4096 bytes. Backing store is
in-memory.

## More detail

- `HANDOFF.md`: status, architecture, the controller front-end design, and the
  hard-won gotchas (DMA, MSI-X, command-set negotiation).
