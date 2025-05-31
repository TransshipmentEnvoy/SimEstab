from setuptools import setup as _setup
from setuptools.dist import Distribution as _Distribution


def setup(*arg, **kwarg):
    class BinaryDistribution(_Distribution):
        def has_c_libraries(self):
            return True
        
        def has_ext_modules(self):
            return True

    kwarg["distclass"] = kwarg.get("distclass", BinaryDistribution)
    return _setup(*arg, **kwarg)
