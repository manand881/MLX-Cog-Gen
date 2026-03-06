# mlx-cog-gen

MLX-powered Cloud Optimized GeoTIFF generator for Apple Silicon.

## About

This project takes GDAL's COG generation pipeline and replaces the pyramid/overview generation step with an MLX-accelerated implementation, making use of Apple Silicon hardware rather than letting it idle when you already have it on your machine.

**GDAL is a required system dependency.** This project links against the installed GDAL library and does not bundle it.

## Future Scope

- **Resampling acceleration** — offload bilinear, cubic, and lanczos resampling algorithms to Apple Silicon via MLX, since these are matrix operations that map naturally to GPU execution

## Usage

```bash
build/mlx_translate input.tif output_cog.tif
```

Outputs a COG with LZW compression by default. Pass `-co KEY=VALUE` to override creation options:

```bash
build/mlx_translate input.tif output_cog.tif -co COMPRESS=DEFLATE
```

## Installation

No pre-built binaries yet — build from source (see below).

## Development

```bash
brew install gdal  # Required system dependency
brew install mlx   # Apple Silicon ML framework for overview acceleration
brew install cmake # Build system
```

## Building & Testing

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

## Benchmarks

Tested on `sample_dem.tif` (4772×5125, 1-band Float32 DEM) on an M1 Pro (16 GB), 5 runs, both using AVERAGE resampling:

| tool | avg |
|---|---|
| `gdal_translate -r average` | 1.771s |
| `mlx_translate` | 1.480s |

**1.19x faster.** Run 1 is slower due to Metal shader compilation — subsequent runs are consistent.

## Contributing

This is an early-stage project. Contributions are welcome once the core pipeline stabilises.

## License

[MIT](https://choosealicense.com/licenses/mit/)
