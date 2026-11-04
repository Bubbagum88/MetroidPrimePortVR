#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux counterpart of quest/Build-QuestDawn.ps1, for CI: builds aurora's pinned Dawn with
# PrimedGun's patches (quest/dawn/apply.py: Vulkan multiview, direct presentation, fragment
# density maps) for Android arm64, and packs it in the stock layout the native build takes as
# AURORA_DAWN_PACKAGE_URL (gradle -PquestDawnPackage=<package>).
#
# Usage: quest/build-quest-dawn.sh <work directory>
# Needs ANDROID_HOME with NDK 29.0.14206865, git, python3 and cmake 3.25+ on PATH, and ninja 1.11+
# (NINJA, else the one on PATH; the SDK's CMake ships an older one, which Dawn's C++ modules refuse).
# Prints nothing but progress; the package is <work directory>/dawn-android-aarch64-primedgun.tar.gz.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mkdir -p "$1" && cd "$1" && pwd)"

# As Build-QuestDawn.ps1: aurora's revision, with its archive hash pinned here.
revision=118436b3f8f68583eb86e811ca605dc41637e8cc
archive_hash=033b1a2eca2dddc67fd9c1ca88a54b42d727f8a9f765ba669b84d9a77bb02576
pinned=$(sed -n 's/.*_aurora_dependency_version(AURORA_DAWN_REF "\([a-f0-9]*\)".*/\1/p' \
  "$repo/extern/aurora/cmake/AuroraDependencyVersions.cmake")
if [ "$pinned" != "$revision" ]; then
  echo "aurora pins Dawn $pinned, not $revision; update this script and Build-QuestDawn.ps1" >&2
  exit 1
fi

ndk_version=29.0.14206865
ndk="$ANDROID_HOME/ndk/$ndk_version"
toolchain="$ndk/build/cmake/android.toolchain.cmake"
[ -f "$toolchain" ] || { echo "NDK $ndk_version not found under $ANDROID_HOME" >&2; exit 1; }

ninja="${NINJA:-$(command -v ninja)}"
ninja_version=$("$ninja" --version)
if [ "$(printf '%s\n1.11\n' "$ninja_version" | sort -V | head -1)" != 1.11 ]; then
  echo "Dawn needs Ninja 1.11 or newer; $ninja is $ninja_version (set NINJA)" >&2
  exit 1
fi

archive="$work/dawn-${revision:0:12}.tar.gz"
if [ ! -f "$archive" ]; then
  echo "Downloading Dawn $revision"
  curl -fsSL "https://github.com/Odrannnn/dawn/archive/$revision.tar.gz" -o "$archive"
fi
echo "$archive_hash  $archive" | sha256sum -c -

source="$work/s"
rm -rf "$source"
mkdir -p "$source"
tar -xzf "$archive" -C "$source" --strip-components=1
python3 "$repo/quest/dawn/apply.py" "$source"

build="$work/b"
install="$work/p"
# The flags go before -C, as in encounter's release workflow: dawn-ci.cmake branches on
# CMAKE_SYSTEM_NAME. No protobuf: only the IR binary format needs it.
cmake -S "$source" -B "$build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" \
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Android \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DDAWN_BUILD_PROTOBUF=OFF \
  -C "$source/.github/workflows/dawn-ci.cmake" \
  "-DPython3_EXECUTABLE=$(command -v python3)" \
  "-DCMAKE_INSTALL_PREFIX=$install"
cmake --build "$build" --parallel "$(nproc)"
rm -rf "$install"
cmake --install "$build"
library="$install/lib/libwebgpu_dawn.a"
[ -f "$library" ] || { echo "Dawn archive missing: $library" >&2; exit 1; }
# As the stock package: debug info would multiply the archive's size.
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-debug "$library"

tar -czf "$work/dawn-android-aarch64-primedgun.tar.gz" -C "$install" .
echo "Patched Dawn for the Quest ready: $work/dawn-android-aarch64-primedgun.tar.gz"
