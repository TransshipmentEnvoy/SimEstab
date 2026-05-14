#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/.." && pwd)"

cd "${repo_root}"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "error: clang-format not found in PATH" >&2
    exit 127
fi

find src test -type f \
    \( \
        -name '*.c' -o \
        -name '*.cc' -o \
        -name '*.cpp' -o \
        -name '*.cxx' -o \
        -name '*.h' -o \
        -name '*.hh' -o \
        -name '*.hpp' -o \
        -name '*.hxx' -o \
        -name '*.ipp' -o \
        -name '*.tpp' -o \
        -name '*.cppm' -o \
        -name '*.ixx' \
    \) \
    -print0 | xargs -0 -r clang-format -style=file -i
