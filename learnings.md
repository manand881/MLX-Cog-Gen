# Learnings

## GDAL

- GDAL's internal source files (`gdalrasterband.cpp`, `gdaldefaultoverviews.cpp`) are not designed to be compiled standalone — they are part of `libgdal` and depend on hundreds of other internal files
- Copying individual GDAL source files into a project and compiling them against the installed `libgdal` causes duplicate symbol conflicts
- The correct way to extend GDAL behaviour is through its public C++ API, not by re-compiling its internals
- COG overview/pyramid generation in GDAL happens via `GDALDefaultOverviews::BuildOverviews()` → `GDALRegenerateOverviewsEx()` per band
- `GDALRegenerateOverviewsEx()` is **CPU-only** — uses SIMD (SSE2/AVX2 on x86, NEON via sse2neon on Apple Silicon). No GPU, no Metal, no CUDA. The Apple Silicon GPU is completely idle during GDAL overview generation
- GDAL computes overview sizes as `ceil(N/2)` — so `targetH * 2` can exceed source height by 1 for odd dimensions
- GDAL's AVERAGE resampling handles NoData by excluding NoData pixels from each block's average — our MLX implementation must do the same or elevation values get contaminated at NoData boundaries
- The COG driver accepts `OVERVIEWS=FORCE_USE_EXISTING` to use pre-built overviews rather than regenerating them — this is how we inject MLX-computed overviews into the COG pipeline

## gdal_translate -of COG Pipeline

1. **Open source file** — reads input `.tif` into a `GDALDataset`
2. **Create output dataset** — creates a GTiff with `LAYOUT=COG` creation option
3. **Copy raster data** — copies pixel data band by band into the output
4. **Build overviews** — calls `GDALDefaultOverviews::BuildOverviews()` → `GDALRegenerateOverviewsEx()` per band
5. **`GDALRegenerateOverviewsEx()`** — the actual CPU resampling step per band, produces each overview level
6. **Write COG structure** — tiles data and writes final file with overviews embedded

**Our replacement point is `GDALRegenerateOverviewsEx()`** — instead of calling it, we read band data into an MLX array, downsample iteratively per overview level on GPU, and write results back via GDAL's public API. Everything else stays as GDAL.

## Our Pipeline (mlx_translate)

1. Open source with GDAL
2. Create in-memory temp GTiff via `/vsimem/`
3. Call `BuildOverviews("NONE", ...)` on temp — allocates overview band structure with zero CPU compute (see NONE resampling note below)
4. Call `MLXBuildOverviews()` — fills overview bands with GPU-computed average downsampling
5. Create final COG from temp using COG driver with `OVERVIEWS=FORCE_USE_EXISTING`

## MLX API

- MLX 0.31.0 available via `brew install mlx` — provides C++ API for GPU-accelerated array ops on Apple Silicon
- Use `mx::default_stream(device)` not `mx::Stream(device)` to get a stream for a device
- `mx::mean()` requires `std::vector<int>` not an initializer list for axes
- `mx::slice()` takes `Shape` (`SmallVector<int>`) — use initializer lists `{start, ...}` not `std::vector<int>`
- MLX ops are lazy — nothing executes until `mx::eval()` is called
- Edge replication for odd dimensions: replicate last row/col before reshape+mean — matches GDAL's `ceil(N/2)` convention

## Resampling

- **GDAL's default for COG overview generation is NEAREST** — listed first in `-r nearest,bilinear,...` and used when no `-r` flag is passed
- NEAREST formula: `out[i,j] = src[2i, 2j]` — picks the top-left pixel of each 2×2 block, discards the other 3 (75% of data lost per level)
- AVERAGE formula: `out[i,j] = Σ valid_src_pixels / count_valid` — all pixels in the block contribute; NoData pixels excluded from sum and count
- NEAREST is correct for **categorical data** (land cover classes, labels) where averaging would create meaningless blended values
- AVERAGE is correct for **continuous data** (elevation, imagery, temperature) — NEAREST can make narrow features (a ridge, a river) disappear entirely at coarser levels depending on pixel alignment
- Our benchmark compares both tools using AVERAGE (`-r average` on `gdal_translate`) — an apples-to-apples comparison

## NoData Handling

- Without NoData masking, averaging `-9999` NoData pixels into real elevation values produces severely corrupted overview pixels (e.g. min dropping to -9842 instead of -4.93)
- Contamination compounds at each overview level because each level downsamples from the previous
- Fix: mask NoData pixels before averaging — zero them out, sum valid pixels only, divide by valid count, output NoData where all 4 pixels in a block are NoData
- Read NoData value per band via `poBand->GetNoDataValue(&hasNodata)` — always check the `hasNodata` flag before masking

