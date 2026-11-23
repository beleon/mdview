#!/usr/bin/env bash
# Clean-room install tests for the built packages, using podman.
#
# Each package is installed by the distro's own package manager into a stock
# container, without recommended packages, so the declared dependencies alone
# have to be enough. Then:
#   - every shared library resolves
#   - the files land where a desktop looks for them, owned by root
#   - without a compositor, mdview fails with an error rather than a crash
#   - under a headless Weston compositor it opens a document with text, a
#     table and a PNG, and the screenshot is checked for the PNG's colour,
#     which is only there if gdk-pixbuf really loaded the image
#   - removing the package leaves no files behind
# Weston and a font are installed alongside, standing in for the desktop the
# package would be installed on; neither is a dependency of mdview.
# Screenshots are kept in <dist>/smoke/<image>.png.
#
# Requires: podman, python3, network, packages in dist/.
#   packaging/test-packages.sh [dist-dir]
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
DIST=$(cd "${1:-"$REPO/dist"}" && pwd)
VERSION=$(sed -n 's/^project(mdview VERSION \([0-9][0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")
DEB="mdview_${VERSION}_amd64.deb"
RPM="mdview-${VERSION}-1.x86_64.rpm"
for p in "$DEB" "$RPM"; do
    [ -f "$DIST/$p" ] || { echo "error: $DIST/$p not found; run build-packages.sh" >&2; exit 1; }
done
mkdir -p "$DIST/smoke"

INNER=$(mktemp -t mdview-pkgtest-XXXXXX.sh)
trap 'rm -f "$INNER"' EXIT
cat > "$INNER" <<'IN'
set -euo pipefail
fail() { echo "FAIL: $*"; exit 1; }
quiet() { "$@" > /tmp/pm.log 2>&1 || { cat /tmp/pm.log; fail "$*"; }; }
case "$PM" in
    apt) export DEBIAN_FRONTEND=noninteractive
         quiet apt-get update
         quiet apt-get install -y --no-install-recommends "/dist/$PKG"
         quiet apt-get install -y --no-install-recommends weston fonts-dejavu-core ;;
    dnf) quiet dnf install -y --setopt=install_weak_deps=False "/dist/$PKG"
         quiet dnf install -y --setopt=install_weak_deps=False weston dejavu-sans-fonts \
             shadow-utils util-linux ;;
esac

! ldd /usr/bin/mdview | grep 'not found' || fail "unresolved libraries"
for f in /usr/bin/mdview /usr/share/applications/mdview.desktop \
         /usr/share/icons/hicolor/256x256/apps/mdview.png; do
    [ -e "$f" ] || fail "missing $f"
    [ "$(stat -c %U:%G "$f")" = root:root ] || fail "$f is not owned by root"
done
# Docs are checked in the package manifest: container images skip installing
# them (Ubuntu's dpkg path-exclude, Fedora's nodocs), real systems do not.
case "$PM" in apt) manifest=$(dpkg -L mdview) ;; dnf) manifest=$(rpm -ql mdview) ;; esac
for f in /usr/share/doc/mdview/copyright /usr/share/doc/mdview/licenses/gumbo-LICENSE \
         /usr/share/doc/mdview/licenses/plutovg-FTL.TXT; do
    printf '%s\n' "$manifest" | grep -qx "$f" || fail "$f not in the package"
done
mdview --help > /dev/null 2>&1 || fail "mdview --help exited $?"

mkdir -p /tmp/doc && cp /smoke/smoke.md /tmp/doc/ && cp /icons/mdview-128.png /tmp/doc/icon.png
chmod -R a+rX /tmp/doc
ec=0; env -u WAYLAND_DISPLAY mdview -f /tmp/doc/smoke.md > /dev/null 2>&1 || ec=$?
[ "$ec" -ne 0 ] && [ "$ec" -lt 128 ] || fail "without a compositor mdview exited $ec"

