#!/bin/bash
# Build Eka plugins and package each one as a self-contained LV2 bundle for the zynthian package store
# (zynthian-packages, see PACKAGING.md):
#
#   dist/lv2/eka-<plugin>.lv2/                the bundle, with
#       zynthian_widget_eka_<name>.py         its zynthian widget, declared as a zyngui:widget UI in manifest.ttl
#       samples/                              its factory samples (mist, pads)
#   dist/eka-<plugin>-lv2-<arch>.tar.gz       the archive the store installs (arch: x86_64 or arm64)
#
# usage: tools/package.sh [plugin ...]        (default: all the plugins in plugins/)
#
# Used by the GitHub CI (.github/workflows/build.yml), and works the same on any Linux with the build
# dependencies. The bundles in dist/lv2 are plain copies (no symbolic links into the source tree), unlike the
# ones that "make install" links into /usr/local/lib/lv2.

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"

case "$(uname -m)" in
    aarch64) ARCH="arm64" ;;
    x86_64) ARCH="x86_64" ;;
    *) ARCH="$(uname -m)" ;;
esac

if [ $# -eq 0 ]; then
    set -- $(cd "$ROOT/plugins" && ls -d */ | tr -d /)
fi

mkdir -p "$DIST/lv2"
for p in "$@"; do
    P="$ROOT/plugins/$p"
    SYMBOL="eka-$p"
    [ -d "$P" ] || { echo "No plugin $p in plugins/" >&2; exit 1; }
    echo "=== $SYMBOL ($ARCH)"
    make -C "$P"

    # Plain copy of the built bundle
    B="$DIST/lv2/$SYMBOL.lv2"
    rm -rf "$B"
    cp -rL "$P/build/$SYMBOL.lv2" "$B"

    # Zynthian widget inside the bundle, declared as a zyngui:widget UI
    # (zynthian-ui finds it in the LV2 data; see zynthian-ui/lv2/zyngui.lv2/zyngui.ttl).
    WIDGET="$(ls "$P"/ui/zynthian_widget_*.py "$P"/widget/zynthian_widget_*.py 2>/dev/null | head -1)"
    URI="$(grep -B1 'a lv2:Plugin' "$P/lv2/manifest.ttl" | sed -n 's/^<\(.*\)>.*/\1/p' | head -1)"
    [ -n "$URI" ] || { echo "No plugin URI in $P/lv2/manifest.ttl" >&2; exit 1; }
    if [ -n "$WIDGET" ]; then
        W="$(basename "$WIDGET")"
        cp "$WIDGET" "$B/$W"
        cat >> "$B/manifest.ttl" <<EOF

# Zynthian widget (added by tools/package.sh)
<$URI> <http://lv2plug.in/ns/extensions/ui#ui> <$URI#zyngui> .
<$URI#zyngui>
    a <http://zynthian.org/ns/lv2/zyngui#widget> ;
    <http://lv2plug.in/ns/extensions/ui#binary> <$W> ;
    <http://zynthian.org/ns/lv2/zyngui#apiVersion> 1 .
EOF
    fi

    # Factory samples (the package script links them into zynthian-my-data/files/Samples)
    if [ -d "$P/samples" ]; then
        cp -rL "$P/samples" "$B/samples"
    fi

    # License and third-party notices (weave4: its Makefile's install puts them in the bundle too)
    for f in "$ROOT/LICENSE" "$P/LICENSE" "$P/THIRD_PARTY.md"; do
        if [ -f "$f" ]; then cp "$f" "$B/"; fi
    done
    if [ "$p" = weave4 ]; then
        cp "$P"/build/bungee/notices/* "$B/"
        cp "$P/third_party/aubio/COPYING" "$B/LICENSE-aubio.txt"
    fi

    # Sanity checks: the bundle has its binary and TTL files
    test -s "$B/manifest.ttl"
    ls "$B"/*.so > /dev/null
    grep -q "$URI" "$B/manifest.ttl"

    tar -C "$DIST/lv2" -czf "$DIST/$SYMBOL-lv2-$ARCH.tar.gz" "$SYMBOL.lv2"
    echo "Packaged: $DIST/$SYMBOL-lv2-$ARCH.tar.gz ($(du -sh "$B" | cut -f1) installed)"
done
