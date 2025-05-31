import os
import re
import subprocess
import shlex
import sys
from typing import Optional, MutableMapping, Any
from setuptools import Extension

from .cmake_if import parse_config
from distutils.errors import DistutilsSetupError

import logging

_logger_cmake_ext = logging.getLogger("setup_ext.CmakeExt")

# Convert distutils Windows platform specifiers to CMake -A arguments
PLAT_TO_CMAKE = {
    "win32": "Win32",
    "win-amd64": "x64",
    "win-arm32": "ARM",
    "win-arm64": "ARM64",
}


def _log_subprocess_output(pipe):
    for line in iter(pipe.readline, b""):  # b'\n'-separated lines
        _logger_cmake_ext.info("\t%s", line.decode("utf-8").rstrip("\n"))


# A CMakeExtension needs a sourcedir instead of a file list.
# The name must be the _single_ output extension from the CMake build.
# If you need multiple extensions, see scikit-build.
class CMakeExtension(Extension):
    def __init__(
        self,
        name: str,
        sourcedir: str = "",
        cmake_generator: Optional[str] = None,
        cmake_configure_argdef: Optional[MutableMapping[str, str]] = None,
        cmake_build_argdef: Optional[MutableMapping[str, str]] = None,
        extra_lib: Optional[dict[str, str]] = None,
    ):
        Extension.__init__(self, name, sources=[])

        self.sourcedir = os.path.abspath(sourcedir)

        self.cmake_generator = cmake_generator

        if cmake_configure_argdef is None:
            cmake_configure_argdef = {}
        self.cmake_configure_argdef = cmake_configure_argdef

        if cmake_build_argdef is None:
            cmake_build_argdef = {}
        self.cmake_build_argdef = cmake_build_argdef

        if extra_lib is None:
            extra_lib = {}
        self.extra_lib = extra_lib  # used in copy back library when setup develop


def build_extension(
    ext: CMakeExtension,
    extdir: str,
    build_temp: str,
    compiler: Any,
    debug: Any,
    plat_name: Any,
    parallel: Optional[Any],
):
    _logger_cmake_ext.info("build cmake ext: %s >>>", ext.name)

    cmake_arg, build_arg, install_arg = parse_config(
        installdir=extdir,
        cmake_generator=ext.cmake_generator,
        cmake_configure_argdef=ext.cmake_configure_argdef,
        cmake_build_argdef=ext.cmake_build_argdef,
        compiler=compiler,
        debug=debug,
        plat_name=plat_name,
        parallel=parallel,
    )

    # create build dir
    build_temp = os.path.join(build_temp, ext.name)
    if os.path.exists(build_temp):
        # if os.path.exists(os.path.join(build_temp, "CMakeCache.txt")):
        #     os.remove(
        #         os.path.join(build_temp, "CMakeCache.txt")
        #     )  # the cached environment has gone, so remove the cache
        pass
    else:
        os.makedirs(build_temp)

    _logger_cmake_ext.info("> working dir: %s", build_temp)

    _logger_cmake_ext.info("> configure: %s", shlex.join(["cmake", ext.sourcedir] + cmake_arg))
    configure_process = subprocess.Popen(
        ["cmake", ext.sourcedir] + cmake_arg, cwd=build_temp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    with configure_process.stdout:
        _log_subprocess_output(configure_process.stdout)
    ret = configure_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to configure ext!")

    _logger_cmake_ext.info("> build: %s", shlex.join(["cmake", "--build", "."] + build_arg))
    build_process = subprocess.Popen(
        ["cmake", "--build", "."] + build_arg, cwd=build_temp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    with build_process.stdout:
        _log_subprocess_output(build_process.stdout)
    ret = build_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to build ext!")

    _logger_cmake_ext.info("> install: %s", shlex.join(["cmake", "--install", "."] + install_arg))
    install_process = subprocess.Popen(
        ["cmake", "--install", "."] + install_arg, cwd=build_temp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    with install_process.stdout:
        _log_subprocess_output(install_process.stdout)
    ret = install_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to install ext!")

    _logger_cmake_ext.info("conclude cmake ext: %s ===", ext.name)
