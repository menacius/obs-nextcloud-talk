#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-${project_dir}/build-linux}"
dist_dir="${DIST_DIR:-${project_dir}/dist}"
stage_dir="${build_dir}/package-root"
build_type="${BUILD_TYPE:-RelWithDebInfo}"
jobs="${JOBS:-$(nproc)}"
version="$(sed -n 's/^project(obs-nextcloud-talk VERSION \([^ ]*\).*/\1/p' "${project_dir}/CMakeLists.txt")"
architecture="$(uname -m)"

if [[ -z "${version}" ]]; then
  echo "Could not read the project version from CMakeLists.txt." >&2
  exit 1
fi

cmake -S "${project_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${build_type}" \
  -DNEXTCLOUD_TALK_BUILD_TESTS=ON
cmake --build "${build_dir}" --parallel "${jobs}"
ctest --test-dir "${build_dir}" --output-on-failure

plugin="${build_dir}/obs-nextcloud-talk.so"
if [[ ! -f "${plugin}" ]]; then
  echo "Expected plugin was not produced: ${plugin}" >&2
  exit 1
fi
if ldd "${plugin}" | grep -q 'not found'; then
  echo "The Linux plugin has unresolved shared-library dependencies:" >&2
  ldd "${plugin}" | grep 'not found' >&2
  exit 1
fi

cmake -E remove_directory "${stage_dir}"
cmake --install "${build_dir}" --prefix "${stage_dir}" --component Runtime
mkdir -p "${dist_dir}"
archive_name="obs-nextcloud-talk-${version}-linux-${architecture}.tar.gz"
archive="${dist_dir}/${archive_name}"
tar -C "${stage_dir}" -czf "${archive}" .
(
  cd "${dist_dir}"
  sha256sum "${archive_name}" > "${archive_name}.sha256"
)

echo "Built and tested: ${plugin}"
echo "Package: ${archive}"
