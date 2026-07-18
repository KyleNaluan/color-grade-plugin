#!/usr/bin/env bash
#
# build.sh - unattended native build from WSL via Windows MSBuild interop.
#
# The worktree lives on WSL ext4; MSBuild against a \\wsl$ path is slow/broken, so we
# mirror native/ to an NTFS scratch dir (C:\dev\cg-native-build) and build there. The
# worktree is the source of truth; the mirror is disposable. The linker drops the .aex
# into AE_PLUGIN_BUILD_DIR (MediaCore) - the only sanctioned writes outside the worktree.
#
# Usage: native/scripts/build.sh [Debug|Release] [--gpu]
#   Debug (default) | Release   build configuration
#   --gpu                       (reserved) build the DirectX GPU configuration
#
# Requires: VS 2022 MSBuild, the four AE SDK env vars (see native/BUILDING.md).
set -euo pipefail

CONFIG="Debug"
CG_GPU="false"
for arg in "$@"; do
  case "$arg" in
    Debug|Release) CONFIG="$arg" ;;
    --gpu) CG_GPU="true" ;;
    *) echo "unknown arg: $arg (usage: build.sh [Debug|Release] [--gpu])" >&2; exit 2 ;;
  esac
done

# --- paths -----------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"           # .../native
REPO_DIR="$(cd "$NATIVE_DIR/.." && pwd)"

MIRROR_WSL="/mnt/c/dev/cg-native-build"              # NTFS mirror (WSL view)
MIRROR_WIN='C:\dev\cg-native-build'                  # NTFS mirror (Windows view)
VCXPROJ_WIN="$MIRROR_WIN\\ColorGradeFX\\Win\\ColorGradeFX.vcxproj"

MSBUILD="/mnt/d/Microsoft/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe"
WIN_SDK_VER="10.0.26100.0"

[ -x "$MSBUILD" ] || { echo "MSBuild not found at: $MSBUILD" >&2; exit 1; }

# --- ensure the embedded LUT header exists (committed; regenerate if missing) ---
if [ ! -f "$NATIVE_DIR/ColorGradeFX/embedded/EmbeddedLut.h" ]; then
  echo "==> generating embedded LUT header"
  ( cd "$REPO_DIR" && npm run --silent native:gen-lut )
fi

# --- sync worktree native/ -> NTFS mirror ----------------------------------
echo "==> syncing $NATIVE_DIR -> $MIRROR_WSL"
mkdir -p "$MIRROR_WSL"
if command -v rsync >/dev/null 2>&1; then
  rsync -a --delete \
    --exclude 'x64/' --exclude '.vs/' --exclude '*.rc' \
    --exclude '*.i' --exclude '*.rr' --exclude '*.rrc' \
    "$NATIVE_DIR/" "$MIRROR_WSL/"
else
  rm -rf "$MIRROR_WSL"/* ; cp -r "$NATIVE_DIR/." "$MIRROR_WSL/"
fi

# Force the CustomBuild steps (PiPL, HLSL kernel) to re-run: their up-to-date check is
# timestamp-based and won't notice the CG_GPU flag flipping CPU<->GPU flags/shaders.
rm -f  "$MIRROR_WSL/ColorGradeFX/Win/ColorGradePiPL.rc"
rm -rf "$MIRROR_WSL/ColorGradeFX/Win/x64/$CONFIG/DirectX_Assets"

# --- build via MSBuild interop ---------------------------------------------
echo "==> MSBuild $CONFIG|x64 CG_GPU=$CG_GPU (WindowsTargetPlatformVersion=$WIN_SDK_VER)"
"$MSBUILD" "$VCXPROJ_WIN" \
  -nologo \
  -p:Configuration="$CONFIG" \
  -p:Platform=x64 \
  -p:WindowsTargetPlatformVersion="$WIN_SDK_VER" \
  -p:CG_GPU="$CG_GPU" \
  -v:minimal \
  -m

echo "==> build OK. .aex -> \$AE_PLUGIN_BUILD_DIR (MediaCore)"
if [ "$CG_GPU" = "true" ]; then
  echo "    DirectX_Assets copied next to the .aex (ColorGradeKernel.cso/.rs)"
fi
