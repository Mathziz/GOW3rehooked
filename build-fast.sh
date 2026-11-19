#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "$0")" && pwd)
type=RelWithDebInfo
ivy=OFF
while (($#)); do
    case "$1" in
        --release) type=Release ;;
        --ivy-bridge) ivy=ON ;;
        *) printf 'Usage: %s [--release] [--ivy-bridge]\n' "$0" >&2; exit 2 ;;
    esac
    shift
done
build="$root/build-fast/Linux/$type"
[[ "$ivy" != ON ]] || build+=-ivybridge

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE="$type" \
    -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DGOW3_IVY_BRIDGE="$ivy" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}" --target gow3 gow3_debug -- -k 0
printf 'Build succeeded: %s/bin\n' "$build"
