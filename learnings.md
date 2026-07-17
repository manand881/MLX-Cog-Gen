# Learnings

## GDAL

- GDAL's internal source files (`gdalrasterband.cpp`, `gdaldefaultoverviews.cpp`) are not designed to be compiled standalone; they are part of `libgdal` and depend on hundreds of other internal files
- Copying individual GDAL source files into a project and compiling them against the installed `libgdal` causes duplicate symbol conflicts
- The correct way to extend GDAL behaviour is through its public C++ API, not by re-compiling its internals
- COG overview/pyramid generation in GDAL happens via `GDALDefaultOverviews::BuildOverviews()` → `GDALRegenerateOverviewsEx()` per band
- `GDALRegenerateOverviewsEx()` is **CPU-only**: uses SIMD (SSE2/AVX2 on x86, NEON via sse2neon on Apple Silicon). No GPU, no Metal, no CUDA. The Apple Silicon GPU is completely idle during GDAL overview generation
- `BuildOverviews()` computes overview sizes as `ceil(N/2)`. However, `gdal_translate -co OVERVIEWS=AUTO` (the COG driver path) uses `floor(N/2)`. These are different internal code paths in GDAL. Our pipeline uses `BuildOverviews` so our overview bands are 1 pixel larger per dimension than what `gdal_translate` produces for odd-sized rasters. The level COUNT matches exactly. The geographic extent is unaffected.
- GDAL's AVERAGE resampling handles NoData by excluding NoData pixels from each block's average; our MLX implementation must do the same or elevation values get contaminated at NoData boundaries
- The COG driver accepts `OVERVIEWS=FORCE_USE_EXISTING` to use pre-built overviews rather than regenerating them; this is how we inject MLX-computed overviews into the COG pipeline

## gdal_translate -of COG Pipeline

1. **Open source file**: reads input `.tif` into a `GDALDataset`
2. **Create output dataset**: creates a GTiff with `LAYOUT=COG` creation option
3. **Copy raster data**: copies pixel data band by band into the output
4. **Build overviews**: calls `GDALDefaultOverviews::BuildOverviews()` → `GDALRegenerateOverviewsEx()` per band
5. **`GDALRegenerateOverviewsEx()`**: the actual CPU resampling step per band, produces each overview level
6. **Write COG structure**: tiles data and writes final file with overviews embedded

**Our replacement point is `GDALRegenerateOverviewsEx()`**: instead of calling it, we read band data into an MLX array, downsample iteratively per overview level on GPU, and write results back via GDAL's public API. Everything else stays as GDAL.

## Our Pipeline (mlx_translate)

1. Open source with GDAL
2. Create in-memory temp GTiff via `/vsimem/`
3. Call `BuildOverviews("NONE", ...)` on temp: allocates overview band structure with zero CPU compute (see NONE resampling note below)
4. Call `MLXBuildOverviews()`: fills overview bands with GPU-computed downsampling (AVERAGE or BILINEAR)
5. Create final COG from temp using COG driver with `OVERVIEWS=FORCE_USE_EXISTING`

## MLX API

- MLX available via `brew install mlx` (verified 0.32.0 on 2026-07-17), provides C++ API for GPU-accelerated array ops on Apple Silicon
- Use `mx::default_stream(device)` not `mx::Stream(device)` to get a stream for a device
- `mx::mean()` requires `std::vector<int>` not an initializer list for axes
- `mx::slice()` takes `Shape` (`SmallVector<int>`); use initializer lists `{start, ...}` not `std::vector<int>`
- MLX ops are lazy; nothing executes until `mx::eval()` is called
- Edge replication for odd dimensions: replicate last row/col before reshape+mean, matching GDAL's `BuildOverviews` ceil(N/2) convention

## Resampling

