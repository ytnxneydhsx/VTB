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
option(VTB_ENABLE_RGA_FACE
    "Build the Rockchip RGA face image transformer" OFF)
option(VTB_ENABLE_OPENCV_FACE_PIPELINE
    "Build the OpenCV-based OpenSeeFace geometry estimator" OFF)
option(VTB_ENABLE_EMBEDDED_GODOT
    "Build the VTB-owned LibGodot host executable" OFF)
set(VTB_ROCKCHIP_SYSROOT "" CACHE PATH
    "Target-board sysroot containing Rockchip MPP and RGA development files")
set(VTB_RKNN_SYSROOT "" CACHE PATH
    "Target-board sysroot containing rknn_api.h and librknnrt.so")
set(VTB_GODOT_SOURCE_ROOT "" CACHE PATH
    "Godot source root containing core/extension/libgodot.h")
set(VTB_LIBGODOT_LIBRARY "" CACHE FILEPATH
    "ARM64 LibGodot shared library containing the OpenVT bridge")
set(VTB_EMBEDDED_RPATH_LINK "" CACHE STRING
    "Semicolon-separated ARM64 dependency directories for the LibGodot host link")

if(VTB_ENABLE_ROCKCHIP_MPP_PUBLISHER OR VTB_ENABLE_RGA_FACE)
    if(VTB_ROCKCHIP_SYSROOT STREQUAL "")
        message(FATAL_ERROR
            "VTB_ROCKCHIP_SYSROOT is required for Rockchip RGA support")
    endif()
    find_path(VTB_RGA_INCLUDE_DIR rga/im2d.h
        PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/include"
        NO_DEFAULT_PATH
        REQUIRED)
    find_library(VTB_RGA_LIBRARY rga
        PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/lib/aarch64-linux-gnu"
        NO_DEFAULT_PATH
        REQUIRED)
endif()

if(VTB_ENABLE_ROCKCHIP_MPP_PUBLISHER OR VTB_ENABLE_EMBEDDED_GODOT)
    if(VTB_ROCKCHIP_SYSROOT STREQUAL "")
        message(FATAL_ERROR
            "VTB_ROCKCHIP_SYSROOT is required for Rockchip MPP support")
    endif()
    find_path(VTB_MPP_INCLUDE_DIR rockchip/rk_mpi.h
        PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/include"
        NO_DEFAULT_PATH
        REQUIRED)
    find_library(VTB_MPP_LIBRARY rockchip_mpp
        PATHS "${VTB_ROCKCHIP_SYSROOT}/usr/lib/aarch64-linux-gnu"
        NO_DEFAULT_PATH
        REQUIRED)
endif()

