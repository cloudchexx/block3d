set(BLOCK3D_LIBRARY_TYPE STATIC)
if(BLOCK3D_BUILD_SHARED)
    set(BLOCK3D_LIBRARY_TYPE SHARED)
endif()

add_library(block3d ${BLOCK3D_LIBRARY_TYPE}
    core.cpp
    reader.cpp
    converter.cpp
    block3d_c_api.cpp
    block3d_cpp_api.cpp
)
add_library(block3d::block3d ALIAS block3d)

target_include_directories(block3d
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(block3d PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(block3d PRIVATE OMP_ENABLED)
endif()

if(WIN32 AND BLOCK3D_BUILD_SHARED)
    set_target_properties(block3d PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()

if(MSVC)
    target_compile_options(block3d PRIVATE
        $<$<CONFIG:Release>:/arch:AVX2 /fp:fast>)
endif()
