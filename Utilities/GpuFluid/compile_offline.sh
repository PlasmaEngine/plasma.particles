#!/bin/bash
# Compiles the GpuFluid shaders offline for other platforms with the engine's ShaderCompiler (plan amendment A6).
#
#   compile_offline.sh <work dir> [platforms] [shader folders]
#
# platforms defaults to "VULKAN DX12"; shader folders (relative to Assets/) default to the GpuFluid top level, Gas and
# Liquid. Every permutation is compiled (-force); a log per platform goes to <work dir>/compile-<PLATFORM>.log and the
# summary line of each is printed. Exits non-zero when a platform reports errors.
#
# The ShaderCompiler resolves '>sdk/' data directories against -sdkroot and the shader paths against -project, so the
# work dir gets a small project (the FluidGallery data directories plus this package's Assets) and an sdk root of
# links to the engine's Data folders and to this package's Assets. Environment: W engine worktree (default
# ~/.t3/worktrees/PlasmaEngine/gpu-fluid), P this package (default: two folders up from this script).
set -u

WORK="${1:?usage: compile_offline.sh <work dir> [platforms] [shader folders]}"
PLATFORMS="${2:-VULKAN DX12}"
FOLDERS="${3:-Shaders/GpuFluid Shaders/GpuFluid/Gas Shaders/GpuFluid/Liquid}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
W="${W:-$HOME/.t3/worktrees/PlasmaEngine/gpu-fluid}"
P="${P:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
BIN="$W/Binaries/Mac_Development"

if [ ! -x "$BIN/ShaderCompiler" ]; then
  echo "No ShaderCompiler at $BIN - build the ShaderCompiler target first." >&2
  exit 2
fi

mkdir -p "$WORK/sdk/Data" "$WORK/project/RuntimeConfigs" "$WORK/cache"
WORK="$(cd "$WORK" && pwd)"
for d in Base Plugins UnitTests; do
  ln -sfn "$W/Data/$d" "$WORK/sdk/Data/$d"
done
ln -sfn "$P/Assets" "$WORK/sdk/Data/GpuFluidPackage"
ln -sfn "$P/Assets/Shaders" "$WORK/project/Shaders"
cp "$W/Data/UnitTests/GameEngineTest/FluidGallery/plProject" "$WORK/project/"
cp "$W/Data/UnitTests/GameEngineTest/FluidGallery/RuntimeConfigs/DataDirectories.ddl" "$WORK/project/RuntimeConfigs/"
cat >> "$WORK/project/RuntimeConfigs/DataDirectories.ddl" <<EOF
DataDir
{
	string %Path{">sdk/Data/GpuFluidPackage"}
	string %RootName{"plasma.particles"}
	bool %Writable{false}
}
EOF

SHADERS=""
for f in $FOLDERS; do
  SHADERS="${SHADERS:+$SHADERS;}$f"
done

STATUS=0
for PLATFORM in $PLATFORMS; do
  LOG="$WORK/compile-$PLATFORM.log"
  (cd "$BIN" && DYLD_LIBRARY_PATH="$BIN" ./ShaderCompiler -project "$WORK/project" -sdkroot "$WORK/sdk" -shader "$SHADERS" \
    -platform "$PLATFORM" -shadercache "$WORK/cache" -force -IgnoreErrors > "$LOG" 2>&1)
  echo "$PLATFORM: $(grep -E "Compiled [0-9]+ shader" "$LOG" | sed 's/^ *//')"
  if grep -q "error:" "$LOG" || ! grep -q " 0 error(s)" "$LOG"; then
    grep -E "Failed to compile shader|error:" "$LOG" | sort -u | head -20
    STATUS=1
  fi
done
exit $STATUS
