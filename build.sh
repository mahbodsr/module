#!/bin/bash
set -e
echo "==> Compiling webauth_mm.so using GCC (Linux 32-bit)..."

INCLUDES=(
    "-I./metamod-r/metamod/src"
    "-I./metamod-r/metamod/include/common"
    "-I./metamod-r/metamod/include/engine"
    "-I./metamod-r/metamod/include/dlls"
    "-I./metamod-r/metamod/include/pm_shared"
    "-I./rehlds/rehlds/public"
    "-I./rehlds/rehlds/public/rehlds"
)

g++ -m32 -shared -std=c++11 -O2 -fPIC plugin.cpp "${INCLUDES[@]}" -lhiredis -static-libgcc -static-libstdc++ -o webauth_mm.so

echo "==> Build successful: webauth_mm.so generated."