# MLX-Cog-Gen

MLX-accelerated Cloud Optimized GeoTIFF generator for Apple Silicon.

## About

mlx-cog-gen replaces GDAL's CPU-based pyramid/overview generation with an MLX implementation that runs on the Apple Silicon GPU. Everything else in the COG pipeline (tiling, compression, metadata) is handled by GDAL as usual.

**GDAL is a required system dependency.** This project links against the installed GDAL library and does not bundle it.

## Why AVERAGE resampling

GDAL's default overview resampling is **NEAREST** — it picks one pixel from each 2×2 block and discards the rest. For continuous data (elevation, imagery, temperature), this throws away 75% of the signal at each level. A ridge that is one pixel wide at full resolution can disappear entirely at the next overview level depending on which pixel was selected.

`mlx_translate` uses **AVERAGE** instead: every pixel in a 2×2 block contributes to the output.

```
out[i,j] = (src[2i,2j] + src[2i,2j+1] + src[2i+1,2j] + src[2i+1,2j+1]) / count_valid
```

This preserves signal energy across zoom levels and is the physically correct choice for any continuous raster.

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

Tested on an M1 Pro (16 GB), 5 runs, both using AVERAGE resampling. Rasters are Float32 single-band DEMs generated via TIN interpolation at three GSDs:

| Raster | Dimensions | GDAL avg | MLX avg | Speedup |
|---|---|---|---|---|
| dem_80cm | 3746×3634 | 1.274s | 1.012s | 1.25× faster |
| dem_40cm | 7491×7268 | 3.936s | 2.771s | 1.42× faster |
| dem_20cm | 14982×14536 | 13.386s | 8.480s | 1.57× faster |

Speedup grows with raster size — the GPU becomes increasingly efficient as the workload scales.

## Roadmap

- Support additional resampling algorithms (bilinear, cubic, lanczos) — larger kernels that map naturally to MLX array ops

## Contributing

This is an early-stage project. Contributions are welcome once the core pipeline stabilises.

## License

[MIT](https://choosealicense.com/licenses/mit/)
