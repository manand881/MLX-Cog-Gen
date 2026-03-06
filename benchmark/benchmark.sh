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
while IFS= read -r f; do RASTERS+=("$f"); done < <(find "$DATA_DIR" -maxdepth 1 -name "*.tif" | sort)

if [ ${#RASTERS[@]} -eq 0 ]; then
    echo "No rasters found in $DATA_DIR -- run generate_test_data.sh first." >&2
    exit 1
fi

echo ""
echo "Found ${#RASTERS[@]} raster(s) to benchmark:"
for r in "${RASTERS[@]}"; do echo "  $r"; done

# Accumulate table rows: "raster|dims|gdal_avg|mlx_avg|speedup"
ROWS=()

for INPUT in "${RASTERS[@]}"; do
    NAME=$(basename "$INPUT" .tif)
    DIMS=$(gdalinfo "$INPUT" | awk '/^Size is/{gsub(",","",$3); gsub(",","",$4); print $3 "x" $4}')
    FSIZE=$(du -sh "$INPUT" | awk '{print $1}')

    echo ""
    echo "========================================"
    echo "  $NAME  ($DIMS px, $FSIZE)"
    echo "========================================"

    GDAL_AVG=$(bench_tool "gdal_translate" \
        "gdal_translate $INPUT $GDAL_OUT -of COG -co COMPRESS=LZW -co OVERVIEWS=AUTO -r average")

    MLX_AVG=$(bench_tool "mlx_translate" \
        "$REPO_ROOT/build/mlx_translate $INPUT $MLX_OUT")

    rm -f "$GDAL_OUT" "$MLX_OUT"

    speedup=$(echo "scale=2; $GDAL_AVG / $MLX_AVG" | bc)
    if (( $(echo "$speedup >= 1" | bc -l) )); then
        ratio="${speedup}x faster"
    else
        slower=$(echo "scale=2; $MLX_AVG / $GDAL_AVG" | bc)
        ratio="${slower}x slower"
    fi

    ROWS+=("$NAME|$DIMS|${GDAL_AVG}s|${MLX_AVG}s|$ratio")
done

# ---------------------------------------------------------------------------
# Consolidated table
# ---------------------------------------------------------------------------
echo ""
echo "========================================"
echo "  Results"
echo "========================================"
printf "%-20s %-18s %-12s %-12s %-16s\n" "Raster" "Dimensions" "GDAL avg" "MLX avg" "Speedup"
printf "%-20s %-18s %-12s %-12s %-16s\n" "------" "----------" "--------" "-------" "-------"
for row in "${ROWS[@]}"; do
    IFS='|' read -r raster dims gdal_avg mlx_avg speedup <<< "$row"
    printf "%-20s %-18s %-12s %-12s %-16s\n" \
        "$(echo "$raster" | xargs)" \
        "$(echo "$dims" | xargs)" \
        "$(echo "$gdal_avg" | xargs)" \
        "$(echo "$mlx_avg" | xargs)" \
        "$(echo "$speedup" | xargs)"
done
echo "========================================"
