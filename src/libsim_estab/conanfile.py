import os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools import files

import os


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
            "sdl/3.2.26",
            options={
                "shared": True,
                "gpu": True,
                # "pipewire": True, # TODO: currently not supported in conan-center-index
                # some unwanted components
                "opengles": False,
                "sndio": False,
            },
        )

        # geometry
        self.requires("cgal/6.1")
        # self.requires("opencascade/7.9.1")  # TODO: replace with custom recipe

        # vulkan backend
        self.requires("vulkan-loader/1.4.313.0")

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
                        files.copy(self, "*.dll", src=bin_path, dst=os.path.join(self.package_folder, "bin"))
                else:
                    # On Linux/macOS
                    files.copy(self, "libboost_*.so*", src=lib_path, dst=os.path.join(self.package_folder, "lib"))
                    files.copy(self, "libboost_*.dylib*", src=lib_path, dst=os.path.join(self.package_folder, "lib"))

    def package_info(self):
        print(self.env_info)
