import os

import yaml
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout

# Versions and options come from the library's conandata.yml, so both builds resolve the same ones.
_LIBSIM_ESTAB_DEPS = ("boost", "fmt", "rang", "indicators", "range-v3", "tsl-robin-map", "tl-function-ref",
                      "scope-lite", "magic_enum", "nlohmann_json", "eigen", "boost-ext-ut", "sdl", "vulkan-loader",
                      "cgal", "flecs")


class RepoRecipe(ConanFile):
    name = "sim_estab_ext"
    version = "0.0.1"

    settings = "os", "compiler", "build_type", "arch"

    def build_requirements(self):
        pass

    def requirements(self):
        with open(os.path.join(self.recipe_folder, "..", "libsim_estab", "conandata.yml")) as f:
            libsim_estab = yaml.safe_load(f)
        options = libsim_estab.get("options", {})
        for name in _LIBSIM_ESTAB_DEPS:
            self.requires(f"{name}/{libsim_estab['requirements'][name]}", options=options.get(name))

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
