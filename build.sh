#!/usr/bin/env bash
# Builds tools/av1mkv/build/av1mkv.exe with llvm-mingw clang, CMake and Ninja.
#
#   LLVM_MINGW  the llvm-mingw bin directory (default: the desk's 20260826 ucrt build)
#   NINJA       a ninja.exe (default: the one tools/oxrsys's pixi environment installed)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_MINGW="${LLVM_MINGW:-C:/Users/ernest.lee/llvm-mingw/llvm-mingw-20260826-ucrt-x86_64/bin}"
NINJA="${NINJA:-C:/Users/ernest.lee/AppData/Local/rattler/cache/envs/oxrsys-windows-2975689634350405989/envs/default/Library/bin/ninja.exe}"
if [ ! -x "$NINJA" ]; then
  NINJA="$(command -v ninja || true)"
fi
[ -n "$NINJA" ] || { echo "no ninja; set NINJA=" >&2; exit 1; }
BUILD="$HERE/build"
if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -S "$HERE" -B "$BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_C_COMPILER="$LLVM_MINGW/x86_64-w64-mingw32-clang.exe" \
    -DCMAKE_CXX_COMPILER="$LLVM_MINGW/x86_64-w64-mingw32-clang++.exe" \
    -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$BUILD" --target av1mkv
echo "built $BUILD/av1mkv.exe"