- **GDAL's default for COG overview generation is NEAREST**: listed first in `-r nearest,bilinear,...` and used when no `-r` flag is passed
- NEAREST formula: `out[i,j] = src[2i, 2j]`, picks the top-left pixel of each 2×2 block and discards the other 3 (75% of data lost per level)
- AVERAGE formula: `out[i,j] = Σ valid_src_pixels / count_valid`, all pixels in the block contribute; NoData pixels excluded from sum and count
- NEAREST is correct for **categorical data** (land cover classes, labels) where averaging would create meaningless blended values
- AVERAGE is correct for **continuous data** (elevation, imagery, temperature). NEAREST can make narrow features (a ridge, a river) disappear entirely at coarser levels depending on pixel alignment
- BILINEAR uses a separable tent filter: for each output pixel i, the sample point in source space is `(i + 0.5) * 2 - 0.5 = 2i + 0.5`, which lands halfway between source pixels 2i and 2i+1. The tent filter assigns weight 0.5 to each. Applied as two 1D passes (horizontal then vertical). For strict 2x downsampling with no NoData, this is numerically identical to AVERAGE because the 0.5/0.5 weights produce the same 2x2 mean. They diverge at NoData boundaries and for non-2x factors.
- GDAL's bilinear overview generation goes through the convolution code path (`GDALResampleChunk_ConvolutionT`), which is a more general implementation than the AVERAGE path (`GDALResampleChunk_AverageOrRMS`). On multi-GSD benches (including dem_5cm), MLX AVERAGE and BILINEAR wall times stay nearly identical because the GPU parallelises both uniformly.
- Our benchmark compares both tools using both AVERAGE and BILINEAR, each method run independently against its GDAL equivalent

## NoData Handling

- Without NoData masking, averaging `-9999` NoData pixels into real elevation values produces severely corrupted overview pixels (e.g. min dropping to -9842 instead of -4.93)
- Contamination compounds at each overview level because each level downsamples from the previous
- Fix: mask NoData pixels before averaging; zero them out, sum valid pixels only, divide by valid count, output NoData where all 4 pixels in a block are NoData
- Read NoData value per band via `poBand->GetNoDataValue(&hasNodata)`; always check the `hasNodata` flag before masking
- **NaN no-data substitution (2026-04-15):** Implemented NaN substitution strategy instead of rejection. When NaN is detected as the nodata value, the implementation:
  1. Detects NaN nodata using `std::isnan()` on the GDAL nodata value
  2. Internally uses -9999 as the working nodata value for MLX operations
  3. Substitutes NaN pixels with -9999 on CPU after RasterIO (before creating MLX array)
  4. Performs all downsampling operations with -9999 as nodata
  5. Restores -9999 back to NaN on CPU before writing to GDAL
  6. Sets output band nodata metadata to NaN via `SetNoDataValue()`
  This approach works around MLX's inability to detect NaN with `mx::equal()` while maintaining full compatibility with NaN nodata datasets. CPU-based substitution is used because MLX may not have `isnan()` support or it may cause segmentation faults.

**Test results (live 2026-07-17):** `sample_dem_nan_nodata.tif` (4772x5125, NaN nodata). Dev = `|MLX − GDAL| / |GDAL|` on min/max/mean/stddev. Comparison is MLX (NaN→-9999 path) vs GDAL native NaN convolution, not GDAL-with--9999.

| Method    | Level      | min dev | max dev | mean dev | stddev dev | Status |
|-----------|------------|---------|---------|----------|------------|--------|
| AVERAGE   | Full res   | 0.00%   | 0.00%   | 0.00%    | 0.00%      | PASS   |
| AVERAGE   | Overview 1 | 0.02%   | 0.04%   | 0.00%    | 0.00%      | PASS   |
| AVERAGE   | Overview 2 | 0.06%   | 0.02%   | 0.00%    | 0.00%      | PASS   |
| AVERAGE   | Overview 3 | 0.10%   | 0.53%   | 0.00%    | 0.00%      | PASS   |
| AVERAGE   | Overview 4 | 0.25%   | 1.05%   | 1.46%    | 0.00%      | PASS   |
| BILINEAR  | Full res   | 0.00%   | 0.00%   | 0.00%    | 0.00%      | PASS   |
| BILINEAR  | Overview 1 | 0.14%   | 0.14%   | 0.75%    | 0.00%      | PASS   |
| BILINEAR  | Overview 2 | 0.41%   | 0.22%   | 1.47%    | 0.05%      | PASS   |
| BILINEAR  | Overview 3 | 0.57%   | 0.02%   | 2.88%    | 0.10%      | PASS   |
| BILINEAR  | Overview 4 | 1.52%   | 0.17%   | 1.42%    | 0.43%      | PASS   |

