cmake_minimum_required(VERSION 3.16)

project(vtb LANGUAGES CXX)

function(vtb_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

add_library(vtb_capture STATIC
    src/v4l2_camera.cpp
)
target_include_directories(vtb_capture PUBLIC include)
vtb_configure_target(vtb_capture)

add_executable(vtb src/main.cpp)
target_link_libraries(vtb PRIVATE vtb_capture)
vtb_configure_target(vtb)

add_executable(camera-info examples/camera-info/main.cpp)
vtb_configure_target(camera-info)
