#!/bin/sh
# Package a static vfu_kvssd as a deb, for hosts that install their emulators
# with apt rather than by downloading a binary.
#
#   deb.sh <static vfu_kvssd> <version> <x86_64|aarch64> <output dir>
#
# The binary is static, so the package depends on nothing.
set -eu
bin=$1
version=$2
case $3 in
x86_64) arch=amd64 ;;
aarch64) arch=arm64 ;;
*) echo "deb.sh: unknown arch $3" >&2; exit 2 ;;
esac
out=$4

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
install -D -m 0755 "$bin" "$stage/usr/bin/vfu_kvssd"
install -D -m 0644 "$(dirname "$0")/../LICENSE" "$stage/usr/share/doc/vfio-user-kvssd/copyright"
mkdir -p "$stage/DEBIAN"
cat > "$stage/DEBIAN/control" <<CONTROL
Package: vfio-user-kvssd
Version: $version
Architecture: $arch
Maintainer: Simon A. F. Lund <os@safl.dk>
Section: misc
Priority: optional
Homepage: https://github.com/safl/vfio-user-kvssd
Description: NVMe Key-Value SSD emulated over vfio-user
 vfu_kvssd serves an NVMe controller with the Key-Value command set on a
 vfio-user socket, for QEMU's vfio-user-pci to attach to a guest.
CONTROL
mkdir -p "$out"
dpkg-deb --root-owner-group -b "$stage" "$out/vfio-user-kvssd_${version}_${arch}.deb" >/dev/null
echo "$out/vfio-user-kvssd_${version}_${arch}.deb"
