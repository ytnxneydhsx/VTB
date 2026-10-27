cmake_minimum_required(VERSION 3.20)

project(dmabuf_rga_mpp_probe LANGUAGES CXX)

add_executable(dmabuf-rga-mpp-probe main.cpp)

target_compile_features(dmabuf-rga-mpp-probe PRIVATE cxx_std_20)
target_compile_options(dmabuf-rga-mpp-probe PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_include_directories(dmabuf-rga-mpp-probe SYSTEM PRIVATE
    "${BOARD_SYSROOT}/usr/include"
    "${BOARD_SYSROOT}/usr/include/rockchip"
    "${BOARD_SYSROOT}/usr/include/rga"
    "${KHRONOS_HEADERS}"
)

target_link_directories(dmabuf-rga-mpp-probe PRIVATE
    "${BOARD_SYSROOT}/usr/lib/aarch64-linux-gnu"
    "${BOARD_SYSROOT}/usr/lib/aarch64-linux-gnu/mali"
    "${BOARD_SYSROOT}/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu"
)

target_link_libraries(dmabuf-rga-mpp-probe PRIVATE
    EGL
    GLESv2
    "${BOARD_SYSROOT}/opt/rockchip/libmali-g13p0-x11-gbm-1.9-1/usr/lib/aarch64-linux-gnu/libmali.so.1.9.0"
    rga
    rockchip_mpp
    dl
    pthread
)
