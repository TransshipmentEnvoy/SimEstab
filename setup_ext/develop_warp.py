import setuptools
from setuptools.command.develop import develop
from distutils import log


class CustomDevelop(develop):
    user_options = develop.user_options + [
        ("debug", "g", "compile/link with debugging information"),
    ]

    def initialize_options(self) -> None:
        super().initialize_options()
        self.debug = None

    def finalize_options(self) -> None:
        super().finalize_options()

    def install_for_development(self):
        self.run_command("egg_info")

        # Build extensions in-place
        self.reinitialize_command("build_ext", inplace=1)
        if self.debug:
            self.reinitialize_command("build_clib", debug=1)
            self.reinitialize_command("build_ext", inplace=1, debug=1)
        self.run_command("build_clib")
        self.run_command("build_ext")

        if setuptools.bootstrap_install_from:
            self.easy_install(setuptools.bootstrap_install_from)
            setuptools.bootstrap_install_from = None

        self.install_namespaces()

        # create an .egg-link in the installation dir, pointing to our egg
        log.info("Creating %s (link to %s)", self.egg_link, self.egg_base)
        if not self.dry_run:
            with open(self.egg_link, "w") as f:
                f.write(self.egg_path + "\n" + self.setup_path)
        # postprocess the installed distro, fixing up .pth, installing scripts,
        # and handling requirements
        self.process_distribution(None, self.dist, not self.no_deps)
