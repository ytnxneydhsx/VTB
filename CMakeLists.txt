cmake_minimum_required(VERSION 3.16)

project(vtb LANGUAGES CXX)

include(CTest)

option(VTB_ENABLE_RTSP_OUTPUT "Build the libavformat RTSP output" ON)
option(VTB_ENABLE_UVC_OUTPUT "Build the Linux V4L2 UVC Gadget output" ON)
option(VTB_ENABLE_UAC2_OUTPUT "Build the ALSA UAC2 Gadget output" ON)
option(VTB_BUILD_OUTPUT_CLI "Build the optional output control example" OFF)
option(VTB_ENABLE_ROCKCHIP_MPP_PUBLISHER
    "Build the RK3588 RGA/MPP RTSP publisher" OFF)
set(VTB_ROCKCHIP_SYSROOT "" CACHE PATH
    "Target-board sysroot containing Rockchip MPP and RGA development files")

function(vtb_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

find_package(Threads REQUIRED)

add_library(vtb_instrumentation STATIC
    src/instrumentation/timer.cpp
)
add_library(vtb::instrumentation ALIAS vtb_instrumentation)
target_include_directories(vtb_instrumentation PUBLIC include)
target_link_libraries(vtb_instrumentation PUBLIC Threads::Threads)
vtb_configure_target(vtb_instrumentation)

add_library(vtb_output_core STATIC
    src/output/media.cpp
    src/output/output_manager.cpp
)
add_library(vtb::output_core ALIAS vtb_output_core)
target_include_directories(vtb_output_core PUBLIC include)
target_link_libraries(vtb_output_core PUBLIC Threads::Threads)
vtb_configure_target(vtb_output_core)

add_library(vtb_output INTERFACE)
add_library(vtb::output ALIAS vtb_output)
target_link_libraries(vtb_output INTERFACE vtb_output_core)

if(VTB_ENABLE_RTSP_OUTPUT)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VTB_FFMPEG REQUIRED IMPORTED_TARGET
        libavformat
        libavcodec
        libavutil
    )
    add_library(vtb_output_rtsp STATIC src/output/rtsp_output.cpp)
    add_library(vtb::output_rtsp ALIAS vtb_output_rtsp)
    target_include_directories(vtb_output_rtsp PUBLIC include)
    target_link_libraries(vtb_output_rtsp
        PUBLIC vtb_output_core
        PRIVATE PkgConfig::VTB_FFMPEG
    )
    vtb_configure_target(vtb_output_rtsp)
    target_link_libraries(vtb_output INTERFACE vtb_output_rtsp)

    add_executable(vtb-media-output-publisher
        examples/media-output-publisher/main.cpp
    )
    target_link_libraries(vtb-media-output-publisher
        PRIVATE
            vtb_output_core
            vtb_output_rtsp
            PkgConfig::VTB_FFMPEG
    )
    vtb_configure_target(vtb-media-output-publisher)

    if(VTB_ENABLE_ROCKCHIP_MPP_PUBLISHER)
        if(VTB_ROCKCHIP_SYSROOT STREQUAL "")
            message(FATAL_ERROR
                "VTB_ROCKCHIP_SYSROOT is required for the Rockchip MPP publisher")
        endif()
        find_path(VTB_RGA_INCLUDE_DIR rga/im2d.h
            PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/include"
            NO_DEFAULT_PATH
            REQUIRED)
        find_path(VTB_MPP_INCLUDE_DIR rockchip/rk_mpi.h
            PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/include"
            NO_DEFAULT_PATH
            REQUIRED)
        find_library(VTB_RGA_LIBRARY rga
            PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/lib/aarch64-linux-gnu"
            NO_DEFAULT_PATH
            REQUIRED)
        find_library(VTB_MPP_LIBRARY rockchip_mpp
            PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/lib/aarch64-linux-gnu"
            NO_DEFAULT_PATH
            REQUIRED)

        add_executable(vtb-mpp-output-publisher
            examples/mpp-output-publisher/main.cpp
        )
        target_include_directories(vtb-mpp-output-publisher SYSTEM PRIVATE
            "${VTB_RGA_INCLUDE_DIR}"
            "${VTB_MPP_INCLUDE_DIR}"
        )
        target_link_libraries(vtb-mpp-output-publisher
            PRIVATE
                vtb_output_core
                vtb_output_rtsp
                PkgConfig::VTB_FFMPEG
                "${VTB_RGA_LIBRARY}"
                "${VTB_MPP_LIBRARY}"
        )
        vtb_configure_target(vtb-mpp-output-publisher)
    endif()
endif()

if(VTB_ENABLE_UVC_OUTPUT)
    add_library(vtb_output_uvc STATIC src/output/uvc_output.cpp)
    add_library(vtb::output_uvc ALIAS vtb_output_uvc)
    target_include_directories(vtb_output_uvc PUBLIC include)
    target_link_libraries(vtb_output_uvc PUBLIC vtb_output_core)
    vtb_configure_target(vtb_output_uvc)
    target_link_libraries(vtb_output INTERFACE vtb_output_uvc)
endif()

if(VTB_ENABLE_UAC2_OUTPUT)
    find_package(ALSA REQUIRED)
    add_library(vtb_output_uac2 STATIC src/output/uac2_output.cpp)
    add_library(vtb::output_uac2 ALIAS vtb_output_uac2)
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

if(VTB_BUILD_OUTPUT_CLI)
    if(NOT VTB_ENABLE_RTSP_OUTPUT OR NOT VTB_ENABLE_UVC_OUTPUT OR
       NOT VTB_ENABLE_UAC2_OUTPUT)
        message(FATAL_ERROR
            "VTB_BUILD_OUTPUT_CLI requires the RTSP, UVC and UAC2 outputs")
    endif()
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
    add_executable(instrumentation-timer-test tests/instrumentation_timer_test.cpp)
    target_link_libraries(instrumentation-timer-test PRIVATE vtb_instrumentation)
    vtb_configure_target(instrumentation-timer-test)
    add_test(NAME instrumentation-timer COMMAND instrumentation-timer-test)

    add_executable(output-manager-test tests/output_manager_test.cpp)
    target_link_libraries(output-manager-test PRIVATE vtb_output_core)
    vtb_configure_target(output-manager-test)
    add_test(NAME output-manager COMMAND output-manager-test)
endif()
