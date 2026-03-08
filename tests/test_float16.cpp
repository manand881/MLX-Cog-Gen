#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_string.h>

#include "../src/mlx_overviews.h"

static const char *INPUT      = "tests/sample_dem.tif";
static const char *FP16_SRC   = "/vsimem/test_fp16_src.tif";
static const char *GDAL_OUT   = "/vsimem/test_fp16_gdal.tif";
static const char *MLX_OUT    = "/vsimem/test_fp16_mlx.tif";
static const float TOLERANCE  = 0.05f;

struct Stats { float min, max, mean, stddev; };

static Stats computeStats(GDALRasterBand *poBand, float nodataVal, bool hasNodata)
{
    int W = poBand->GetXSize();
    int H = poBand->GetYSize();
    std::vector<float> data(static_cast<size_t>(W) * H);
    poBand->RasterIO(GF_Read, 0, 0, W, H, data.data(), W, H,
                     GDT_Float32, 0, 0);

    double sum = 0.0, sumSq = 0.0;
    float minVal = std::numeric_limits<float>::max();
    float maxVal = std::numeric_limits<float>::lowest();
    long count = 0;

    for (float v : data)
    {
        if (hasNodata && v == nodataVal) continue;
        sum   += v;
        sumSq += static_cast<double>(v) * v;
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
        count++;
    }

    assert(count > 0);
    float mean   = static_cast<float>(sum / count);
    float stddev = static_cast<float>(
        std::sqrt(sumSq / count - (sum / count) * (sum / count)));
    return {minVal, maxVal, mean, stddev};
}

static bool withinTolerance(float a, float b, float pct)
{
    float denom = std::max(std::abs(b), 1e-6f);
    return std::abs(a - b) / denom <= pct;
}

static void checkStats(const char *label, Stats gdal, Stats mlx, float pct)
{
    bool ok = withinTolerance(mlx.min,    gdal.min,    pct) &&
              withinTolerance(mlx.max,    gdal.max,    pct) &&
              withinTolerance(mlx.mean,   gdal.mean,   pct) &&
              withinTolerance(mlx.stddev, gdal.stddev, pct);

    printf("  %s:\n", label);
    printf("    GDAL  min=%-8.3f max=%-8.3f mean=%-8.3f stddev=%-8.3f\n",
           gdal.min, gdal.max, gdal.mean, gdal.stddev);
    printf("    MLX   min=%-8.3f max=%-8.3f mean=%-8.3f stddev=%-8.3f\n",
           mlx.min,  mlx.max,  mlx.mean,  mlx.stddev);

    if (!ok)
    {
        fprintf(stderr, "FAIL: %s stats exceed %.0f%% tolerance\n",
                label, pct * 100);
        assert(false);
    }
    printf("    [PASS] within %.0f%% tolerance\n", pct * 100);
}

// Convert a dataset to Float16 in vsimem
static GDALDataset *toFloat16(GDALDataset *poSrcDS, const char *outPath)
{
    const char *args[] = { "-ot", "Float16", nullptr };
    GDALTranslateOptions *opts =
        GDALTranslateOptionsNew(const_cast<char **>(args), nullptr);
    int err = 0;
    GDALDataset *poDS = static_cast<GDALDataset *>(
        GDALTranslate(outPath, poSrcDS, opts, &err));
    GDALTranslateOptionsFree(opts);
    assert(poDS != nullptr && err == 0);
    return poDS;
}

static GDALDataset *buildGDALCOG(GDALDataset *poSrcDS, const char *outPath)
{
    const char *args[] = {
        "-of", "COG",
        "-co", "COMPRESS=LZW",
        "-co", "OVERVIEWS=AUTO",
        nullptr
    };
    GDALTranslateOptions *opts =
        GDALTranslateOptionsNew(const_cast<char **>(args), nullptr);
    int err = 0;
    GDALDataset *poDS = static_cast<GDALDataset *>(
        GDALTranslate(outPath, poSrcDS, opts, &err));
    GDALTranslateOptionsFree(opts);
    assert(poDS != nullptr && err == 0);
    return poDS;
}

