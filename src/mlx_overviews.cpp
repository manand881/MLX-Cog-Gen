#include "mlx_overviews.h"

#include <mlx/mlx.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mx = mlx::core;

/**
 * Average (box filter) 2x downsample on GPU with NoData masking.
 *
 * Takes a 2D float32 MLX array of shape [H, W] and returns [targetH, targetW].
 *
 * NoData handling:
 *   - NoData pixels are excluded from the average of each 2x2 block.
 *   - If all 4 pixels in a block are NoData, the output pixel is NoData.
 *   - Matches GDAL's AVERAGE resampling behaviour at NoData boundaries.
 *
 * Odd dimensions: the last row/column is replicated before averaging so that
 * edge pixels average with themselves — matching GDAL's ceil(N/2) convention.
 */
static mx::array mlx_downsample_average(const mx::array &input, int targetH,
                                        int targetW, float nodataVal,
                                        bool hasNodata)
{
    int H = input.shape()[0];
    int W = input.shape()[1];

    // Pad odd dimensions by replicating the last row/column
    mx::array padded = input;

    if (targetH * 2 > H)
    {
        mx::array lastRow = mx::slice(padded, {H - 1, 0}, {H, W});
        padded = mx::concatenate({padded, lastRow}, 0);
    }

    if (targetW * 2 > W)
    {
        int curH = padded.shape()[0];
        mx::array lastCol = mx::slice(padded, {0, W - 1}, {curH, W});
        padded = mx::concatenate({padded, lastCol}, 1);
    }

    if (!hasNodata)
    {
        // No NoData — simple reshape and mean
        mx::array reshaped = mx::reshape(padded, {targetH, 2, targetW, 2});
        return mx::mean(reshaped, std::vector<int>{1, 3});
    }

    // Build a valid-pixel mask (1.0 where data is valid, 0.0 where NoData)
    mx::array nodataScalar = mx::array(nodataVal, mx::float32);
    mx::array valid = mx::astype(
        mx::logical_not(mx::equal(padded, nodataScalar)), mx::float32);

    // Zero out NoData pixels so they don't contribute to the sum
    mx::array zeroed = mx::multiply(padded, valid);

    // Reshape both data and mask to [targetH, 2, targetW, 2]
    mx::array dataR  = mx::reshape(zeroed, {targetH, 2, targetW, 2});
    mx::array validR = mx::reshape(valid,  {targetH, 2, targetW, 2});

    // Sum valid data and count valid pixels per 2x2 block
    mx::array dataSum   = mx::sum(dataR,  std::vector<int>{1, 3});
    mx::array validCount = mx::sum(validR, std::vector<int>{1, 3});

    // Average = sum / count, guarding against divide-by-zero
    mx::array ones = mx::ones({targetH, targetW}, mx::float32);
    mx::array safeDenom = mx::maximum(validCount, ones);
    mx::array avg = mx::divide(dataSum, safeDenom);

    // Where all pixels were NoData, write NoData
    mx::array zeros = mx::zeros({targetH, targetW}, mx::float32);
    mx::array allNodata = mx::equal(validCount, zeros);
    mx::array nodataFill = mx::full({targetH, targetW}, nodataVal, mx::float32);

    return mx::where(allNodata, nodataFill, avg);
}

/**
 * Bilinear 2x downsample on GPU with NoData masking.
 *
 * Matches GDAL's BILINEAR overview resampling: a separable tent filter applied
 * as two sequential 1D passes (horizontal then vertical).
 *
 * The tent filter kernel is GWKBilinear(x) = 1 - |x|  for |x| <= 1.
 * For each output pixel i, the sample point in source space is:
 *   x = (i + 0.5) * 2 - 0.5 = 2i + 0.5
 * This lands halfway between source pixels 2i and 2i+1, so both get weight
 * GWKBilinear(0.5) = 0.5. Applied identically in Y.
 *
 * Boundary clamping: odd dimensions are padded by replicating the last
 * row/column before either pass, so the last output pixel at x = 2*(W-1)+0.5
 * sees source pixel W-1 twice, giving it weight 1.0 — matching GDAL's
 * behaviour of clamping the kernel to the valid source extent.
 *
 * For the no-NoData path this is implemented as two separable reshape+mean
 * passes, which is structurally equivalent to GDAL's separable convolution.
 * For the NoData path a 2D masked sum is used to match GDAL's 2D weight
 * accumulation (a separable NoData pass would over-weight pixels that
 * partially survived the horizontal pass).
 *
 * Numerical result for 2x with no NoData: identical to mlx_downsample_average
 * because the equal 0.5/0.5 weights make the separable and box approaches
 * produce the same sum. They diverge for non-2x factors and at NoData
 * boundaries where the 2D weight distribution differs from a separable one.
 */
