#!/usr/bin/env bash
set -euo pipefail

build_type="${BUILD_TYPE:-Release}"
build_dir="${BUILD_DIR:-build/native-linux}"
package_dir="src/sideview"

cmake \
  -S native \
  -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE="${build_type}"
cmake --build "${build_dir}" --parallel

install -m 755 \
  "${build_dir}/libsideview_native.so" \
  "${package_dir}/libsideview_native.so"

echo "Installed ${package_dir}/libsideview_native.so"
