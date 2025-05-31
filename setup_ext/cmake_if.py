import os
import sys
import re

import typing
from typing import Optional, Mapping, MutableMapping, Any

from copy import deepcopy

# Convert distutils Windows platform specifiers to CMake -A arguments
PLAT_TO_CMAKE = {
    "win32": "Win32",
    "win-amd64": "x64",
    "win-arm32": "ARM",
    "win-arm64": "ARM64",
}


def parse_config(
    installdir: str,
    cmake_generator: Optional[str],
    cmake_configure_argdef: Mapping[str, str],
    cmake_build_argdef: Mapping[str, str],
    compiler: Any,
    debug: Any,
    plat_name: Any,
    parallel: Any,
):
    # copy argdef
    configure_argdef: MutableMapping[str, str] = deepcopy(cmake_configure_argdef)
    build_argdef: MutableMapping[str, str] = deepcopy(cmake_build_argdef)

    # required for auto-detection & inclusion of auxiliary "native" libs
    if not installdir.endswith(os.path.sep):
        installdir += os.path.sep

    # initialize the cmake_arg and build_arg
    cmake_arg, build_arg, install_arg = [], [], []

    # determine build type. use explicit setting first, then check self.debug
    if "CMAKE_BUILD_TYPE" not in configure_argdef:
        if debug:
            if compiler.compiler_type != "msvc":
                configure_argdef["CMAKE_BUILD_TYPE"] = "Debug"
            else:
                # FIXME: workaround
                configure_argdef["CMAKE_BUILD_TYPE"] = "RelWithDebInfo"
        else:
            configure_argdef["CMAKE_BUILD_TYPE"] = "Release"
    build_type = configure_argdef["CMAKE_BUILD_TYPE"]

    # determine generator (and platform if msvc)
    if compiler.compiler_type != "msvc":
        # Using Ninja-build since it a) is available as a wheel and b)
        # multithreads automatically. MSVC would require all variables be
        # exported for Ninja to pick it up, which is a little tricky to do.
        # Users can override the generator with CMAKE_GENERATOR in CMake
        # 3.15+.
        if cmake_generator is None:
            try:
                # FIXME: should use system ninja, rather rely on the ninja in env
                import ninja  # noqa: F401

                cmake_generator = "Ninja"
            except ImportError:
                pass
    else:
        # Single config generators are handled "normally"
        single_config = any(x in cmake_generator for x in {"NMake", "Ninja"}) if cmake_generator is not None else False

        # CMake allows an arch-in-generator style for backward compatibility
        contains_arch = any(x in cmake_generator for x in {"ARM", "Win64"}) if cmake_generator is not None else False

        # Specify the arch if using MSVC generator, but only if it doesn't
        # contain a backward-compatibility arch spec already in the
        # generator name.
        if not single_config and not contains_arch:
            cmake_arg += ["-A", PLAT_TO_CMAKE[plat_name]]

        # Multi-config generators have a different way to specify configs
        if not single_config:
            configure_argdef.pop("CMAKE_BUILD_TYPE")
            build_arg += ["--config", build_type]
            install_arg += ["--config", build_type]
    if cmake_generator is not None:
        cmake_arg += ["-G", cmake_generator]

    # OSX?
    if sys.platform.startswith("darwin"):
        # Cross-compile support for macOS - respect ARCHFLAGS if set
        archs = re.findall(r"-arch (\S+)", os.environ.get("ARCHFLAGS", ""))
        if archs:
            configure_argdef["CMAKE_OSX_ARCHITECTURES"] = ";".join(archs)

    # Set CMAKE_BUILD_PARALLEL_LEVEL to control the parallel build level
    # across all generators.
    if "CMAKE_BUILD_PARALLEL_LEVEL" not in build_argdef:
        # self.parallel is a Python 3 only way to set parallel jobs by hand
        # using -j in the build_clib call, not supported by pip or PyPA-build.
        if parallel:
            # CMake 3.12+ only.
            build_arg += [f"-j{parallel}"]
        else:
            build_arg += [f"-j{os.cpu_count()}"]
    else:
        build_arg += [f"-j{build_argdef['CMAKE_BUILD_PARALLEL_LEVEL']}"]

    # install
    configure_argdef["CMAKE_INSTALL_PREFIX"] = installdir

    # conclude configure and build argdef
    for arg_key, arg_value in configure_argdef.items():
        cmake_arg += [f"-D{arg_key}={arg_value}"]

    return cmake_arg, build_arg, install_arg
