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

CPLErr MLXBuildOverviews(GDALDataset *poDS, int nBands, const int *panBandList)
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
                mlx_downsample_average(current, oH, oW, nodataVal, hasNodata);
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

        fprintf(stderr, "  Band %d: %d overview level(s) computed on GPU\n",
                panBandList[iBand], nOvrCount);
    }

    return CE_None;
}
