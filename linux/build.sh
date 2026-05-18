#!/usr/bin/env bash
#
# build.sh — convenience wrapper around CMake + ctest.
#
# Usage:
#   ./build.sh                 # Release build + tests
#   ./build.sh debug           # Debug build + tests
#   ./build.sh notest          # Release build, skip tests
#   ./build.sh debug notest    # Debug build, skip tests

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

build_type="Release"
run_tests=1

for arg in "$@"; do
    case "$arg" in
        debug)   build_type="Debug" ;;
        release) build_type="Release" ;;
        notest)  run_tests=0 ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Usage: $0 [debug|release] [notest]" >&2
            exit 2
            ;;
    esac
done

mkdir -p build
cd build

cmake -DCMAKE_BUILD_TYPE="$build_type" ..
cmake --build . -j "$(nproc 2>/dev/null || echo 4)"

if [[ "$run_tests" -eq 1 ]]; then
    ctest --output-on-failure
fi