- Worst AVERAGE: ~1.5% (mean @ ovr 4). Worst BILINEAR: ~2.9% (mean @ ovr 3).
- Gates in `check_nan_nodata.cpp`: **5% AVERAGE and BILINEAR** (`AVG_TOLERANCE` / `BIL_TOLERANCE`). BILINEAR gate tightened from 15% → 5% on 2026-07-17 after live remeasure (old table had stale BILINEAR mean up to 16.81% and is obsolete).
- **Float16 nodata quantization issue**: Float16 precision near 10000 is ±8 (exponent 13, 10-bit mantissa). `-9999` stored as Float16 rounds to `-10000.0`. The nodata metadata still says `-9999.0`. Both GDAL and our implementation compare the stored pixel value against the metadata value and find a mismatch; nodata pixels are silently treated as valid data. This is not specific to our code; GDAL has the same problem. Confirmed experimentally: GDAL overview min drops to -10560 (contamination from nodata averaging), while MLX min stays at -10000 (contamination present but bounded since all nodata pixels have the same quantised value). This is the same class of failure as nodata=0; any nodata value that cannot be exactly represented in Float16 triggers it.

## Benchmark

- `sample_dem.tif` lives in `tests/`; gitignored via `*.tif` but explicitly unignored via `!tests/sample_dem.tif`. Current file has 1.9% nodata coverage. A better test file with 22.24% nodata (128MB, exceeds GitHub size limit) is available locally and validates nodata handling correctly at higher nodata percentages.
- `build/` is gitignored; must be recreated on fresh clone

### Benchmark methodology notes

- `mlx_translate` copies the source into `/vsimem/` (RAM filesystem); all reads hit RAM throughout
- `gdal_translate` reads from disk, but macOS's OS page cache keeps the file in RAM after the first read; subsequent runs confirm no disk variance
- Cannot compare speedup ratios across different benchmark sessions; system state, memory pressure, and page cache warmth all vary; only absolute times within the same session are meaningful

### GDAL performance flags: the full map

`gdal_translate -of COG` performance is controlled by more than just `GDAL_NUM_THREADS`. The benchmark currently only controls threading. The following flags can all shift GDAL wall-clock time and must be understood before drawing conclusions about what "maximum credible GDAL" looks like.

**Threading (currently benchmarked)**
- `GDAL_NUM_THREADS` (`--config`): default `1`. Parallelises **both** overview downsampling computation **and** LZW/DEFLATE tile compression. `ALL_CPUS` enables all cores. Supported since GDAL 3.2 for overview generation; LZW compression also benefits. This is the single largest lever.
- `NUM_THREADS` (`-co`): COG-driver-specific form of the same control. Canonical for COG creation. Both `--config GDAL_NUM_THREADS` and `-co NUM_THREADS` are respected; in benchmarks we use the `--config` form.

**Memory and block cache (not currently benchmarked)**
- `GDAL_CACHEMAX` (`--config`): default 5% of RAM (~800MB on 16GB). Controls how many decoded source blocks GDAL keeps in RAM during overview generation. At large raster sizes, a small cache causes blocks to be evicted and re-read for each successive overview level. Setting `4096MB` or higher could measurably reduce I/O during multi-level overview generation.
- `GDAL_SWATH_SIZE` (`--config`): defaults to `GDAL_CACHEMAX / 4`. Controls the in-flight transfer buffer when GDAL copies pixels between datasets (used during the final COG assembly step). Automatically scales with GDAL_CACHEMAX.
- `GDAL_BAND_BLOCK_CACHE` (`--config`): default `AUTO`. `ARRAY` mode is faster for typical rasters (direct array indexing vs. hash lookup). AUTO chooses based on block count and usually picks ARRAY.
- `VSI_CACHE` (`--config`): default `FALSE`. Set to `TRUE` to add a per-file-handle RAM read-ahead cache on top of the OS page cache. Useful when the same source blocks are read repeatedly across overview levels.
- `VSI_CACHE_SIZE` (`--config`): default 25MB. Size of per-file VSI cache. Only effective when `VSI_CACHE=TRUE`.

