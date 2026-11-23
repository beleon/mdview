#!/usr/bin/env bash
# Repair the Debian changelog inside a built .deb.
#
# nfpm writes `mdview (1.1.1)` -- no distribution, no urgency -- and names
# it changelog.Debian.gz. Debian policy requires
#   package (version) distribution; urgency=level
# followed by a blank line, and a native package (a version with no Debian
# revision) wants changelog.gz. dpkg-parsechangelog rejects the file as nfpm
# leaves it. nfpm exposes no control over the heading, so the file is
# rewritten in place. (Same repair as pdfed's packaging.)
set -euo pipefail
DEB="${1:?usage: fix-deb-changelog.sh <package.deb>}"
DIST="${DEB_DISTRIBUTION:-unstable}"
URG="${DEB_URGENCY:-medium}"

work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
dpkg-deb -R "$DEB" "$work/x"
doc="$work/x/usr/share/doc/mdview"
src="$doc/changelog.Debian.gz"
[ -f "$src" ] || { echo "no changelog in $DEB -- did nfpm's changelog: key go?" >&2; exit 1; }

gunzip -c "$src" > "$work/ch"
awk -v dist="$DIST" -v urg="$URG" '
  /^[a-z0-9][a-z0-9+.-]* \([^)]*\)[[:space:]]*$/ {
      printf "%s %s; urgency=%s\n\n", $0, dist, urg; next }
  { print }
' "$work/ch" > "$work/ch.fixed"

gzip -9nc "$work/ch.fixed" > "$doc/changelog.gz"
rm -f "$src"
chmod 644 "$doc/changelog.gz"

# DEBIAN/md5sums must describe the files actually in the package.
if [ -f "$work/x/DEBIAN/md5sums" ]; then
    ( cd "$work/x" \
      && sed -i '\#usr/share/doc/mdview/changelog\.Debian\.gz#d' DEBIAN/md5sums \
      && md5sum usr/share/doc/mdview/changelog.gz >> DEBIAN/md5sums \
      && LC_ALL=C sort -k2 -o DEBIAN/md5sums DEBIAN/md5sums )
fi

# Keep nfpm's compression: `dpkg-deb -b` defaults to zstd on modern dpkg,
# which older dpkg (< 1.21.18) cannot read.
comp=$(ar t "$DEB" | sed -n 's/^data\.tar\.//p')
case "$comp" in
    gz|"") comp=gzip ;;
    xz)    comp=xz ;;
    zst)   comp=zstd ;;
    bz2)   comp=bzip2 ;;
esac
# --root-owner-group: `dpkg-deb -R` unpacked as the invoking user, and `-b`
# would otherwise record that user as owner, undoing nfpm's root:root.
dpkg-deb --root-owner-group -b -Z"$comp" "$work/x" "$DEB" > /dev/null

# Post-conditions on what this script just rewrote.
_lst=$(dpkg-deb -c "$DEB")
[ -n "$_lst" ] || { echo "error: could not list $DEB" >&2; exit 1; }
if printf '%s\n' "$_lst" | awk '{print $2}' | grep -qv '^\(root/root\|0/0\)$'; then
    echo "error: $(basename "$DEB") is not owned by root after the repair" >&2
    exit 1
fi
_files=$(printf '%s\n' "$_lst" | awk '$1 ~ /^-/ {print substr($NF,3)}' | LC_ALL=C sort)
dpkg-deb -e "$DEB" "$work/ctl"
_sums=$(awk '{print $2}' "$work/ctl/md5sums" | LC_ALL=C sort)
if [ "$_files" != "$_sums" ]; then
    echo "error: $(basename "$DEB") md5sums does not match its contents:" >&2
    diff <(printf '%s\n' "$_files") <(printf '%s\n' "$_sums") | head -5 >&2
    exit 1
fi
echo "  changelog repaired in $(basename "$DEB")"
