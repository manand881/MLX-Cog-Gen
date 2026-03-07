# MLX-Cog-Gen

MLX-accelerated Cloud Optimized GeoTIFF generator for Apple Silicon.

## About

mlx-cog-gen replaces GDAL's CPU-based pyramid/overview generation with an MLX implementation that runs on the Apple Silicon GPU. Everything else in the COG pipeline (tiling, compression, metadata) is handled by GDAL as usual.

**GDAL is a required system dependency.** This project links against the installed GDAL library and does not bundle it.

## Why Apple Silicon

On an x86 machine, GDAL's overview generation may or may not be leaving performance on the table depending on whether a discrete GPU is present. The behaviour varies by hardware configuration: some machines have it, some don't, and GDAL never uses it regardless.

On Apple Silicon the situation is deterministic. Every M-series device, from the base M1 MacBook Air to the M4 Mac Pro, ships with a high-performance GPU and Neural Engine in the same package as the CPU, sharing the same memory pool. GDAL uses none of it. The GPU is completely idle during the entire overview generation step on every Apple Silicon machine, every single time.

This is not a niche edge case. Apple Silicon has become the dominant platform for professional macOS users including a large portion of the geospatial community. Optimising this workflow for Apple Silicon means every one of those machines benefits — not a subset with a particular hardware configuration, but all of them unconditionally.

## Why AVERAGE resampling

GDAL's default overview resampling is **NEAREST** — it picks one pixel from each 2×2 block and discards the rest. For continuous data (elevation, imagery, temperature), this throws away 75% of the signal at each level. A ridge that is one pixel wide at full resolution can disappear entirely at the next overview level depending on which pixel was selected.

`mlx_translate` uses **AVERAGE** instead: every pixel in a 2×2 block contributes to the output.

```
out[i,j] = (src[2i,2j] + src[2i,2j+1] + src[2i+1,2j] + src[2i+1,2j+1]) / count_valid
```

This preserves signal energy across zoom levels and is the physically correct choice for any continuous raster. AVERAGE is also the simplest averaging method: a straightforward 2×2 box filter. More sophisticated methods like bilinear or cubic use larger kernels and are on the roadmap.

## Limitations

`mlx_translate` loads each raster band fully into unified memory before dispatching to the GPU. This means **the uncompressed raster must fit within available system memory**. On Apple Silicon, CPU and GPU share the same memory pool, so available memory is whatever is free at runtime across both.

To estimate uncompressed size: `width × height × bands × bytes_per_pixel`. For a Float32 single-band raster that is 30000×30000, this is 30000 × 30000 × 1 × 4 = ~3.4 GB. For a uint16 RGB raster of the same dimensions it would be 30000 × 30000 × 3 × 2 = ~5.1 GB.

GDAL's approach processes in horizontal strips and handles arbitrarily large rasters. If your input exceeds available memory, use `gdal_translate` instead.

## Build

Install dependencies:

```bash
brew install gdal cmake mlx
```

Build and test:

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/opt/homebrew
make
ctest --output-on-failure
```

Three test suites run via `ctest`:

- `test_mlx` — verifies MLX install, GPU device access, and basic array ops
- `test_overview_dims` — verifies overview dimensions match GDAL's `ceil(N/2)` convention across even/odd/multi-level inputs
- `test_cog_stats` — runs both GDAL and MLX COG generation on a real DEM and checks that raster stats are within 5% at every overview level

## Usage

```bash
build/mlx_translate input.tif output_cog.tif
```

Outputs a COG with LZW compression by default. Pass `-co KEY=VALUE` to override creation options:

```bash
build/mlx_translate input.tif output_cog.tif -co COMPRESS=DEFLATE
```

## Benchmarks

Tested on an M1 Pro (16 GB), 5 runs, both using AVERAGE resampling. Rasters are Float32 single-band DEMs generated via TIN interpolation at six GSDs:

| Raster | Dimensions | File size | GDAL avg | MLX avg | Speedup |
|---|---|---|---|---|---|
| dem_160cm | 1873×1817 | 4.6 MB | 0.379s | 0.356s | 1.06× faster |
| dem_80cm | 3746×3634 | 14 MB | 0.959s | 0.747s | 1.28× faster |
| dem_40cm | 7491×7268 | 39 MB | 2.934s | 2.053s | 1.42× faster |
| dem_20cm | 14982×14536 | 113 MB | 10.376s | 7.035s | 1.47× faster |
| dem_10cm | 29967×29074 | 323 MB | 37.952s | 7.324s | 5.18× faster |
| dem_5cm | 59927×58141 | 926 MB | 165.742s | 21.523s | 7.70× faster |

Speedup grows with raster size — the GPU becomes increasingly efficient as the workload scales. At the largest tested size (dem_5cm, ~60k×58k pixels), MLX is **7.7× faster** than GDAL, completing in 21s versus GDAL's 165s.

## Roadmap

- Additional resampling algorithms (bilinear, cubic, lanczos)
- GPU-accelerated tile block creation (512×512 blocking, currently delegated to GDAL)
- OOM detection and graceful fallback to GDAL

## Contributing

Open an issue before raising a PR. Describe what you want to do and why in as much detail as possible. Once there is agreement on the approach, go ahead and open the PR.

## License

[MIT](https://choosealicense.com/licenses/mit/)
