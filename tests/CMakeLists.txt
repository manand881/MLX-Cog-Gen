add_executable(check_mlx_availability check_mlx_availability.cpp)
target_link_libraries(check_mlx_availability PRIVATE mlx)
add_test(NAME check_mlx_availability COMMAND check_mlx_availability)

add_executable(check_overview_dimensions check_overview_dimensions.cpp)
target_link_libraries(check_overview_dimensions PRIVATE mlx_overviews)
add_test(NAME check_overview_dimensions COMMAND check_overview_dimensions)

add_executable(check_cog_statistics check_cog_statistics.cpp)
target_link_libraries(check_cog_statistics PRIVATE mlx_overviews)
add_test(NAME check_cog_statistics COMMAND check_cog_statistics
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(check_dtype_rejection check_dtype_rejection.cpp)
target_link_libraries(check_dtype_rejection PRIVATE ${GDAL_LIBRARIES})
target_include_directories(check_dtype_rejection PRIVATE ${GDAL_INCLUDE_DIRS})
target_compile_definitions(check_dtype_rejection PRIVATE
    MLX_TRANSLATE_BIN="$<TARGET_FILE:mlx_translate>")
add_test(NAME check_dtype_rejection COMMAND check_dtype_rejection)

add_executable(check_nan_nodata check_nan_nodata.cpp)
target_link_libraries(check_nan_nodata PRIVATE mlx_overviews)
add_test(NAME check_nan_nodata COMMAND check_nan_nodata
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
