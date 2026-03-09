#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <vector>

#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_string.h>
#include <cpl_vsi.h>

#include "../src/mlx_overviews.h"

static const char *INPUT = "tests/sample_dem.tif";
static const char *GDAL_OUT = "/vsimem/test_stats_gdal.tif";
static const char *MLX_OUT  = "/vsimem/test_stats_mlx.tif";
static const float TOLERANCE = 0.05f; // 5%

struct Stats
{
    float min, max, mean, stddev;
};

static Stats computeStats(GDALRasterBand *poBand, float nodataVal,
                           bool hasNodata)
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
        if (hasNodata && v == nodataVal)
            continue;
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

// Check whether two values are within `pct` (e.g. 0.05 = 5%) of each other
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

// Check overview count matches and file sizes are within 5% of each other.
// Overview dimensions intentionally differ by up to 1px per level (MLX uses
// ceil(N/2), GDAL COG driver uses floor(N/2)); count must match exactly.
static void checkStructure(GDALDataset *poGDAL, GDALDataset *poMLX,
                            const char *gdalPath, const char *mlxPath,
                            const char *methodLabel)
{
    printf("  [structure - %s]\n", methodLabel);

    // --- Overview count ---
    int gdalCount = poGDAL->GetRasterBand(1)->GetOverviewCount();
    int mlxCount  = poMLX->GetRasterBand(1)->GetOverviewCount();
    printf("    Overview count: GDAL=%d  MLX=%d\n", gdalCount, mlxCount);
    if (gdalCount != mlxCount)
    {
        fprintf(stderr, "FAIL: overview count mismatch: GDAL=%d MLX=%d\n",
                gdalCount, mlxCount);
        assert(false);
    }
    printf("    [PASS] overview count matches\n");

    // --- File size within 5% ---
    VSIStatBufL stat;
    (void)VSIStatL(gdalPath, &stat);
    vsi_l_offset gdalBytes = stat.st_size;
    (void)VSIStatL(mlxPath, &stat);
    vsi_l_offset mlxBytes = stat.st_size;

    double larger  = static_cast<double>(gdalBytes > mlxBytes ? gdalBytes : mlxBytes);
    double smaller = static_cast<double>(gdalBytes < mlxBytes ? gdalBytes : mlxBytes);
    double diff    = (larger - smaller) / larger;

    printf("    File size: GDAL=%" PRIu64 "B  MLX=%" PRIu64 "B  diff=%.1f%%\n",
           static_cast<uint64_t>(gdalBytes),
           static_cast<uint64_t>(mlxBytes),
           diff * 100.0);

    if (diff > TOLERANCE)
    {
        fprintf(stderr,
                "FAIL: file size difference %.1f%% exceeds %.0f%% tolerance\n",
                diff * 100.0, TOLERANCE * 100.0);
        assert(false);
    }
    printf("    [PASS] file size within %.0f%% tolerance\n", TOLERANCE * 100.0);
}

// Build a GDAL COG via GDALTranslate and return the opened dataset
static GDALDataset *buildGDALCOG(GDALDataset *poSrcDS, const char *outPath)
{
    const char *args[] = {
        "-of", "COG",
        "-co", "COMPRESS=LZW",
        "-co", "OVERVIEWS=AUTO",
        "-co", "OVERVIEW_RESAMPLING=AVERAGE",
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

// Build an MLX COG using our pipeline and return the opened dataset
static GDALDataset *buildMLXCOG(GDALDataset *poSrcDS, const char *outPath,
                                 ResampleMethod method = ResampleMethod::AVERAGE)
{
    int nBands = poSrcDS->GetRasterCount();
    int srcW   = poSrcDS->GetRasterXSize();
    int srcH   = poSrcDS->GetRasterYSize();

    // Create temp in-memory GTiff copy
    const char *tmpPath = "/vsimem/test_stats_mlx_tmp.tif";
    GDALDriver *poTiffDriver =
        GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset *poTmpDS = poTiffDriver->CreateCopy(
        tmpPath, poSrcDS, FALSE, nullptr, nullptr, nullptr);
    assert(poTmpDS != nullptr);

    // Compute overview levels (powers of 2 until smallest fits in 512px tile)
    std::vector<int> ovrLevels;
    int factor = 2, w = srcW, h = srcH;
    while (w > 512 || h > 512)
    {
        ovrLevels.push_back(factor);
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        factor *= 2;
    }

    // Allocate overview structure without CPU resampling
    CPLErr eErr = poTmpDS->BuildOverviews(
        "NONE", static_cast<int>(ovrLevels.size()), ovrLevels.data(),
        0, nullptr, GDALDummyProgress, nullptr);
    assert(eErr == CE_None);

    // Overwrite with MLX GPU-computed overviews
    std::vector<int> bandList(nBands);
    for (int i = 0; i < nBands; i++) bandList[i] = i + 1;
    eErr = MLXBuildOverviews(poTmpDS, nBands, bandList.data(), method);
    assert(eErr == CE_None);

    // Write final COG
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

static void runStatsTest(GDALDataset *poSrcDS, GDALDataset *poGDAL,
                         GDALDataset *poMLX, const char *methodLabel)
{
    int nBands = poSrcDS->GetRasterCount();

    for (int iBand = 1; iBand <= nBands; iBand++)
    {
        GDALRasterBand *poBand = poSrcDS->GetRasterBand(iBand);
        int hasNodata = 0;
        double nodataDouble = poBand->GetNoDataValue(&hasNodata);
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
}

int main()
{
    GDALAllRegister();

    printf("=== COG Stats Comparison Test (tolerance: %.0f%%) ===\n\n",
           TOLERANCE * 100);

    GDALDataset *poSrcDS = static_cast<GDALDataset *>(
        GDALOpen(INPUT, GA_ReadOnly));
    assert(poSrcDS != nullptr);

    // --- AVERAGE ---
    printf("-- AVERAGE --\n");
    GDALDataset *poGDAL = buildGDALCOG(poSrcDS, GDAL_OUT);
    GDALDataset *poMLX  = buildMLXCOG(poSrcDS, MLX_OUT, ResampleMethod::AVERAGE);
    checkStructure(poGDAL, poMLX, GDAL_OUT, MLX_OUT, "AVERAGE");
    runStatsTest(poSrcDS, poGDAL, poMLX, "AVERAGE");
    GDALClose(poGDAL);
    GDALClose(poMLX);
    GDALDeleteDataset(nullptr, GDAL_OUT);
    GDALDeleteDataset(nullptr, MLX_OUT);

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
    poGDAL = static_cast<GDALDataset *>(
        GDALTranslate(GDAL_OUT, poSrcDS, bilOpts, &err));
    GDALTranslateOptionsFree(bilOpts);
    assert(poGDAL != nullptr && err == 0);

    poMLX = buildMLXCOG(poSrcDS, MLX_OUT, ResampleMethod::BILINEAR);
    checkStructure(poGDAL, poMLX, GDAL_OUT, MLX_OUT, "BILINEAR");
    runStatsTest(poSrcDS, poGDAL, poMLX, "BILINEAR");
    GDALClose(poGDAL);
    GDALClose(poMLX);
    GDALDeleteDataset(nullptr, GDAL_OUT);
    GDALDeleteDataset(nullptr, MLX_OUT);

    GDALClose(poSrcDS);

    printf("\n=== All stats tests passed ===\n");
    return 0;
}