useradd -m t
mkdir -p /tmp/xdg && chown t /tmp/xdg && chmod 700 /tmp/xdg
runuser -u t -- env XDG_RUNTIME_DIR=/tmp/xdg bash -euc '
    cd /tmp
    weston --backend=headless --renderer=pixman --shell=kiosk --debug --no-config \
        --socket=wl-smoke --width=1000 --height=800 > /tmp/weston.log 2>&1 &
    for i in $(seq 100); do [ -S /tmp/xdg/wl-smoke ] && break; sleep 0.1; done
    [ -S /tmp/xdg/wl-smoke ] || { echo "FAIL: weston did not start"; cat /tmp/weston.log; exit 1; }
    export WAYLAND_DISPLAY=wl-smoke
    mdview -f /tmp/doc/smoke.md > /tmp/mdview.log 2>&1 & p=$!
    sleep 3
    kill -0 $p 2>/dev/null || { echo "FAIL: mdview exited early"; cat /tmp/mdview.log; exit 1; }
    weston-screenshooter > /dev/null
    kill $p'
cp /tmp/wayland-screenshot-*.png "/out/$NAME.png"

case "$PM" in
    apt) quiet apt-get remove -y mdview ;;
    dnf) quiet dnf remove -y mdview ;;
esac
left=$(ls /usr/bin/mdview /usr/share/applications/mdview.desktop \
          /usr/share/icons/hicolor/*/apps/mdview.png 2>/dev/null || true)
[ -z "$left" ] || fail "left behind after removal: $left"
echo "ok"
IN

# Decodes the screenshot with the standard library only (8-bit RGB/RGBA,
# non-interlaced, which is what weston writes) and counts two colours.
check_shot() {
    python3 -I - "$1" <<'PY'
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
pos, idat = 8, b""
while pos < len(data):
    n, kind = struct.unpack(">I4s", data[pos:pos + 8]); body = data[pos + 8:pos + 8 + n]
    if kind == b"IHDR": w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
    elif kind == b"IDAT": idat += body
    pos += 12 + n
assert depth == 8 and ctype in (2, 6) and not interlace, "unexpected PNG format"
bpp = 3 if ctype == 2 else 4
raw, stride, prev, rows, i = zlib.decompress(idat), w * bpp, bytearray(w * bpp), [], 0
for _ in range(h):
    f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
    for x in range(stride):
        a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
        if f == 1: line[x] = (line[x] + a) & 255
        elif f == 2: line[x] = (line[x] + b) & 255
        elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
        elif f == 4:
            p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    rows.append(line); prev = line
def count(rgb, tol):
    return sum(1 for r in rows for x in range(0, stride, bpp)
               if all(abs(r[x + k] - rgb[k]) <= tol for k in range(3)))
purple, text = count((125, 91, 166), 8), count((34, 34, 34), 8)
print(f"  screenshot: {purple} px of the image's purple, {text} px of body text")
sys.exit(0 if purple >= 1000 and text >= 200 else 1)
PY
}

run() {  # image, package manager, package
    local image=$1 pm=$2 pkg=$3 name=${1##*/}; name=${name//:/-}
    echo "== $image: $pkg =="
    podman --log-level=error run --rm -e PM="$pm" -e PKG="$pkg" -e NAME="$name" \
        -v "$DIST:/dist:ro" -v "$REPO/packaging/smoke:/smoke:ro" -v "$REPO/icons:/icons:ro" \
        -v "$DIST/smoke:/out" -v "$INNER:/test.sh:ro" \
        "$image" bash /test.sh
    check_shot "$DIST/smoke/$name.png" || { echo "FAIL: $image screenshot does not show the document"; exit 1; }
}

run docker.io/library/ubuntu:24.04 apt "$DEB"
run docker.io/library/ubuntu:26.04 apt "$DEB"
run docker.io/library/debian:13    apt "$DEB"
run docker.io/library/fedora:43    dnf "$RPM"
run docker.io/library/fedora:45    dnf "$RPM"

# The Debian changelog must actually parse.
tmpc=$(mktemp -d)
dpkg-deb -x "$DIST/$DEB" "$tmpc"
gunzip -c "$tmpc/usr/share/doc/mdview/changelog.gz" > "$tmpc/ch"
if command -v dpkg-parsechangelog > /dev/null; then
    dpkg-parsechangelog -l "$tmpc/ch" > /dev/null || { echo "FAIL: deb changelog does not parse"; exit 1; }
else
    head -1 "$tmpc/ch" | grep -qE '^mdview \([^)]+\) [a-z]+; urgency=' \
        || { echo "FAIL: deb changelog heading is not Debian-conformant"; exit 1; }
fi
rm -rf "$tmpc"
echo "deb changelog parses"
echo "all package tests passed"
