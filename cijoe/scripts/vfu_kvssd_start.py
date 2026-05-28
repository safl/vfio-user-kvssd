"""
Start the vfu_kvssd device and the QEMU guest attached to it
============================================================

Host-side step. Mirrors the pattern in xNVMe's ``xnvme_guest_start_nvme.py``:
build the extra QEMU arguments for the device under test and hand them to
``cijoe.qemu.wrapper.Guest.start(extra_args=...)``. Here the "device" is an
external process (the vfu_kvssd vfio-user server) rather than a built-in QEMU
``-device nvme``:

1. Launch the static ``vfu_kvssd`` binary, listening on a UNIX socket.
2. Boot the guest with:
   - ``-object memory-backend-memfd,share=on`` + ``-machine memory-backend``
     so guest RAM is shareable -- the vfu_kvssd process mmaps it for DMA
     (the same requirement the local client harness meets with a shared memfd).
   - ``-device vfio-user-pci,socket=...`` -- the vfio-user client, available in
     upstream QEMU since 10.1.

Guest readiness is left to a following ``core.wait_for_transport`` step (as in
bty's ``usb_ventoy_guest_start``), so this script returns once the guest is
daemonised.

Config (``[kvssd]`` table):
    binary  Path to the vfu_kvssd device binary (host).
    socket  UNIX socket path the device listens on / QEMU connects to.
    memory  Guest RAM size for the memfd backend (must be shareable).

Retargetable: False (host-side).
"""

from __future__ import annotations

import logging as log
from argparse import ArgumentParser
from pathlib import Path

from cijoe.qemu.wrapper import Guest


def add_args(parser: ArgumentParser):
    parser.add_argument("--guest_name", type=str, help="Name of the qemu guest.")


def main(args, cijoe):
    guest_name = args.guest_name or cijoe.getconf("qemu.default_guest")
    if not guest_name:
        log.error("missing config value (qemu.default_guest)")
        return 1

    binary = cijoe.getconf("kvssd.binary", "zig-out/bin/vfu_kvssd")
    socket = cijoe.getconf("kvssd.socket", "/tmp/vfu_kvssd.sock")
    memory = cijoe.getconf("kvssd.memory", "4G")
    log_path = str(Path(socket).with_suffix(".log"))

    # Start the device server, detached, before QEMU connects to the socket.
    cijoe.run_local(f"rm -f {socket}")
    err, _ = cijoe.run_local(
        f"setsid {binary} -s {socket} > {log_path} 2>&1 < /dev/null & echo started"
    )
    if err:
        log.error(f"failed to start vfu_kvssd: err({err})")
        return err

    # Wait for the device to create its listening socket.
    err, _ = cijoe.run_local(
        f"for _ in $(seq 1 100); do [ -S {socket} ] && break; sleep 0.1; done; "
        f"[ -S {socket} ]"
    )
    if err:
        log.error(f"vfu_kvssd did not create socket {socket}; see {log_path}")
        return err

    # Upstream QEMU's vfio-user-pci takes 'socket' as a SocketAddress object,
    # not a path string, so it must be given in JSON form. cijoe runs the qemu
    # command via a shell, so wrap the (space-free) JSON in single quotes to
    # preserve the inner double quotes.
    vfio_dev = (
        '{"driver":"vfio-user-pci",'
        '"socket":{"type":"unix","path":"' + socket + '"}}'
    )
    extra_args = [
        # q35 for PCIe; memory-backend wires guest RAM to the shareable memfd.
        "-machine",
        "q35,memory-backend=pcram",
        "-object",
        f"memory-backend-memfd,id=pcram,size={memory},share=on",
        "-device",
        "'" + vfio_dev + "'",
    ]

    # Optional cloud-init NoCloud seed (label cidata) to enable SSH on first
    # boot, attached as a CD-ROM. Staged by the workflow next to boot.img.
    seed = cijoe.getconf("kvssd.seed", None)
    if seed:
        err, _ = cijoe.run_local(f"[ -f {seed} ]")
        if not err:
            extra_args += ["-drive", f"file={seed},media=cdrom"]

    guest = Guest(cijoe, cijoe.config, guest_name)
    err = guest.start(extra_args=extra_args)
    if err:
        log.error(f"guest.start() : err({err})")
        return err

    log.info(f"guest '{guest_name}' started with vfio-user device on {socket}")
    return 0
