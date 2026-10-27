#!/usr/bin/env python3

import ctypes
import ctypes.util
import os
import pathlib
import sys


ZSTD_CONTENTSIZE_ERROR = (1 << 64) - 1
ZSTD_CONTENTSIZE_UNKNOWN = (1 << 64) - 2


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} BASE PATCH OUTPUT", file=sys.stderr)
        return 2

    base_path = pathlib.Path(sys.argv[1])
    patch_path = pathlib.Path(sys.argv[2])
    output_path = pathlib.Path(sys.argv[3])

    base = base_path.read_bytes()
    patch = patch_path.read_bytes()

    library_path = ctypes.util.find_library("zstd")
    if library_path is None:
        print("libzstd is unavailable", file=sys.stderr)
        return 3
    zstd = ctypes.CDLL(library_path)
    zstd.ZSTD_getFrameContentSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    zstd.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
    zstd.ZSTD_createDCtx.argtypes = []
    zstd.ZSTD_createDCtx.restype = ctypes.c_void_p
    zstd.ZSTD_freeDCtx.argtypes = [ctypes.c_void_p]
    zstd.ZSTD_freeDCtx.restype = ctypes.c_size_t
    zstd.ZSTD_decompress_usingDict.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    zstd.ZSTD_decompress_usingDict.restype = ctypes.c_size_t
    zstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
    zstd.ZSTD_isError.restype = ctypes.c_uint
    zstd.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
    zstd.ZSTD_getErrorName.restype = ctypes.c_char_p

    patch_buffer = ctypes.create_string_buffer(patch, len(patch))
    output_size = zstd.ZSTD_getFrameContentSize(patch_buffer, len(patch))
    if output_size in (ZSTD_CONTENTSIZE_ERROR, ZSTD_CONTENTSIZE_UNKNOWN):
        print("patch frame does not declare a valid output size", file=sys.stderr)
        return 3

    base_buffer = ctypes.create_string_buffer(base, len(base))
    output_buffer = ctypes.create_string_buffer(output_size)
    context = zstd.ZSTD_createDCtx()
    if not context:
        print("ZSTD_createDCtx failed", file=sys.stderr)
        return 4

    try:
        decoded_size = zstd.ZSTD_decompress_usingDict(
            context,
            output_buffer,
            output_size,
            patch_buffer,
            len(patch),
            base_buffer,
            len(base),
        )
        if zstd.ZSTD_isError(decoded_size):
            error_name = zstd.ZSTD_getErrorName(decoded_size).decode("utf-8", "replace")
            print(f"zstd patch decode failed: {error_name}", file=sys.stderr)
            return 5
        if decoded_size != output_size:
            print(
                f"decoded size mismatch: expected={output_size} actual={decoded_size}",
                file=sys.stderr,
            )
            return 6
    finally:
        zstd.ZSTD_freeDCtx(context)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(output_path.name + ".tmp")
    temporary_path.write_bytes(output_buffer.raw[:output_size])
    os.chmod(temporary_path, base_path.stat().st_mode & 0o777)
    temporary_path.replace(output_path)
    print(f"decoded {output_size} bytes to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
