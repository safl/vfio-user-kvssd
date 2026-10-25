#!/bin/sh
# Install a vfio-user-kvssd deb into a fresh system and check that the
# binary it carries runs and is the version the package claims.
#
#   test-deb.sh <deb>
set -eu
deb=$(realpath "$1")
export DEBIAN_FRONTEND=noninteractive
apt-get -qy update >/dev/null
apt-get -qy install --no-install-recommends "$deb" >/dev/null
version=$(dpkg-query -W -f '${Version}' vfio-user-kvssd)
got=$(vfu_kvssd --version)
[ "$got" = "vfu_kvssd $version" ] || {
	echo "test-deb.sh: '$got', not vfu_kvssd $version" >&2
	exit 1
}
echo "test-deb.sh: ok ($got)"
