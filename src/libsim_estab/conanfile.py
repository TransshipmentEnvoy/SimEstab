import os
import shutil
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
        pass

    def requirements(self):
        # util
        self.requires("boost/1.89.0", options={"shared": True})

        self.requires("fmt/12.1.0")
        self.requires("rang/3.2")
        self.requires("indicators/2.3")

        self.requires("range-v3/0.12.0")

        self.requires("tsl-robin-map/1.4.0")
        self.requires("tl-function-ref/1.0.0")
        self.requires("scope-lite/0.2.0")
        self.requires("magic_enum/0.9.7")

        # config
        self.requires("nlohmann_json/3.12.0")
        #self.requires("yaml-cpp/0.8.0")
        #self.requires("tomlplusplus/3.4.0")

        # math
        self.requires("eigen/5.0.1")

        # unittest
        self.requires("boost-ext-ut/2.3.1")

        # graphics
        self.requires(
            "sdl/3.3.2",
            options={
                "shared": True,
                "gpu": True,
                # "pipewire": True, # TODO: currently not supported in conan-center-index
                # some unwanted components
                "opengles": False,
                "sndio": False,
                "pulseaudio": False,
            },
        )
        # vulkan backend & shader tools
        self.requires("vulkan-loader/1.4.313.0")
        self.requires("glslang/1.4.313.0")
        self.requires("spirv-cross/1.4.313.0")

        # geometry
        self.requires("cgal/6.1")
        # self.requires("opencascade/7.9.1")  # TODO: replace with custom recipe

        # ecs
        self.requires("flecs/4.1.1")

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

    def package_info(self):
        print(self.env_info)
