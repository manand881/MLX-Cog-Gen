#!/usr/bin/env bash
set -euo pipefail

ITERATIONS=${1:-5}
INPUT="tests/sample_dem.tif"
GDAL_OUT="/tmp/benchmark_gdal_out.tif"
MLX_OUT="/tmp/benchmark_mlx_out.tif"

echo "========================================"
echo "  mlx-cog-gen benchmark"
echo "========================================"

# System info
CHIP=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || sysctl -n hw.model)
PHYSICAL_CORES=$(sysctl -n hw.physicalcpu)
LOGICAL_CORES=$(sysctl -n hw.logicalcpu)
TOTAL_RAM_BYTES=$(sysctl -n hw.memsize)
TOTAL_RAM_GB=$(echo "scale=1; $TOTAL_RAM_BYTES / 1073741824" | bc)
GPU=$(system_profiler SPDisplaysDataType 2>/dev/null | awk -F': ' '/Chipset Model/{print $2; exit}')
PAGE_SIZE=$(vm_stat | awk '/page size of/{gsub(/[^0-9]/,"",$0); print}')
FREE_PAGES=$(vm_stat | awk '/Pages free/{gsub(/[^0-9.]/,"",$3); gsub(/\./,"",$3); print $3}')
AVAIL_RAM_GB=$(echo "scale=1; $FREE_PAGES * $PAGE_SIZE / 1073741824" | bc | awk '{printf "%.1f", $1}')
GDAL_VERSION=$(gdal-config --version)
MLX_VERSION=$(brew list mlx --versions | awk '{print $2}')

echo "  Chip         : $CHIP"
echo "  Cores        : $PHYSICAL_CORES physical / $LOGICAL_CORES logical"
echo "  GPU          : $GPU"
echo "  Total RAM    : ${TOTAL_RAM_GB}GB"
echo "  Available RAM: ${AVAIL_RAM_GB}GB"
echo "  GDAL version : $GDAL_VERSION"
echo "  MLX version  : $MLX_VERSION"
echo "  Input        : $INPUT"
echo "  Runs         : $ITERATIONS"
echo "========================================"

run_benchmark() {
    local cmd_name=$1
    local cmd=$2
    local output=$3
    local times=()

    echo ""
    echo "--- $cmd_name ---"
    for i in $(seq 1 "$ITERATIONS"); do
        elapsed=$( { time eval "$cmd" > /dev/null 2>&1; } 2>&1 | grep real | awk '{print $2}' )
        # Convert mXs format to seconds
        secs=$(echo "$elapsed" | awk -F'm' '{printf "%.3f", $1*60 + $2}' | tr -d 's')
        times+=("$secs")
        printf "  Run %d: %ss\n" "$i" "$secs"
    done

    # Compute min, max, avg
    local min max avg sum
    min=${times[0]}; max=${times[0]}; sum=0
    for t in "${times[@]}"; do
        sum=$(echo "$sum + $t" | bc)
        min=$(echo "if ($t < $min) $t else $min" | bc)
        max=$(echo "if ($t > $max) $t else $max" | bc)
    done
    avg=$(echo "scale=3; $sum / $ITERATIONS" | bc)

    echo "  ----------------------------------------"
    printf "  min: %ss  max: %ss  avg: %ss\n" "$min" "$max" "$avg"

    # Export for comparison
    eval "${cmd_name//-/_}_avg=$avg"
}

run_benchmark "gdal_translate" \
    "gdal_translate $INPUT $GDAL_OUT -of COG -co COMPRESS=LZW -co OVERVIEWS=AUTO" \
    "$GDAL_OUT"

run_benchmark "mlx_translate" \
    "build/mlx_translate $INPUT $MLX_OUT" \
    "$MLX_OUT"

echo ""
echo "========================================"
echo "  Summary"
echo "========================================"
printf "  gdal_translate avg: %ss\n" "$gdal_translate_avg"
printf "  mlx_translate  avg: %ss\n" "$mlx_translate_avg"

speedup=$(echo "scale=2; $gdal_translate_avg / $mlx_translate_avg" | bc)
if (( $(echo "$speedup >= 1" | bc -l) )); then
    printf "  mlx_translate is %sx faster\n" "$speedup"
else
    slower=$(echo "scale=2; $mlx_translate_avg / $gdal_translate_avg" | bc)
    printf "  mlx_translate is %sx slower\n" "$slower"
fi
echo "========================================"

rm -f "$GDAL_OUT" "$MLX_OUT"
