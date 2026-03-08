#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include <gdal_priv.h>
#include <cpl_string.h>

#include "mlx_overviews.h"

/**
 * Compute overview decimation levels (powers of 2) until the smallest
 * overview fits within a single tile (default 512x512).
 */
static std::vector<int> computeOverviewLevels(int width, int height,
                                              int tileSize = 512)
{
    std::vector<int> levels;
    int factor = 2;
    int w = width;
    int h = height;

    while (w > tileSize || h > tileSize)
    {
        levels.push_back(factor);
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        factor *= 2;
    }

    return levels;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr,
                "Usage: mlx_translate <input.tif> <output_cog.tif> "
                "[-r AVERAGE|BILINEAR] [-co KEY=VALUE ...]\n");
        return 1;
    }

    const char *inputPath = argv[1];
    const char *outputPath = argv[2];

    // Parse -r (resampling method) and -co options
    ResampleMethod resampleMethod = ResampleMethod::AVERAGE;
    char **papszCOOptions = nullptr;
    for (int i = 3; i < argc; i++)
    {
        if (EQUAL(argv[i], "-r") && i + 1 < argc)
        {
            const char *method = argv[++i];
            if (EQUAL(method, "BILINEAR"))
                resampleMethod = ResampleMethod::BILINEAR;
            else if (!EQUAL(method, "AVERAGE"))
            {
                fprintf(stderr, "Unknown resampling method: %s. "
                                "Supported: AVERAGE, BILINEAR\n", method);
                return 1;
            }
        }
        else if (EQUAL(argv[i], "-co") && i + 1 < argc)
        {
            papszCOOptions = CSLAddString(papszCOOptions, argv[++i]);
        }
    }

    // Default compression
    if (!CSLFetchNameValue(papszCOOptions, "COMPRESS"))
        papszCOOptions =
            CSLSetNameValue(papszCOOptions, "COMPRESS", "LZW");

    GDALAllRegister();

    // Open source
    GDALDataset *poSrcDS =
        static_cast<GDALDataset *>(GDALOpen(inputPath, GA_ReadOnly));
    if (!poSrcDS)
    {
        fprintf(stderr, "Failed to open: %s\n", inputPath);
        CSLDestroy(papszCOOptions);
        return 1;
    }

    int nBands = poSrcDS->GetRasterCount();
    int srcW = poSrcDS->GetRasterXSize();
    int srcH = poSrcDS->GetRasterYSize();
    fprintf(stderr, "Input: %s (%dx%d, %d band(s))\n",
            inputPath, srcW, srcH, nBands);

    // Create in-memory temp GTiff copy
    const char *tmpPath = "/vsimem/mlx_translate_tmp.tif";
    GDALDriver *poTiffDriver =
        GetGDALDriverManager()->GetDriverByName("GTiff");
    GDALDataset *poTmpDS = poTiffDriver->CreateCopy(
        tmpPath, poSrcDS, FALSE, nullptr, nullptr, nullptr);
    if (!poTmpDS)
    {
        fprintf(stderr, "Failed to create temp dataset\n");
        GDALClose(poSrcDS);
        CSLDestroy(papszCOOptions);
        return 1;
    }

    // Compute overview levels
    std::vector<int> ovrLevels = computeOverviewLevels(srcW, srcH);

    if (ovrLevels.empty())
    {
        fprintf(stderr, "Input is too small to require overviews\n");
    }
    else
    {
        fprintf(stderr, "Overview levels: ");
        for (int l : ovrLevels)
            fprintf(stderr, "%d ", l);
        fprintf(stderr, "\n");

        // Step 1: Call GDAL BuildOverviews with "NONE" to allocate overview
        // band structure without any CPU resampling. "NONE" is handled in
        // GDALRegenerateOverviewsEx() (overview.cpp) as an immediate return —
        // the TIFF IFDs are created but no pixel data is computed.
        // MLX overwrites the slots in step 2.
        CPLErr eErr = poTmpDS->BuildOverviews(
            "NONE", static_cast<int>(ovrLevels.size()),
            ovrLevels.data(), 0, nullptr, GDALDummyProgress, nullptr);
        if (eErr != CE_None)
        {
            fprintf(stderr, "Failed to allocate overview structure\n");
            GDALClose(poTmpDS);
            GDALClose(poSrcDS);
            CSLDestroy(papszCOOptions);
            return 1;
        }

        // Step 2: Overwrite overview data with MLX GPU-computed values
        fprintf(stderr, "Running MLX overview generation on GPU...\n");
        std::vector<int> bandList(nBands);
        for (int i = 0; i < nBands; i++)
            bandList[i] = i + 1;

        eErr = MLXBuildOverviews(poTmpDS, nBands, bandList.data(), resampleMethod);
        if (eErr != CE_None)
        {
            fprintf(stderr, "MLX overview generation failed\n");
            GDALClose(poTmpDS);
            GDALClose(poSrcDS);
            CSLDestroy(papszCOOptions);
            return 1;
        }
    }

    // Write final COG using the COG driver with pre-built overviews
    papszCOOptions =
        CSLSetNameValue(papszCOOptions, "OVERVIEWS", "FORCE_USE_EXISTING");

    GDALDriver *poCOGDriver =
        GetGDALDriverManager()->GetDriverByName("COG");
    if (!poCOGDriver)
    {
        fprintf(stderr, "COG driver not available\n");
        GDALClose(poTmpDS);
        GDALClose(poSrcDS);
        CSLDestroy(papszCOOptions);
        return 1;
    }

    fprintf(stderr, "Writing COG: %s\n", outputPath);
    GDALDataset *poCOGDS = poCOGDriver->CreateCopy(
        outputPath, poTmpDS, FALSE, papszCOOptions,
        GDALTermProgress, nullptr);

    if (!poCOGDS)
    {
        fprintf(stderr, "Failed to write COG output\n");
        GDALClose(poTmpDS);
        GDALClose(poSrcDS);
        CSLDestroy(papszCOOptions);
        return 1;
    }

    // Note: OVERVIEW_RESAMPLING is not written to IMAGE_STRUCTURE metadata.
    // The COG driver only stores it when it generates overviews itself.
    // With FORCE_USE_EXISTING, GDAL ignores -co OVERVIEW_RESAMPLING and
    // SetMetadataItem("...", "IMAGE_STRUCTURE") does not persist in GTiff.

    // Cleanup
    GDALClose(poCOGDS);
    GDALClose(poTmpDS);
    GDALClose(poSrcDS);
    GDALDeleteDataset(nullptr, tmpPath);
    CSLDestroy(papszCOOptions);

    fprintf(stderr, "Done.\n");
    return 0;
}
