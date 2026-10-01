#!/usr/bin/env bash

# Regenerate the conan.lock beside each Conan recipe of the project. Run it after changing
# src/libsim_estab/conandata.yml or a recipe in buildsys/conan/recipe.
# One Linux profile covers all three, because dependencies are always built as Release.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"

cd "${repo_root}"

if ! command -v conan >/dev/null 2>&1; then
    echo "error: conan not found in PATH (activate .venv)" >&2
    exit 127
fi

export CONAN_HOME="${repo_root}/buildsys/conan_home"

for recipe in src/libsim_estab src/sim_estab_ext; do
    # --lockfile="": resolve from scratch instead of from the lockfile being replaced
    conan lock create "${recipe}" \
        -pr:a buildsys/conan/profile/linux_release \
        --lockfile="" \
        --lockfile-out "${recipe}/conan.lock"
done
