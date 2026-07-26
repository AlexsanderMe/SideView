#!/usr/bin/env bash
set -euo pipefail

build_type="${BUILD_TYPE:-Release}"
build_dir="${BUILD_DIR:-build/native-linux}"
package_dir="src/native_webview_widget"

cmake \
  -S native \
  -B "${build_dir}" \
  -DCMAKE_BUILD_TYPE="${build_type}"
cmake --build "${build_dir}" --parallel

install -m 755 \
  "${build_dir}/libnative_webview_widget.so" \
  "${package_dir}/libnative_webview_widget.so"

echo "Installed ${package_dir}/libnative_webview_widget.so"
