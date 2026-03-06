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
3. Call `BuildOverviews("NEAREST", ...)` on temp — allocates overview band structure (fast CPU, placeholder data)
4. Call `MLXBuildOverviews()` — overwrites overview bands with GPU-computed average downsampling
5. Create final COG from temp using COG driver with `OVERVIEWS=FORCE_USE_EXISTING`

## MLX API

- MLX 0.31.0 available via `brew install mlx` — provides C++ API for GPU-accelerated array ops on Apple Silicon
- Use `mx::default_stream(device)` not `mx::Stream(device)` to get a stream for a device
- `mx::mean()` requires `std::vector<int>` not an initializer list for axes
- `mx::slice()` takes `Shape` (`SmallVector<int>`) — use initializer lists `{start, ...}` not `std::vector<int>`
- MLX ops are lazy — nothing executes until `mx::eval()` is called
- Edge replication for odd dimensions: replicate last row/col before reshape+mean — matches GDAL's `ceil(N/2)` convention

## NoData Handling

- Without NoData masking, averaging `-9999` NoData pixels into real elevation values produces severely corrupted overview pixels (e.g. min dropping to -9842 instead of -4.93)
- Contamination compounds at each overview level because each level downsamples from the previous
- Fix: mask NoData pixels before averaging — zero them out, sum valid pixels only, divide by valid count, output NoData where all 4 pixels in a block are NoData
- Read NoData value per band via `poBand->GetNoDataValue(&hasNodata)` — always check the `hasNodata` flag before masking

## Benchmark

- `sample_dem.tif` lives in `tests/` — gitignored via `*.tif` but explicitly unignored via `!tests/sample_dem.tif`
- `build/` is gitignored — must be recreated on fresh clone
- The 2-pass approach (NEAREST to allocate structure, then MLX to overwrite) adds overhead — real gains will be larger on bigger rasters where overview computation dominates I/O time

### Results (sample_dem.tif, M1 Pro, 5 runs)

| tool | min | max | avg |
|---|---|---|---|
| `gdal_translate` | 1.774s | 3.025s | 2.028s |
| `mlx_translate` | 1.468s | 2.133s | 1.608s |
