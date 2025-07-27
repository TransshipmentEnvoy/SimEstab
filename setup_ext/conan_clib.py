import os
import re
import subprocess
import shlex
import sys
import json
from typing import Optional, MutableMapping, Any

from . import conan_if
from . import path_util
from distutils.errors import DistutilsSetupError

import logging

_logger_conan_clib = logging.getLogger("setup_ext.ConanClib")


def _log_subprocess_output(pipe):
    for line in iter(pipe.readline, b""):  # b'\n'-separated lines
        _logger_conan_clib.info("\t%s", line.decode("utf-8").rstrip("\n"))


class ConanClib:

    @property
    def package_name(self) -> str:
        return f"{self.name}/{self.version}"

    def __init__(
        self,
        name: str,
        version: str,
        sourcedir: str = "",
        targetdir: str = "",
        build_type: str = "Release",
        conan_profile_path: str = None,
        conan_home_dir: str = None,
        conan_local_dep: dict[str, dict[str, str]] = None,
    ) -> None:
        self.name = name
        self.version = version

        self.sourcedir = os.path.normcase(os.path.normpath(os.path.abspath(sourcedir)))
        if type(targetdir) in path_util.SCAN_MAP:
            self.targetdir = targetdir
        else:
            self.targetdir = os.path.normcase(os.path.normpath(targetdir))  # could be abs/rel

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
        self.conan_local_dep = conan_local_dep if conan_local_dep is not None else []


def detect_conan_package(package_name: str, conan_home_dir: str = None) -> dict:
    env = os.environ.copy()
    env_new = {}
    if conan_home_dir is not None:
        env_new["CONAN_HOME"] = os.path.normpath(os.path.abspath(os.path.expanduser(conan_home_dir)))
    env.update(**env_new)

    _logger_conan_clib.info("    exec conan cmd: %s", shlex.join(["conan", "list", package_name, "--format", "json"]))
    check_process = subprocess.Popen(
        ["conan", "list", package_name, "--format", "json"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    stdout, _ = check_process.communicate()
    ret = check_process.returncode

    if ret != 0:
        raise DistutilsSetupError(f"failed to check conan package: {package_name}")

    try:
        result = json.loads(stdout.decode('utf-8'))
    except json.JSONDecodeError as e:
        raise DistutilsSetupError(f"failed to parse JSON output for conan package {package_name}: {e}")

    status = package_name in result.get("Local Cache", {})
    return status


def create_conan_package(package_name: str,
                         recipe_path: str,
                         conan_home_dir: str = None,
                         profile_path: str = None,
                         build_dir: str = None):
    env = os.environ.copy()
    env_new = {}
    if conan_home_dir is not None:
        env_new["CONAN_HOME"] = os.path.normpath(os.path.abspath(os.path.expanduser(conan_home_dir)))
    env.update(**env_new)

    recipe_dir = os.path.dirname(os.path.abspath(recipe_path))
    recipe_dir = os.path.normpath(recipe_dir)

    extra_args = []
    if profile_path is not None:
        extra_args += ["-pr:a", os.path.normpath(os.path.abspath(os.path.expanduser(profile_path)))]
    if build_dir is not None:
        extra_args += ["-of", os.path.normpath(os.path.abspath(os.path.expanduser(build_dir)))]

    _logger_conan_clib.info("    exec conan cmd at %s: %s", recipe_dir,
                            shlex.join(["conan", "install", "."] + ["--build", "missing"] + extra_args))
    install_process = subprocess.Popen(
        ["conan", "install", "."] + ["--build", "missing"] + extra_args,
        cwd=recipe_dir,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    ret = install_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to install conan package! recipe_path: {recipe_path}")

    _logger_conan_clib.info("    exec conan cmd at %s: %s", recipe_dir,
                            shlex.join(["conan", "build", "."] + ["-nr"] + extra_args))
    build_process = subprocess.Popen(
        ["conan", "build", "."] + ["-nr"] + extra_args,
        cwd=recipe_dir,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    ret = build_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to create conan package! recipe_path: {recipe_path}")

    _logger_conan_clib.info("    exec conan cmd at %s: %s", recipe_dir,
                            shlex.join(["conan", "export-pkg", "."] + ["-nr"] + extra_args))
    export_process = subprocess.Popen(
        ["conan", "export-pkg", "."] + ["-nr"] + extra_args,
        cwd=recipe_dir,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    ret = export_process.wait()
    if ret != 0:
        raise DistutilsSetupError(f"failed to create conan package! recipe_path: {recipe_path}")

    if not detect_conan_package(package_name, conan_home_dir):
        raise DistutilsSetupError(f"conan package {package_name} not found after creation!")


def build_clib(
    clib: ConanClib,
    clibdir: str,
    build_temp: str,
    compiler: Any,
    debug: Any,
    plat_name: Any,
    parallel: Optional[Any],
):
    _logger_conan_clib.info("build conan clib: %s >>>", clib.name)

    # create build dir
    build_temp = os.path.abspath(os.path.join(build_temp, "conan_clib"))
    build_temp = os.path.join(build_temp, clib.name)
    if os.path.exists(build_temp):
        # if os.path.exists(os.path.join(build_temp, "CMakeCache.txt")):
        #     os.remove(
        #         os.path.join(build_temp, "CMakeCache.txt")
        #     )  # the cached environment has gone, so remove the cache
        pass
    else:
        os.makedirs(build_temp)
    _logger_conan_clib.info("  build temp: %s", build_temp)

    # select profile
    if not os.path.isdir(clib.conan_profile_path):
        conan_profile_path = clib.conan_profile_path
    else:
        conan_profile_path = conan_if.parse_profile(
            conan_profile_path=clib.conan_profile_path,
            build_type=clib.build_type,
            compiler=compiler,
            debug=debug,
            plat_name=plat_name,
        )
    _logger_conan_clib.info("  conan profile: %s", conan_profile_path)

    # select home
    conan_home_dir = clib.conan_home_dir
    _logger_conan_clib.info("  conan home dir: %s", conan_home_dir)

    # select local dep
    _logger_conan_clib.info("  conan local dep: %s", clib.conan_local_dep)
    for package_name, package_info in clib.conan_local_dep.items():
        # detect if there has already the package in conan home
        if detect_conan_package(package_name, clib.conan_home_dir):
            _logger_conan_clib.info("  conan local dep %s already exists, skipping creation.", package_name)
            continue

        _logger_conan_clib.info("  creating conan local dep: %s >>>", package_name)
        create_conan_package(
            package_name=package_name,
            recipe_path=conan_if.find_recipe(package_info["package"]),
            conan_home_dir=clib.conan_home_dir,
            profile_path=package_info.get("profile", None),
            build_dir=os.path.join(build_temp, "local_dep", package_name.replace('/', '++')),
        )

    # always create package
    create_conan_package(package_name=clib.package_name,
                         recipe_path=conan_if.find_recipe(clib.sourcedir),
                         conan_home_dir=clib.conan_home_dir,
                         profile_path=conan_profile_path,
                         build_dir=os.path.join(build_temp, clib.package_name.replace('/', '++')))
    
    # get package_id and filepath
    assert False
