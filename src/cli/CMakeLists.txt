if(BLOCK3D_BUILD_CLI)
    add_executable(block3d_cli cli.cpp)
    target_link_libraries(block3d_cli PRIVATE block3d::block3d block3d_benchmark_cache)
endif()
