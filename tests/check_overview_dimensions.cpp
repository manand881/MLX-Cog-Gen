// Check overview dimensions
//
// Agenda
// ------
// For every supported resampling method (AVERAGE, BILINEAR), MLX overview
// band widths and heights must equal GDAL's overview band widths and heights
// at every level, for the same input size and the same overview factors.
//
// Why it matters
// --------------
// MLX does not allocate overview IFDs itself. Production allocates structure
// with BuildOverviews("NONE", factors) then fills pixels with MLXBuildOverviews.
// Dimensions come from GDAL's BuildOverviews size rules. If MLX ever diverged
// (wrong factor chain, wrong odd-size handling, method-dependent size bugs),
// COG structure and geographic alignment would break silently.
//
// What we compare against
// -----------------------
// Reference: GDALDataset::BuildOverviews("<METHOD>", factors) for AVERAGE and
// BILINEAR. That is the same overview-size path as BuildOverviews("NONE").
//
// Not compared here: gdal_translate -of COG -co OVERVIEWS=AUTO, which uses
// floor(N/2) and can differ by 1 px per level. That is a known COG-driver
// difference (see docs/known-issues.md), not a resampling-method difference.
//
// Pass criteria
// -------------
// Overview count equal; each level width and height equal. Exact match only.

#include <cassert>
#include <cstdio>
#include <vector>

#include <gdal_priv.h>
#include <cpl_string.h>

#include "../src/mlx_overviews.h"

static const char *methodName(ResampleMethod method)
{
    return method == ResampleMethod::BILINEAR ? "BILINEAR" : "AVERAGE";
}

static const char *gdalResampling(ResampleMethod method)
{
    return method == ResampleMethod::BILINEAR ? "BILINEAR" : "AVERAGE";
}

static std::vector<int> overviewFactors(int nOvrLevels)
{
    std::vector<int> levels;
    for (int i = 0; i < nOvrLevels; i++)
        levels.push_back(1 << (i + 1)); // 2, 4, 8, ...
    return levels;
}

static GDALDataset *createFilledRaster(const char *path, int W, int H)
{
    GDALDriver *poDriver =
        GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset *poDS =
        poDriver->Create(path, W, H, 1, GDT_Float32, nullptr);
    assert(poDS != nullptr);

    std::vector<float> data(static_cast<size_t>(W) * H, 1.0f);
    CPLErr eErr = poDS->GetRasterBand(1)->RasterIO(
        GF_Write, 0, 0, W, H, data.data(), W, H, GDT_Float32, 0, 0);
    assert(eErr == CE_None);
    return poDS;
}

// GDAL reference: full BuildOverviews with the supported resampling method.
static GDALDataset *buildGdalOverviews(int W, int H, int nOvrLevels,
                                       ResampleMethod method)
{
    char path[80];
    snprintf(path, sizeof(path), "/vsimem/ovr_gdal_%s_%dx%d.tif",
             methodName(method), W, H);

    GDALDataset *poDS = createFilledRaster(path, W, H);
    std::vector<int> levels = overviewFactors(nOvrLevels);

    CPLErr eErr = poDS->BuildOverviews(
        gdalResampling(method), static_cast<int>(levels.size()),
        levels.data(), 0, nullptr, GDALDummyProgress, nullptr);
    assert(eErr == CE_None);
    return poDS;
}

// MLX path: same factors as production (NONE allocate + MLX fill).
static GDALDataset *buildMlxOverviews(int W, int H, int nOvrLevels,
                                      ResampleMethod method)
{
    char path[80];
    snprintf(path, sizeof(path), "/vsimem/ovr_mlx_%s_%dx%d.tif",
             methodName(method), W, H);

    GDALDataset *poDS = createFilledRaster(path, W, H);
    std::vector<int> levels = overviewFactors(nOvrLevels);

    CPLErr eErr = poDS->BuildOverviews(
        "NONE", static_cast<int>(levels.size()), levels.data(),
        0, nullptr, GDALDummyProgress, nullptr);
    assert(eErr == CE_None);

    int bandList[] = {1};
    eErr = MLXBuildOverviews(poDS, 1, bandList, method);
    assert(eErr == CE_None);
    return poDS;
}

