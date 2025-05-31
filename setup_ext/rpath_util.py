import os


def compute_rpath(ext_name, lib_path):
    ext_dir = "/".join(ext_name.split(".")[:-1])
    rel_path = os.path.relpath(lib_path, ext_dir)
    rel_path = os.path.normpath(rel_path)
    res = os.path.join(r"$ORIGIN", rel_path)
    return res


def compute_relpath(ext_name, lib_path):
    ext_dir = "/".join(ext_name.split(".")[:-1])
    rel_path = os.path.relpath(lib_path, ext_dir)
    res = os.path.normpath(rel_path)
    return res
