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
#   PL_GPUFLUID_VIDEO=1  also records the video frames (Handoff Sphere Video, Smoke Column Video, Corridor Smoke Video,
#                        Smoke Around Obstacles Video)
#   PL_GPUFLUID_TUNE_<NAME>=<value>  overrides a look value of the smoke scenes (see Tune() in GpuFluidGalleryTest.cpp)
#
# Changed shaders are not picked up from cached permutations, so the gas part of the GpuFluid shader cache is deleted
# first. A filter can name one sub-test, e.g. "^Smoke Column$".
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

# Every place a GpuFluid permutation can be cached (the cache root moved between engine versions). Only the gas
# shaders' part: the top level of every GpuFluid folder and its Gas subfolder. GpuFluid/Liquid belongs to the liquid
# track, which may be running from the same binaries.
for CACHE in "$W/Output/ShaderCache" "$BIN/ShaderCache"; do
  if [ -d "$CACHE" ]; then
    find "$CACHE" -type d -name GpuFluid -prune -print 2>/dev/null | while read -r DIR; do
      find "$DIR" -maxdepth 1 -name '*.plPermutation' -delete
      rm -rf "$DIR/Gas"
    done
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

grep -E "N0 |N2 |N3 |N4 |N9 |lost to the solids|Video end|Smoke |Gas safety|Corridor video|Obstacles video|Tier |Sub-Test .* (succeeded|failed)|Tests? (passed|failed)|All tests passed|Error:" "$OUT/console.log" | tail -60
echo "exit $STATUS after $((END - START)) s; log: $OUT/console.log"
exit $STATUS