static mx::array mlx_downsample_bilinear(const mx::array &input, int targetH,
                                         int targetW, float nodataVal,
                                         bool hasNodata)
{
    int H = input.shape()[0];
    int W = input.shape()[1];

    // Pad odd dimensions: replicates last row/col to simulate kernel clamping
    // at the boundary, matching GDAL's GDALResampleChunk_ConvolutionT behaviour.
    mx::array padded = input;

    if (targetH * 2 > H)
    {
        mx::array lastRow = mx::slice(padded, {H - 1, 0}, {H, W});
        padded = mx::concatenate({padded, lastRow}, 0);
    }

    if (targetW * 2 > W)
    {
        int curH = padded.shape()[0];
        mx::array lastCol = mx::slice(padded, {0, W - 1}, {curH, W});
        padded = mx::concatenate({padded, lastCol}, 1);
    }

    int pH = padded.shape()[0];

    if (!hasNodata)
    {
        // Horizontal pass: pair adjacent columns, apply tent weights (0.5, 0.5)
        // reshape [pH, pW] → [pH, targetW, 2], mean over axis 2 → [pH, targetW]
        mx::array horiz =
            mx::mean(mx::reshape(padded, {pH, targetW, 2}),
                     std::vector<int>{2});

        // Vertical pass: pair adjacent rows, apply tent weights (0.5, 0.5)
        // reshape [pH, targetW] → [targetH, 2, targetW], mean over axis 1
        return mx::mean(mx::reshape(horiz, {targetH, 2, targetW}),
                        std::vector<int>{1});
    }

    // NoData path: 2D masked weighted sum — matches GDAL's 2D weight
    // accumulation in GDALResampleChunk_ConvolutionT more closely than a
    // separable masked pass would.
    mx::array nodataScalar = mx::array(nodataVal, mx::float32);
    mx::array valid = mx::astype(
        mx::logical_not(mx::equal(padded, nodataScalar)), mx::float32);

    mx::array zeroed = mx::multiply(padded, valid);

    mx::array dataR  = mx::reshape(zeroed, {targetH, 2, targetW, 2});
    mx::array validR = mx::reshape(valid,  {targetH, 2, targetW, 2});

    mx::array dataSum    = mx::sum(dataR,  std::vector<int>{1, 3});
    mx::array validCount = mx::sum(validR, std::vector<int>{1, 3});

    mx::array ones      = mx::ones({targetH, targetW}, mx::float32);
    mx::array safeDenom = mx::maximum(validCount, ones);
    mx::array result    = mx::divide(dataSum, safeDenom);

    mx::array zeros     = mx::zeros({targetH, targetW}, mx::float32);
    mx::array allNodata = mx::equal(validCount, zeros);
    mx::array nodataFill = mx::full({targetH, targetW}, nodataVal, mx::float32);

    return mx::where(allNodata, nodataFill, result);
}

CPLErr MLXBuildOverviews(GDALDataset *poDS, int nBands,
                         const int *panBandList, ResampleMethod method)
{
    for (int iBand = 0; iBand < nBands; iBand++)
    {
        GDALRasterBand *poBand = poDS->GetRasterBand(panBandList[iBand]);
        int W = poBand->GetXSize();
        int H = poBand->GetYSize();
        int nOvrCount = poBand->GetOverviewCount();

        if (nOvrCount == 0)
            continue;

        // Read full band as float32
        std::vector<float> bandData(static_cast<size_t>(W) * H);
        CPLErr eErr = poBand->RasterIO(GF_Read, 0, 0, W, H, bandData.data(),
                                       W, H, GDT_Float32, 0, 0);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "MLXBuildOverviews: RasterIO read failed for band %d",
                     panBandList[iBand]);
            return eErr;
        }

        // Read NoData value for this band
        int hasNodata = 0;
        double nodataDouble = poBand->GetNoDataValue(&hasNodata);
        float nodataVal = static_cast<float>(nodataDouble);

        // Load into MLX array on GPU
        mx::array current =
            mx::array(bandData.data(), {H, W}, mx::float32);
        mx::eval(current);

        // Iteratively downsample each overview level from the previous level
        for (int iOvr = 0; iOvr < nOvrCount; iOvr++)
        {
            GDALRasterBand *poOvr = poBand->GetOverview(iOvr);
            int oW = poOvr->GetXSize();
            int oH = poOvr->GetYSize();

            mx::array downsampled =
                (method == ResampleMethod::BILINEAR)
                    ? mlx_downsample_bilinear(current, oH, oW, nodataVal, hasNodata)
                    : mlx_downsample_average(current, oH, oW, nodataVal, hasNodata);
            mx::eval(downsampled);

            // Write result into the overview band
            std::vector<float> ovrData(static_cast<size_t>(oW) * oH);
            std::memcpy(ovrData.data(), downsampled.data<float>(),
                        static_cast<size_t>(oW) * oH * sizeof(float));

            eErr = poOvr->RasterIO(GF_Write, 0, 0, oW, oH, ovrData.data(),
                                   oW, oH, GDT_Float32, 0, 0);
            if (eErr != CE_None)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "MLXBuildOverviews: RasterIO write failed for band "
                         "%d overview %d",
                         panBandList[iBand], iOvr);
                return eErr;
            }

            current = downsampled;
        }

        fprintf(stderr, "  Band %d: %d overview level(s) computed on GPU (%s)\n",
                panBandList[iBand], nOvrCount,
                method == ResampleMethod::BILINEAR ? "bilinear" : "average");
    }

    return CE_None;
}
