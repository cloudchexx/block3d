cmake_minimum_required(VERSION 3.14)
project(block3d VERSION 1.0.0 LANGUAGES C CXX)

option(BLOCK3D_BUILD_SHARED "Build block3d as a shared library" OFF)
option(BLOCK3D_BUILD_CLI "Build block3d_cli" ON)
option(BLOCK3D_BUILD_RUN_TEST "Build run_test benchmark tool" ON)
option(BLOCK3D_BUILD_TESTS "Build C/C++ tests" ON)
option(BLOCK3D_BUILD_EXAMPLES "Build examples" ON)
option(BLOCK3D_BUILD_PYTHON "Build Python module" OFF)
option(BLOCK3D_INSTALL "Enable install rules" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Python3 COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep all executables and modules colocated under build/<config>/ even though
# targets now live in subdirectories. test_cli depends on finding block3d_cli
# next to the test executable, and existing docs/scripts use build/Release/*.exe.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(OpenMP)

add_subdirectory(src)

if(BLOCK3D_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BLOCK3D_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BLOCK3D_BUILD_PYTHON)
    add_subdirectory(python)
endif()

if(MSVC)
    set(_block3d_ipo_targets block3d block3d_benchmark_cache)
    if(TARGET block3d_cli)
        list(APPEND _block3d_ipo_targets block3d_cli)
    endif()
    if(TARGET run_test)
        list(APPEND _block3d_ipo_targets run_test)
    endif()
    if(TARGET test_block3d)
        list(APPEND _block3d_ipo_targets test_block3d)
    endif()
    if(TARGET test_cli)
        list(APPEND _block3d_ipo_targets test_cli)
    endif()
    if(TARGET test_c_api)
        list(APPEND _block3d_ipo_targets test_c_api)
    endif()
    if(TARGET test_cpp_api)
        list(APPEND _block3d_ipo_targets test_cpp_api)
    endif()
    if(TARGET basic_read_slice_c)
        list(APPEND _block3d_ipo_targets basic_read_slice_c)
    endif()
    if(TARGET basic_read_slice_cpp)
        list(APPEND _block3d_ipo_targets basic_read_slice_cpp)
    endif()
    if(TARGET api_random_sequential_cpp)
        list(APPEND _block3d_ipo_targets api_random_sequential_cpp)
    endif()
    if(TARGET block3d_python)
        list(APPEND _block3d_ipo_targets block3d_python)
    endif()
    set_target_properties(${_block3d_ipo_targets}
        PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

include(cmake/Block3DInstall.cmake)
