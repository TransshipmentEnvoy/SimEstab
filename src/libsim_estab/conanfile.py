import os
import shutil
import stat
import glob
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout


def copy_shared_lib(src_dir, dst_dir, pattern, follow_symlinks=True):
    """Copy shared libraries matching pattern from src_dir to dst_dir.
    
    Args:
        src_dir: Source directory to search for files
        dst_dir: Destination directory to copy files to
        pattern: Glob pattern to match files (e.g., "libboost_*.so.*")
        follow_symlinks: If True, dereference symlinks (copy actual file content)
    """
    os.makedirs(dst_dir, exist_ok=True)
    for src_file in glob.glob(os.path.join(src_dir, pattern)):
        dst_file = os.path.join(dst_dir, os.path.basename(src_file))
        if follow_symlinks and os.path.islink(src_file):
            # Dereference symlink: copy the actual file content
            real_src = os.path.realpath(src_file)
            shutil.copy2(real_src, dst_file)
        else:
            shutil.copy2(src_file, dst_file, follow_symlinks=follow_symlinks)


class RepoRecipe(ConanFile):
    name = "sim_estab"
    version = "0.0.1"

    settings = "os", "compiler", "build_type", "arch"

    options = {"BUILD_TESTS": [True, False]}
    default_options = {"BUILD_TESTS": False}

    def build_requirements(self):
        if self.settings.os == "Linux":
            self.tool_requires("patchelf/0.18")

    def requirements(self):
        # versions and options live in conandata.yml, shared with src/sim_estab_ext
        options = self.conan_data.get("options", {})
        for name, version in self.conan_data["requirements"].items():
            self.requires(f"{name}/{version}", options=options.get(name))

    def configure(self):
        pass

    def layout(self):
        cmake_layout(self)  # TODO: override the layout for package dir

    def generate(self):
        tc = CMakeToolchain(self, generator="Ninja")
        tc.user_presets_path = False
        if self.options.BUILD_TESTS:
            tc.cache_variables["BUILD_TESTS"] = True
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

        # copy the boost shared libraries
        boost_dep = self.dependencies["boost"]
        boost_cpp_info = boost_dep.cpp_info
        # copy from lib directories
        for lib_dir in boost_cpp_info.libdirs:
            lib_path = os.path.join(boost_dep.package_folder, lib_dir)
            if os.path.exists(lib_path):
                if self.settings.os == "Windows":
                    # On Windows, DLLs might be in bin directory
                    bin_path = os.path.join(boost_dep.package_folder, "bin")
                    if os.path.exists(bin_path):
                        copy_shared_lib(bin_path, os.path.join(self.package_folder, "bin"), "*.dll")
                else:
                    # On Linux
                    copy_shared_lib(lib_path, os.path.join(self.package_folder, "lib"), "libboost_*.so.*")
                    # copy_shared_lib(lib_path, os.path.join(self.package_folder, "lib"), "libboost_*.dylib*")

        # copy the sdl shared libraries
        sdl_dep = self.dependencies["sdl"]
        sdl_cpp_info = sdl_dep.cpp_info
        for lib_dir in sdl_cpp_info.libdirs:
            lib_path = os.path.join(sdl_dep.package_folder, lib_dir)
            if os.path.exists(lib_path):
                if self.settings.os == "Windows":
                    bin_path = os.path.join(sdl_dep.package_folder, "bin")
                    if os.path.exists(bin_path):
                        copy_shared_lib(bin_path, os.path.join(self.package_folder, "bin"), "SDL3.dll")
                else:
                    copy_shared_lib(lib_path, os.path.join(self.package_folder, "lib"), "libSDL3.so.0")
                    # copy_shared_lib(lib_path, os.path.join(self.package_folder, "lib"), "libSDL3.dylib*")

        # copy the vulkan-loader shared libraries
        vulkan_dep = self.dependencies["vulkan-loader"]
        vulkan_cpp_info = vulkan_dep.cpp_info
        for lib_dir in vulkan_cpp_info.libdirs:
            lib_path = os.path.join(vulkan_dep.package_folder, lib_dir)
            if os.path.exists(lib_path):
                if self.settings.os == "Windows":
                    bin_path = os.path.join(vulkan_dep.package_folder, "bin")
                    if os.path.exists(bin_path):
                        copy_shared_lib(bin_path, os.path.join(self.package_folder, "bin"), "vulkan-1.dll")
                else:
                    copy_shared_lib(lib_path, os.path.join(self.package_folder, "lib"), "libvulkan.so.1")
                    # copy_shared_lib(lib_path, os.path.join(self.package_folder, "lib"), "libvulkan.dylib*")

        if self.settings.os == "Linux":
            self._patch_bundled_runpaths()

    def _patch_bundled_runpaths(self):
        """Bundled deps (boost, SDL3, vulkan) ship without RUNPATH, so their own
        NEEDED entries (e.g. boost_log -> boost_filesystem) are unresolvable next
        to each other: DT_RUNPATH is not consulted for indirect dependencies.
        Stamp $ORIGIN so every lib resolves its siblings in the same directory."""
        lib_dir = os.path.join(self.package_folder, "lib")
        for f in glob.glob(os.path.join(lib_dir, "*.so*")):
            if os.path.islink(f) or os.path.basename(f) == "libsim_estab.so":
                continue
            os.chmod(f, os.stat(f).st_mode | stat.S_IWUSR)
            self.run(f"patchelf --set-rpath '$ORIGIN' \"{f}\"")

    def package_info(self):
        print(self.env_info)
