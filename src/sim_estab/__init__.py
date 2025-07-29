import os
import platform


def prepare_dll():
    this_dir = os.path.normpath(__file__)
    root_dir = os.path.dirname(this_dir)
    lib_dir = os.path.join(root_dir, "lib")
    os.add_dll_directory(lib_dir)


if platform.system() == "Windows":
    prepare_dll()

from . import _if

__all__: list[str] = []
