// Check NaN nodata handling
//
// Agenda
// ------
// Build GDAL and MLX overviews for a DEM whose nodata value is NaN. Compare
// min/max/mean/stddev (ignoring NaN pixels) for AVERAGE and BILINEAR.
//
// Why it matters
// --------------
// mx::equal cannot detect NaN (NaN != NaN). Without a substitution or rejection
// strategy, NaN nodata corrupts every overview level. This test guards the
// chosen handling path against GDAL on a real NaN-nodata DEM.
//
// Pass criteria
// -------------
// AVERAGE and BILINEAR: stats within 5% of GDAL at full res and every
// overview. No NaN in reported min/max of valid-pixel stats.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_string.h>

#include "../src/mlx_overviews.h"

static const char *INPUT = "tests/sample_dem_nan_nodata.tif";
static const char *GDAL_OUT = "/vsimem/check_nan_gdal.tif";
static const char *MLX_OUT = "/vsimem/check_nan_mlx.tif";
static const float AVG_TOLERANCE = 0.05f; // 5%
static const float BIL_TOLERANCE = 0.05f; // 5%

struct Stats { float min, max, mean, stddev; };

static Stats computeStats(GDALRasterBand *poBand)
{
    int W = poBand->GetXSize();
    int H = poBand->GetYSize();
    std::vector<float> data(static_cast<size_t>(W) * H);
    CPLErr err = poBand->RasterIO(GF_Read, 0, 0, W, H, data.data(), W, H,
                              GDT_Float32, 0, 0);
    assert(err == CE_None);

    double sum = 0.0, sumSq = 0.0;
    float minVal = std::numeric_limits<float>::max();
    float maxVal = std::numeric_limits<float>::lowest();
    long count = 0;

    for (float v : data)
    {
        if (std::isnan(v)) continue;
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

static bool checkStats(const char *label, const Stats &a, const Stats &b, float pct)
{
    bool minOk = withinTolerance(a.min, b.min, pct);
    bool maxOk = withinTolerance(a.max, b.max, pct);
    bool meanOk = withinTolerance(a.mean, b.mean, pct);
    bool stddevOk = withinTolerance(a.stddev, b.stddev, pct);

    printf("  %s:\n", label);
    printf("    GDAL: min=%.3f max=%.3f mean=%.3f stddev=%.3f\n",
           a.min, a.max, a.mean, a.stddev);
    printf("    MLX:  min=%.3f max=%.3f mean=%.3f stddev=%.3f\n",
           b.min, b.max, b.mean, b.stddev);

    if (!minOk || !maxOk || !meanOk || !stddevOk)
    {
        printf("    [FAIL] stats differ by more than %.0f%%\n", pct * 100);
        if (!minOk) printf("      min: diff=%.1f%%\n", 
            100 * std::abs(a.min - b.min) / std::max(std::abs(b.min), 1e-6f));
        if (!maxOk) printf("      max: diff=%.1f%%\n", 
            100 * std::abs(a.max - b.max) / std::max(std::abs(b.max), 1e-6f));
        if (!meanOk) printf("      mean: diff=%.1f%%\n", 
            100 * std::abs(a.mean - b.mean) / std::max(std::abs(b.mean), 1e-6f));
        if (!stddevOk) printf("      stddev: diff=%.1f%%\n", 
            100 * std::abs(a.stddev - b.stddev) / std::max(std::abs(b.stddev), 1e-6f));
    }
    else
    {
        printf("    [PASS] all stats within %.0f%%\n", pct * 100);
    }
    return minOk && maxOk && meanOk && stddevOk;
}

static bool compareDatasets(GDALDataset *poGDAL, GDALDataset *poMLX, float tolerance)
{
    bool allOk = true;

    GDALRasterBand *gdalBand = poGDAL->GetRasterBand(1);
    GDALRasterBand *mlxBand  = poMLX->GetRasterBand(1);

    Stats gdalStats = computeStats(gdalBand);
    Stats mlxStats = computeStats(mlxBand);

    bool fullOk = checkStats("Full resolution", gdalStats, mlxStats, tolerance);
    if (!fullOk) allOk = false;

    if (std::isnan(gdalStats.min) || std::isnan(gdalStats.max) ||
        std::isnan(mlxStats.min) || std::isnan(mlxStats.max))
    {
        printf("\n[FAIL] NaN detected in full resolution\n");
        allOk = false;
    }

    for (int i = 0; i < gdalBand->GetOverviewCount(); i++)
    {
        char label[32];
        snprintf(label, sizeof(label), "Overview %d", i + 1);

        GDALRasterBand *gdalOvr = gdalBand->GetOverview(i);
        GDALRasterBand *mlxOvr  = mlxBand->GetOverview(i);

        Stats gdalOvrStats = computeStats(gdalOvr);
        Stats mlxOvrStats = computeStats(mlxOvr);

        bool ovrOk = checkStats(label, gdalOvrStats, mlxOvrStats, tolerance);
        if (!ovrOk) allOk = false;

        if (std::isnan(gdalOvrStats.min) || std::isnan(gdalOvrStats.max) ||
            std::isnan(mlxOvrStats.min) || std::isnan(mlxOvrStats.max))
        {
            printf("\n[FAIL] NaN detected in overview %d\n", i + 1);
            allOk = false;
        }
    }

    return allOk;
}

static GDALDataset *buildGDALCOG(GDALDataset *poSrcDS, const char *outPath)
{
    GDALDriver *poMEMDriver =
        GetGDALDriverManager()->GetDriverByName("MEM");
    GDALDataset *poMemDS = poMEMDriver->CreateCopy(
        outPath, poSrcDS, FALSE, nullptr, nullptr, nullptr);
    assert(poMemDS != nullptr);

    std::vector<int> ovrLevels;
    int srcW = poSrcDS->GetRasterXSize();
    int srcH = poSrcDS->GetRasterYSize();
    int factor = 2, w = srcW, h = srcH;
    while (w > 512 || h > 512)
    {
        ovrLevels.push_back(factor);
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        factor *= 2;
    }

    CPLErr eErr = poMemDS->BuildOverviews(
        "AVERAGE", static_cast<int>(ovrLevels.size()), ovrLevels.data(),
        0, nullptr, GDALDummyProgress, nullptr);
    assert(eErr == CE_None);

    return poMemDS;
}

static GDALDataset *buildMLXCOG(GDALDataset *poSrcDS, const char *outPath,
                                ResampleMethod method)
{
    int nBands = poSrcDS->GetRasterCount();
    int srcW   = poSrcDS->GetRasterXSize();
    int srcH   = poSrcDS->GetRasterYSize();

    const char *tmpPath = "/vsimem/check_nan_mlx_tmp.tif";
    GDALDriver *poTiffDriver =
        GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset *poTmpDS = poTiffDriver->CreateCopy(
        tmpPath, poSrcDS, FALSE, nullptr, nullptr, nullptr);
    assert(poTmpDS != nullptr);

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
    eErr = MLXBuildOverviews(poTmpDS, nBands, bandList.data(), method);
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

    printf("=== Check NaN nodata handling ===\n\n");

    GDALDataset *poSrc = static_cast<GDALDataset *>(
        GDALOpen(INPUT, GA_ReadOnly));
    assert(poSrc != nullptr);

    printf("Source: %dx%d, nodata=nan\n\n",
           poSrc->GetRasterXSize(), poSrc->GetRasterYSize());

    // Test AVERAGE with NaN nodata
    printf("-- AVERAGE --\n");
    printf("Building GDAL COG...\n");
    GDALDataset *poGDAL_Avg = buildGDALCOG(poSrc, GDAL_OUT);

    printf("Building MLX COG...\n");
    GDALDataset *poMLX_Avg = buildMLXCOG(poSrc, MLX_OUT, ResampleMethod::AVERAGE);

    printf("Comparing stats (tolerance: %.0f%%)...\n", AVG_TOLERANCE * 100);
    bool avgOk = compareDatasets(poGDAL_Avg, poMLX_Avg, AVG_TOLERANCE);
    GDALClose(poGDAL_Avg);
    GDALClose(poMLX_Avg);

    // --- BILINEAR ---
    printf("\n-- BILINEAR --\n");
    const char *bilArgs[] = {
        "-of", "COG",
        "-co", "COMPRESS=LZW",
        "-co", "OVERVIEWS=AUTO",
        "-co", "OVERVIEW_RESAMPLING=BILINEAR",
        nullptr
    };
    GDALTranslateOptions *bilOpts =
        GDALTranslateOptionsNew(const_cast<char **>(bilArgs), nullptr);
    int err = 0;
    GDALDataset *poGDAL_Bil = static_cast<GDALDataset *>(
        GDALTranslate(GDAL_OUT, poSrc, bilOpts, &err));
    GDALTranslateOptionsFree(bilOpts);
    assert(poGDAL_Bil != nullptr && err == 0);

    printf("Building MLX COG...\n");
    GDALDataset *poMLX_Bil = buildMLXCOG(poSrc, MLX_OUT, ResampleMethod::BILINEAR);

    printf("Comparing stats (tolerance: %.0f%%)...\n", BIL_TOLERANCE * 100);
    bool bilOk = compareDatasets(poGDAL_Bil, poMLX_Bil, BIL_TOLERANCE);
    GDALClose(poGDAL_Bil);
    GDALClose(poMLX_Bil);

    GDALClose(poSrc);
    GDALDeleteDataset(nullptr, GDAL_OUT);
    GDALDeleteDataset(nullptr, MLX_OUT);

    if (!avgOk || !bilOk)
    {
        printf("\n=== NaN nodata checks FAILED ===\n");
        return 1;
    }

    printf("\n=== NaN nodata checks passed (AVERAGE and BILINEAR) ===\n");
    return 0;
}