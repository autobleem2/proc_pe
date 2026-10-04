#!/usr/bin/env bash
# Builds proc_pe in the autobleem-build image (ghcr.io/autobleem2/autobleem-build) and packages it:
#
#   ci/build.sh native                        host build + the self-test (build_linux/)
#   ci/build.sh psc|rpi|rpi64|pcusb|win       one target's binary, checked, into dist/pe/bin/<key>/
#   ci/build.sh package                       dist/pe-<version>.zip from what the targets left in dist/pe
#   ci/build.sh all                           every one of them, then the package
#
# The package is the processor's folder as a stick has it - System/Processors/pe/: processor.ini, the
# README and licences, and one binary per platform key. The keys are the generic ones where they fit, so
# one build serves every machine of that kind: psc (the console), linux-armhf and linux-arm64 (any Pi),
# linux-i386 (the PC stick), windows-x86_64. The version is processor.ini's Version=, or AB_VERSION.
#
# On the build server: docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src \
#                          ghcr.io/autobleem2/autobleem-build:develop ci/build.sh all
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${AB_VERSION:-$(sed -n 's/^Version=//p' package/processor.ini | tr -d '\r')}"
JOBS="${JOBS:-$(nproc)}"
STAGE=dist/pe
LAUNCHER=()
if [ -z "${AB_NO_SCCACHE:-}" ] && command -v sccache >/dev/null 2>&1; then
    LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache)
fi

banner() { printf '\n==== %s ====\n' "$*"; }

configure() { # configure <dir> <cmake args...>
    local dir="$1"
    shift
    cmake -S . -B "$dir" -G Ninja "${LAUNCHER[@]}" "$@" >/dev/null
}

stage() { # stage <built binary> <key> [exe name]
    local bin="$1" key="$2" name="${3:-pe}"
    mkdir -p "$STAGE/bin/$key"
    cp "$bin" "$STAGE/bin/$key/$name"
    # a smaller file on a slow stick (the launcher packs its binaries too); not the Windows one - some
    # virus scanners take a UPX-packed exe for a threat
    if [ "$key" != windows-x86_64 ] && command -v upx >/dev/null 2>&1 && [ -z "${AB_NO_UPX:-}" ]; then
        upx -q --best --lzma "$STAGE/bin/$key/$name" >/dev/null || echo "upx: left unpacked"
    fi
    ls -l "$STAGE/bin/$key/$name"
}

build_native() {
    banner "native: build + self-test (build_linux)"
    configure build_linux -DCMAKE_BUILD_TYPE=Release
    ninja -C build_linux -j "$JOBS"
    ctest --test-dir build_linux --output-on-failure
}

build_psc() {
    banner "psc: the console (build_psc)"
    configure build_psc -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/ab/toolchains/psc/PSCtoolchainV8.cmake \
        -DAB_PSC_TOOLCHAIN="${AB_PSC_TOOLCHAIN:-/opt/psc}"
    ninja -C build_psc -j "$JOBS" pe
    # the console's glibc 2.24 / GLIBCXX 3.4.22, no RPATH
    bash /opt/ab/tools/check_psc_binary.sh build_psc/pe "${AB_PSC_TOOLCHAIN:-/opt/psc}"
    stage build_psc/pe psc
}

build_rpi() { # build_rpi rpi|rpi64
    local target="$1" dir="build_$1" toolchain=toolchains/rpi/RPitoolchain.cmake key=linux-armhf
    if [ "$target" = rpi64 ]; then
        toolchain=toolchains/rpi64/RPi64toolchain.cmake
        key=linux-arm64
    fi
    banner "$target: Raspberry Pi ($dir -> $key)"
    configure "$dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$toolchain"
    ninja -C "$dir" -j "$JOBS" pe
    file "$dir/pe"
    stage "$dir/pe" "$key"
}

build_pcusb() {
    banner "pcusb: the 32-bit PC stick (build_pcusb -> linux-i386)"
    configure build_pcusb -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=toolchains/pcusb/PcUsbToolchain.cmake
    ninja -C build_pcusb -j "$JOBS"
    file build_pcusb/pe | grep -q 'ELF 32-bit LSB.*Intel 80386'
    # i386 runs here: the self-test again, on the stick's own architecture
    ctest --test-dir build_pcusb --output-on-failure
    stage build_pcusb/pe linux-i386
}

build_win() {
    banner "win: Windows (build_mingw -> windows-x86_64)"
    configure build_mingw -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=toolchains/mingw/MinGWtoolchain.cmake
    ninja -C build_mingw -j "$JOBS" pe
    file build_mingw/pe.exe | grep -q 'PE32+'
    stage build_mingw/pe.exe windows-x86_64 pe.exe
}

package() {
    banner "package: dist/pe-$VERSION.zip"
    [ -d "$STAGE/bin" ] || { echo "nothing built into $STAGE" >&2; exit 1; }
    cp package/processor.ini README.md LICENSE "$STAGE/"
    cp third_party/miniz/LICENSE "$STAGE/LICENSE.miniz"
    cp third_party/libarchive/COPYING "$STAGE/LICENSE.libarchive"
    cp third_party/xz/COPYING.0BSD "$STAGE/LICENSE.liblzma"
    sed -i "s/^Version=.*/Version=$VERSION/" "$STAGE/processor.ini"
    local zip="dist/pe-$VERSION.zip"
    rm -f "$zip"
    (cd dist && python3 - "$(basename "$zip")" <<'EOF'
import os, sys, zipfile
# pe/ as it goes into System/Processors, with each file's mode (the programs stay executable where the
# filesystem keeps it)
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk("pe"):
        dirs.sort()
        for name in sorted(files):
            z.write(os.path.join(root, name))
EOF
    )
    ls -l "$zip"
    find "$STAGE" -type f | sort
}

[ $# -gt 0 ] || { echo "usage: $0 native|psc|rpi|rpi64|pcusb|win|package|all" >&2; exit 2; }
for target in "$@"; do
    case "$target" in
        native) build_native ;;
        psc) build_psc ;;
        rpi | rpi64) build_rpi "$target" ;;
        pcusb) build_pcusb ;;
        win) build_win ;;
        package) package ;;
        all) rm -rf "$STAGE"; build_native; build_psc; build_rpi rpi; build_rpi rpi64; build_pcusb; build_win; package ;;
        *) echo "unknown target: $target" >&2; exit 2 ;;
    esac
done
