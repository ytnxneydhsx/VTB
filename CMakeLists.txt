cmake_minimum_required(VERSION 3.16)

project(vtb_capture VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(vtb_capture_lib
    src/frame_ring_buffer.cpp
    src/v4l2_capture.cpp
)

target_include_directories(vtb_capture_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(vtb_capture_lib PUBLIC cxx_std_17)
target_compile_options(vtb_capture_lib PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(vtb_capture_lib PUBLIC Threads::Threads)

add_executable(vtb-capture src/main.cpp)
target_compile_features(vtb-capture PRIVATE cxx_std_17)
target_compile_options(vtb-capture PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(vtb-capture PRIVATE vtb_capture_lib)

include(CTest)
if(BUILD_TESTING)
    add_executable(frame-ring-buffer-test tests/frame_ring_buffer_test.cpp)
    target_compile_features(frame-ring-buffer-test PRIVATE cxx_std_17)
    target_compile_options(frame-ring-buffer-test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(frame-ring-buffer-test PRIVATE vtb_capture_lib)
    add_test(NAME frame-ring-buffer-test COMMAND frame-ring-buffer-test)
endif()
