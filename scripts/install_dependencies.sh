#!/usr/bin/env bash

set -Eeuo pipefail

if [[ ! -r /etc/os-release ]]; then
    echo "Cannot detect the Linux distribution." >&2
    exit 1
fi

# shellcheck disable=SC1091
source /etc/os-release

case "${ID:-}" in
    ubuntu|debian)
        package_manager=(apt-get)
        packages=(
            build-essential
            clang
            clang-format
            clang-tidy
            cmake
            ccache
            git
            libgtest-dev
            netcat-openbsd
            ninja-build
            pkg-config
            python3
        )
        ;;
    fedora)
        package_manager=(dnf)
        packages=(
            gcc-c++
            clang
            clang-tools-extra
            cmake
            ccache
            git
            gtest-devel
            ninja-build
            nmap-ncat
            pkgconf-pkg-config
            python3
        )
        ;;
    *)
        echo "Unsupported distribution: ${ID:-unknown}" >&2
        echo "Supported distributions: Ubuntu, Debian, Fedora." >&2
        exit 1
        ;;
esac

if (( EUID == 0 )); then
    runner=()
elif command -v sudo >/dev/null 2>&1; then
    runner=(sudo)
else
    echo "Please run this script as root or install sudo." >&2
    exit 1
fi

if [[ "${package_manager[0]}" == "apt-get" ]]; then
    "${runner[@]}" apt-get update
    "${runner[@]}" apt-get install -y "${packages[@]}"
else
    "${runner[@]}" dnf install -y "${packages[@]}"
fi

echo "Sphinx dependencies installed successfully."