## Benchmark

- `sample_dem.tif` lives in `tests/` — gitignored via `*.tif` but explicitly unignored via `!tests/sample_dem.tif`
- `build/` is gitignored — must be recreated on fresh clone

### Benchmark methodology notes

- `mlx_translate` copies the source into `/vsimem/` (RAM filesystem) — all reads hit RAM throughout
- `gdal_translate` reads from disk, but macOS's OS page cache keeps the file in RAM after the first read — subsequent runs confirm no disk variance
- Cannot compare speedup ratios across different benchmark sessions — system state, memory pressure, and page cache warmth all vary; only absolute times within the same session are meaningful

### MLX vs GDAL — architectural differences

- **Execution hardware**: GDAL uses CPU SIMD (NEON on Apple Silicon, single-threaded per band via `GDALResampleChunk_AverageOrRMS`). MLX dispatches to the Apple Silicon GPU via Metal — massively parallel
- **Memory access pattern**: GDAL processes in horizontal chunks/strips — reads a few rows at a time, writes them, repeats. Designed to handle rasters larger than RAM. MLX loads the entire band into GPU memory once, computes all levels, writes back — simpler but requires the full band to fit in memory
- **Overview chain**: both cascade identically — level N is sourced from level N-1, not from the original band. GDAL does this explicitly in `GDALRegenerateCascadingOverviews()` for AVERAGE; MLX does it via `current = downsampled`
- **Resampling math**: same 2×2 box filter, different form — GDAL is a pixel loop with SIMD intrinsics; MLX expresses it as `reshape([H, 2, W, 2])` + `mean([1, 3])` which the GPU executes as a single kernel
- **COG assembly**: identical — both use the same GDAL COG driver with `OVERVIEWS=FORCE_USE_EXISTING`
- **Key architectural constraint**: MLX requires the full band to fit in GPU/unified memory. GDAL's chunked model handles arbitrarily large rasters. This is the one real limitation of the MLX approach

### Overview structure allocation — NONE resampling

- `BuildOverviews("NONE", ...)` is a valid public API call — GDAL creates the TIFF IFD structures (overview band slots at correct dimensions) but immediately returns without computing any pixel data (`GDALRegenerateOverviewsEx` bails out at the `EQUAL(pszResampling, "NONE")` check in `overview.cpp:4816`)
- This replaces the previous NEAREST warmup pass — we used to call `BuildOverviews("NEAREST", ...)` just to allocate structure, which wasted a full CPU resample pass that was immediately overwritten by MLX
- Switching to NONE eliminated the wasted CPU pass entirely — MLX absolute times improved (~0.95s at 20cm, ~0.64s at 40cm, ~0.25s at 80cm)
- Cannot compare speedup ratios across different benchmark sessions — only absolute times within the same session are meaningful

### New methodology — multi-GSD synthetic DEMs

- Replaced the single fixed test file with dynamically generated DEMs at multiple GSDs (80cm, 40cm, 20cm) so benchmarks capture how speedup scales with raster size
- DEMs are generated from 5k random points via TIN interpolation (`gdal_grid -a linear`) — synthetic but realistic Float32 single-band rasters with NoData outside the convex hull
- **Key finding: speedup grows with raster size** — 1.25× at 80cm (~3.7k×3.6k), 1.42× at 40cm (~7.5k×7.3k), 1.57× at 20cm (~15k×14.5k). The GPU becomes increasingly efficient relative to CPU as the workload scales
- A single-raster benchmark at one scale is misleading — the 1.18× result from the old `sample_dem.tif` was real but not representative of larger inputs where the advantage is more pronounced

### gdal_grid notes

- `gdal_grid -a linear:radius=-1` performs TIN interpolation; `radius=-1` restricts output to the convex hull of the input points (pixels outside get nodata)
- `-txe` and `-tye` (explicit extent) are **required** when `-tr` (resolution) is used — gdal_grid errors without them
- GSD can be expressed in degrees when working in WGS84 — no need to reproject to a metric CRS just for raster generation; convert from metres using `1° lat ≈ 111,320 m`
- Generation time scales roughly with output pixel count: 5k points over a ~3km×3km area at 20cm GSD (~15k×15k pixels) takes ~11 minutes on M1 Pro
- OGR VRT is the correct way to make a CSV readable as a spatial layer by GDAL tools — specify `GeometryType`, `LayerSRS`, and `GeometryField` with `encoding="PointFromColumns"`

### Bash compatibility

- macOS ships with bash 3.2 — `mapfile` is not available; use `while IFS= read -r f; do arr+=("$f"); done < <(...)` instead
- Separate stdout and stderr in bench functions (`>&2` for progress, plain `echo` for the return value) to cleanly capture averages via command substitution
