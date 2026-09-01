add_executable(test_block3d test_block3d.cpp)
target_link_libraries(test_block3d PRIVATE block3d::block3d)
add_test(NAME test_block3d COMMAND test_block3d)

if(BLOCK3D_BUILD_CLI)
    # CLI integration / regression test: drives the real block3d_cli binary
    # to verify argument parsing and graceful error handling, not just the library API.
    add_executable(test_cli test_cli.cpp)
    add_dependencies(test_cli block3d_cli)
    add_test(NAME test_cli COMMAND test_cli)
endif()

add_executable(test_c_api api/test_c_api.c)
set_target_properties(test_c_api PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(test_c_api PRIVATE block3d::block3d)
add_test(NAME test_c_api COMMAND test_c_api)

add_executable(test_cpp_api api/test_cpp_api.cpp)
target_link_libraries(test_cpp_api PRIVATE block3d::block3d)
add_test(NAME test_cpp_api COMMAND test_cpp_api)

if(Python3_Interpreter_FOUND)
    add_test(NAME test_dispatch_compare_tool
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/compare_dispatch_ab.py
            --round-robin ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch_rr_1.log
                          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch_rr_2.log
                          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch_rr_3.log
            --contiguous ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch_contig_1.log
                         ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch_contig_2.log
                         ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/dispatch_contig_3.log)
endif()
