#include "mlx_overviews.h"

#include <mlx/mlx.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Ms    = std::chrono::duration<double, std::milli>;

static double elapsed_ms(Clock::time_point start)
{
    return Ms(Clock::now() - start).count();
}
} // namespace

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
 * edge pixels average with themselves, matching GDAL's ceil(N/2) convention.
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
        // No NoData: simple reshape and mean
        mx::array reshaped = mx::reshape(padded, {targetH, 2, targetW, 2});
        return mx::mean(reshaped, std::vector<int>{1, 3});
    }

    // Build a bool mask (true where data is valid, false where NoData).
    // Kept as bool (1 byte/pixel) rather than float32 to reduce memory bandwidth.
    mx::array nodataScalar = mx::array(nodataVal, mx::float32);
    mx::array valid = mx::logical_not(mx::equal(padded, nodataScalar));

    // Zero out NoData pixels via where, avoiding widening the mask to float32
    mx::array zeroed = mx::where(valid, padded, mx::array(0.0f));

    // Reshape both data and mask to [targetH, 2, targetW, 2]
    mx::array dataR  = mx::reshape(zeroed, {targetH, 2, targetW, 2});
    mx::array validR = mx::reshape(valid,  {targetH, 2, targetW, 2});

    // Sum valid data and count valid pixels per 2x2 block
    mx::array dataSum    = mx::sum(dataR,  std::vector<int>{1, 3});
    mx::array validCount = mx::sum(validR, std::vector<int>{1, 3});

    // Average = sum / count, guarding against divide-by-zero
    mx::array validCountF = mx::astype(validCount, mx::float32);
    mx::array safeDenom   = mx::maximum(validCountF, mx::array(1.0f));
    mx::array avg         = mx::divide(dataSum, safeDenom);

    // Where all pixels were NoData, write NoData
    mx::array allNodata  = mx::equal(validCountF, mx::array(0.0f));
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
 * sees source pixel W-1 twice, giving it weight 1.0, matching GDAL's
 * behaviour of clamping the kernel to the valid source extent.
 *
 * For the no-NoData path this is implemented as two separable reshape+mean
 * passes, which is structurally equivalent to GDAL's separable convolution.
 * For the NoData path, a separable masked convolution is used to match GDAL's
 * GDALResampleChunk_ConvolutionT exactly: horizontal pass produces intermediate
 * values and mask, vertical pass applies weights to intermediate using mask.
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

    // NoData path: separable masked convolution to match GDAL's
    // GDALResampleChunk_ConvolutionT implementation exactly.
    // Horizontal pass applies 1D tent weights with mask, produces intermediate
    // values and mask. Vertical pass applies 1D tent weights to intermediate
    // values using intermediate mask. Final 2D weights = product of 1D weights.
    // Mask kept as bool (1 byte/pixel) to reduce memory bandwidth.
    mx::array nodataScalar = mx::array(nodataVal, mx::float32);
    mx::array valid = mx::logical_not(mx::equal(padded, nodataScalar));

    // Horizontal pass: reshape [pH, pW] -> [pH, targetW, 2], apply masked mean
    // For 2x downsampling, tent weights are 0.5, 0.5. With masking:
    // - If both valid: (0.5*p1 + 0.5*p2) / (0.5+0.5) = mean
    // - If one valid: (0.5*p) / 0.5 = p
    // - If neither valid: NoData
    mx::array zeroed = mx::where(valid, padded, mx::array(0.0f));
    mx::array dataH  = mx::reshape(zeroed, {pH, targetW, 2});
    mx::array validH = mx::reshape(valid,  {pH, targetW, 2});

    // Multiply data by weights (0.5) before summing
    mx::array weightedH = dataH * mx::array(0.5f);
    mx::array dataSumH    = mx::sum(weightedH, std::vector<int>{2});
    mx::array validCountH = mx::sum(validH, std::vector<int>{2});

    // Normalize by sum of weights (each valid pixel contributes weight 0.5)
    // Matching GDAL: if weightSum > 0, output = dataSum / weightSum; else output = 0
    mx::array validCountHF = mx::astype(validCountH, mx::float32);
    mx::array weightSumH   = validCountHF * mx::array(0.5f);
    mx::array hasWeightH   = mx::greater(weightSumH, mx::array(0.0f));
    mx::array horiz        = mx::where(hasWeightH, mx::divide(dataSumH, weightSumH), mx::array(0.0f));

    // Intermediate mask: true if any valid pixel survived horizontal pass
    mx::array horizValid = mx::greater(validCountHF, mx::array(0.0f));

    // Vertical pass: reshape [pH, targetW] -> [targetH, 2, targetW], apply masked mean
    // using intermediate mask from horizontal pass
    mx::array zeroedHoriz = mx::where(horizValid, horiz, mx::array(0.0f));
    mx::array dataV       = mx::reshape(zeroedHoriz, {targetH, 2, targetW});
    mx::array validV      = mx::reshape(horizValid,  {targetH, 2, targetW});

    // Multiply data by weights (0.5) before summing
    mx::array weightedV = dataV * mx::array(0.5f);
    mx::array dataSumV    = mx::sum(weightedV, std::vector<int>{1});
    mx::array validCountV = mx::sum(validV, std::vector<int>{1});

    // Normalize by sum of weights (each valid pixel contributes weight 0.5)
    // Matching GDAL: if weightSum > 0, output = dataSum / weightSum; else output = 0
    mx::array validCountVF = mx::astype(validCountV, mx::float32);
    mx::array weightSumV   = validCountVF * mx::array(0.5f);
    mx::array hasWeightV   = mx::greater(weightSumV, mx::array(0.0f));
    mx::array result       = mx::where(hasWeightV, mx::divide(dataSumV, weightSumV), mx::array(0.0f));

    // Output NoData where no valid pixels survived either pass
    mx::array allNodata  = mx::equal(validCountVF, mx::array(0.0f));
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

        // Read NoData value for this band
        int hasNodata = 0;
        double nodataDouble = poBand->GetNoDataValue(&hasNodata);
        float nodataVal = static_cast<float>(nodataDouble);

        // Detect NaN no-data values: mx::equal() cannot detect NaN (NaN != NaN by IEEE 754)
        // We substitute NaN with -9999 internally for MLX operations, then restore NaN before writing
        bool nodataIsNaN = hasNodata && std::isnan(nodataVal);
        float workingNodataVal = nodataIsNaN ? -9999.0f : nodataVal;

        auto tRead = Clock::now();
        size_t nbytes = static_cast<size_t>(W) * H * sizeof(float);
        auto buffer = mx::allocator::malloc(nbytes);
        float *rawPtr = static_cast<float *>(buffer.raw_ptr());

        CPLErr eErr = poBand->RasterIO(
            GF_Read, 0, 0, W, H, rawPtr, W, H, GDT_Float32, 0, 0);
        double msRead = elapsed_ms(tRead);
        if (eErr != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "MLXBuildOverviews: RasterIO read failed for band %d",
                     panBandList[iBand]);
            mx::allocator::free(buffer);
            return eErr;
        }

        // Substitute NaN with working nodata value if original nodata is NaN
        // Done on CPU before creating MLX array since mx::isnan() may not be available
        if (nodataIsNaN)
        {
            size_t pixelCount = static_cast<size_t>(W) * H;
            for (size_t i = 0; i < pixelCount; i++)
            {
                if (std::isnan(rawPtr[i]))
                {
                    rawPtr[i] = workingNodataVal;
                }
            }
        }

        auto tArrayCopy = Clock::now();
        mx::array current =
            mx::array(rawPtr, {H, W}, mx::float32,
                     [buffer](void *) { mx::allocator::free(buffer); });
        double msArrayCopy = elapsed_ms(tArrayCopy);

        double msGpuTotal = 0.0;
        double msIoTotal = 0.0;
        double msGpuLevel = 0.0;
        double msIoLevel = 0.0;

        // Iteratively downsample each overview level from the previous level
        for (int iOvr = 0; iOvr < nOvrCount; iOvr++)
        {
            GDALRasterBand *poOvr = poBand->GetOverview(iOvr);
            int oW = poOvr->GetXSize();
            int oH = poOvr->GetYSize();

            auto tGpu = Clock::now();
            mx::array downsampled =
                (method == ResampleMethod::BILINEAR)
                    ? mlx_downsample_bilinear(current, oH, oW, workingNodataVal, hasNodata)
                    : mlx_downsample_average(current, oH, oW, workingNodataVal, hasNodata);
            mx::eval(downsampled);
            msGpuLevel = elapsed_ms(tGpu);

            // Write result directly from MLX unified memory into the overview
            // band. No intermediate copy is needed since eval() has completed.
            auto tIo = Clock::now();
            
            // Restore NaN before writing if original nodata was NaN
            // Done on CPU by modifying the MLX array data in-place
            const float *mlxDPtr = downsampled.data<float>();
            std::vector<float> outputBuffer(mlxDPtr, mlxDPtr + static_cast<size_t>(oW) * oH);
            if (nodataIsNaN)
            {
                size_t pixelCount = static_cast<size_t>(oW) * oH;
                for (size_t i = 0; i < pixelCount; i++)
                {
                    if (outputBuffer[i] == workingNodataVal)
                    {
                        outputBuffer[i] = std::numeric_limits<float>::quiet_NaN();
                    }
                }
            }
            
            eErr = poOvr->RasterIO(GF_Write, 0, 0, oW, oH,
                                   outputBuffer.data(),
                                   oW, oH, GDT_Float32, 0, 0);
            msIoLevel = elapsed_ms(tIo);

            msGpuTotal += msGpuLevel;
            msIoTotal += msIoLevel;

            if (eErr != CE_None)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "MLXBuildOverviews: RasterIO write failed for band "
                         "%d overview %d",
                         panBandList[iBand], iOvr);
                return eErr;
            }

            fprintf(stderr, "    Level %d: %dx%d  gpu: %.1fms  io: %.1fms\n",
                    iOvr + 1, oW, oH, msGpuLevel, msIoLevel);

            current = downsampled;
        }

        // Restore NaN nodata value in output band metadata if original was NaN
        if (nodataIsNaN)
        {
            poBand->SetNoDataValue(std::numeric_limits<float>::quiet_NaN());
        }

        fprintf(stderr, "  Band %d: %d overview level(s) computed on GPU (%s)  "
                "read: %.1fms  array: %.1fms  gpu: %.1fms  io: %.1fms\n",
                panBandList[iBand], nOvrCount,
                method == ResampleMethod::BILINEAR ? "bilinear" : "average",
                msRead, msArrayCopy, msGpuTotal, msIoTotal);
    }

    return CE_None;
}
