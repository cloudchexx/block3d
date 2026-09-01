add_executable(basic_read_slice_c c/basic_read_slice.c)
set_target_properties(basic_read_slice_c PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(basic_read_slice_c PRIVATE block3d::block3d)

add_executable(basic_read_slice_cpp cpp/basic_read_slice.cpp)
target_link_libraries(basic_read_slice_cpp PRIVATE block3d::block3d)

add_executable(api_random_sequential_cpp cpp/api_random_sequential.cpp)
target_link_libraries(api_random_sequential_cpp PRIVATE block3d::block3d)
if(MSVC)
    target_compile_options(api_random_sequential_cpp PRIVATE /utf-8)
endif()
