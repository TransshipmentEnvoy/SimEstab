import os
import re
import subprocess
import shlex
import sys
from typing import Optional, MutableMapping, Any
from setuptools import Extension

from .cmake_if import parse_config
from . import conan_if
from .conan_clib import detect_conan_package, create_conan_package, install_conan_dependency
from .subprocess_util import safe_decode_stdout
from distutils.errors import DistutilsSetupError

import logging

_logger_conan_cmake_ext = logging.getLogger("setup_ext.CmakeExt")


def _log_subprocess_output(pipe):
    for line in iter(pipe.readline, b""):  # b'\n'-separated lines
        _logger_conan_cmake_ext.info("\t%s", safe_decode_stdout(line).rstrip("\n"))


# A ConanCMakeExtension needs a sourcedir instead of a file list.
# The name must be the _single_ output extension from the CMake build.
# If you need multiple extensions, see scikit-build.
# This is the conan variant, which use conan for dependency management.
class ConanCMakeExtension(Extension):

    @property
    def conan_package_name(self) -> str:
        return f"{self.conan_name}/{self.conan_version}"

    def __init__(
        self,
        name: str,
        sourcedir: str = "",
        build_type: str = "Release",
        conan_name: str = None,
        conan_version: str = None,
        conan_profile_path: str = None,
        conan_home_dir: str = None,
        conan_local_dep: dict[str, dict[str, str]] = None,
        cmake_generator: Optional[str] = None,
        cmake_configure_argdef: Optional[MutableMapping[str, str]] = None,
        cmake_build_argdef: Optional[MutableMapping[str, str]] = None,
        extra_lib: Optional[dict[str, str]] = None,
        py_limited_api: bool = False,
    ):
        Extension.__init__(self, name, sources=[], py_limited_api=py_limited_api)

        self.sourcedir = os.path.abspath(sourcedir)

        # conan
        self.conan_name = conan_name
        self.conan_version = conan_version

        # conan build profile
        self.build_type = build_type
        if conan_profile_path is not None:
            self.conan_profile_path = conan_profile_path
        else:
            self.conan_profile_path = os.path.normpath(
                os.path.abspath(os.path.join(os.path.dirname(os.path.dirname(__file__)), "buildsys", "conan")))

        # set conan home dir
        self.conan_home_dir = conan_home_dir if conan_home_dir is not None else os.path.normpath(
            os.path.abspath(os.path.join(os.path.dirname(os.path.dirname(__file__)), "buildsys", "conan_home")))

        # check conan local dep, bundled conanfiles for customization
        self.conan_local_dep = conan_local_dep if conan_local_dep is not None else {}

        # cmake
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
    ext: ConanCMakeExtension,
    extdir: str,
    build_temp: str,
    compiler: Any,
    debug: Any,
    plat_name: Any,
    parallel: Optional[Any],
):
    _logger_conan_cmake_ext.info("build conan-cmake ext: %s >>>", ext.name)

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

    # conan
    # select profile
    if not os.path.isdir(ext.conan_profile_path):
        conan_profile_path = ext.conan_profile_path
    else:
        conan_profile_path = conan_if.parse_profile(
            conan_profile_path=ext.conan_profile_path,
            build_type=ext.build_type,
            compiler=compiler,
            debug=debug,
            plat_name=plat_name,
        )
    _logger_conan_cmake_ext.info("  conan profile: %s", conan_profile_path)

    # select home
    conan_home_dir = ext.conan_home_dir
    _logger_conan_cmake_ext.info("  conan home dir: %s", conan_home_dir)

    # select local dep
    _logger_conan_cmake_ext.info("  conan local dep: %s", ext.conan_local_dep)
    for package_name, package_info in ext.conan_local_dep.items():
        # detect if there has already the package in conan home
        if detect_conan_package(package_name, ext.conan_home_dir):
            _logger_conan_cmake_ext.info("  conan local dep %s already exists, skipping creation.", package_name)
            continue

        _logger_conan_cmake_ext.info("  creating conan local dep: %s >>>", package_name)
        create_conan_package(
            package_name=package_name,
            recipe_path=conan_if.find_recipe(package_info["package"]),
            conan_home_dir=ext.conan_home_dir,
            profile_path=package_info.get("profile", None),
            build_dir=os.path.join(build_temp, "local_dep", package_name.replace('/', '++')),
        )

    # conan install deps
    install_conan_dependency(
        package_name=ext.conan_package_name,
        recipe_path=conan_if.find_recipe(ext.sourcedir),
        conan_home_dir=ext.conan_home_dir,
        profile_path=conan_profile_path,
        build_dir=build_temp,
    )

    # cmake extra
    toolchain_argdef = {
        "CMAKE_TOOLCHAIN_FILE": os.path.join(build_temp, "build", ext.build_type, "generators", "conan_toolchain.cmake")
    }
    cmake_configure_argdef = {**toolchain_argdef, **ext.cmake_configure_argdef}
    cmake_arg, build_arg, install_arg = parse_config(
        installdir=extdir,
        cmake_generator=ext.cmake_generator,
        cmake_configure_argdef=cmake_configure_argdef,
        cmake_build_argdef=ext.cmake_build_argdef,
        compiler=compiler,
        debug=debug,
        plat_name=plat_name,
        parallel=parallel,
    )

    _logger_conan_cmake_ext.info("> working dir: %s", build_temp)

    _logger_conan_cmake_ext.info("> configure: %s", shlex.join(["cmake", ext.sourcedir] + cmake_arg))
    configure_process = subprocess.Popen(["cmake", ext.sourcedir] + cmake_arg,
                                         cwd=build_temp,
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT)
    with configure_process.stdout:
        _log_subprocess_output(configure_process.stdout)
    ret = configure_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to configure ext!")

    _logger_conan_cmake_ext.info("> build: %s", shlex.join(["cmake", "--build", "."] + build_arg))
    build_process = subprocess.Popen(["cmake", "--build", "."] + build_arg,
                                     cwd=build_temp,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
    with build_process.stdout:
        _log_subprocess_output(build_process.stdout)
    ret = build_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to build ext!")

    _logger_conan_cmake_ext.info("> install: %s", shlex.join(["cmake", "--install", "."] + install_arg))
    install_process = subprocess.Popen(["cmake", "--install", "."] + install_arg,
                                       cwd=build_temp,
                                       stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
    with install_process.stdout:
        _log_subprocess_output(install_process.stdout)
    ret = install_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to install ext!")

    _logger_conan_cmake_ext.info("conclude conan-cmake ext: %s ===", ext.name)
