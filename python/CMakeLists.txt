find_package(Python3 COMPONENTS Interpreter Development.Module REQUIRED)
set(PYBIND11_FINDPYTHON ON)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(block3d_python MODULE pybind_module.cpp)
set_target_properties(block3d_python PROPERTIES
    OUTPUT_NAME block3d
    ARCHIVE_OUTPUT_NAME block3d_python)
target_link_libraries(block3d_python PRIVATE block3d::block3d)
