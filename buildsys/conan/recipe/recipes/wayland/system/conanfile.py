import shutil

from conan import ConanFile, conan_version
from conan.tools.gnu import PkgConfig
from conan.tools.system import package_manager
from conan.errors import ConanInvalidConfiguration, ConanException
from conan.tools.scm import Version

required_conan_version = ">=1.50.0"


class WaylandConan(ConanFile):
    name = "wayland"
    package_type = "shared-library"
    url = "https://github.com/conan-io/conan-center-index"
    license = "MIT"
    homepage = "https://wayland.freedesktop.org/"
    description = "Wayland is a protocol for a compositor to talk to its clients"
    settings = "os", "arch", "compiler", "build_type"
    topics = ("wayland", "compositor", "display-server")

    # make sure points here
    version = "system"

    def validate(self):
        if self.settings.os not in ["Linux", "FreeBSD"]:
            raise ConanInvalidConfiguration("This recipe supports only Linux and FreeBSD")
        # Check wayland-scanner is available (required for building wayland clients)
        if not shutil.which("wayland-scanner"):
            raise ConanInvalidConfiguration("wayland-scanner not found in PATH. ")

    def package_id(self):
        self.info.clear()

    def system_requirements(self):
        # Debian/Ubuntu
        apt = package_manager.Apt(self)
        apt.install([
            "libwayland-dev",
            "libwayland-client0",
            "libwayland-server0",
            "libwayland-cursor0",
            "libwayland-egl1",
            "wayland-protocols",
        ],
                    update=True,
                    check=True)

        # RedHat (RHEL, CentOS)
        yum = package_manager.Yum(self)
        yum.install([
            "wayland-devel",
            "wayland-protocols-devel",
        ], update=True, check=True)

        # Fedora
        dnf = package_manager.Dnf(self)
        dnf.install([
            "wayland-devel",
            "wayland-protocols-devel",
        ], update=True, check=True)

        # openSUSE
        zypper = package_manager.Zypper(self)
        zypper.install([
            "wayland-devel",
            "wayland-protocols-devel",
        ], update=True, check=True)

        # Arch
        pacman = package_manager.PacMan(self)
        pacman.install([
            "wayland",
            "wayland-protocols",
        ], update=True, check=True)

        # FreeBSD
        package_manager.Pkg(self).install([
            "wayland",
            "wayland-protocols",
        ], update=True, check=True)

        # Alpine Linux
        if Version(conan_version) >= "2.0.10":
            alpine = package_manager.Apk(self)
            alpine.install([
                "wayland-dev",
                "wayland-protocols",
            ], update=True, check=True)

    def package_info(self):
        if Version(conan_version) >= 2:
            self.cpp_info.bindirs = []
            self.cpp_info.includedirs = []
            self.cpp_info.libdirs = []

        # Wayland core components
        for name in ["wayland-client", "wayland-server", "wayland-cursor", "wayland-egl"]:
            try:
                pkg_config = PkgConfig(self, name)
                pkg_config.fill_cpp_info(self.cpp_info.components[name], is_system=self.settings.os != "FreeBSD")
                self.cpp_info.components[name].version = pkg_config.version
                self.cpp_info.components[name].set_property("pkg_config_name", name)
                self.cpp_info.components[name].set_property("component_version", pkg_config.version)
                # Note: Do NOT clear bindirs/includedirs/libdirs here - they contain pkg-config info
                self.cpp_info.components[name].set_property(
                    "pkg_config_custom_content",
                    "\n".join(f"{key}={value}" for key, value in pkg_config.variables.items()
                              if key not in ["pcfiledir", "prefix", "includedir"]))
            except ConanException:
                self.output.warning(f"Package {name} not found in pkg-config. "
                                    "This may lead to missing components in the package info.")
                if name in self.cpp_info.components:
                    del self.cpp_info.components[name]

        # Add aggregate component for compatibility (wayland -> wayland-client)
        # This component inherits from wayland-client and should have empty dirs
        if "wayland-client" in self.cpp_info.components:
            self.cpp_info.components["wayland"].requires = ["wayland-client"]
            self.cpp_info.components["wayland"].bindirs = []
            self.cpp_info.components["wayland"].includedirs = []
            self.cpp_info.components["wayland"].libdirs = []

