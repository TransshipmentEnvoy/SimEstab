from setuptools import setup

import os
import sys
import pathlib
import importlib
import warnings

# get setup.py location
here = pathlib.Path(__file__).parent.resolve()
long_description = (here / "README.md").read_text(encoding="utf-8")
license_content = (here / "LICENSE.md").read_text(encoding="utf-8")

# import setup_ext
sys.path.insert(0, str(here))
import setup_ext
from setup_ext import setuptools_wrap, meta_build
from setup_ext import cmake_clib, cmake_extension, cmake_if
from setup_ext import conan_clib
from setup_ext import conan_cmake_extension
from setup_ext import develop_warp
from setup_ext import path_util, rpath_util

sys.path.pop(0)

# current packages
packages = [
    "sim_estab",
]

# clib & ext
import nanobind

libraries = [
    conan_clib.ConanClib(
        name="sim_estab",
        version="0.0.1",
        sourcedir="src/libsim_estab",
        targetdir=path_util.PathPrefixBuildLib("sim_estab"),
        build_type="Debug",  # TODO: use envvar
        conan_profile_path=str(here / "buildsys" / "conan" / "profile"),
        conan_local_dep={
            "xorg/system": {
                "package": str(here / "buildsys" / "conan" / "recipe" / "xorg"),
            },
            "cgal/6.1": {
                "package": str(here / "buildsys" / "conan" / "recipe" / "cgal"),
                "obtain_source": True,
                "feed_version": True,
            },
            "sdl/3.3.2": {
                "package": str(here / "buildsys" / "conan" / "recipe" / "sdl"),
                "obtain_source": True,
                "feed_version": True,
            }
        }),
]

ext_modules = [
    conan_cmake_extension.ConanCMakeExtension(
        "sim_estab._if",
        sourcedir=str(here / "src" / "sim_estab_ext"),
        build_type="Debug",  # TODO: use envvar
        conan_name="sim_estab_ext",
        conan_version="0.0.1",
        conan_profile_path=str(here / "buildsys" / "conan" / "profile"),
        cmake_configure_argdef={
            "nanobind_DIR": nanobind.cmake_dir(),
            "libsim_estab_DIR": path_util.PathPrefixBuildLib("sim_estab/lib/cmake/libsim_estab"),
            "DESIGNATED_RPATH": rpath_util.compute_rpath("sim_estab._if", "sim_estab/lib"),
        },
        extra_lib={},
        py_limited_api=False,  # True
    )
]

setuptools_wrap.setup(
    name="sim_estab",
    version="0.0.1",
    description="Simulated Establishment",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/TransshipmentEnvoy/SimEstab",
    license=license_content,
    license_files=('LICENSE.md',),
    author="TransshipmentEnvoy",
    author_email="TransshipmentEnvoy@outlook.com",
    # dep
    python_requires=">=3.11, <4",
    # pkg
    package_dir={"": "src"},
    packages=packages,
    # cmdclass
    cmdclass={
        "develop": develop_warp.CustomDevelop,
        "build_clib": meta_build.MetaBuildClib,
        "build_ext": meta_build.MetaBuildExt,
    },
    libraries=libraries,
    ext_modules=ext_modules,
)
