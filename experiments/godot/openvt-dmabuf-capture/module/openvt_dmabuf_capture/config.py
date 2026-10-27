import os


def can_build(env, platform):
    return platform == "linuxbsd" and bool(os.environ.get("OPENVT_BOARD_SYSROOT"))


def configure(env):
    pass

