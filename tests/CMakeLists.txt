add_executable(test_mlx test_mlx.cpp)
target_link_libraries(test_mlx PRIVATE mlx)
add_test(NAME test_mlx COMMAND test_mlx)

add_executable(test_overview_dims test_overview_dims.cpp)
target_link_libraries(test_overview_dims PRIVATE mlx_overviews)
add_test(NAME test_overview_dims COMMAND test_overview_dims)

add_executable(test_cog_stats test_cog_stats.cpp)
target_link_libraries(test_cog_stats PRIVATE mlx_overviews)
add_test(NAME test_cog_stats COMMAND test_cog_stats
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
