#!/usr/bin/env bash
set -Eeuo pipefail

solution="${SOLUTION:-${1:-}}"
build_type="${BUILD_TYPE:-Release}"

if [[ -z "${solution}" ]]; then
    printf 'Usage: docker run ... <solution>\n' >&2
    printf 'Example: docker run --rm -v "$PWD:/workspace" sscma-sg200x helloworld\n' >&2
    exit 2
fi

solution_dir="/workspace/solutions/${solution}"
build_dir="${solution_dir}/build"

if [[ ! -f "${solution_dir}/CMakeLists.txt" ]]; then
    printf 'Unknown solution: %s\n' "${solution}" >&2
    printf 'Expected a CMake solution under /workspace/solutions/\n' >&2
    exit 2
fi

"${CC}" --version >/dev/null
"${CXX}" --version >/dev/null
rm -f "${build_dir}/CMakeCache.txt"
rm -rf "${build_dir}/CMakeFiles"

cmake -S "${solution_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}"
cmake --build "${build_dir}"

if [[ "${PACKAGE:-0}" == "1" ]]; then
    cmake --build "${build_dir}" --target package
fi