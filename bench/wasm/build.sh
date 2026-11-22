#!/bin/bash
# Builds module.mjs, earstrip and earcut.hpp in one WebAssembly module, for bench.mjs.
# Usage: EMSDK=<emsdk dir> EARCUT=<dir with earcut.hpp> ./build.sh
set -e
cd "$(dirname "$0")"
[ -n "$EMSDK" ] && source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
[ -n "$EARCUT" ] || { echo "EARCUT=<dir with earcut.hpp> is required" >&2; exit 1; }
# WebAssembly SIMD as in the package, without autovectorization: that makes earcut.hpp 5 to 9% slower in V8 and
# JavaScriptCore and earstrip no faster
em++ -O3 -std=c++23 -msimd128 -fno-vectorize -fno-slp-vectorize -DNDEBUG -DEARSTRIP_SMALL_CONSTRAINED -I../../include -I"$EARCUT" -sMODULARIZE -sEXPORT_ES6 \
  -sENVIRONMENT=node,web -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=4GB -sEXPORTED_RUNTIME_METHODS=HEAP16,HEAPU16,HEAPU32 \
  -fno-exceptions module.cpp -o module.mjs