**Compression (not currently benchmarked)**
- `PREDICTOR` (`-co`): default `NO` (= 1, no predictor). Setting `2` (horizontal differencing) before LZW reduces input entropy so the compressor does less work. Standard recommendation for integer DEM data; can improve LZW speed by 10–30% while also improving compression ratio. Setting `3` (floating-point differencing) is appropriate for Float32 data. LZW has no configurable level; PREDICTOR is the only way to tune LZW performance.
- `LEVEL` (`-co`): controls effort for DEFLATE (1–12, default 6) and ZSTD (1–22, default 9). Level 1 is maximally fast. Has no effect on LZW.
- `OVERVIEW_COMPRESS` / `OVERVIEW_PREDICTOR` (`-co`): same controls applied independently to overview tiles.

**I/O path (minor, situational)**
- `GTIFF_VIRTUAL_MEM_IO` (`--config`): default `NO`. Set to `YES` or `IF_ENOUGH_RAM` to use `mmap()` instead of GDAL's block cache for reading uncompressed source TIFFs. Can reduce source-read time on large uncompressed inputs by delegating memory management to the OS. No effect on compressed sources.
- `GTIFF_DIRECT_IO` (`--config`): default `NO`. Bypasses block cache for uncompressed TIFF reads. Lower priority than `GTIFF_VIRTUAL_MEM_IO`; only effective when mmap is not in use.
- `GDAL_DISABLE_READDIR_ON_OPEN` (`--config`): default `FALSE`. Set to `EMPTY_DIR` to skip scanning the source directory for `.aux`/`.ovr`/`.tfw` sidecar files on every `GDALOpen()`. Eliminates a `readdir` syscall per benchmark iteration.

**What the benchmark currently covers vs. does not cover**
- Covered: `GDAL_NUM_THREADS` = 1 and `ALL_CPUS`; `COMPRESS=LZW`; `OVERVIEWS=AUTO`; default tile size (512)
- Not covered: `GDAL_CACHEMAX`, `GDAL_SWATH_SIZE`, `PREDICTOR`, `GTIFF_VIRTUAL_MEM_IO`, `VSI_CACHE`
- The "true maximum GDAL" baseline has not been established. Before concluding that MLX underperforms or that any fix closes the gap, the impact of `GDAL_CACHEMAX` and `PREDICTOR=2` on the GDAL ALL_CPUS numbers must be measured.

### MLX vs GDAL: architectural differences

- **Execution hardware**: GDAL uses CPU SIMD (NEON on Apple Silicon) via `GDALResampleChunk_AverageOrRMS`. The resampling runs in a job queue (`OvrJob` in `overview.cpp`) that is **single-threaded by default** (`GDAL_NUM_THREADS` defaults to `"1"`). Setting `GDAL_NUM_THREADS=ALL_CPUS` parallelises the resampling chunks across CPU cores; this has been supported since GDAL 3.2. MLX dispatches to the Apple Silicon GPU via Metal, massively parallel.
- **Memory access pattern**: GDAL processes in horizontal chunks/strips; reads a few rows at a time, writes them, repeats. Designed to handle rasters larger than RAM. MLX loads the entire band into GPU memory once, computes all levels, writes back; simpler but requires the full band to fit in memory.
- **Overview chain**: both cascade identically; level N is sourced from level N-1, not from the original band. GDAL does this explicitly in `GDALRegenerateCascadingOverviews()` for AVERAGE; MLX does it via `current = downsampled`.
- **Resampling math (AVERAGE)**: same 2×2 box filter, different form. GDAL is a pixel loop with SIMD intrinsics; MLX expresses it as `reshape([H, 2, W, 2])` + `mean([1, 3])` which the GPU executes as a single kernel. **BILINEAR**: GDAL uses a separable convolution with a tent filter kernel via `GDALResampleChunk_ConvolutionT`; MLX implements it as two sequential reshape+mean passes (horizontal then vertical), which is structurally equivalent and numerically identical at 2x.
- **COG assembly**: identical; both use the same GDAL COG driver with `OVERVIEWS=FORCE_USE_EXISTING`.
- **Key architectural constraint**: MLX requires the full band to fit in GPU/unified memory. GDAL's chunked model handles arbitrarily large rasters. This is the one real limitation of the MLX approach.

### Overview structure allocation: NONE resampling

