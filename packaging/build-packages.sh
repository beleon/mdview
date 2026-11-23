#!/usr/bin/env bash
# Build mdview's amd64 .deb and .rpm with nfpm.
#
# Each package is compiled natively on its own distro family, so it links
# exactly what that family ships and declares dependencies that exist there:
#   deb  the release Containerfile (Ubuntu 24.04)  -> Ubuntu 24.04+, Debian 13+
#   rpm  packaging/Containerfile.fedora (Fedora 43) -> Fedora 43+
# The plain binary on the release page is unaffected: build.sh still makes it.
#
# Runtime dependencies are derived inside each build container by the
# distro's own tooling, never written by hand: dpkg-shlibdeps for the deb,
# rpm's elfdeps (the generator rpmbuild itself runs) for the rpm.
#
# Requires: podman, nfpm, dpkg-deb, network.
#   packaging/build-packages.sh [out-dir]
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-"$REPO/dist"}
cd "$REPO"

VERSION=$(sed -n 's/^project(mdview VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VERSION" ] || { echo "error: no version found in CMakeLists.txt" >&2; exit 1; }
TOP=$(sed -n 's/^- semver: *//p' packaging/changelog.yaml | head -1)
if [ "$TOP" != "$VERSION" ]; then
    echo "error: packaging/changelog.yaml's newest entry is $TOP but CMakeLists.txt" \
         "says $VERSION; add this release's entry first" >&2
    exit 1
fi
export VERSION

WORK=$(mktemp -d -t mdview-pkg-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# build <family> <containerfile> <dependency script>
# Leaves the stripped binary at $WORK/<family>/mdview and one runtime
# dependency per line in $WORK/<family>/depends.
build() {
    local fam=$1 cf=$2 deps=$3
    echo "==> building mdview $VERSION for $fam ($cf)"
    podman --log-level=error build -q -t "mdview-pkg-$fam" -f "$cf" . > /dev/null
    mkdir -p "$WORK/$fam"
    podman --log-level=error run --rm -v "$WORK/$fam:/out" "mdview-pkg-$fam" bash -euc "
        install -m755 /build/mdview/build/mdview /out/mdview
        strip /out/mdview
        $deps"
    [ -s "$WORK/$fam/depends" ] || { echo "error: no dependencies derived for $fam" >&2; exit 1; }
}

# dpkg-shlibdeps insists on a debian/control; its answer is one
# comma-separated line, split here into one dependency per line.
build deb Containerfile '
    mkdir -p /tmp/s/debian && cd /tmp/s
    printf "Source: mdview\n\nPackage: mdview\nArchitecture: any\n" > debian/control
    dpkg-shlibdeps -O /out/mdview | sed -n "s/^shlibs:Depends=//p" | sed "s/, /\n/g" > /out/depends'
# elfdeps emits soname requirements such as libcairo.so.2()(64bit): they
# name a library, not a package, exactly as rpmbuild would record them.
build rpm packaging/Containerfile.fedora '
    /usr/lib/rpm/elfdeps -R /out/mdview > /out/depends'

grep -q '^libc6 ' "$WORK/deb/depends" || { echo "error: deb dependencies lack libc6" >&2; exit 1; }
grep -q '^libc\.so\.6' "$WORK/rpm/depends" || { echo "error: rpm dependencies lack libc.so.6" >&2; exit 1; }

for fam in deb rpm; do
    st="$WORK/stage-$fam"
    install -Dm755 "$WORK/$fam/mdview" "$st/usr/bin/mdview"
    install -Dm644 packaging/mdview.desktop "$st/usr/share/applications/mdview.desktop"
    for png in icons/mdview-*.png; do
        n=${png##*-}; n=${n%.png}
        install -Dm644 "$png" "$st/usr/share/icons/hicolor/${n}x${n}/apps/mdview.png"
    done
    doc="$st/usr/share/doc/mdview"
    install -Dm644 packaging/copyright                     "$doc/copyright"
    install -Dm644 LICENSE                                 "$doc/licenses/mdview-LICENSE"
    install -Dm644 thirdparty/md4c-LICENSE.md              "$doc/licenses/md4c-LICENSE.md"
    install -Dm644 thirdparty/litehtml/LICENSE             "$doc/licenses/litehtml-LICENSE"
    install -Dm644 thirdparty/litehtml/src/gumbo/LICENSE   "$doc/licenses/gumbo-LICENSE"
    install -Dm644 thirdparty/lunasvg/LICENSE              "$doc/licenses/lunasvg-LICENSE"
    install -Dm644 thirdparty/lunasvg/plutovg/LICENSE      "$doc/licenses/plutovg-LICENSE"
    install -Dm644 thirdparty/lunasvg/plutovg/source/FTL.TXT "$doc/licenses/plutovg-FTL.TXT"

    cp packaging/changelog.yaml "$st/"
    {
        cat packaging/nfpm.yaml
        echo
        echo "# Derived by build-packages.sh for this package's distro family."
        echo "depends:"
        sed "s/.*/  - '&'/" "$WORK/$fam/depends"
        echo "recommends:"
        echo "  - xdg-utils   # opens external links"
        echo "suggests:"
        echo "  - bat         # syntax highlighting in code blocks"
    } > "$st/nfpm.yaml"
    (cd "$st" && nfpm package -f nfpm.yaml -p "$fam" -t "$OUT/" > /dev/null)
done

"$REPO/packaging/fix-deb-changelog.sh" "$OUT/mdview_${VERSION}_amd64.deb"

echo
echo "deb depends: $(paste -sd' ' "$WORK/deb/depends" | sed 's/) /), /g')"
echo "rpm requires: $(paste -sd' ' "$WORK/rpm/depends")"
ls -l "$OUT/mdview_${VERSION}_amd64.deb" "$OUT/mdview-${VERSION}-1.x86_64.rpm"
