add_library(block3d_benchmark_cache
    benchmark_cache.cpp
)
target_include_directories(block3d_benchmark_cache
    PUBLIC
        ${PROJECT_SOURCE_DIR}/tools
)

if(BLOCK3D_BUILD_RUN_TEST)
    add_executable(run_test run_test.cpp)
    target_link_libraries(run_test PRIVATE block3d::block3d block3d_benchmark_cache)
endif()
