cmake_minimum_required(VERSION 3.14)
project(block3d_consumer LANGUAGES CXX)

find_package(block3d CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE block3d::block3d)
