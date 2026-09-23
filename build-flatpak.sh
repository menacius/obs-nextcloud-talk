#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
manifest="$project_root/packaging/flatpak/com.obsproject.Studio.Plugin.NextcloudTalk.yml"
cache_root="${XDG_CACHE_HOME:-$HOME/.cache}/obs-nextcloud-talk-flatpak"
build_dir="$cache_root/build"
repo_dir="$cache_root/repo"
state_dir="$cache_root/state"
dist_dir="$project_root/dist"
bundle="$dist_dir/obs-nextcloud-talk-0.7.2-flatpak-x86_64.flatpak"
extension_id="com.obsproject.Studio.Plugin.NextcloudTalk"

for command_name in flatpak flatpak-builder; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Missing required command: $command_name" >&2
    exit 1
  fi
done

if ! flatpak info --user com.obsproject.Studio >/dev/null 2>&1; then
  echo "Install the OBS Studio Flatpak for the current user before building." >&2
  exit 1
fi
if ! flatpak info --user org.freedesktop.Sdk//25.08 >/dev/null 2>&1; then
  echo "Install org.freedesktop.Sdk//25.08 for the current user before building." >&2
  exit 1
fi

mkdir -p "$dist_dir"
mkdir -p "$cache_root"
flatpak-builder --user --force-clean --state-dir="$state_dir" --repo="$repo_dir" "$build_dir" "$manifest"
flatpak build-bundle "$repo_dir" "$bundle" "$extension_id" stable --runtime

flatpak install --user --noninteractive -y --reinstall "$bundle"
flatpak run --user --command=sh --devel com.obsproject.Studio -c \
  'set -eu; plugin=/app/plugins/NextcloudTalk/lib/obs-plugins/obs-nextcloud-talk.so; test -f "$plugin"; ! ldd "$plugin" | grep -q "not found"; nm -D "$plugin" | grep -q " obs_module_load$"'

if command -v xvfb-run >/dev/null 2>&1; then
  obs_log="$(mktemp)"
  mkdir -p "$HOME/.var/app/com.obsproject.Studio/config/obs-studio/basic/scenes"
  set +e
  timeout 20s xvfb-run -a flatpak run --user --env=LIBGL_ALWAYS_SOFTWARE=1 \
    com.obsproject.Studio --verbose --unfiltered_log --disable-shutdown-check >"$obs_log" 2>&1
  obs_status=$?
  set -e
  if ! grep -Fq '[nextcloud-talk] Loaded version 0.7.2' "$obs_log"; then
    cat "$obs_log" >&2
    rm -f "$obs_log"
    echo "OBS Flatpak did not load the Nextcloud Talk module (exit $obs_status)." >&2
    exit 1
  fi
  rm -f "$obs_log"
  echo "OBS Flatpak loaded Nextcloud Talk 0.7.2 successfully."
fi

(cd "$dist_dir" && sha256sum "$(basename "$bundle")" > "$(basename "$bundle").sha256")
echo "Built, installed and verified: $bundle"