- `BuildOverviews("NONE", ...)` is a valid public API call; GDAL creates the TIFF IFD structures (overview band slots at correct dimensions) but immediately returns without computing any pixel data (`GDALRegenerateOverviewsEx` bails out at the `EQUAL(pszResampling, "NONE")` check in `overview.cpp:4816`)
- This replaces the previous NEAREST warmup pass; we used to call `BuildOverviews("NEAREST", ...)` just to allocate structure, which wasted a full CPU resample pass that was immediately overwritten by MLX
- Switching to NONE eliminated the wasted CPU pass entirely; MLX absolute times improved (measured in a prior benchmark session, exact deltas no longer verifiable against current data)

### New methodology: multi-GSD synthetic DEMs

- Replaced the single fixed test file with dynamically generated DEMs at multiple GSDs (80cm, 40cm, 20cm) so benchmarks capture how speedup scales with raster size
- DEMs are generated from 5k random points via TIN interpolation (`gdal_grid -a linear`); synthetic but realistic Float32 single-band rasters with NoData outside the convex hull
- **Project goal: beat GDAL ALL_CPUS (nT), not single-threaded GDAL.** Single-threaded GDAL is not a meaningful target; any real user will invoke GDAL with `GDAL_NUM_THREADS=ALL_CPUS`. The relevant speedup column is vs GDAL nT.
- MLX is slower than GDAL nT at small rasters where Metal kernel launch overhead dominates over GPU compute time; it wins at large rasters where GPU parallelism overwhelms CPU core count. The crossover point shifts with optimisations (see README for current numbers).
- Optimising the small-raster regime is the primary open problem. The dominant fixed cost is Metal framework overhead (kernel launch, command buffer submission) plus GDAL I/O setup, not GPU compute time.
- A single-raster benchmark at one scale is misleading; behaviour must be measured across raster sizes

### Batched eval experiment (2026-03-09): no improvement

Tried building the full downsample chain as a lazy graph and calling `mx::eval(all_levels)` once instead of `mx::eval` per level. Result on dem_160cm/80cm/40cm: **no measurable improvement** (differences within 5ms, run-to-run noise). Conclusion: per-level `eval()` fences were not the bottleneck; memory bandwidth and Metal/GDAL fixed costs dominate.

**Current code (2026-07-17):** still **per-level** `mx::eval(downsampled)` inside the overview loop in `MLXBuildOverviews` (needed before CPU NaN restore / RasterIO write). Batched-eval is historical only, not production structure.

### Where performance is still left on the table

**Easy wins:**
- **ZSTD default instead of LZW**: ZSTD compresses 2–3× faster than LZW at similar ratio; supported since GDAL 2.3. One-line default change. Directly reduces COG write time, which is the second-largest cost at large rasters.
- **PREDICTOR=3 for Float32**: reduces LZW/DEFLATE input entropy before compression. Standard for Float32 rasters. One creation-option change. Makes compression faster AND improves ratio.

**Medium effort:**
- **Eliminate the /vsimem double-pass**: currently the pipeline writes all overview data into /vsimem, then the COG driver reads it all back to tile+compress. Every pixel (full-res + overviews ≈ 4/3 × band size) is touched twice for the write path alone. Eliminating this requires structuring the pipeline so the COG driver reads directly from the MLX output without an intermediate GTiff copy.
- **Multi-band GPU parallelism**: bands are processed serially. For multi-band rasters (RGB, multispectral), all bands could be batched as a `[bands, H, W]` array and downsampled simultaneously. No benefit for single-band DEMs (current test data).

**Large effort / diminishing returns:**
- **GPU-accelerated tiling**: the COG write step tiles into 512×512 blocks and reorders them (pure data movement that the GPU could do faster). But using GPU-tiled output requires implementing the TIFF/COG file structure (IFDs, tile offsets) manually, essentially replacing the COG driver.
- **Avoid CPU copy on overview write**: read path already uses `mx::allocator::malloc` so RasterIO fills MLX-owned memory. Write path still copies into a `std::vector<float>` when restoring NaN nodata before RasterIO. Numeric-nodata could write from `downsampled.data<float>()` directly; NaN path needs the copy (or an in-place restore if MLX buffer is writable after eval).

### gdal_grid notes

