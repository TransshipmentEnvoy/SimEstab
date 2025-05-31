import os
from typing import Mapping, MutableMapping
import dataclasses


@dataclasses.dataclass
class PathEssential:
    # this intended to be a build-tool independent path storage
    # there is two part:
    #     `temp_dir` for building
    #     `prefix_dir` for installing/deploying
    temp_dir: str
    prefix_dir: str

    def __getitem__(self, key):
        return getattr(self, key)

    def get(self, key, default=None):
        return getattr(self, key, default)

    def __iter__(self):
        return self.keys()

    def keys(self):
        keys = [t.name for t in dataclasses.fields(self)]
        return iter(keys)

    def values(self):
        values = [getattr(self, t.name) for t in dataclasses.fields(self)]
        return iter(values)

    def items(self):
        data = [(t.name, getattr(self, t.name)) for t in dataclasses.fields(self)]
        return iter(data)


class PathPrefixBuildLib:
    def __init__(self, offset="") -> None:
        self.offset = os.path.normcase(os.path.normpath(offset))


class PathPrefixBuildTemp:
    def __init__(self, offset="") -> None:
        self.offset = os.path.normcase(os.path.normpath(offset))


SCAN_MAP = {
    PathPrefixBuildTemp: "temp_dir",
    PathPrefixBuildLib: "prefix_dir",
}


class PathExpand:
    def __init__(self, path_store: Mapping) -> None:
        self.path_store = path_store

    def expand_path(self, v):
        type_v = type(v)
        if type_v in SCAN_MAP:
            _prefix_key = SCAN_MAP[type_v]
            _prefix = self.path_store[_prefix_key]
            offset = v.offset
            new_v = os.path.normcase(os.path.normpath(os.path.abspath(os.path.join(_prefix, offset))))
            return new_v
        else:
            return v

    def expand_prefix_argdef(self, argdef: MutableMapping[str, str]):
        for k in argdef:
            v = argdef[k]
            argdef[k] = self.expand_path(v)

    def prefix_path(self, v: str):
        if not os.path.isabs(v):
            build_lib = self.path_store["prefix_dir"]
            res = os.path.normcase(os.path.normpath(os.path.abspath(os.path.join(build_lib, v))))
        else:
            res = os.path.normcase(os.path.normpath(v))
        return res
