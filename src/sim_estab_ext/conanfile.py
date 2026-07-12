import os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout
from conan.tools import files

import os


class RepoRecipe(ConanFile):
    name = "sim_estab_ext"
    version = "0.0.1"

    settings = "os", "compiler", "build_type", "arch"

    def build_requirements(self):
        pass

    def requirements(self):
        # util
        self.requires("boost/1.91.0")

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
            "sdl/3.4.12",
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

        # geometry
        self.requires("cgal/6.2")
        # self.requires("opencascade/7.9.1")  # TODO: replace with custom recipe

        # ecs
        self.requires("flecs/4.1.5")

    def configure(self):
        self.options["boost"].shared = True

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self, generator="Ninja")
        tc.user_presets_path = False
        tc.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        # cmake = CMake(self)
        # cmake.configure()
        # cmake.build()
        # cmake.install()
        pass

    def package_info(self):
        print(self.env_info)
