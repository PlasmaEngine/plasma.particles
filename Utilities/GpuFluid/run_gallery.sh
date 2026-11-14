#!/bin/bash
# Runs ParticlesTest's GpuFluid tests headless on Metal.
#
#   run_gallery.sh <output dir> [filter] [extra test arguments...]
#
# filter defaults to "GpuFluid Gallery". Environment:
#   W        engine worktree (default ~/.t3/worktrees/PlasmaEngine/gpu-fluid)
#   P        this package (default: two folders up from this script)
#   CONFIG   build configuration whose binaries run (default Development)
#   TIMEOUT  seconds before the run is killed (default 1500)
#   PL_GPUFLUID_VIDEO=1  also records the video frames (Handoff Sphere Video)
#
# Changed shaders are not picked up from cached permutations, so the GpuFluid part of the shader cache is deleted first.
set -u

OUT="${1:?usage: run_gallery.sh <output dir> [filter] [extra test arguments...]}"
FILTER="${2:-GpuFluid Gallery}"
shift $(( $# >= 2 ? 2 : 1 ))

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
W="${W:-$HOME/.t3/worktrees/PlasmaEngine/gpu-fluid}"
P="${P:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
CONFIG="${CONFIG:-Development}"
TIMEOUT="${TIMEOUT:-1500}"

BIN="$W/Binaries/Mac_$CONFIG"
TEST_BIN="$P/Bin/Tests/Mac_x64_$CONFIG/ParticlesTest"

if [ ! -x "$TEST_BIN" ]; then
  echo "No test binary at $TEST_BIN - build ParticlesTest first." >&2
  exit 2
fi

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# The test runs next to the engine binaries so it finds the renderer plugins.
cp "$TEST_BIN" "$BIN/"

# Every place a GpuFluid permutation can be cached (the cache root moved between engine versions).
for CACHE in "$W/Output/ShaderCache" "$BIN/ShaderCache"; do
  if [ -d "$CACHE" ]; then
    find "$CACHE" -type d -name GpuFluid -prune -exec rm -rf {} + 2>/dev/null
  fi
done

echo "Running '$FILTER' -> $OUT"
START=$(date +%s)
cd "$BIN" || exit 2
DYLD_LIBRARY_PATH="$BIN:$P/Bin/Mac_x64_$CONFIG" PL_SILENT_ASSERTS=1 \
  perl -e 'alarm shift; exec @ARGV' "$TIMEOUT" ./ParticlesTest -run -close -noGui -noSave -all -assert 0 -renderer Metal \
  -outputDir "$OUT" -filter "$FILTER" "$@" > "$OUT/console.log" 2>&1
STATUS=$?
END=$(date +%s)

grep -E "N0 |Video end|Sub-Test .* (succeeded|failed)|Tests? (passed|failed)|All tests passed|Error:" "$OUT/console.log" | tail -40
echo "exit $STATUS after $((END - START)) s; log: $OUT/console.log"
exit $STATUS
