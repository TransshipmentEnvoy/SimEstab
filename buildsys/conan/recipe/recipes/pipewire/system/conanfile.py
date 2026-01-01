from conan import ConanFile, conan_version
from conan.tools.gnu import PkgConfig
from conan.tools.system import package_manager
from conan.errors import ConanInvalidConfiguration, ConanException
from conan.tools.scm import Version

required_conan_version = ">=1.50.0"


class PipewireConan(ConanFile):
    name = "pipewire"
    package_type = "shared-library"
    url = "https://github.com/conan-io/conan-center-index"
    license = "MIT"
    homepage = "https://pipewire.org/"
    description = "PipeWire is a server and user space API to deal with multimedia pipelines"
    settings = "os", "arch", "compiler", "build_type"
    topics = ("pipewire", "multimedia", "audio", "video")

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
            "libpipewire-0.3-dev",
            "libpipewire-0.3-0",
            "libspa-0.2-dev",
        ], update=True, check=True)

        # RedHat (RHEL, CentOS)
        yum = package_manager.Yum(self)
        yum.install([
            "pipewire-devel",
            "pipewire-libs",
        ], update=True, check=True)

        # Fedora
        dnf = package_manager.Dnf(self)
        dnf.install([
            "pipewire-devel",
            "pipewire-libs",
        ], update=True, check=True)

        # openSUSE
        zypper = package_manager.Zypper(self)
        zypper.install([
            "pipewire-devel",
        ], update=True, check=True)

        # Arch
        pacman = package_manager.PacMan(self)
        pacman.install([
            "pipewire",
        ], update=True, check=True)

        # FreeBSD
        package_manager.Pkg(self).install([
            "pipewire",
        ], update=True, check=True)

        # Alpine Linux
        if Version(conan_version) >= "2.0.10":
            alpine = package_manager.Apk(self)
            alpine.install([
                "pipewire-dev",
            ], update=True, check=True)

    def package_info(self):
        if Version(conan_version) >= 2:
            self.cpp_info.bindirs = []
            self.cpp_info.includedirs = []
            self.cpp_info.libdirs = []

        # PipeWire core component
        for name in ["libpipewire-0.3"]:
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

        # Add simplified component name for compatibility (pipewire -> libpipewire-0.3)
        # This component inherits from libpipewire-0.3 and should have empty dirs
        if "libpipewire-0.3" in self.cpp_info.components:
            self.cpp_info.components["pipewire"].requires = ["libpipewire-0.3"]
            self.cpp_info.components["pipewire"].bindirs = []
            self.cpp_info.components["pipewire"].includedirs = []
            self.cpp_info.components["pipewire"].libdirs = []
