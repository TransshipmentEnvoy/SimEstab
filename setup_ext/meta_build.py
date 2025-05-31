from setuptools.command.build_clib import build_clib
from setuptools.command.build_ext import build_ext
from distutils.errors import DistutilsSetupError
import os
import logging
from . import cmake_clib, cmake_extension, path_util


_logger_clib = logging.getLogger("setup_ext.MetaBuildClib")


# mata-build-clib
class MetaBuildClib(build_clib):
    def check_library_list(self, libraries):
        if libraries is None:
            return

        libraries_ = []
        for lib_item in libraries:
            if isinstance(lib_item, cmake_clib.CMakeClib):
                pass
            else:
                libraries_.append(lib_item)

        return super().check_library_list(libraries_)

    def get_source_files(self):
        if self.libraries is None:
            return []

        self.check_library_list(self.libraries)

        filenames = []
        for lib_item in self.libraries:
            if isinstance(lib_item, cmake_clib.CMakeClib):
                pass  # TODO: append source
            else:
                (lib_name, build_info) = lib_item
                sources = build_info.get("sources")
                if sources is None or not isinstance(sources, (list, tuple)):
                    raise DistutilsSetupError(
                        "in 'libraries' option (library '%s'), "
                        "'sources' must be present and must be "
                        "a list of source filenames" % lib_name
                    )

                filenames.extend(sources)
        return filenames

    def get_library_names(self):
        if self.libraries is None:
            return None

        lib_names = []
        for lib_item in self.libraries:
            if isinstance(lib_item, cmake_clib.CMakeClib):
                pass
            else:
                (lib_name, build_info) = lib_item
                lib_names.append(lib_name)
        return lib_names

    def build_libraries(self, libraries) -> None:
        libraries_ = []
        for lib_item in libraries:
            if isinstance(lib_item, cmake_clib.CMakeClib):
                _logger_clib.info("detect cmake clib: %s at %s", lib_item.name, lib_item.sourcedir)

                build_temp = self.path_essential.temp_dir
                lib_dir = self.prefix_expand.expand_path(lib_item.targetdir)
                lib_dir = self.prefix_expand.prefix_path(lib_dir)
                self.prefix_expand.expand_prefix_argdef(lib_item.cmake_configure_argdef)

                # call if
                cmd_build_ext = self.get_finalized_command("build_ext")
                _logger_clib.info("build cmake clib: %s at %s", lib_item.name, build_temp)
                _logger_clib.info("build cmake clib: %s to %s", lib_item.name, lib_dir)
                cmake_clib.build_clib(
                    lib_item,
                    lib_dir,
                    build_temp=build_temp,
                    compiler=self.compiler,
                    debug=self.debug,
                    plat_name=cmd_build_ext.plat_name,
                    parallel=cmd_build_ext.parallel,
                )
            else:
                libraries_.append(lib_item)

        return super().build_libraries(libraries_)

    def run(self):
        # get inplace build info
        cmd_build_ext = self.get_finalized_command("build_ext")
        inplace = cmd_build_ext.inplace or cmd_build_ext.editable_mode

        # setup essential path
        # get build_temp
        build_temp = self.build_temp

        # get build_lib
        if not inplace:
            build_lib = os.path.abspath(cmd_build_ext.build_lib)
        else:
            cmd_build_py = self.get_finalized_command("build_py")
            build_lib = os.path.abspath(cmd_build_py.get_package_dir(""))

        # store path essential
        self.path_essential = path_util.PathEssential(temp_dir=build_temp, prefix_dir=build_lib)

        # setup prefix replacement mechaism
        self.prefix_expand = path_util.PathExpand(self.path_essential)

        # run!
        res = super().run()

        # cleanup
        del self.prefix_expand
        del self.path_essential

        # return
        return res


# extend cmdclasses
class MetaBuildExt(build_ext):
    def build_extension(self, ext) -> None:
        if isinstance(ext, cmake_extension.CMakeExtension):
            # use setuptools provided extdir! this will auto do sep build in dev mode
            extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))

            self.prefix_expand.expand_prefix_argdef(ext.cmake_configure_argdef)
            cmake_extension.build_extension(
                ext=ext,
                extdir=extdir,
                build_temp=self.build_temp,
                compiler=self.compiler,
                debug=self.debug,
                plat_name=self.plat_name,
                parallel=self.parallel,
            )
        else:
            # fallback to default
            super().build_extension(ext)

    def copy_extensions_to_source(self):
        super().copy_extensions_to_source()

        build_py = self.get_finalized_command("build_py")
        for ext in self.extensions:
            if isinstance(ext, cmake_extension.CMakeExtension):
                inplace_file, regular_file = self._get_inplace_equivalent(build_py, ext)
                inplace_dir = os.path.dirname(inplace_file)
                regular_dir = os.path.dirname(regular_file)

                # scan extra_lib under regular_dir, copy them together
                for lib_file, lib_offset in ext.extra_lib.items():
                    regular_lib_file = os.path.join(regular_dir, lib_offset, lib_file)
                    inplace_lib_file = os.path.join(inplace_dir, lib_offset, lib_file)
                    if os.path.exists(regular_lib_file):
                        self.copy_file(regular_lib_file, inplace_lib_file, level=self.verbose)
            else:
                pass

    def run(self):
        # get inplace build info
        inplace = self.inplace or self.editable_mode

        # setup essential path
        # get build_temp
        build_temp = self.build_temp

        # get build_lib
        if not inplace:
            build_lib = os.path.abspath(self.build_lib)
        else:
            cmd_build_py = self.get_finalized_command("build_py")
            build_lib = os.path.abspath(cmd_build_py.get_package_dir(""))

        # store path essential
        self.path_essential = path_util.PathEssential(temp_dir=build_temp, prefix_dir=build_lib)

        # setup prefix replacement mechaism
        self.prefix_expand = path_util.PathExpand(self.path_essential)

        # run!
        res = super().run()

        # cleanup
        del self.prefix_expand
        del self.path_essential

        # return
        return res
