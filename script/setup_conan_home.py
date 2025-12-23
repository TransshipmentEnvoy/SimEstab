#!/usr/bin/env python3
"""
Setup Conan home by copying the appropriate default profile based on current platform.

This script:
1. Copies the platform-specific Conan profile from buildsys/conan/profile_home/
   to buildsys/conan_home/profiles/default
2. Checks if buildsys/conan/recipe is registered as a Conan remote repository
3. Ensures the recipe repository has the highest priority
"""
import os
import sys
import shutil
from pathlib import Path
import subprocess
import json

# Add setup_ext to path to import utilities
script_dir = Path(__file__).parent
project_root = script_dir.parent
sys.path.insert(0, str(project_root / "setup_ext"))

from plat_util import get_current_platform, get_platform_category


def get_profile_name_for_platform(platform_category: str) -> str:
    """
    Map platform category to profile filename.
    
    Args:
        platform_category: Platform category (e.g., 'linux', 'windows', 'macos')
    
    Returns:
        Profile filename (e.g., 'default.linux')
    """
    profile_map = {
        "linux": "default.linux",
        "windows": "default.windows",
        "macos": "default.macos",
        "darwin": "default.macos",  # Alternative naming
    }

    profile_name = profile_map.get(platform_category)
    if profile_name is None:
        raise ValueError(f"Unsupported platform category: {platform_category}. "
                         f"Supported platforms: {', '.join(profile_map.keys())}")

    return profile_name


def setup_conan_profile(verbose: bool = True) -> None:
    """
    Copy the appropriate Conan profile to the default location.
    
    Args:
        verbose: Whether to print status messages
    """
    # Determine paths
    profile_home_dir = project_root / "buildsys" / "conan" / "profile_home"
    conan_home_dir = project_root / "buildsys" / "conan_home"
    profiles_dir = conan_home_dir / "profiles"
    target_profile = profiles_dir / "default"

    # Get current platform
    current_platform = get_current_platform()
    platform_category = get_platform_category(current_platform)

    if verbose:
        print(f"Detected platform: {current_platform}")
        print(f"Platform category: {platform_category}")

    # Get source profile filename
    try:
        profile_name = get_profile_name_for_platform(platform_category)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    source_profile = profile_home_dir / profile_name

    # Check if source profile exists
    if not source_profile.exists():
        print(f"Error: Source profile not found: {source_profile}", file=sys.stderr)
        print(f"Please create {profile_name} in {profile_home_dir}", file=sys.stderr)
        sys.exit(1)

    # Create profiles directory if it doesn't exist
    profiles_dir.mkdir(parents=True, exist_ok=True)

    # Copy profile
    try:
        shutil.copy2(source_profile, target_profile)
        if verbose:
            print(f"✓ Copied {source_profile.name} -> {target_profile}")
            print(f"  Source: {source_profile}")
            print(f"  Target: {target_profile}")
    except Exception as e:
        print(f"Error copying profile: {e}", file=sys.stderr)
        sys.exit(1)

    if verbose:
        print("\nConan profile setup complete!")


def setup_conan_recipe_remote(verbose: bool = True) -> None:
    """
    Setup local Conan recipe repository as a remote with highest priority.

    This function:
    1. Checks if the recipe directory is already registered as a remote
    2. If not registered, adds it as a new remote
    3. Ensures it has the highest priority (priority 0)

    Args:
        verbose: Whether to print status messages
    """
    # Get recipe directory path
    recipe_dir = project_root / "buildsys" / "conan" / "recipe"
    recipe_dir_abs = recipe_dir.resolve()

    if not recipe_dir_abs.exists():
        if verbose:
            print(f"Warning: Recipe directory does not exist: {recipe_dir_abs}")
        return

    # Set CONAN_HOME environment variable
    conan_home = project_root / "buildsys" / "conan_home"
    env = os.environ.copy()
    env["CONAN_HOME"] = str(conan_home.resolve())

    remote_name = "bundled-recipe-repo"
    remote_url = str(recipe_dir_abs)

    try:
        # Get list of current remotes in JSON format
        result = subprocess.run(["conan", "remote", "list", "--format=json"],
                                capture_output=True,
                                text=True,
                                check=True,
                                env=env)

        remotes_list = json.loads(result.stdout)

        # Check if our recipe directory is already registered
        remote_exists = False
        current_priority = None

        for idx, remote in enumerate(remotes_list):
            if remote.get("url") == remote_url or remote.get("name") == remote_name:
                remote_exists = True
                current_priority = idx  # Priority is the index in the list
                if verbose:
                    print(f"✓ Recipe repository already registered: {remote.get('name')}")
                    print(f"  URL: {remote.get('url')}")
                    print(f"  Priority: {current_priority}")
                break

        # Add remote if it doesn't exist
        if not remote_exists:
            if verbose:
                print(f"Adding recipe repository as remote '{remote_name}'...")

            subprocess.run(["conan", "remote", "add", remote_name, remote_url],
                           check=True,
                           env=env,
                           capture_output=True)

            if verbose:
                print(f"✓ Added recipe repository: {remote_name}")
                print(f"  URL: {remote_url}")

            current_priority = len(remotes_list)  # Will be last

        # Ensure highest priority (priority 0)
        if current_priority != 0:
            if verbose:
                print(f"Setting '{remote_name}' to highest priority...")

            subprocess.run(["conan", "remote", "update", remote_name, "--index=0"],
                           check=True,
                           env=env,
                           capture_output=True)

            if verbose:
                print(f"✓ Set '{remote_name}' to highest priority (0)")
        else:
            if verbose:
                print(f"✓ '{remote_name}' already has highest priority")

        if verbose:
            print("\nConan recipe remote setup complete!")

    except subprocess.CalledProcessError as e:
        print(f"Error managing Conan remotes: {e}", file=sys.stderr)
        if e.stderr:
            print(f"  {e.stderr}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error parsing Conan remote list: {e}", file=sys.stderr)
        sys.exit(1)


def main():
    """Main entry point."""
    import argparse

    parser = argparse.ArgumentParser(
        description="Setup Conan home with appropriate default profile for current platform")
    parser.add_argument("-q", "--quiet", action="store_true", help="Suppress verbose output")
    parser.add_argument("--show-platform", action="store_true", help="Show detected platform information and exit")

    args = parser.parse_args()

    if args.show_platform:
        current_platform = get_current_platform()
        platform_category = get_platform_category(current_platform)
        print(f"Platform: {current_platform}")
        print(f"Category: {platform_category}")
        try:
            profile_name = get_profile_name_for_platform(platform_category)
            print(f"Profile: {profile_name}")
        except ValueError as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)
        return

    verbose = not args.quiet
    setup_conan_profile(verbose=verbose)
    setup_conan_recipe_remote(verbose=verbose)


if __name__ == "__main__":
    main()
