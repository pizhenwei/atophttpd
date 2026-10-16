#!/bin/sh
#
# Build a Debian binary package (.deb) for atophttpd.
#
# Usage: sh packaging/debian/makedeb.sh <project-root>
#
set -eu
umask 022

ROOT_PATH="${1:-$(pwd)}"
DEB_DIR="$ROOT_PATH/packaging/debian"
PACKAGE_PATH="$DEB_DIR/deb"
CONTROL_PATH="$PACKAGE_PATH/DEBIAN"
DOC_PATH="$PACKAGE_PATH/usr/share/doc/atophttpd"

rm -rf "$PACKAGE_PATH"
mkdir -p "$CONTROL_PATH" "$DOC_PATH"

# 1, install the payload (binary, systemd unit, man page, config) into the
#    package tree.
make -C "$ROOT_PATH" install prefix="$PACKAGE_PATH"

# 2, compress the man page (lintian: manpage-not-compressed).
if [ -f "$PACKAGE_PATH/usr/share/man/man1/atophttpd.1" ]; then
	gzip -9n "$PACKAGE_PATH/usr/share/man/man1/atophttpd.1"
fi

# 2b, drop the compiler .comment section (lintian: binary-has-unneeded-section).
if [ -f "$PACKAGE_PATH/usr/bin/atophttpd" ]; then
	strip --remove-section=.comment "$PACKAGE_PATH/usr/bin/atophttpd" 2>/dev/null || true
fi

# 3, documentation: copyright + changelog.
install -m 0644 "$DEB_DIR/copyright" "$DOC_PATH/copyright"
ATOP_VERSION=$(grep ATOPVERS "$ROOT_PATH/atop/version.h" | tr '"' ' ' | awk '{print $3}')
cat > "$DOC_PATH/changelog" <<EOF
atophttpd ($ATOP_VERSION) unstable; urgency=medium

  * Package built against atop $ATOP_VERSION.

 -- zhenwei pi <pizhenwei@bytedance.com>  $(date -R)
EOF
gzip -9n "$DOC_PATH/changelog"

# 4, control files (control, conffiles, maintainer scripts).
install -m 0644 "$DEB_DIR/control"   "$CONTROL_PATH/control"
install -m 0644 "$DEB_DIR/conffiles" "$CONTROL_PATH/conffiles"
install -m 0755 "$DEB_DIR/postinst"  "$CONTROL_PATH/postinst"
install -m 0755 "$DEB_DIR/prerm"     "$CONTROL_PATH/prerm"
install -m 0755 "$DEB_DIR/postrm"    "$CONTROL_PATH/postrm"

# 5, expand placeholders in control.
#    atophttpd reads atop's version-specific raw-log format and getnumvers()
#    only compares major.minor, so depend on the matching atop minor series.
ATOP_MAJ=${ATOP_VERSION%%.*}
ATOP_REST=${ATOP_VERSION#*.}
ATOP_MIN=${ATOP_REST%%.*}
ATOP_MAJMIN="$ATOP_MAJ.$ATOP_MIN"
ATOP_NEXTMIN="$ATOP_MAJ.$((ATOP_MIN + 1))"
ATOPHTTPD_ARCH=$(dpkg --print-architecture)
ATOPHTTPD_SIZE=$(du -k -d 0 "$PACKAGE_PATH" | awk '{print $1}')

sed -i \
	-e "s/ATOP_MAJMIN/$ATOP_MAJMIN/g" \
	-e "s/ATOP_NEXTMIN/$ATOP_NEXTMIN/g" \
	-e "s/ATOP_VERSION/$ATOP_VERSION/g" \
	-e "s/ATOPHTTPD_ARCH/$ATOPHTTPD_ARCH/g" \
	-e "s/ATOPHTTPD_SIZE/$ATOPHTTPD_SIZE/g" \
	"$CONTROL_PATH/control"

# 6, build the package with root:root ownership (no fakeroot needed).
#    First record md5sums of the payload (everything but the control dir).
( cd "$PACKAGE_PATH" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' \
	| xargs -0 md5sum > "$CONTROL_PATH/md5sums" )
chmod 0644 "$CONTROL_PATH/md5sums"

dpkg-deb --root-owner-group -b "$PACKAGE_PATH" \
	"$ROOT_PATH/atophttpd_${ATOP_VERSION}_${ATOPHTTPD_ARCH}.deb"

# 7, cleanup.
rm -rf "$PACKAGE_PATH"
