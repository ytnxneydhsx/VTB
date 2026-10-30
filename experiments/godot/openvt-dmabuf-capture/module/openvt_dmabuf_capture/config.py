import os


def can_build(env, platform):
    return (
        platform == "linuxbsd"
        and bool(os.environ.get("OPENVT_BOARD_SYSROOT"))
        and bool(os.environ.get("OPENVT_FFMPEG_SYSROOT"))
        and bool(os.environ.get("OPENVT_VTB_SOURCE_ROOT"))
        and bool(os.environ.get("OPENVT_OPENCV_ROOT"))
        and bool(os.environ.get("OPENVT_RKNN_ROOT"))
    )


def configure(env):
    pass

