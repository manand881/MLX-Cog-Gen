#!/usr/bin/env bash
# Run mlx_translate vs gdal_translate benchmark across all rasters in data/.
# Each raster is benchmarked independently; results are consolidated in a
# single table at the end.
#
# Usage: ./benchmark/benchmark.sh [iterations]
# Default: 5 iterations per raster per tool

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$SCRIPT_DIR/data"
ITERATIONS=${1:-5}
LOG_FILE="$SCRIPT_DIR/benchmark-$(date '+%Y-%m-%d-%H-%M-%S').log"
exec > >(tee "$LOG_FILE") 2>&1
GDAL_OUT="/tmp/benchmark_gdal_out.tif"
MLX_OUT="/tmp/benchmark_mlx_out.tif"

# ---------------------------------------------------------------------------
# System info
# ---------------------------------------------------------------------------
CHIP=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || sysctl -n hw.model)
PHYSICAL_CORES=$(sysctl -n hw.physicalcpu)
LOGICAL_CORES=$(sysctl -n hw.logicalcpu)
TOTAL_RAM_BYTES=$(sysctl -n hw.memsize)
TOTAL_RAM_GB=$(echo "scale=1; $TOTAL_RAM_BYTES / 1073741824" | bc)
GPU=$(system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/{print $2; exit}')
GDAL_VERSION=$(gdal-config --version)
MLX_VERSION=$(brew list mlx --versions | awk '{print $2}')

echo "========================================"
echo "  mlx-cog-gen benchmark"
echo "========================================"
echo "  Chip         : $CHIP"
echo "  Cores        : $PHYSICAL_CORES physical / $LOGICAL_CORES logical"
echo "  GPU          : $GPU"
echo "  Total RAM    : ${TOTAL_RAM_GB}GB"
echo "  GDAL version : $GDAL_VERSION"
echo "  MLX version  : $MLX_VERSION"
echo "  Runs         : $ITERATIONS"
echo "========================================"

# ---------------------------------------------------------------------------
# Benchmark a single command; echoes the average time to stdout.
# All progress output goes to stderr so it doesn't pollute the return value.
# ---------------------------------------------------------------------------
bench_tool() {
    local label=$1
    local cmd=$2
    local times=()

    echo "  [$label]" >&2
    for i in $(seq 1 "$ITERATIONS"); do
        elapsed=$( { time eval "$cmd" > /dev/null 2>&1; } 2>&1 | grep real | awk '{print $2}' )
        secs=$(echo "$elapsed" | awk -F'm' '{printf "%06.3f", $1*60 + $2}' | tr -d 's')
        times+=("$secs")
        printf "    Run %d: %ss\n" "$i" "$secs" >&2
    done

    local min max sum avg
    min=${times[0]}; max=${times[0]}; sum=0
    for t in "${times[@]}"; do
        sum=$(echo "$sum + $t" | bc)
        min=$(echo "if ($t < $min) $t else $min" | bc)
        max=$(echo "if ($t > $max) $t else $max" | bc)
    done
    avg=$(echo "scale=3; $sum / $ITERATIONS" | bc)
    printf "    min: %ss  max: %ss  avg: %ss\n" "$min" "$max" "$avg" >&2

    echo "$avg"
}

# ---------------------------------------------------------------------------
# Discover rasters -- sorted, dynamic, no hardcoding
# ---------------------------------------------------------------------------
RASTERS=()
while IFS= read -r f; do RASTERS+=("$f"); done < <(find "$DATA_DIR" -maxdepth 1 -name "*.tif" -exec du -k {} + | sort -n | awk '{print $2}')

if [ ${#RASTERS[@]} -eq 0 ]; then
    echo "No rasters found in $DATA_DIR -- run generate_test_data.sh first." >&2
    exit 1
fi

echo ""
echo "Found ${#RASTERS[@]} raster(s) to benchmark:"
for r in "${RASTERS[@]}"; do echo "  $r"; done

# ---------------------------------------------------------------------------
# Warmup — run mlx_translate once on the smallest raster before any timing.
# This initialises the MLX runtime, loads mlx.metallib into GPU memory, and
# creates the Metal command queue so none of that one-time cost appears in
# the timed runs.
# ---------------------------------------------------------------------------
WARMUP_INPUT="${RASTERS[0]}"
echo ""
echo "Warming up MLX runtime (untimed)..."
"$REPO_ROOT/build/mlx_translate" "$WARMUP_INPUT" "$MLX_OUT" > /dev/null 2>&1 || true
rm -f "$MLX_OUT"
echo "  Done."

# Accumulate table rows per method: "raster|dims|fsize|gdal_1t|gdal_nt|mlx_avg|speedup_1t|speedup_nt"
ROWS_AVERAGE=()
ROWS_BILINEAR=()

speedup_label() {
    local gdal=$1 mlx=$2
    local s
    s=$(echo "scale=2; $gdal / $mlx" | bc)
    if (( $(echo "$s >= 1" | bc -l) )); then
        echo "${s}x faster"
    else
        echo "$(echo "scale=2; $mlx / $gdal" | bc)x slower"
    fi
}

for INPUT in "${RASTERS[@]}"; do
    NAME=$(basename "$INPUT" .tif)
    DIMS=$(gdalinfo "$INPUT" | awk '/^Size is/{gsub(",","",$3); gsub(",","",$4); print $3 "x" $4}')
    FSIZE=$(du -sh "$INPUT" | awk '{print $1}')

    echo ""
    echo "========================================"
    echo "  $NAME  ($DIMS px, $FSIZE)"
    echo "========================================"

    echo "  -- AVERAGE --" >&2
    GDAL_AVG_1T=$(bench_tool "gdal average (1 thread)" \
        "gdal_translate $INPUT $GDAL_OUT -of COG -co COMPRESS=LZW -co OVERVIEWS=AUTO -co OVERVIEW_RESAMPLING=AVERAGE")
    GDAL_AVG_NT=$(bench_tool "gdal average (ALL_CPUS)" \
        "gdal_translate --config GDAL_NUM_THREADS ALL_CPUS $INPUT $GDAL_OUT -of COG -co COMPRESS=LZW -co OVERVIEWS=AUTO -co OVERVIEW_RESAMPLING=AVERAGE")
    MLX_AVG_AVG=$(bench_tool "mlx average" \
        "$REPO_ROOT/build/mlx_translate $INPUT $MLX_OUT -r AVERAGE")
    rm -f "$GDAL_OUT" "$MLX_OUT"
    ROWS_AVERAGE+=("$NAME|$DIMS|$FSIZE|${GDAL_AVG_1T}s|${GDAL_AVG_NT}s|${MLX_AVG_AVG}s|$(speedup_label "$GDAL_AVG_1T" "$MLX_AVG_AVG")|$(speedup_label "$GDAL_AVG_NT" "$MLX_AVG_AVG")")

    echo "  -- BILINEAR --" >&2
    GDAL_BIL_1T=$(bench_tool "gdal bilinear (1 thread)" \
        "gdal_translate $INPUT $GDAL_OUT -of COG -co COMPRESS=LZW -co OVERVIEWS=AUTO -co OVERVIEW_RESAMPLING=BILINEAR")
    GDAL_BIL_NT=$(bench_tool "gdal bilinear (ALL_CPUS)" \
        "gdal_translate --config GDAL_NUM_THREADS ALL_CPUS $INPUT $GDAL_OUT -of COG -co COMPRESS=LZW -co OVERVIEWS=AUTO -co OVERVIEW_RESAMPLING=BILINEAR")
    MLX_AVG_BIL=$(bench_tool "mlx bilinear" \
        "$REPO_ROOT/build/mlx_translate $INPUT $MLX_OUT -r BILINEAR")
    rm -f "$GDAL_OUT" "$MLX_OUT"
    ROWS_BILINEAR+=("$NAME|$DIMS|$FSIZE|${GDAL_BIL_1T}s|${GDAL_BIL_NT}s|${MLX_AVG_BIL}s|$(speedup_label "$GDAL_BIL_1T" "$MLX_AVG_BIL")|$(speedup_label "$GDAL_BIL_NT" "$MLX_AVG_BIL")")
done

# ---------------------------------------------------------------------------
# Consolidated tables
# ---------------------------------------------------------------------------
print_table() {
    local title=$1; shift
    local rows=("$@")
    echo ""
    echo "=================================================================="
    echo "  $title"
    echo "=================================================================="
    printf "%-14s %-18s %-8s %-12s %-12s %-12s %-18s %-18s\n" \
        "Raster" "Dimensions" "Size" "GDAL 1T" "GDAL nT" "MLX" "vs GDAL 1T" "vs GDAL nT"
    printf "%-14s %-18s %-8s %-12s %-12s %-12s %-18s %-18s\n" \
        "------" "----------" "----" "-------" "-------" "---" "----------" "----------"
    for row in "${rows[@]}"; do
        IFS='|' read -r raster dims fsize gdal_1t gdal_nt mlx_avg speedup_1t speedup_nt <<< "$row"
        printf "%-14s %-18s %-8s %-12s %-12s %-12s %-18s %-18s\n" \
            "$(echo "$raster"     | xargs)" \
            "$(echo "$dims"       | xargs)" \
            "$(echo "$fsize"      | xargs)" \
            "$(echo "$gdal_1t"    | xargs)" \
            "$(echo "$gdal_nt"    | xargs)" \
            "$(echo "$mlx_avg"    | xargs)" \
            "$(echo "$speedup_1t" | xargs)" \
            "$(echo "$speedup_nt" | xargs)"
    done
    echo "=================================================================="
}

print_table "Results — AVERAGE"  "${ROWS_AVERAGE[@]}"
print_table "Results — BILINEAR" "${ROWS_BILINEAR[@]}"
