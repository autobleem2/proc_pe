#!/bin/sh
# Builds tests/data/dpkg_1.0.mod with the real dpkg-deb (a Debian/Ubuntu machine; the laptop has it): the
# format a real package has, as opposed to make_fixtures.py's Python-made ones. Run again only to change it.
#   sh tests/make_dpkg_fixture.sh
set -e
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
pkg="$work/pkg"
dir="$pkg/media/project_eris/etc/project_eris/SUP/launchers/dpkgapp"
mkdir -p "$dir" "$pkg/DEBIAN"
printf 'launcher_filename="dpkgapp"\nlauncher_title="Dpkg App"\nlauncher_publisher="Real Maker"\n' >"$dir/launcher.cfg"
printf '#!/bin/sh\ncd /var/volatile/launchtmp\n./dpkgapp\n' >"$dir/launch.sh"
printf 'program bytes\n' >"$dir/dpkgapp"
chmod 755 "$dir/launch.sh" "$dir/dpkgapp"
cat >"$pkg/DEBIAN/control" <<EOF
Package: dpkgapp
Version: 1.0
Architecture: armhf
Depends:
Maintainer: Real Maker <maker@example.com>
Description: A package made by dpkg-deb
 Type: USB_MOD
 Built by the real tool.
 Author: Real Maker
 Platform: SONYPSC armhf
EOF
dpkg-deb -Zxz --root-owner-group -v --build "$pkg" "$work/out.deb" >/dev/null
cp "$work/out.deb" "$here/data/dpkg_1.0.mod"
ar t "$here/data/dpkg_1.0.mod"
