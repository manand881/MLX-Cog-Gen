cmake_minimum_required(VERSION 3.20)
project(mlx-cog-gen CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(GDAL REQUIRED)
find_package(MLX REQUIRED)

# Shared library of MLX overview logic, used by mlx_translate and tests
add_library(mlx_overviews STATIC
    src/mlx_overviews.cpp
)

target_include_directories(mlx_overviews PUBLIC
    ${GDAL_INCLUDE_DIRS}
    src
)

target_link_libraries(mlx_overviews PUBLIC
    ${GDAL_LIBRARIES}
    mlx
)

# mlx_translate: COG generator with MLX-accelerated overview generation
add_executable(mlx_translate
    src/mlx_translate.cpp
)

target_include_directories(mlx_translate PRIVATE src)

target_link_libraries(mlx_translate PRIVATE mlx_overviews)

# Tests
enable_testing()
add_subdirectory(tests)