static GDALDataset *buildMLXCOG(GDALDataset *poSrcDS, const char *outPath)
{
    int nBands = poSrcDS->GetRasterCount();
    int srcW   = poSrcDS->GetRasterXSize();
    int srcH   = poSrcDS->GetRasterYSize();

    const char *tmpPath = "/vsimem/test_fp16_mlx_tmp.tif";
    GDALDriver *poTiffDriver =
        GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset *poTmpDS = poTiffDriver->CreateCopy(
        tmpPath, poSrcDS, FALSE, nullptr, nullptr, nullptr);
    assert(poTmpDS != nullptr);

    // Report what data type was carried through
    GDALDataType dt = poTmpDS->GetRasterBand(1)->GetRasterDataType();
    printf("  Temp dataset data type: %s\n", GDALGetDataTypeName(dt));

    std::vector<int> ovrLevels;
    int factor = 2, w = srcW, h = srcH;
    while (w > 512 || h > 512)
    {
        ovrLevels.push_back(factor);
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        factor *= 2;
    }

    CPLErr eErr = poTmpDS->BuildOverviews(
        "NONE", static_cast<int>(ovrLevels.size()), ovrLevels.data(),
        0, nullptr, GDALDummyProgress, nullptr);
    assert(eErr == CE_None);

    std::vector<int> bandList(nBands);
    for (int i = 0; i < nBands; i++) bandList[i] = i + 1;
    eErr = MLXBuildOverviews(poTmpDS, nBands, bandList.data(),
                             ResampleMethod::AVERAGE);
    assert(eErr == CE_None);

    char **papszOpts = nullptr;
    papszOpts = CSLSetNameValue(papszOpts, "COMPRESS", "LZW");
    papszOpts = CSLSetNameValue(papszOpts, "OVERVIEWS", "FORCE_USE_EXISTING");
    GDALDriver *poCOGDriver =
        GetGDALDriverManager()->GetDriverByName("COG");
    GDALDataset *poCOGDS = poCOGDriver->CreateCopy(
        outPath, poTmpDS, FALSE, papszOpts, GDALDummyProgress, nullptr);
    assert(poCOGDS != nullptr);

    GDALClose(poTmpDS);
    GDALDeleteDataset(nullptr, tmpPath);
    CSLDestroy(papszOpts);
    return poCOGDS;
}

int main()
{
    GDALAllRegister();

    printf("=== Float16 Pipeline Test (tolerance: %.0f%%) ===\n\n", TOLERANCE * 100);

    // Open Float32 source
    GDALDataset *poSrc32 = static_cast<GDALDataset *>(
        GDALOpen(INPUT, GA_ReadOnly));
    assert(poSrc32 != nullptr);

    GDALDataType srcType = poSrc32->GetRasterBand(1)->GetRasterDataType();
    printf("Source data type: %s\n\n", GDALGetDataTypeName(srcType));

    // Convert to Float16
    GDALDataset *poSrc16 = toFloat16(poSrc32, FP16_SRC);
    GDALDataType fp16Type = poSrc16->GetRasterBand(1)->GetRasterDataType();
    printf("Float16 source data type: %s\n\n", GDALGetDataTypeName(fp16Type));

    // Build COGs from the Float16 source
    printf("-- Building GDAL COG from Float16 source --\n");
    GDALDataset *poGDAL = buildGDALCOG(poSrc16, GDAL_OUT);

    printf("\n-- Building MLX COG from Float16 source --\n");
    GDALDataset *poMLX = buildMLXCOG(poSrc16, MLX_OUT);

    // Compare stats at each overview level
    printf("\n-- Comparing stats --\n");
    int nBands = poSrc16->GetRasterCount();
    for (int iBand = 1; iBand <= nBands; iBand++)
    {
        int hasNodata = 0;
        double nodataDouble = poSrc16->GetRasterBand(iBand)
                                     ->GetNoDataValue(&hasNodata);
        float nodataVal = static_cast<float>(nodataDouble);

        printf("  Band %d:\n", iBand);

        Stats gdalStats = computeStats(poGDAL->GetRasterBand(iBand),
                                       nodataVal, hasNodata);
        Stats mlxStats  = computeStats(poMLX->GetRasterBand(iBand),
                                       nodataVal, hasNodata);
        checkStats("Full resolution", gdalStats, mlxStats, TOLERANCE);

        int nOvr = poGDAL->GetRasterBand(iBand)->GetOverviewCount();
        for (int iOvr = 0; iOvr < nOvr; iOvr++)
        {
            GDALRasterBand *gdalOvr =
                poGDAL->GetRasterBand(iBand)->GetOverview(iOvr);
            GDALRasterBand *mlxOvr =
                poMLX->GetRasterBand(iBand)->GetOverview(iOvr);

            char label[64];
            snprintf(label, sizeof(label), "Overview %d (%dx%d)",
                     iOvr + 1, gdalOvr->GetXSize(), gdalOvr->GetYSize());

            gdalStats = computeStats(gdalOvr, nodataVal, hasNodata);
            mlxStats  = computeStats(mlxOvr,  nodataVal, hasNodata);
            checkStats(label, gdalStats, mlxStats, TOLERANCE);
        }
    }

    GDALClose(poGDAL);
    GDALClose(poMLX);
    GDALClose(poSrc16);
    GDALClose(poSrc32);
    GDALDeleteDataset(nullptr, FP16_SRC);
    GDALDeleteDataset(nullptr, GDAL_OUT);
    GDALDeleteDataset(nullptr, MLX_OUT);

    printf("\n=== Float16 test passed ===\n");
    return 0;
}
