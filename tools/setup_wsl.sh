#!/usr/bin/env bash
# Prepare a WSL2 Ubuntu (24.04 or 26.04) for the Linux release: tools/make_release_linux.py, its --prove, and the
# release skill's scripts (.claude/skills/release/scripts).
#
#   wsl.exe -d Ubuntu -- bash /mnt/c/<checkout>/tools/setup_wsl.sh
#
# make_release_linux.py builds everything the player's machine runs against the Steam Runtime sniper sysroot it
# downloads, with the LLVM release it pins, so the distribution's compilers and libraries are not used for the release
# itself. What it needs from Ubuntu: make, a C compiler and Perl (hactool, zstd, OpenSSL's Configure, Tcl/Tk), the
# .NET 10 SDK (the shader translator), tar/xz/unzip/curl/git, and a Python 3.12+ with Pillow (the AppImage icon). The
# prove links the game and runs the probes, which open a window: WSLg and Mesa's Vulkan drivers (dozen, over /dev/dxg)
# give them a device.
#
# Decisions:
# - The Python is ~/venv-ac, on ext4 and outside the checkout: the checkout is the Windows one under /mnt/c, whose
#   .venv is the Windows venv (an earlier version of this script made .venv in the current directory and replaced it).
# - The release skill's scripts name /home/a (the account the earlier releases were made with): another account gets a
#   /home/a link to its home.
# - Idempotent: a second run only checks.
set -euo pipefail

sudo apt-get update -qq
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  build-essential make perl pkg-config git curl xz-utils unzip file \
  python3 python3-venv python3-pip dotnet-sdk-10.0 \
  libvulkan1 mesa-vulkan-drivers vulkan-tools libxft2 libxrender1 libgl1 libegl1

if [ ! -e /home/a ]; then
  sudo ln -s "$HOME" /home/a
fi
if [ ! -x ~/venv-ac/bin/python ]; then
  python3 -m venv ~/venv-ac
fi
~/venv-ac/bin/pip install -q Pillow

echo "--- release prerequisites"
~/venv-ac/bin/python -c "import PIL, sys; print('python', sys.version.split()[0], 'Pillow', PIL.__version__)"
dotnet --list-sdks
make --version | head -n 1
perl -e 'print "perl $^V\n"'
ls -ld /home/a
echo "--- vulkan (the prove's probes)"
vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName|apiVersion' || echo "no Vulkan device: the probes of --prove will fail"
echo "setup_wsl: ok"
