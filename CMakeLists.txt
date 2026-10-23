cmake_minimum_required(VERSION 3.16)

project(vtb_camera_info LANGUAGES CXX)

add_executable(camera-info src/main.cpp)
target_compile_features(camera-info PRIVATE cxx_std_17)
target_compile_options(camera-info PRIVATE -Wall -Wextra -Wpedantic)
