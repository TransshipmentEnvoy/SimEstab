import os
import re
import subprocess
import shlex
import sys
import json
import shutil
from typing import Optional, MutableMapping, Any

from . import conan_if
from . import path_util
from distutils.errors import DistutilsSetupError

import logging

_logger_conan_clib = logging.getLogger("setup_ext.ConanClib")


def _safe_decode_stdout(stdout_bytes: bytes) -> str:
    """
    Safely decode stdout bytes with auto-detection of encoding.
    Falls back to utf-8 with error handling if detection fails.
    """
    if not stdout_bytes:
        return ""

    try:
        # Try to import chardet for encoding detection
        import chardet
        detected = chardet.detect(stdout_bytes)
        if detected and detected.get('encoding') and detected.get('confidence', 0) > 0.5:
            encoding = detected['encoding']
            try:
                return stdout_bytes.decode(encoding)
            except (UnicodeDecodeError, LookupError):
                pass
    except ImportError:
        # chardet not available, continue with fallback
        pass

    # Fallback to common encodings
    for encoding in ['utf-8', 'cp1252', 'latin1']:
        try:
            return stdout_bytes.decode(encoding)
        except UnicodeDecodeError:
            continue

    # Last resort: decode with utf-8 and replace errors
    return stdout_bytes.decode('utf-8', errors='replace')


def _log_subprocess_output(pipe):
    for line in iter(pipe.readline, b""):  # b'\n'-separated lines
        _logger_conan_clib.info("\t%s", _safe_decode_stdout(line).rstrip("\n"))


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
        result = json.loads(_safe_decode_stdout(stdout))
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


def extract_conan_package_id(recipe_path: str,
                             conan_home_dir: Optional[str] = None,
                             profile_path: Optional[str] = None) -> Optional[str]:
    env = os.environ.copy()
    env_new = {}
    if conan_home_dir is not None:
        env_new["CONAN_HOME"] = os.path.normpath(os.path.abspath(os.path.expanduser(conan_home_dir)))
    env.update(**env_new)

    recipe_path = os.path.normpath(os.path.abspath(os.path.expanduser(recipe_path)))
    recipe_dir = os.path.normpath(os.path.dirname(os.path.abspath(recipe_path)))

    extra_args = []
    if profile_path is not None:
        extra_args += ["-pr:a", os.path.normpath(os.path.abspath(os.path.expanduser(profile_path)))]

    _logger_conan_clib.info(
        "    exec conan cmd at %s: %s", recipe_dir,
        shlex.join(["conan", "graph", "info", recipe_path] + ["-nr", "--format", "json"] + extra_args))
    extract_process = subprocess.Popen(
        ["conan", "graph", "info", recipe_path] + ["-nr", "--format", "json"] + extra_args,
        cwd=recipe_dir,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    stdout, _ = extract_process.communicate()
    ret = extract_process.returncode
    if ret != 0:
        _logger_conan_clib.warning("failed to extract conan package ID! recipe_path: %s", recipe_path)
        return None

    # decode stdout into json
    try:
        result = json.loads(_safe_decode_stdout(stdout))
    except json.JSONDecodeError as e:
        _logger_conan_clib.warning("failed to parse JSON output for conan package ID extraction: %s", e)
        return None

    # Extract package ID from the JSON result
    # The consumer node (node "0") contains the package_id we need
    try:
        graph = result.get("graph", {})
        nodes = graph.get("nodes", {})

        # Look for the consumer node (usually node "0")
        for node_id, node_data in nodes.items():
            if node_data.get("recipe") == "Consumer":
                package_id = node_data.get("package_id")
                if package_id:
                    return package_id

        _logger_conan_clib.warning("Consumer node with package_id not found in conan graph info output")
        return None

    except (KeyError, AttributeError) as e:
        _logger_conan_clib.warning("failed to extract package ID from JSON structure: %s", e)
        return None


def extract_conan_package_path(package_name: str,
                               package_id: str,
                               conan_home_dir: Optional[str] = None) -> Optional[str]:
    env = os.environ.copy()
    env_new = {}
    if conan_home_dir is not None:
        env_new["CONAN_HOME"] = os.path.normpath(os.path.abspath(os.path.expanduser(conan_home_dir)))
    env.update(**env_new)

    _logger_conan_clib.info("    exec conan cmd: %s",
                            shlex.join(["conan", "cache", "path", f"{package_name}:{package_id}"]))
    extract_process = subprocess.Popen(
        ["conan", "cache", "path", f"{package_name}:{package_id}"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    stdout, _ = extract_process.communicate()
    ret = extract_process.returncode
    if ret != 0:
        _logger_conan_clib.warning("failed to extract conan package path for %s:%s", package_name, package_id)
        return None
    
    # Decode stdout to get the package path
    try:
        package_path = _safe_decode_stdout(stdout).strip()
        if package_path:
            return package_path
        else:
            _logger_conan_clib.warning("empty package path returned for %s:%s", package_name, package_id)
            return None
    except Exception as e:
        _logger_conan_clib.warning("failed to decode package path output: %s", e)
        return None


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
    if detect_conan_package(clib.package_name, clib.conan_home_dir):
        _logger_conan_clib.info("  conan package %s already exists, skipping creation.", clib.package_name)
    else:
        create_conan_package(package_name=clib.package_name,
                             recipe_path=conan_if.find_recipe(clib.sourcedir),
                             conan_home_dir=clib.conan_home_dir,
                             profile_path=conan_profile_path,
                             build_dir=os.path.join(build_temp, clib.package_name.replace('/', '++')))

    # get package_id and filepath
    package_id = extract_conan_package_id(recipe_path=conan_if.find_recipe(clib.sourcedir),
                                          conan_home_dir=clib.conan_home_dir,
                                          profile_path=conan_profile_path)
    if package_id is None:
        raise DistutilsSetupError(f"failed to extract package ID for {clib.name}!")
    _logger_conan_clib.info("  conan package ID: %s", package_id)

    package_path = extract_conan_package_path(clib.package_name, package_id, conan_home_dir=clib.conan_home_dir)
    if package_path is None:
        raise DistutilsSetupError(f"failed to extract package path for {clib.name}!")
    _logger_conan_clib.info("  conan package path: %s", package_path)

    # copy files to targetdir
    _logger_conan_clib.info("  copying files to targetdir: %s", clibdir)
    
    # Use the already expanded clibdir path
    target_dir = clibdir
    os.makedirs(target_dir, exist_ok=True)
    _logger_conan_clib.info("    target dir: %s", target_dir)
    
    # Copy package contents to target directory
    try:
        # Look for common directories to copy from the package
        package_dirs_to_copy = ['lib', 'bin', 'include', 'share']
        
        for dir_name in package_dirs_to_copy:
            src_dir = os.path.join(package_path, dir_name)
            if os.path.exists(src_dir) and os.path.isdir(src_dir):
                dst_dir = os.path.join(target_dir, dir_name)
                _logger_conan_clib.info("    copying %s -> %s", src_dir, dst_dir)
                
                # Remove destination if it exists to ensure clean copy
                if os.path.exists(dst_dir):
                    shutil.rmtree(dst_dir)
                
                # Copy the directory tree
                shutil.copytree(src_dir, dst_dir, dirs_exist_ok=True)
                _logger_conan_clib.info("    copied %s successfully", dir_name)
        
        _logger_conan_clib.info("  file copying completed successfully")
        
    except Exception as e:
        raise DistutilsSetupError(f"failed to copy package files from {package_path} to {target_dir}: {e}")

    _logger_conan_clib.info("build conan clib: %s <<<", clib.name)
