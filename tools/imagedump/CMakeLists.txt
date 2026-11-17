cmake_minimum_required(VERSION 3.25)
project(sc5_imagedump LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue 0.10.0 REQUIRED CONFIG)

add_executable(imagedump main.cpp)
target_link_libraries(imagedump PRIVATE rex::runtime)
