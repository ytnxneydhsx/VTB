cmake_minimum_required(VERSION 3.21)
project(vtb_qt LANGUAGES CXX OBJCXX)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VTB_PREVIEW_FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
find_library(VTB_AVFOUNDATION AVFoundation REQUIRED)
find_library(VTB_COREMEDIA CoreMedia REQUIRED)
find_library(VTB_QUARTZCORE QuartzCore REQUIRED)
include(FetchContent)
set(NO_EXAMPLES ON)
set(NO_TESTS ON)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libdatachannel)
set(CMAKE_AUTOMOC ON)

qt_add_executable(vtb-qt MACOSX_BUNDLE
    src/main.cc
    src/board_client.cc
    src/board_client.hh
    src/board_monitor.cc
    src/board_monitor.hh
    src/board_monitor_page.cc
    src/video_preview.cc
    src/mac_preview.mm
    src/rtsp_receiver.cc
    src/webrtc_receiver.cc
    src/theme.cc
    src/widgets.cc
    src/main_window.cc
)
qt_add_resources(vtb-qt board_protocol
    PREFIX /vtb
    BASE board
    FILES board/control.py
)
target_compile_features(vtb-qt PRIVATE cxx_std_20)
target_compile_definitions(vtb-qt PRIVATE QT_NO_KEYWORDS)
target_link_libraries(vtb-qt PRIVATE Qt6::Widgets Qt6::Network PkgConfig::VTB_PREVIEW_FFMPEG
    LibDataChannel::LibDataChannel
    "${VTB_AVFOUNDATION}" "${VTB_COREMEDIA}" "${VTB_QUARTZCORE}")
target_compile_options(vtb-qt PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
set_target_properties(vtb-qt PROPERTIES
    OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER org.vtb.desktop
    MACOSX_BUNDLE_BUNDLE_NAME "VTB Qt"
)
