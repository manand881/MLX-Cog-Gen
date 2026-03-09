#include <cassert>
#include <cstdio>
#include <vector>

#include <gdal_priv.h>
#include <cpl_string.h>

#include "../src/mlx_overviews.h"

// Expected overview sizes for a given input dimension using ceil(N/2) per level
static std::vector<int> expectedSizes(int n, int levels)
{
    std::vector<int> sizes;
    for (int i = 0; i < levels; i++)
    {
        n = (n + 1) / 2;
        sizes.push_back(n);
    }
    return sizes;
}

// Create an in-memory single-band GTiff of given size, build MLX overviews,
// and verify each overview band has the expected dimensions.
static void testDimensions(int W, int H, int nOvrLevels)
{
    GDALDriver *poDriver =
        GetGDALDriverManager()->GetDriverByName("GTiff");

    char path[64];
    snprintf(path, sizeof(path), "/vsimem/test_%dx%d.tif", W, H);

    GDALDataset *poDS =
        poDriver->Create(path, W, H, 1, GDT_Float32, nullptr);
    assert(poDS != nullptr);

    // Fill with dummy data
    std::vector<float> data(static_cast<size_t>(W) * H, 1.0f);
    poDS->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, W, H,
                                     data.data(), W, H,
                                     GDT_Float32, 0, 0);

    // Build overview structure via GDAL (allocates bands at ceil(N/2) sizes)
    std::vector<int> levels;
    int w = W, h = H;
    for (int i = 0; i < nOvrLevels; i++)
    {
        levels.push_back(1 << (i + 1)); // 2, 4, 8, ...
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    CPLErr eErr = poDS->BuildOverviews("NEAREST",
                                       static_cast<int>(levels.size()),
                                       levels.data(), 0, nullptr,
                                       GDALDummyProgress, nullptr);
    assert(eErr == CE_None);

    // Overwrite with MLX
    int bandList[] = {1};
    eErr = MLXBuildOverviews(poDS, 1, bandList);
    assert(eErr == CE_None);

    // Verify overview dimensions match ceil(N/2) per level
    auto expectedW = expectedSizes(W, nOvrLevels);
    auto expectedH = expectedSizes(H, nOvrLevels);

    GDALRasterBand *poBand = poDS->GetRasterBand(1);
    for (int i = 0; i < nOvrLevels; i++)
    {
        GDALRasterBand *poOvr = poBand->GetOverview(i);
        int oW = poOvr->GetXSize();
        int oH = poOvr->GetYSize();

        if (oW != expectedW[i] || oH != expectedH[i])
        {
            fprintf(stderr,
                    "FAIL [%dx%d] level %d: got %dx%d, expected %dx%d\n",
                    W, H, i + 1, oW, oH, expectedW[i], expectedH[i]);
            assert(false);
        }
    }

    fprintf(stdout, "[PASS] Input %dx%d: %d overview level(s) correct\n",
            W, H, nOvrLevels);

    GDALClose(poDS);
    GDALDeleteDataset(nullptr, path);
}

int main()
{
    GDALAllRegister();

    fprintf(stdout, "=== Overview Dimension Tests ===\n");

    // Even x Even
    testDimensions(512, 512, 1);

    // Even x Odd
    testDimensions(512, 513, 1);

    // Odd x Even
    testDimensions(513, 512, 1);

    // Odd x Odd
    testDimensions(513, 513, 1);

    // Multi-level even
    testDimensions(1024, 1024, 3);

    // Multi-level odd
    testDimensions(1025, 1025, 3);

    // Realistic: matches sample_dem.tif dimensions
    testDimensions(4772, 5125, 4);

    fprintf(stdout, "=== All dimension tests passed ===\n");
    return 0;
}
