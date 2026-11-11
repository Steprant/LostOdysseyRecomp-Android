#!/usr/bin/env bash
# CI-only: modern C++23 tools on the AppImage's oldest supported glibc.
# Do not add a newer Ubuntu distribution's apt sources to obtain a compiler.
set -euo pipefail
. /etc/os-release
if [[ "$ID" != ubuntu || "$VERSION_ID" != 22.04 ]]; then
  echo 'AppImage release builds require Ubuntu 22.04 (glibc 2.35).' >&2
  exit 1
fi
: "${GITHUB_ENV:?This installer configures a GitHub Actions job}"
: "${GITHUB_PATH:?This installer configures a GitHub Actions job}"
sudo apt-get update
sudo apt-get install -y --no-install-recommends ca-certificates wget gnupg software-properties-common
key=$(mktemp)
trap 'rm -f "$key"' EXIT
# add-apt-repository asks the Launchpad API for the PPA's key; when that API
# times out (11/11/2026), the same PPA is configured directly with its key
# from the Ubuntu keyserver, checked against its fingerprint.
if ! timeout 120 sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test; then
  echo 'add-apt-repository failed; configuring ppa:ubuntu-toolchain-r/test directly.' >&2
  wget -qO "$key" 'https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x60C317803A41BA51845E371A1E9377A2BA9EF27F'
  ppa_fingerprint=$(gpg --show-keys --with-colons "$key" | awk -F: '$1 == "fpr" {print $10; exit}')
  if [[ "$ppa_fingerprint" != 60C317803A41BA51845E371A1E9377A2BA9EF27F ]]; then
    echo 'Unexpected ubuntu-toolchain-r signing key.' >&2
    exit 1
  fi
  gpg --dearmor < "$key" | sudo tee /usr/share/keyrings/lo-toolchain-r.gpg >/dev/null
  printf '%s\n' 'deb [signed-by=/usr/share/keyrings/lo-toolchain-r.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu jammy main' |
    sudo tee /etc/apt/sources.list.d/lo-toolchain-r.list >/dev/null
fi
wget -qO "$key" https://apt.llvm.org/llvm-snapshot.gpg.key
fingerprint=$(gpg --show-keys --with-colons "$key" | awk -F: '$1 == "fpr" {print $10; exit}')
if [[ "$fingerprint" != 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421 ]]; then
  echo 'Unexpected LLVM apt signing key.' >&2
  exit 1
fi
sudo install -m 644 "$key" /usr/share/keyrings/lo-llvm.asc
printf '%s\n' 'deb [arch=amd64 signed-by=/usr/share/keyrings/lo-llvm.asc] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main' |
  sudo tee /etc/apt/sources.list.d/lo-llvm.list >/dev/null
sudo apt-get update
sudo apt-get install -y --no-install-recommends clang-18 clang-tools-18 lld-18 g++-13 binutils libfuse2
# The preinstalled runner may have several GCC versions. Select the headers
# explicitly so clang cannot silently switch to a newer, unrelated toolchain.
gcc_dir=$(dirname "$(g++-13 -print-libgcc-file-name)")
{
  echo 'CC=clang-18'
  echo 'CXX=clang++-18'
  echo "CFLAGS=--gcc-install-dir=$gcc_dir"
  echo "CXXFLAGS=--gcc-install-dir=$gcc_dir"
  echo 'LDFLAGS=-fuse-ld=lld'
} >> "$GITHUB_ENV"
echo '/usr/lib/llvm-18/bin' >> "$GITHUB_PATH"
[[ "$(getconf GNU_LIBC_VERSION)" == 'glibc 2.35' ]]
clang++-18 --version
