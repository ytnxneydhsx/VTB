cmake_minimum_required(VERSION 3.16)

project(vtb LANGUAGES CXX)

include(CTest)

option(VTB_ENABLE_RTSP_OUTPUT "Build the libavformat RTSP output" ON)
option(VTB_ENABLE_UVC_OUTPUT "Build the Linux V4L2 UVC Gadget output" ON)
option(VTB_ENABLE_UAC2_OUTPUT "Build the ALSA UAC2 Gadget output" ON)

function(vtb_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

find_package(Threads REQUIRED)

add_library(vtb_output_core STATIC
    src/output/media.cpp
    src/output/output_manager.cpp
)
target_include_directories(vtb_output_core PUBLIC include)
target_link_libraries(vtb_output_core PUBLIC Threads::Threads)
vtb_configure_target(vtb_output_core)

add_library(vtb_output INTERFACE)
target_link_libraries(vtb_output INTERFACE vtb_output_core)

if(VTB_ENABLE_RTSP_OUTPUT)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VTB_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
    )
    add_library(vtb_output_rtsp STATIC src/output/rtsp_output.cpp)
    target_include_directories(vtb_output_rtsp PUBLIC include)
    target_link_libraries(vtb_output_rtsp
        PUBLIC vtb_output_core
        PRIVATE PkgConfig::VTB_FFMPEG
    )
    vtb_configure_target(vtb_output_rtsp)
    target_link_libraries(vtb_output INTERFACE vtb_output_rtsp)
endif()

if(VTB_ENABLE_UVC_OUTPUT)
    add_library(vtb_output_uvc STATIC src/output/uvc_output.cpp)
    target_include_directories(vtb_output_uvc PUBLIC include)
    target_link_libraries(vtb_output_uvc PUBLIC vtb_output_core)
    vtb_configure_target(vtb_output_uvc)
    target_link_libraries(vtb_output INTERFACE vtb_output_uvc)
endif()

if(VTB_ENABLE_UAC2_OUTPUT)
    find_package(ALSA REQUIRED)
    add_library(vtb_output_uac2 STATIC src/output/uac2_output.cpp)
    target_include_directories(vtb_output_uac2
        PUBLIC
            include
            ${ALSA_INCLUDE_DIRS}
    )
    target_link_libraries(vtb_output_uac2
        PUBLIC vtb_output_core
        PRIVATE ${ALSA_LIBRARIES}
    )
    vtb_configure_target(vtb_output_uac2)
    target_link_libraries(vtb_output INTERFACE vtb_output_uac2)
endif()

if(VTB_ENABLE_RTSP_OUTPUT AND VTB_ENABLE_UVC_OUTPUT AND VTB_ENABLE_UAC2_OUTPUT)
    add_library(vtb_output_cli STATIC src/output/output_cli.cpp)
    target_include_directories(vtb_output_cli PUBLIC include)
    target_link_libraries(vtb_output_cli PUBLIC vtb_output)
    vtb_configure_target(vtb_output_cli)

    add_executable(vtb-output-cli examples/output-cli/main.cpp)
    target_link_libraries(vtb-output-cli PRIVATE vtb_output_cli)
    vtb_configure_target(vtb-output-cli)
endif()

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

if(BUILD_TESTING)
    add_executable(output-manager-test tests/output_manager_test.cpp)
    target_link_libraries(output-manager-test PRIVATE vtb_output_core)
    vtb_configure_target(output-manager-test)
    add_test(NAME output-manager COMMAND output-manager-test)
endif()
