# Known Issues

## Correctness

### NaN nodata pixels treated as valid data
`mx::equal(padded, NaN)` returns false for every pixel because `NaN != NaN` by IEEE 754.
NaN nodata pixels pass through as valid, NaN propagates through the average, and any
2×2 block containing a NaN nodata pixel produces NaN output instead of the average of
the remaining valid pixels.

GDAL uses an explicit `IsNaN()` check. We do not. NaN is a common nodata value for
Float32 DEMs. Any dataset authored with `nodata=nan` will produce corrupted overviews.

**Workaround:** convert the dataset to use a numeric nodata value (e.g. `-9999`) before
passing it to `mlx_translate`.

---

### BILINEAR nodata boundary mismatch vs GDAL
At nodata boundaries, our BILINEAR implementation disagrees with GDAL on which output
pixels should be nodata. In testing with a 960×960 circular raster (34% nodata), 883
output pixels at overview level 1 had a different nodata/valid classification compared
to GDAL BILINEAR, with max absolute error of 0.51 at those locations.

GDAL's bilinear nodata path uses a separable convolution (`GDALResampleChunk_ConvolutionT`)
where the horizontal pass produces a partially-valid intermediate before the vertical pass.
Our implementation uses a single 2D masked sum, which diverges from GDAL's result at
pixels where exactly one of the two source rows (or columns) is nodata.

AVERAGE is unaffected: 0 nodata location mismatches in the same test.

**Impact:** limited to pixels immediately adjacent to nodata boundaries; interior valid
pixels and interior nodata pixels are correct. Not observable in any current benchmark
raster since all have zero actual nodata pixels.

---

### nodata=0 with valid zero-valued pixels
If a raster uses `0` as its nodata value but also contains genuinely valid pixels at
elevation 0 (e.g. a DEM covering coastal terrain at sea level), both GDAL and
`mlx_translate` will incorrectly treat those valid zero pixels as nodata and exclude
them from the average.

This is a fundamental limitation of the nodata-value approach, not a bug in our code.
The correct fix is at the dataset level: DEMs should use a nodata value outside the
valid data range (convention is `-9999` or `-32768`). Neither implementation can recover
a dataset authored with this mistake.

---

### Float16 nodata quantization
Float16 precision near 10000 is ±8 (10-bit mantissa). `-9999` stored as Float16 rounds
to `-10000.0`. The nodata metadata still says `-9999.0`. Both GDAL and our implementation
compare the stored pixel value against the metadata value and find a mismatch; nodata
pixels are silently treated as valid data and corrupt the overview averages.

This affects any nodata value that cannot be exactly represented in Float16. Our rasters
are Float32 so this is not triggered by normal usage, but any Float16 input with a
non-power-of-two nodata value is affected.

---

## Accuracy

### Overview dimensions: ceil vs floor (1px per level)
`BuildOverviews("NONE", factors)` allocates IFD structures using `ceil(N/2)` for
overview dimensions. However, `gdal_translate -co OVERVIEWS=AUTO` (the COG driver path
we benchmark against) uses `floor(N/2)`. Our overview bands are therefore 1 pixel wider
and taller per level than GDAL's.

The level count matches exactly for all tested rasters. The geographic extent is
unaffected. The difference is ~0.01% extra work per level.

This cannot be fixed without bypassing `BuildOverviews` for structure allocation,
since we rely on it to create the TIFF IFDs.

---

## Limitations

### Full band must fit in GPU/unified memory
MLX loads the entire raster band into GPU-accessible unified memory before computing
overviews. There is no chunked processing path. For very large rasters on machines with
limited RAM, `mlx_translate` will fail or cause memory pressure.

GDAL's chunked model (`GDAL_SWATH_SIZE`) handles arbitrarily large rasters regardless
of available memory. On a 16 GB M1 Pro, the practical limit for `mlx_translate` is
rasters whose full-res band fits comfortably in memory alongside the OS working set
(verified up to ~928 MB / `dem_5cm`).
