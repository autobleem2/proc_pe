#!/usr/bin/env bash
# the Windows build (MSYS2 UCRT64: gcc, cmake, ninja) into build_win/, the self-test, and the processor folder
# as it goes onto a stick - dist/pe/ with bin/windows-x86_64/pe.exe
set -e
cd "$(dirname "$0")"
mkdir -p build_win
cmake -S . -B build_win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_win
(cd build_win && ctest --output-on-failure)
rm -rf dist/pe
mkdir -p dist/pe/bin/windows-x86_64
cp package/processor.ini README.md LICENSE dist/pe/
cp third_party/miniz/LICENSE dist/pe/LICENSE.miniz
cp third_party/libarchive/COPYING dist/pe/LICENSE.libarchive
cp third_party/xz/COPYING.0BSD dist/pe/LICENSE.liblzma
cp build_win/pe.exe dist/pe/bin/windows-x86_64/
echo "dist/pe is ready: copy it to <stick>/System/Processors/pe"
