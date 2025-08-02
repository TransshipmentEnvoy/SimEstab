import os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools import files

import os


class RepoRecipe(ConanFile):
    name = "sim_estab"
    version = "0.0.1"

    settings = "os", "compiler", "build_type", "arch"
    
    options = {
        "BUILD_TESTS": [True, False]
    }
    default_options = {
        "BUILD_TESTS": False
    }

    def build_requirements(self):
        pass

    def requirements(self):
        # util
        self.requires("boost/1.88.0")

        self.requires("fmt/11.2.0")
        self.requires("rang/3.2")
        self.requires("indicators/2.3")

        self.requires("range-v3/0.12.0")

        self.requires("tsl-robin-map/1.3.0")
        self.requires("tl-function-ref/1.0.0")
        self.requires("scope-lite/0.2.0")
        self.requires("magic_enum/0.9.7")

        # config
        self.requires("nlohmann_json/3.11.3")
        #self.requires("yaml-cpp/0.8.0")
        #self.requires("tomlplusplus/3.4.0")

        # math
        self.requires("eigen/3.4.0")

        # unittest
        self.requires("boost-ext-ut/2.1.0")

        # graphics
        self.requires("sdl/3.2.14")

        # ecs
        self.requires("flecs/4.0.4")

    def configure(self):
        self.options["boost"].shared = True

    def layout(self):
        cmake_layout(self) # TODO: override the layout for package dir

    def generate(self):
        tc = CMakeToolchain(self)
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

    def package_info(self):
        print(self.env_info)
