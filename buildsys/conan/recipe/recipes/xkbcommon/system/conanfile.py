from conan import ConanFile, conan_version
from conan.tools.gnu import PkgConfig
from conan.tools.system import package_manager
from conan.errors import ConanInvalidConfiguration, ConanException
from conan.tools.scm import Version

required_conan_version = ">=1.50.0"


class XkbcommonConan(ConanFile):
    name = "xkbcommon"
    package_type = "shared-library"
    url = "https://github.com/conan-io/conan-center-index"
    license = "MIT"
    homepage = "https://xkbcommon.org/"
    description = "xkbcommon is a library for handling of keyboard descriptions"
    settings = "os", "arch", "compiler", "build_type"
    topics = ("xkbcommon", "keyboard", "xkb", "wayland", "x11")

    # make sure points here
    version = "system"

    def validate(self):
        if self.settings.os not in ["Linux", "FreeBSD"]:
            raise ConanInvalidConfiguration("This recipe supports only Linux and FreeBSD")

    def package_id(self):
        self.info.clear()

    def system_requirements(self):
        # Debian/Ubuntu
        apt = package_manager.Apt(self)
        apt.install([
            "libxkbcommon-dev",
            "libxkbcommon-x11-dev",
        ], update=True, check=True)

        # RedHat (RHEL, CentOS)
        yum = package_manager.Yum(self)
        yum.install([
            "libxkbcommon-devel",
            "libxkbcommon-x11-devel",
        ], update=True, check=True)

        # Fedora
        dnf = package_manager.Dnf(self)
        dnf.install([
            "libxkbcommon-devel",
            "libxkbcommon-x11-devel",
        ], update=True, check=True)

        # openSUSE
        zypper = package_manager.Zypper(self)
        zypper.install([
            "libxkbcommon-devel",
            "libxkbcommon-x11-devel",
        ], update=True, check=True)

        # Arch
        pacman = package_manager.PacMan(self)
        pacman.install([
            "libxkbcommon",
            "libxkbcommon-x11",
        ], update=True, check=True)

        # FreeBSD
        package_manager.Pkg(self).install([
            "libxkbcommon",
        ], update=True, check=True)

        # Alpine Linux
        if Version(conan_version) >= "2.0.10":
            alpine = package_manager.Apk(self)
            alpine.install([
                "libxkbcommon-dev",
                "libxkbcommon-x11",
            ], update=True, check=True)

    def package_info(self):
        if Version(conan_version) >= 2:
            self.cpp_info.bindirs = []
            self.cpp_info.includedirs = []
            self.cpp_info.libdirs = []

        # xkbcommon components
        for name in ["xkbcommon", "xkbcommon-x11"]:
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

        # xkbcommon-x11 depends on xkbcommon
        if "xkbcommon-x11" in self.cpp_info.components and "xkbcommon" in self.cpp_info.components:
            self.cpp_info.components["xkbcommon-x11"].requires = ["xkbcommon"]