- `gdal_grid -a linear:radius=-1` performs TIN interpolation; `radius=-1` restricts output to the convex hull of the input points (pixels outside get nodata)
- `-txe` and `-tye` (explicit extent) are **required** when `-tr` (resolution) is used; gdal_grid errors without them
- GSD can be expressed in degrees when working in WGS84; no need to reproject to a metric CRS just for raster generation; convert from metres using `1 deg lat approx 111,320 m`
- Generation time scales roughly with output pixel count: 5k points over a ~3km×3km area at 20cm GSD (~15k×15k pixels) takes ~11 minutes on M1 Pro single-threaded; ~6 minutes with `--config GDAL_NUM_THREADS ALL_CPUS`
- `gdal_grid` does not accept `-multi`; multithreading is enabled via `--config GDAL_NUM_THREADS ALL_CPUS` passed as the first argument. Uniform point distributions benefit most since all threads get equal work.
- OGR VRT is the correct way to make a CSV readable as a spatial layer by GDAL tools; specify `GeometryType`, `LayerSRS`, and `GeometryField` with `encoding="PointFromColumns"`

### Bash compatibility

- macOS ships with bash 3.2, so `mapfile` is not available; use `while IFS= read -r f; do arr+=("$f"); done < <(...)` instead
- Separate stdout and stderr in bench functions (`>&2` for progress, plain `echo` for the return value) to cleanly capture averages via command substitution

## Bilinear Implementation: MLX vs GDAL

### GDAL's Bilinear Implementation (GDALResampleChunk_ConvolutionT)

Location: `gcore/overview.cpp` (`GDALResampleChunk_ConvolutionT`; line numbers shift by GDAL version)

**Architecture:**
- Two-pass separable convolution: horizontal filter first, then vertical filter
- General-purpose implementation supporting arbitrary downsample factors (not just 2x)
- Supports kernels with negative weights (Cubic, Lanczos) via `bKernelWithNegativeWeights` template parameter
- Kernel radius for bilinear: 1 (tent filter spans 2 pixels in each dimension)

**Horizontal Pass (lines 3550-3820):**
- For each output pixel `iDstPixel`, compute source position:
  ```cpp
  dfSrcPixel = (iDstPixel + 0.5) * dfXRatioDstToSrc + dfSrcXDelta
  ```
  For 2x downsampling: `dfXRatioDstToSrc = 2.0`, `dfSrcXDelta = -0.5`
  Result: `dfSrcPixel = 2 * iDstPixel + 0.5` (halfway between source pixels)
- Determine source pixel range: `[dfSrcPixel - dfXScaledRadius, dfSrcPixel + dfXScaledRadius]`
  For 2x bilinear: radius = 1, scaled radius = 2, so samples from 4 source pixels
- Compute kernel weights for each source pixel using tent filter: `weight = 1 - |x|` where `x` is distance from sample point
- Normalize weights so they sum to 1.0 (unless sum is 0)
- **NoData path:** Uses `GDALResampleConvolutionHorizontalWithMask` (line 2774):
  - Multiplies each weight by the mask value (0 for NoData, 1 for valid)
  - Accumulates `dfVal += pixel * weight` and `dfWeightSum += weight`
  - After pass: if `dfWeightSum > 0`, output `dfVal / dfWeightSum`, else NoData
  - For kernels with negative weights, checks for consecutive valid pixels; if < 50% consecutive, rejects entire output as NoData
- Stores intermediate result in `padfHorizontalFiltered` array
- Also computes `pabyChunkNodataMaskHorizontalFiltered` (1 if any valid weight survived, else 0)

**Vertical Pass (lines 3830-4100):**
- Same logic as horizontal pass but applied vertically
- Operates on `padfHorizontalFiltered` from the horizontal pass
- Uses `pabyChunkNodataMaskHorizontalFiltered` as the mask
- For each output line, computes source line position and kernel weights
- **NoData path:** Same masked weighted sum approach as horizontal
- Final output written to destination buffer

**Key GDAL characteristics:**
- Separable convolution: horizontal and vertical passes are independent
- 2D weight accumulation in NoData path: the horizontal pass produces a mask that the vertical pass uses, so the final 2D weight distribution is the product of 1D weights
- Boundary handling: clamps kernel to valid source extent (no extrapolation)
- Weight normalization: weights always normalized to sum to 1.0 (unless all masked out)