// Exact dimension parity: count and every level's width/height.
static bool compareOverviewDims(GDALDataset *poGdal, GDALDataset *poMlx,
                                int W, int H, ResampleMethod method)
{
    GDALRasterBand *gdalBand = poGdal->GetRasterBand(1);
    GDALRasterBand *mlxBand = poMlx->GetRasterBand(1);

    int gdalCount = gdalBand->GetOverviewCount();
    int mlxCount = mlxBand->GetOverviewCount();

    if (gdalCount != mlxCount)
    {
        fprintf(stderr,
                "FAIL [%dx%d %s] overview count: GDAL=%d MLX=%d\n",
                W, H, methodName(method), gdalCount, mlxCount);
        return false;
    }

    bool ok = true;
    for (int i = 0; i < gdalCount; i++)
    {
        GDALRasterBand *gdalOvr = gdalBand->GetOverview(i);
        GDALRasterBand *mlxOvr = mlxBand->GetOverview(i);
        int gW = gdalOvr->GetXSize();
        int gH = gdalOvr->GetYSize();
        int mW = mlxOvr->GetXSize();
        int mH = mlxOvr->GetYSize();

        if (gW != mW || gH != mH)
        {
            fprintf(stderr,
                    "FAIL [%dx%d %s] level %d: GDAL=%dx%d MLX=%dx%d\n",
                    W, H, methodName(method), i + 1, gW, gH, mW, mH);
            ok = false;
        }
        else
        {
            fprintf(stdout,
                    "  level %d: %dx%d (GDAL == MLX)\n",
                    i + 1, gW, gH);
        }
    }
    return ok;
}

static bool testDimensions(int W, int H, int nOvrLevels,
                           ResampleMethod method)
{
    fprintf(stdout, "[%s] Input %dx%d, %d level(s)\n",
            methodName(method), W, H, nOvrLevels);

    GDALDataset *poGdal = buildGdalOverviews(W, H, nOvrLevels, method);
    GDALDataset *poMlx = buildMlxOverviews(W, H, nOvrLevels, method);

    bool ok = compareOverviewDims(poGdal, poMlx, W, H, method);

    char gdalPath[80], mlxPath[80];
    snprintf(gdalPath, sizeof(gdalPath), "/vsimem/ovr_gdal_%s_%dx%d.tif",
             methodName(method), W, H);
    snprintf(mlxPath, sizeof(mlxPath), "/vsimem/ovr_mlx_%s_%dx%d.tif",
             methodName(method), W, H);

    GDALClose(poGdal);
    GDALClose(poMlx);
    GDALDeleteDataset(nullptr, gdalPath);
    GDALDeleteDataset(nullptr, mlxPath);

    if (ok)
        fprintf(stdout, "  [PASS] dimensions match GDAL %s exactly\n",
                methodName(method));
    return ok;
}

int main()
{
    GDALAllRegister();

    fprintf(stdout,
            "=== Overview Dimension Tests ===\n"
            "Compare MLX overview sizes to GDAL for AVERAGE and BILINEAR.\n"
            "Pass only on exact width/height match at every level.\n\n");

    struct Case
    {
        int W, H, nOvrLevels;
        const char *label;
    };

    const Case cases[] = {
        {512, 512, 1, "even x even"},
        {512, 513, 1, "even x odd"},
        {513, 512, 1, "odd x even"},
        {513, 513, 1, "odd x odd"},
        {1024, 1024, 3, "multi-level even"},
        {1025, 1025, 3, "multi-level odd"},
        {4772, 5125, 4, "sample_dem.tif size"},
    };

    const ResampleMethod methods[] = {
        ResampleMethod::AVERAGE,
        ResampleMethod::BILINEAR,
    };

    bool allOk = true;
    for (ResampleMethod method : methods)
    {
        fprintf(stdout, "-- %s --\n", methodName(method));
        for (const Case &c : cases)
        {
            fprintf(stdout, "(%s)\n", c.label);
            if (!testDimensions(c.W, c.H, c.nOvrLevels, method))
                allOk = false;
        }
        fprintf(stdout, "\n");
    }

    if (!allOk)
    {
        fprintf(stderr, "=== Overview dimension tests FAILED ===\n");
        return 1;
    }

    fprintf(stdout, "=== All dimension tests passed ===\n");
    return 0;
}
