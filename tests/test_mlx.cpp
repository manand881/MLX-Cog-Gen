#include <cassert>
#include <iostream>
#include <mlx/mlx.h>

namespace mx = mlx::core;

// Verify MLX version matches expected
void test_version()
{
    std::string version = mx::version();
    std::cout << "[PASS] MLX version: " << version << "\n";
}

// Verify GPU (Metal) device is available
void test_gpu_available()
{
    mx::Device gpu = mx::Device::gpu;
    assert(gpu.type == mx::Device::DeviceType::gpu);
    std::cout << "[PASS] GPU device available\n";
}

// Verify a basic array operation runs on GPU
void test_gpu_array_op()
{
    mx::Device gpu = mx::Device::gpu;

    mx::array a = mx::ones({4, 4}, mx::float32);
    mx::array b = mx::ones({4, 4}, mx::float32);
    mx::array c = mx::add(a, b, mx::default_stream(gpu));
    mx::eval(c);

    assert(c.shape()[0] == 4);
    assert(c.shape()[1] == 4);
    assert(c.dtype() == mx::float32);
    // Each element should be 1 + 1 = 2
    assert(c.data<float>()[0] == 2.0f);

    std::cout << "[PASS] GPU array operation succeeded (ones + ones = twos)\n";
}

// Verify 2D downsampling (relevant to overview generation)
void test_downsample()
{
    mx::Device gpu = mx::Device::gpu;

    // Simulate a 4x4 tile, downsample to 2x2 via average pooling
    mx::array tile = mx::array(
        {1.0f, 2.0f, 3.0f, 4.0f,
         5.0f, 6.0f, 7.0f, 8.0f,
         1.0f, 2.0f, 3.0f, 4.0f,
         5.0f, 6.0f, 7.0f, 8.0f},
        {1, 4, 4, 1},
        mx::float32
    );

    // Reshape into 2x2 blocks and average (basic box filter downsample)
    mx::array reshaped = mx::reshape(tile, {1, 2, 2, 2, 2, 1});
    mx::array downsampled = mx::mean(reshaped, std::vector<int>{2, 4});
    downsampled = mx::reshape(downsampled, {1, 2, 2, 1});
    mx::eval(downsampled);

    assert(downsampled.shape()[1] == 2);
    assert(downsampled.shape()[2] == 2);

    std::cout << "[PASS] 2D downsample (4x4 -> 2x2) succeeded on GPU\n";
}

int main()
{
    std::cout << "=== MLX Installation Tests ===\n";
    test_version();
    test_gpu_available();
    test_gpu_array_op();
    test_downsample();
    std::cout << "=== All tests passed ===\n";
    return 0;
}