### MLX's Bilinear Implementation (mlx_downsample_bilinear)

Location: `src/mlx_overviews.cpp` (`mlx_downsample_bilinear`)

**No-NoData path:**
- Two-pass separable convolution using reshape+mean
- Horizontal: reshape `[pH, pW] → [pH, targetW, 2]`, mean over axis 2
- Vertical: reshape `[pH, targetW] → [targetH, 2, targetW]`, mean over axis 1
- At 2x with no NoData, tent weights 0.5/0.5 match reshape+mean exactly
- Odd dims: pad by replicating last row/col (simulates GDAL kernel clamp)

**NoData path (current, post 2026-04-15 fix):**
- Separable masked convolution matching GDAL's `GDALResampleChunk_ConvolutionT`
- Horizontal: reshape `[pH, targetW, 2]`, weight 0.5, mask, `dataSum/weightSum` if weightSum > 0 else 0; intermediate validity mask
- Vertical: same on intermediate using intermediate mask; final 2D weights = product of 1D weights
- Output NoData where no valid weight survived either pass

### Historical: pre-fix 2D NoData path (obsolete)

Before 2026-04-15, the NoData path used a **2D box filter** (reshape to 2×2 blocks, uniform mean of valid pixels). That diverged from GDAL's separable tent product at nodata boundaries (example: single valid pixel in 2×2 got weight 1.0 in MLX vs 0.25 in GDAL; max abs error ~0.51 on a circular nodata test). That analysis and the "2D weighting" docs are **historical only**.

### Current key differences vs GDAL

1. **NoData strategy:** both separable masked convolution (aligned after fix)
2. **Boundary:** GDAL clamps kernel; MLX replicates last row/col (same effect for no-NoData 2x)
3. **Generality:** GDAL arbitrary factors/kernels; MLX 2x only (enough for COG power-of-2 overviews)
4. **No NoData, 2x:** mathematically equivalent
5. **Residual divergence:** FP order (CPU vs GPU), 1px ceil/floor overview dim drift vs COG AUTO, and NaN-nodata vs GDAL native NaN path (see NaN table above)

**Numeric-nodata COG stats** (`check_cog_statistics`, live reconfirmed 2026-07-17): gate **3%**. Max observed **2.48%** (BILINEAR ovr 3 mean).

| Method    | Level      | min dev | max dev | mean dev | stddev dev |
|-----------|------------|---------|---------|----------|------------|
| AVERAGE   | Full res   | 0.00%   | 0.00%   | 0.00%    | 0.00%      |
| AVERAGE   | Overview 1 | 0.01%   | 0.08%   | 0.19%    | 0.01%      |
| AVERAGE   | Overview 2 | 0.04%   | 0.03%   | 0.18%    | 0.01%      |
| AVERAGE   | Overview 3 | 0.33%   | 0.35%   | 2.02%    | 0.01%      |
| AVERAGE   | Overview 4 | 0.20%   | 0.73%   | 1.12%    | 0.02%      |
| BILINEAR  | Full res   | 0.00%   | 0.00%   | 0.00%    | 0.00%      |
| BILINEAR  | Overview 1 | 0.14%   | 0.14%   | 0.51%    | 0.01%      |
| BILINEAR  | Overview 2 | 0.39%   | 0.22%   | 1.22%    | 0.00%      |
| BILINEAR  | Overview 3 | 0.56%   | 0.02%   | 2.48%    | 0.07%      |
| BILINEAR  | Overview 4 | 1.51%   | 0.18%   | 1.80%    | 0.41%      |

## Removed check_float16_pipeline (2026-07-17)

- Deleted `tests/check_float16_pipeline.cpp` and CMake/ctest registration
- Reason: current phase is Float32-only (DSM/DTM/DEM/slope/etc.); a forced Float16 library probe is out of scope
- Float16 remains covered only as a **rejected** dtype in `check_dtype_rejection`
- Historical finding kept in learnings (Float16 nodata quantization of -9999 → -10000); not an active test

## Current phase scope: Float32 only (2026-07-17)

