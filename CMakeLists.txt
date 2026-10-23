cmake_minimum_required(VERSION 3.16)

project(vtb LANGUAGES CXX)

function(vtb_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

add_executable(vtb src/main.cpp)
vtb_configure_target(vtb)

add_executable(camera-info examples/camera-info/main.cpp)
vtb_configure_target(camera-info)
