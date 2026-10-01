from conan import ConanFile, conan_version
from conan.tools.gnu import PkgConfig
from conan.tools.system import package_manager
from conan.errors import ConanInvalidConfiguration, ConanException
from conan.tools.scm import Version

required_conan_version = ">=1.50.0"


class LibdecorConan(ConanFile):
    name = "libdecor"
    package_type = "shared-library"
    url = "https://github.com/conan-io/conan-center-index"
    license = "MIT"
    homepage = "https://gitlab.freedesktop.org/libdecor/libdecor"
    description = "Client-side window decorations for Wayland"
    settings = "os", "arch", "compiler", "build_type"
    topics = ("libdecor", "wayland", "decorations")

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
        apt.install(["libdecor-0-dev"], update=True, check=True)

        # RedHat (RHEL, CentOS)
        yum = package_manager.Yum(self)
        yum.install(["libdecor-devel"], update=True, check=True)

        # Fedora
        dnf = package_manager.Dnf(self)
        dnf.install(["libdecor-devel"], update=True, check=True)

        # openSUSE
        zypper = package_manager.Zypper(self)
        zypper.install(["libdecor-devel"], update=True, check=True)

        # Arch
        pacman = package_manager.PacMan(self)
        pacman.install(["libdecor"], update=True, check=True)

        # FreeBSD
        package_manager.Pkg(self).install(["libdecor"], update=True, check=True)

        # Alpine Linux
        if Version(conan_version) >= "2.0.10":
            alpine = package_manager.Apk(self)
            alpine.install(["libdecor-dev"], update=True, check=True)

    def package_info(self):
        if Version(conan_version) >= 2:
            self.cpp_info.bindirs = []
            self.cpp_info.includedirs = []
            self.cpp_info.libdirs = []

        # Unlike the other system recipes, a missing libdecor is an error rather than a warning:
        # SDL would otherwise build its Wayland driver without window decorations, and nothing
        # would say so.
        name = "libdecor-0"
        try:
            pkg_config = PkgConfig(self, name)
            pkg_config.fill_cpp_info(self.cpp_info.components[name], is_system=self.settings.os != "FreeBSD")
        except ConanException as e:
            raise ConanException(f"{name} not found by pkg-config. Install the libdecor development package "
                                 "(Debian/Ubuntu: libdecor-0-dev, Fedora/openSUSE: libdecor-devel, "
                                 "Arch: libdecor).") from e
        self.cpp_info.components[name].version = pkg_config.version
        self.cpp_info.components[name].set_property("pkg_config_name", name)
        self.cpp_info.components[name].set_property("component_version", pkg_config.version)
        self.cpp_info.components[name].set_property(
            "pkg_config_custom_content",
            "\n".join(f"{key}={value}" for key, value in pkg_config.variables.items()
                      if key not in ["pcfiledir", "prefix", "includedir"]))

        # Add simplified component name for compatibility (libdecor -> libdecor-0)
        self.cpp_info.components["libdecor"].requires = [name]
        self.cpp_info.components["libdecor"].bindirs = []
        self.cpp_info.components["libdecor"].includedirs = []
        self.cpp_info.components["libdecor"].libdirs = []