function(vtb_configure_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

find_package(Threads REQUIRED)

include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL
        https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH
        SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(vtb_application_config STATIC
    src/config/config_file_reader.cpp
    src/config/vtb_application_config.cpp
)
add_library(vtb::application_config ALIAS vtb_application_config)
target_include_directories(vtb_application_config PUBLIC include)
target_link_libraries(vtb_application_config
    PRIVATE nlohmann_json::nlohmann_json)
vtb_configure_target(vtb_application_config)

add_library(vtb_instrumentation STATIC
    src/instrumentation/metrics_jsonl_reporter.cpp
    src/instrumentation/timer.cpp
)
add_library(vtb::instrumentation ALIAS vtb_instrumentation)
target_include_directories(vtb_instrumentation PUBLIC include)
target_link_libraries(vtb_instrumentation PUBLIC Threads::Threads)
vtb_configure_target(vtb_instrumentation)

add_library(vtb_output_core STATIC
    src/output/av_synchronizer.cpp
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
    src/video/video_stream_control_protocol.cpp
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
    src/face/image_transformer.cpp
    src/face/cpu_image_transformer.cpp
    src/face/face_types.cpp
    src/face/face_pipeline.cpp
    src/face/face_frame_consumer.cpp
)
add_library(vtb::face_core ALIAS vtb_face_core)
target_include_directories(vtb_face_core PUBLIC include)
target_link_libraries(vtb_face_core PUBLIC vtb_video_core)
vtb_configure_target(vtb_face_core)

add_library(vtb_live2d_values STATIC
    src/live2d/live2d_parameter_interpolator.cpp
    src/live2d/live2d_parameters.cpp
    src/live2d/live2d_mapping_result.cpp
)
add_library(vtb::live2d_values ALIAS vtb_live2d_values)
target_include_directories(vtb_live2d_values PUBLIC include)
vtb_configure_target(vtb_live2d_values)

add_library(vtb_live2d_mapper STATIC src/live2d/live2d_face_mapper.cpp)
add_library(vtb::live2d_mapper ALIAS vtb_live2d_mapper)
target_include_directories(vtb_live2d_mapper PUBLIC include)
target_link_libraries(vtb_live2d_mapper PUBLIC vtb_face_core vtb_live2d_values)
vtb_configure_target(vtb_live2d_mapper)

add_library(vtb_render_runtime STATIC
    src/render/godot_runtime.cpp
)
add_library(vtb::render_runtime ALIAS vtb_render_runtime)
target_include_directories(vtb_render_runtime PUBLIC include)
target_link_libraries(vtb_render_runtime PUBLIC vtb_live2d_values)
vtb_configure_target(vtb_render_runtime)

if(VTB_ENABLE_OPENCV_FACE_PIPELINE)
    find_package(OpenCV REQUIRED COMPONENTS
        core
        calib
    )

    add_library(vtb_face_geometry_opencv STATIC
        src/face/openseeface_geometry_estimator.cpp
    )
    add_library(vtb::face_geometry_opencv ALIAS vtb_face_geometry_opencv)
    target_include_directories(vtb_face_geometry_opencv PUBLIC include)
    target_include_directories(vtb_face_geometry_opencv
        SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(vtb_face_geometry_opencv
        PUBLIC vtb_face_core
        PRIVATE ${OpenCV_LIBS}
    )
    vtb_configure_target(vtb_face_geometry_opencv)
endif()

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

if(VTB_ENABLE_RGA_FACE)
    add_library(vtb_face_rga STATIC
        src/face/rga_image_transformer.cpp
    )
    add_library(vtb::face_rga ALIAS vtb_face_rga)
    target_include_directories(vtb_face_rga PUBLIC include)
    target_include_directories(vtb_face_rga
        SYSTEM PRIVATE "${VTB_RGA_INCLUDE_DIR}")
    target_link_libraries(vtb_face_rga
        PUBLIC vtb_face_core
        PRIVATE "${VTB_RGA_LIBRARY}"
    )
    vtb_configure_target(vtb_face_rga)
endif()

add_library(vtb_video_processing_runtime STATIC
    src/pipeline/video_processing_runtime.cpp
)
add_library(vtb::video_processing_runtime ALIAS vtb_video_processing_runtime)
target_include_directories(vtb_video_processing_runtime PUBLIC include)
target_link_libraries(vtb_video_processing_runtime
    PUBLIC
        vtb_video_core
        Threads::Threads
)
vtb_configure_target(vtb_video_processing_runtime)

# Application composition remains buildable without concrete inference backends.
add_library(vtb_live2d_tracking_session STATIC
    apps/vtb-live2d/live2d_tracking_snapshot.cpp
    apps/vtb-live2d/live2d_tracking_session.cpp
)
target_include_directories(vtb_live2d_tracking_session PUBLIC apps/vtb-live2d)
target_link_libraries(vtb_live2d_tracking_session
    PUBLIC vtb_live2d_mapper
    PRIVATE vtb_video_processing_runtime Threads::Threads)
vtb_configure_target(vtb_live2d_tracking_session)

add_library(vtb_live2d_control STATIC apps/vtb-live2d/live2d_control_server.cpp)
target_include_directories(vtb_live2d_control PUBLIC apps/vtb-live2d)
target_link_libraries(vtb_live2d_control
    PUBLIC vtb_application_config
    PRIVATE vtb_live2d_tracking_session Threads::Threads)
vtb_configure_target(vtb_live2d_control)

add_library(vtb_application_video_input STATIC apps/vtb-live2d/video_input_factory.cpp)
target_include_directories(vtb_application_video_input PUBLIC apps/vtb-live2d)
target_link_libraries(vtb_application_video_input
    PUBLIC vtb_application_config vtb_video_core
    PRIVATE vtb_capture)
vtb_configure_target(vtb_application_video_input)

add_library(vtb_media_output_session STATIC apps/vtb-live2d/media_output_session.cpp)
target_include_directories(vtb_media_output_session PUBLIC apps/vtb-live2d)
target_link_libraries(vtb_media_output_session
    PUBLIC vtb_output_core
    PRIVATE vtb_instrumentation)
vtb_configure_target(vtb_media_output_session)

add_executable(vtb src/main.cpp)
target_link_libraries(vtb PRIVATE vtb_capture)
vtb_configure_target(vtb)

if(VTB_ENABLE_EMBEDDED_GODOT)
    if(NOT VTB_ENABLE_OPENCV_FACE_PIPELINE OR NOT VTB_ENABLE_RKNN_FACE OR
       NOT VTB_ENABLE_RGA_FACE OR NOT VTB_ENABLE_RTSP_OUTPUT)
        message(FATAL_ERROR
            "VTB_ENABLE_EMBEDDED_GODOT requires OpenCV, RKNN, RGA face backends and RTSP output")
    endif()
    if(VTB_GODOT_SOURCE_ROOT STREQUAL "")
        message(FATAL_ERROR
            "VTB_GODOT_SOURCE_ROOT is required for the embedded Godot host")
    endif()
    if(VTB_LIBGODOT_LIBRARY STREQUAL "")
        message(FATAL_ERROR
            "VTB_LIBGODOT_LIBRARY is required for the embedded Godot host")
    endif()
    if(NOT EXISTS "${VTB_GODOT_SOURCE_ROOT}/core/extension/libgodot.h")
        message(FATAL_ERROR "VTB_GODOT_SOURCE_ROOT does not contain LibGodot headers")
    endif()
    if(NOT EXISTS "${VTB_LIBGODOT_LIBRARY}")
        message(FATAL_ERROR "VTB_LIBGODOT_LIBRARY does not exist")
    endif()

    add_library(vtb_face_diagnostic_output STATIC
        apps/vtb-live2d/face_diagnostic_session.cpp
    )
    target_include_directories(vtb_face_diagnostic_output
        PUBLIC apps/vtb-live2d)
    target_include_directories(vtb_face_diagnostic_output
        SYSTEM PRIVATE "${VTB_MPP_INCLUDE_DIR}")
    target_link_libraries(vtb_face_diagnostic_output
        PUBLIC
            vtb_application_config
            vtb_face_core
        PRIVATE
            Threads::Threads
            "${VTB_MPP_LIBRARY}"
    )
    vtb_configure_target(vtb_face_diagnostic_output)

    add_executable(vtb-live2d
        apps/vtb-live2d/main.cpp
        apps/vtb-live2d/vtb_application.cpp
    )
    target_include_directories(vtb-live2d
        PRIVATE
            "${VTB_GODOT_SOURCE_ROOT}"
            experiments/godot/embedded-runtime/module
    )
    target_link_libraries(vtb-live2d
        PRIVATE
            vtb_live2d_tracking_session
            vtb_live2d_control
            vtb_media_output_session
            vtb_output_rtsp
            vtb_application_config
            vtb_application_video_input
            vtb_face_rga
            vtb_face_rknn
            vtb_face_geometry_opencv
            vtb_face_diagnostic_output
            "${VTB_LIBGODOT_LIBRARY}"
    )
    target_link_options(vtb-live2d PRIVATE -Wl,--allow-shlib-undefined)
    foreach(dependency_directory IN LISTS VTB_EMBEDDED_RPATH_LINK)
        target_link_options(vtb-live2d PRIVATE
            "-Wl,-rpath-link,${dependency_directory}")
    endforeach()
    set_target_properties(vtb-live2d PROPERTIES
        BUILD_RPATH "$ORIGIN"
        BUILD_RPATH_USE_ORIGIN TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
    )
    vtb_configure_target(vtb-live2d)
endif()

add_executable(camera-info examples/camera-info/main.cpp)
vtb_configure_target(camera-info)

if(BUILD_TESTING)
    add_executable(vtb-application-config-test
        tests/vtb_application_config_test.cpp)
    target_link_libraries(vtb-application-config-test
        PRIVATE vtb_application_config)
    vtb_configure_target(vtb-application-config-test)
    add_test(NAME vtb-application-config COMMAND vtb-application-config-test)

    add_executable(instrumentation-timer-test tests/instrumentation_timer_test.cpp)
    target_link_libraries(instrumentation-timer-test PRIVATE vtb_instrumentation)
    vtb_configure_target(instrumentation-timer-test)
    add_test(NAME instrumentation-timer COMMAND instrumentation-timer-test)

    add_executable(monotonic-clock-test tests/monotonic_clock_test.cpp)
    target_include_directories(monotonic-clock-test PRIVATE include)
    vtb_configure_target(monotonic-clock-test)
    add_test(NAME monotonic-clock COMMAND monotonic-clock-test)

    add_executable(metrics-jsonl-reporter-test tests/metrics_jsonl_reporter_test.cpp)
    target_link_libraries(metrics-jsonl-reporter-test PRIVATE vtb_instrumentation)
    vtb_configure_target(metrics-jsonl-reporter-test)
    add_test(NAME metrics-jsonl-reporter COMMAND metrics-jsonl-reporter-test)

    add_executable(output-manager-test tests/output_manager_test.cpp)
    target_link_libraries(output-manager-test PRIVATE vtb_output_core)
    vtb_configure_target(output-manager-test)
    add_test(NAME output-manager COMMAND output-manager-test)

    add_executable(av-synchronizer-test tests/av_synchronizer_test.cpp)
    target_link_libraries(av-synchronizer-test PRIVATE vtb_output_core)
    vtb_configure_target(av-synchronizer-test)
    add_test(NAME av-synchronizer COMMAND av-synchronizer-test)

    add_executable(godot-runtime-state-machine-test
        tests/godot_runtime_state_machine_test.cpp)
    target_link_libraries(godot-runtime-state-machine-test
        PRIVATE vtb_render_runtime)
    vtb_configure_target(godot-runtime-state-machine-test)
    add_test(NAME godot-runtime-state-machine
        COMMAND godot-runtime-state-machine-test)

    add_executable(live2d-parameter-interpolator-test
        tests/live2d_parameter_interpolator_test.cpp)
    target_link_libraries(live2d-parameter-interpolator-test
        PRIVATE vtb_live2d_values)
    vtb_configure_target(live2d-parameter-interpolator-test)
    add_test(NAME live2d-parameter-interpolator
        COMMAND live2d-parameter-interpolator-test)

    add_executable(video-stream-control-protocol-test
        tests/video_stream_control_protocol_test.cpp)
    target_link_libraries(video-stream-control-protocol-test
        PRIVATE vtb_video_core Threads::Threads)
    vtb_configure_target(video-stream-control-protocol-test)
    add_test(NAME video-stream-control-protocol
        COMMAND video-stream-control-protocol-test)

    add_executable(video-processing-runtime-protocol-test
        tests/video_processing_runtime_protocol_test.cpp)
    target_link_libraries(video-processing-runtime-protocol-test
        PRIVATE vtb_video_processing_runtime Threads::Threads)
    vtb_configure_target(video-processing-runtime-protocol-test)
    add_test(NAME video-processing-runtime-protocol
        COMMAND video-processing-runtime-protocol-test)
endif()
