cmake_minimum_required(VERSION 3.16)

project(vtb LANGUAGES CXX)

include(CTest)

option(VTB_ENABLE_RTSP_OUTPUT "Build the libavformat RTSP output" ON)
option(VTB_ENABLE_UVC_OUTPUT "Build the Linux V4L2 UVC Gadget output" ON)
option(VTB_ENABLE_UAC2_OUTPUT "Build the ALSA UAC2 Gadget output" ON)
option(VTB_BUILD_OUTPUT_CLI "Build the optional output control example" OFF)
option(VTB_ENABLE_ROCKCHIP_MPP_PUBLISHER
    "Build the RK3588 RGA/MPP RTSP publisher" OFF)
option(VTB_ENABLE_RKNN_FACE
    "Build the RK3588 RKNN OpenSeeFace detector and landmark models" OFF)
set(VTB_ROCKCHIP_SYSROOT "" CACHE PATH
    "Target-board sysroot containing Rockchip MPP and RGA development files")
set(VTB_RKNN_SYSROOT "" CACHE PATH
    "Target-board sysroot containing rknn_api.h and librknnrt.so")

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
                vtb_instrumentation
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
add_library(vtb_video_core STATIC
    src/video/frame.cpp
    src/video/latest_frame_slot.cpp
)
add_library(vtb::video_core ALIAS vtb_video_core)
target_include_directories(vtb_video_core PUBLIC include)
target_link_libraries(vtb_video_core PUBLIC Threads::Threads)
vtb_configure_target(vtb_video_core)

add_library(vtb_capture STATIC
    src/v4l2_camera.cpp
    src/video/camera_source.cpp
)
add_library(vtb::capture ALIAS vtb_capture)
target_include_directories(vtb_capture PUBLIC include)
target_link_libraries(vtb_capture PUBLIC vtb_video_core)
vtb_configure_target(vtb_capture)

add_library(vtb_face_core STATIC
    src/face/model_input.cpp
    src/face/cpu_image_transformer.cpp
    src/face/face_pipeline.cpp
)
add_library(vtb::face_core ALIAS vtb_face_core)
target_include_directories(vtb_face_core PUBLIC include)
target_link_libraries(vtb_face_core PUBLIC vtb_video_core)
vtb_configure_target(vtb_face_core)

if(VTB_ENABLE_RKNN_FACE)
    if(VTB_RKNN_SYSROOT STREQUAL "")
        message(FATAL_ERROR "VTB_RKNN_SYSROOT is required for RKNN face models")
    endif()
    find_path(VTB_RKNN_INCLUDE_DIR rknn_api.h
        PATHS
            "${VTB_RKNN_SYSROOT}/usr/include/rknn"
            "${VTB_RKNN_SYSROOT}/usr/include"
        NO_DEFAULT_PATH
        REQUIRED)
    find_library(VTB_RKNN_LIBRARY rknnrt
        PATHS
            "${VTB_RKNN_SYSROOT}/usr/lib/aarch64-linux-gnu"
            "${VTB_RKNN_SYSROOT}/usr/lib"
        NO_DEFAULT_PATH
        REQUIRED)

    add_library(vtb_face_rknn STATIC
        src/face/rknn_face_models.cpp
    )
    add_library(vtb::face_rknn ALIAS vtb_face_rknn)
    target_include_directories(vtb_face_rknn PUBLIC include)
    target_include_directories(vtb_face_rknn
        SYSTEM PRIVATE "${VTB_RKNN_INCLUDE_DIR}")
    target_link_libraries(vtb_face_rknn
        PUBLIC vtb_face_core
        PRIVATE "${VTB_RKNN_LIBRARY}"
    )
    vtb_configure_target(vtb_face_rknn)
endif()

add_library(vtb_pipeline_runtime STATIC
    src/pipeline/pipeline_runtime.cpp
)
add_library(vtb::pipeline_runtime ALIAS vtb_pipeline_runtime)
target_include_directories(vtb_pipeline_runtime PUBLIC include)
target_link_libraries(vtb_pipeline_runtime
    PUBLIC
        vtb_video_core
        vtb_face_core
        Threads::Threads
)
vtb_configure_target(vtb_pipeline_runtime)

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
