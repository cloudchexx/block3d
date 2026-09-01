if(NOT BLOCK3D_INSTALL)
    return()
endif()

set(BLOCK3D_CONFIG_FIND_OPENMP "")
if(OpenMP_CXX_FOUND)
    set(BLOCK3D_CONFIG_FIND_OPENMP "find_dependency(OpenMP)")
endif()

install(TARGETS block3d
    EXPORT block3dTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(TARGET block3d_cli)
    install(TARGETS block3d_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET run_test)
    install(TARGETS run_test
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET block3d_python)
    install(TARGETS block3d_python
        LIBRARY DESTINATION .
        RUNTIME DESTINATION .)
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/block3d
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp")

install(EXPORT block3dTargets
    NAMESPACE block3d::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/block3d)

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/block3dConfig.cmake.in
    ${PROJECT_BINARY_DIR}/block3dConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/block3d)

write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/block3dConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${PROJECT_BINARY_DIR}/block3dConfig.cmake
    ${PROJECT_BINARY_DIR}/block3dConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/block3d)
