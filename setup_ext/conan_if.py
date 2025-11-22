import os
import sys
import re

import typing
from typing import Optional, Mapping, MutableMapping, Any

from copy import deepcopy

from . import plat_util
from . import compiler_util

PROFILE_MAPPING = {
    ("linux", "debug"): "linux_debug",
    ("linux", "release"): "linux_release",
    ("linux", "relwithdebinfo"): "linux_relwithdebinfo",
    ("win32", "debug"): "windows_debug",
    ("win32", "release"): "windows_release",
    ("win32", "relwithdebinfo"): "windows_relwithdebinfo",
}


def find_recipe(sourcedir: str):
    if os.path.exists(os.path.join(sourcedir, "conanfile.py")):
        return os.path.normpath(os.path.abspath(os.path.join(sourcedir, "conanfile.py")))
    elif os.path.exists(os.path.join(sourcedir, "conanfile.txt")):
        return os.path.normpath(os.path.abspath(os.path.join(sourcedir, "conanfile.txt")))
    else:
        raise FileNotFoundError(f"Conan recipe not found in {sourcedir}. Expected conanfile.py or conanfile.txt.")


def parse_profile(
    build_type: str,
    compiler: Any,
    debug: Any,
    plat_name: Any,
    conan_profile_path: Optional[str] = None,
) -> Optional[str]:
    platform_category = plat_util.get_platform_category(plat_name)
    compiler_type = compiler.compiler_type
    build_type_query = build_type.lower()
    if debug and build_type_query == "debug":
        build_type, build_type_query = "Debug", "debug"
    elif debug and build_type_query == "release":
        build_type, build_type_query = "RelWithDebInfo", "relwithdebinfo"

    profile_filename = PROFILE_MAPPING[(platform_category, build_type_query)]
    if conan_profile_path is not None:
        profile_path = os.path.join(conan_profile_path, profile_filename)
    else:
        profile_path = None
    if not os.path.exists(profile_path):
        profile_path = None

    return profile_path, build_type
