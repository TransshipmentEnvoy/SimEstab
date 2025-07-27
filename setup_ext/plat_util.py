# Comprehensive list of distutils platform specifiers
# These are the platform strings that distutils/setuptools use to identify target platforms
DISTUTILS_PLATFORM_SPECIFIERS = [
    # Windows platforms
    "win32",  # 32-bit Windows (Intel x86)
    "win-amd64",  # 64-bit Windows (AMD64/Intel x64)
    "win-arm32",  # 32-bit Windows on ARM
    "win-arm64",  # 64-bit Windows on ARM64

    # macOS platforms (format: macosx-{min_version}-{arch})
    "macosx-10.9-x86_64",  # macOS 10.9+ on Intel x64
    "macosx-10.9-intel",  # macOS 10.9+ on Intel (universal)
    "macosx-10.9-universal2",  # macOS 10.9+ universal2 (Intel + Apple Silicon)
    "macosx-11.0-arm64",  # macOS 11.0+ on Apple Silicon
    "macosx-12.0-universal2",  # macOS 12.0+ universal2
    "macosx-13.0-arm64",  # macOS 13.0+ on Apple Silicon
    "macosx-14.0-arm64",  # macOS 14.0+ on Apple Silicon

    # Linux platforms
    "linux-x86_64",  # Linux on x86_64 (most common)
    "linux-i686",  # Linux on i686 (32-bit Intel)
    "linux-aarch64",  # Linux on ARM64 (64-bit ARM)
    "linux-armv7l",  # Linux on ARMv7 (32-bit ARM)
    "linux-armv6l",  # Linux on ARMv6 (32-bit ARM)
    "linux-ppc64le",  # Linux on PowerPC 64-bit little-endian
    "linux-ppc64",  # Linux on PowerPC 64-bit big-endian
    "linux-s390x",  # Linux on IBM System z
    "linux-riscv64",  # Linux on RISC-V 64-bit

    # FreeBSD platforms
    "freebsd-12-amd64",  # FreeBSD 12 on AMD64
    "freebsd-13-amd64",  # FreeBSD 13 on AMD64
    "freebsd-14-amd64",  # FreeBSD 14 on AMD64

    # Other Unix-like platforms
    "netbsd-9-amd64",  # NetBSD
    "openbsd-7-amd64",  # OpenBSD
    "dragonfly-6-x86_64",  # DragonFly BSD

    # Solaris
    "solaris-2.11-x86_64",  # Solaris on x86_64
    "solaris-2.11-sparc64",  # Solaris on SPARC

    # AIX
    "aix-7.2-powerpc64",  # AIX on PowerPC

    # Special/Generic platforms
    "any",  # Platform-independent (pure Python)
]


def get_current_platform() -> str:
    """Get the current platform using sysconfig."""
    try:
        import sysconfig
        return sysconfig.get_platform()
    except ImportError:
        try:
            # Note: distutils is deprecated in Python 3.12+
            from distutils.util import get_platform  # type: ignore
            return get_platform()
        except ImportError:
            # Fallback - construct manually
            import platform
            system = platform.system().lower()
            machine = platform.machine().lower()

            if system == "windows":
                if machine in ("amd64", "x86_64"):
                    return "win-amd64"
                elif machine == "arm64":
                    return "win-arm64"
                else:
                    return "win32"
            elif system == "darwin":
                import platform
                version = platform.mac_ver()[0]
                if version:
                    major, minor = version.split(".")[:2]
                    mac_version = f"{major}.{minor}"
                else:
                    mac_version = "10.9"  # fallback

                if machine in ("arm64", "aarch64"):
                    return f"macosx-{mac_version}-arm64"
                else:
                    return f"macosx-{mac_version}-x86_64"
            elif system == "linux":
                return f"linux-{machine}"
            else:
                return f"{system}-{machine}"


def is_valid_platform_specifier(platform_spec: str) -> bool:
    """Check if a platform specifier is in the known list."""
    return platform_spec in DISTUTILS_PLATFORM_SPECIFIERS


def get_platform_category(platform_spec: str) -> str:
    """Get the general category (OS) of a platform specifier."""
    if platform_spec.startswith(("win32", "win-")):
        return "windows"
    elif platform_spec.startswith("macosx-"):
        return "macos"
    elif platform_spec.startswith("linux-"):
        return "linux"
    elif platform_spec.startswith("freebsd-"):
        return "freebsd"
    elif platform_spec.startswith(("netbsd-", "openbsd-", "dragonfly-")):
        return "bsd"
    elif platform_spec.startswith("solaris-"):
        return "solaris"
    elif platform_spec.startswith("aix-"):
        return "aix"
    elif platform_spec == "any":
        return "any"
    else:
        return "unknown"


def list_platform_specifiers_by_category() -> dict[str, list[str]]:
    """Return platform specifiers grouped by category."""
    categories: dict[str, list[str]] = {}

    for platform in DISTUTILS_PLATFORM_SPECIFIERS:
        category = get_platform_category(platform)
        if category not in categories:
            categories[category] = []
        categories[category].append(platform)

    return categories


def print_platform_specifiers() -> None:
    """Print all platform specifiers grouped by category."""
    categories = list_platform_specifiers_by_category()

    print("Distutils Platform Specifiers:")
    print("=" * 50)

    for category, platforms in sorted(categories.items()):
        print(f"\n{category.upper()}:")
        for platform in platforms:
            print(f"  {platform}")

    print(f"\nTotal: {len(DISTUTILS_PLATFORM_SPECIFIERS)} platform specifiers")