- Policy: Float32-only for this phase
- In scope: DSM, DTM, DEM, slope, aspect, similar continuous analytic rasters
- Out of scope: Byte orthos, UInt16 imagery, Float16, integer elevation, categorical maps, Byte hillshade deliverables
- Non-Float32 must be rejected at CLI, not converted
- Known Limitations corrected: previously said other dtypes were "converted on read"; actual behaviour is reject

## Common raster data types by product (2026-07-17)


Industry-typical dtypes for products users might feed into a COG pipeline:

| Product | Most common dtype | Why |
|---------|-------------------|-----|
| Ortho / orthomosaic | **Byte (UInt8)** RGB/RGBA | Display imagery, 0–255 per band |
| High-bit aerial/sat | **UInt16** | 12–14 bit sensors stored in 16-bit |
| DSM / DTM / DEM | **Float32** | Continuous elevation, decimals, nodata |
| Hillshade | **Byte** | Visualization product (0–255 gray) |
| Slope | **Float32** | Degrees or percent, continuous |
| Aspect | **Float32** | Degrees 0–360 |
| Roughness / TPI / curvature | **Float32** | Continuous morphometrics |
| Land cover / class maps | **Byte** or **UInt16** | Categories, not continuous |

Implications for mlx-cog-gen:

- **Float32-only is correct for the elevation/analytic target** (DSM, DTM, DEM, slope, aspect, related morphometrics)
- **Orthos are usually Byte multichannel** and are a different workflow; not the primary fit for this tool
- **Hillshade as a deliverable is often Byte**, even when derived from a Float32 DEM
- CLI dtype rejection (Float32 only) matches DEM-centric use; accepting Byte orthos would need a separate product path, not silent conversion to Float32

Related tests:

- `check_dtype_rejection`: enforces Float32-only at the CLI (Float16 among rejected types)

## Test naming and documentation standard (2026-07-17)

- Each test file documents agenda, why it matters, and pass criteria in a header comment
- Full English file names, e.g. `check_mlx_availability.cpp` not `test_mlx.cpp`
- Renames applied (current suite):
  - `test_mlx.cpp` → `check_mlx_availability.cpp`
  - `test_overview_dims.cpp` → `check_overview_dimensions.cpp`
  - `test_cog_stats.cpp` → `check_cog_statistics.cpp`
  - `test_dtype_rejection.cpp` → `check_dtype_rejection.cpp`
  - `test_nan_nodata.cpp` → `check_nan_nodata.cpp`
- `test_float16.cpp` was renamed to `check_float16_pipeline.cpp` then **removed** (Float32-only phase; see above)
- CMake targets and ctest names match the five active `check_*.cpp` stems
- README test list updated to the full suite

## check_overview_dimensions agenda fix (2026-07-17)


- Previous test checked MLX overview sizes against a hardcoded `ceil(N/2)` formula only.
- That does not state the real contract: for supported resampling methods (AVERAGE, BILINEAR), MLX overview dimensions must match GDAL's overview dimensions exactly.
- Rewrite:
  - Reference path: `BuildOverviews("AVERAGE"|"BILINEAR", factors)`
  - MLX path: `BuildOverviews("NONE", factors)` then `MLXBuildOverviews(..., method)` (production allocate+fill)
  - Cases: even/odd single-level, multi-level even/odd, `sample_dem.tif` size (4772x5125, 4 levels)
  - Methods: both AVERAGE and BILINEAR
  - Pass criteria: overview count equal; each level width/height equal (exact)
- Documented in the test file header: not comparing COG `OVERVIEWS=AUTO` floor path (known 1px difference in docs/known-issues.md)
- Resampling method does not change GDAL overview sizes, but both methods are still exercised so a method-specific regression cannot slip through

## learnings.md stale-content cleanup (2026-07-17)


- Replaced obsolete NaN BILINEAR deviation table (claimed mean up to 16.81%) with live 2026-07-17 numbers (worst ~2.9%); documented 15% → 5% gate tighten
- Rewrote bilinear NoData section: current path is separable masked convolution; 2D box path is historical only
- Corrected batched-eval note: production still per-level `mx::eval`
- Corrected read/write buffer description: MLX allocator on read; vector copy mainly for NaN restore on write
- MLX version note 0.32.0; GDAL citations use tree paths only (e.g. `gcore/overview.cpp`); float16 rename marked removed

