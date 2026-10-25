#!/usr/bin/env python3
"""
Package the static vfu_kvssd as a deb, one per architecture
===========================================================

For hosts that install their emulators with apt rather than by downloading a
binary. Each binary in 'packaging.binaries' becomes vfio-user-kvssd for its
architecture, installing /usr/bin/vfu_kvssd. The binary is static, so the
package depends on nothing.

Retargetable: true
------------------
"""
import errno
import logging as log
import shlex

ARCH = {"x86_64": "amd64", "aarch64": "arm64"}


def main(args, cijoe):
    """Build a vfio-user-kvssd deb per binary into 'packaging.out'"""

    version = cijoe.getconf("packaging.version")
    out = cijoe.getconf("packaging.out")
    license = cijoe.getconf("packaging.license")
    binaries = {k: v for k, v in cijoe.getconf("packaging.binaries", {}).items() if v}
    if not (version and out and binaries):
        log.error("set KVSSD_VERSION, KVSSD_OUT and at least one KVSSD_BIN_<arch>")
        return errno.EINVAL

    for machine, binary in binaries.items():
        arch = ARCH[machine]
        control = f"""Package: vfio-user-kvssd
Version: {version}
Architecture: {arch}
Maintainer: Simon A. F. Lund <os@safl.dk>
Section: misc
Priority: optional
Homepage: https://github.com/safl/vfio-user-kvssd
Description: NVMe Key-Value SSD emulated over vfio-user
 vfu_kvssd serves an NVMe controller with the Key-Value command set on a
 vfio-user socket, for QEMU's vfio-user-pci to attach to a guest.
"""
        err, _ = cijoe.run(
            "stage=$(mktemp -d) && "
            f"install -D -m 0755 {binary} $stage/usr/bin/vfu_kvssd && "
            f"install -D -m 0644 {license} $stage/usr/share/doc/vfio-user-kvssd/copyright && "
            f"mkdir -p $stage/DEBIAN {out} && "
            f"printf '%s' {shlex.quote(control)} > $stage/DEBIAN/control && "
            f"dpkg-deb --root-owner-group -Zxz -b $stage {out}/vfio-user-kvssd_{version}_{arch}.deb && "
            "rm -rf $stage"
        )
        if err:
            return err

    return 0
