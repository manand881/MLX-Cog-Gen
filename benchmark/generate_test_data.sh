#!/usr/bin/env bash
# Generate DEMs at multiple GSDs from points.csv via TIN (linear) interpolation.
# Uses only GDAL tools: gdal_grid.
#
# Input : benchmark/points.csv  (x=lon, y=lat, elevation columns, WGS84)
# Output: benchmark/data/dem_<gsd>.tif  (Float32, WGS84, LZW compressed)
#
# GSDs are expressed in degrees, converted from metric at lat ~13.
# 1 deg lat approx 111,320 m  ->  160cm, 80cm, 40cm, 20cm, 10cm, 5cm

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
POINTS_CSV="$SCRIPT_DIR/points.csv"
DATA_DIR="$SCRIPT_DIR/data"

mkdir -p "$DATA_DIR"

# ---------------------------------------------------------------------------
# Step 1: Write an OGR VRT so gdal_grid can read the CSV as a point layer
# ---------------------------------------------------------------------------
VRT="$DATA_DIR/points.vrt"
cat > "$VRT" << EOF
<OGRVRTDataSource>
  <OGRVRTLayer name="points">
    <SrcDataSource>$POINTS_CSV</SrcDataSource>
    <GeometryType>wkbPoint</GeometryType>
    <LayerSRS>EPSG:4326</LayerSRS>
    <GeometryField encoding="PointFromColumns" x="x" y="y"/>
  </OGRVRTLayer>
</OGRVRTDataSource>
EOF

# ---------------------------------------------------------------------------
# Step 2: Get extent from the VRT via ogrinfo
# ---------------------------------------------------------------------------
echo "Reading point extent..."
EXTENT_LINE=$(ogrinfo -al -so "$VRT" | grep "Extent:")
# Format: Extent: (xmin, ymin) - (xmax, ymax)
XMIN=$(echo "$EXTENT_LINE" | sed 's/Extent: (\(.*\), \(.*\)) - (\(.*\), \(.*\))/\1/')
YMIN=$(echo "$EXTENT_LINE" | sed 's/Extent: (\(.*\), \(.*\)) - (\(.*\), \(.*\))/\2/')
XMAX=$(echo "$EXTENT_LINE" | sed 's/Extent: (\(.*\), \(.*\)) - (\(.*\), \(.*\))/\3/')
YMAX=$(echo "$EXTENT_LINE" | sed 's/Extent: (\(.*\), \(.*\)) - (\(.*\), \(.*\))/\4/')
echo "  Extent: ($XMIN, $YMIN) - ($XMAX, $YMAX)"

# ---------------------------------------------------------------------------
# Step 3: Run gdal_grid (TIN / linear interpolation) at each GSD
#
#   -a linear:radius=-1   TIN interpolation; pixels outside convex hull → nodata
#   -zfield elevation     read Z from the CSV "elevation" column
#   -txe / -tye           explicit extent (required when -tr is used)
#   -tr <gsd> <gsd>       pixel size in degrees
# ---------------------------------------------------------------------------

# label and gsd_in_degrees pairs (1 deg lat approx 111320m at lat ~13)
LABELS=("160cm" "80cm" "40cm" "20cm" "10cm" "5cm" "2.5cm")
RESOLS=("0.0000143734" "0.0000071867" "0.0000035933" "0.0000017967" "0.0000008983" "0.0000004492" "0.0000002246")

echo ""
echo "========================================"
echo "  DEM generation -- TIN interpolation"
echo "========================================"

for i in "${!LABELS[@]}"; do
    label="${LABELS[$i]}"
    gsd="${RESOLS[$i]}"
    output="$DATA_DIR/dem_${label}.tif"

    echo ""
    echo "--- GSD: $label ($gsd deg) ---"

    if [ -f "$output" ]; then
        echo "  Skipping: $output already exists"
        continue
    fi

    gdal_grid \
        --config GDAL_NUM_THREADS ALL_CPUS \
        -a linear:radius=-1:nodata=-9999 \
        -zfield "elevation" \
        -txe "$XMIN" "$XMAX" \
        -tye "$YMIN" "$YMAX" \
        -tr "$gsd" "$gsd" \
        -of GTiff \
        -ot Float32 \
        -co COMPRESS=LZW \
        -co PREDICTOR=3 \
        "$VRT" "$output"

    dims=$(gdalinfo "$output" | awk '/^Size is/{print $3, $4}')
    fsize=$(du -sh "$output" | awk '{print $1}')
    echo "  Written : $output"
    echo "  Dims    : $dims pixels"
    echo "  On disk : $fsize"
done

echo ""
echo "========================================"
echo "  Verifying outputs"
echo "========================================"
printf "%-10s  %-18s  %-10s  %-10s  %s\n" "Label" "Dimensions" "Type" "NoData" "Size"
for i in "${!LABELS[@]}"; do
    label="${LABELS[$i]}"
    output="$DATA_DIR/dem_${label}.tif"

    if [ -f "$output" ]; then
        info=$(gdalinfo "$output")
        dims=$(echo "$info"   | awk '/^Size is/{w=$3; sub(/,/,"",w); printf "%sx%s", w, $4}')
        dtype=$(echo "$info"  | awk '/Type=/{sub(/.*Type=/,""); sub(/,.*/,""); print; exit}')
        nodata=$(echo "$info" | awk '/NoData Value=/{sub(/.*NoData Value=/,""); print; exit}')
        fsize=$(du -sh "$output" | awk '{print $1}')
        printf "%-10s  %-18s  %-10s  %-10s  %s\n" "$label" "$dims" "$dtype" "$nodata" "$fsize"
    else
        printf "%-10s  MISSING\n" "$label"
    fi
done

echo ""
echo "========================================"
echo "  Done"
echo "========================================"
