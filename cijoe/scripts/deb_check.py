#!/usr/bin/env python3
"""
Install the vfio-user-kvssd deb and check the binary it carries
===============================================================

Installs the deb for this machine's architecture from 'packaging.out' and
checks that vfu_kvssd runs and reports the version the package claims. Run it
in a fresh system: it installs the package there.

Retargetable: true
------------------
"""
import errno
import logging as log


def main(args, cijoe):
    """Install and check the deb for this architecture"""

    out = cijoe.getconf("packaging.out")

    err, _ = cijoe.run(
        "export DEBIAN_FRONTEND=noninteractive && apt-get -qy update && "
        f"apt-get -qy install --no-install-recommends "
        f"{out}/vfio-user-kvssd_*_$(dpkg --print-architecture).deb"
    )
    if err:
        return err

    err, state = cijoe.run(
        "echo \"$(dpkg-query -W -f '${Version}' vfio-user-kvssd) $(vfu_kvssd --version)\""
    )
    if err:
        return err
    line = state.output().strip().splitlines()[-1].split()
    if len(line) != 3 or line[1] != "vfu_kvssd" or line[0] != line[2]:
        log.error(f"vfu_kvssd reports {' '.join(line[1:])!r}, not the package's {line[0]}")
        return errno.EINVAL
    return 0
