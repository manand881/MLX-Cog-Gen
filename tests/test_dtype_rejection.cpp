#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <gdal_priv.h>
#include <cpl_string.h>

#ifndef MLX_TRANSLATE_BIN
#error "MLX_TRANSLATE_BIN must be defined by CMake"
#endif

// Creates a single-band raster of the given type at path (on disk)
static void createRaster(const char *path, GDALDataType dt)
{
    GDALDriver *drv = GetGDALDriverManager()->GetDriverByName("GTiff");
    assert(drv != nullptr);
    GDALDataset *ds = drv->Create(path, 1024, 1024, 1, dt, nullptr);
    assert(ds != nullptr);
    GDALClose(ds);
}

static int runTranslate(const char *inputPath, const char *outputPath)
{
    std::string cmd = std::string(MLX_TRANSLATE_BIN)
        + " " + inputPath
        + " " + outputPath
        + " 2>/dev/null";
    return std::system(cmd.c_str());
}

int main()
{
    GDALAllRegister();

    // --- Non-Float32 types must be rejected ---
    const struct { GDALDataType dt; const char *name; } badTypes[] = {
        { GDT_Byte,    "byte"    },
        { GDT_UInt16,  "uint16"  },
        { GDT_Int16,   "int16"   },
        { GDT_Int32,   "int32"   },
        { GDT_Float64, "float64" },
    };

    for (const auto &tc : badTypes)
    {
        char inPath[64], outPath[64];
        snprintf(inPath,  sizeof(inPath),  "/tmp/test_dtype_%s.tif", tc.name);
        snprintf(outPath, sizeof(outPath), "/tmp/test_dtype_%s_out.tif", tc.name);

        createRaster(inPath, tc.dt);
        int rc = runTranslate(inPath, outPath);
        int exitCode = WEXITSTATUS(rc);

        printf("  dtype=%s exit=%d", tc.name, exitCode);
        if (exitCode != 1)
        {
            printf("  [FAIL] expected exit 1\n");
            assert(false);
        }
        printf("  [PASS]\n");

        VSIUnlink(inPath);
        VSIUnlink(outPath);
    }

    // --- Float32 must be accepted (exit 0) ---
    const char *fp32In  = "/tmp/test_dtype_fp32.tif";
    const char *fp32Out = "/tmp/test_dtype_fp32_out.tif";
    createRaster(fp32In, GDT_Float32);
    int rc = runTranslate(fp32In, fp32Out);
    int exitCode = WEXITSTATUS(rc);
    printf("  dtype=float32 exit=%d", exitCode);
    if (exitCode != 0)
    {
        printf("  [FAIL] expected exit 0\n");
        assert(false);
    }
    printf("  [PASS]\n");
    VSIUnlink(fp32In);
    VSIUnlink(fp32Out);

    printf("\n=== dtype rejection tests passed ===\n");
    return 0;
}
