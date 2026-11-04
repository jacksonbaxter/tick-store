#!/usr/bin/env bash
# Installs pinned tools into .tools/, which git ignores.
#
#     scripts/install_tools.sh [actionlint] [shellcheck] [ninja] [llvm]
#
# A download is untrusted until its SHA-256 matches the pin below. Each one lands in a new,
# empty folder, and nothing is unpacked or run before that check passes. The pins come from
# each project's release page, and the digests GitHub publishes for the release assets.
# Exit 1 on a mismatch, leaving nothing installed. LLVM is about 1.6 GB to download, of which
# about 1.3 GB is unpacked: the tools the checks use, not the whole release.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
TOOLS=$ROOT/.tools

ACTIONLINT_VERSION=1.7.12
SHELLCHECK_VERSION=0.11.0
NINJA_VERSION=1.13.2
LLVM_VERSION=23.1.4

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)
        ACTIONLINT_ASSET=darwin_arm64
        ACTIONLINT_SHA256=aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f
        SHELLCHECK_ASSET=darwin.aarch64
        SHELLCHECK_SHA256=339b930feb1ea764467013cc1f72d09cd6b869ebf1013296ba9055ab2ffbd26f
        NINJA_ASSET=ninja-mac.zip
        NINJA_SHA256=c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b
        LLVM_ASSET=LLVM-$LLVM_VERSION-macOS-ARM64.tar.xz
        LLVM_SHA256=5d3179db39adeca5b17a34d82708fe976bc60e055b05fe557878fb3fb64d39ef
        ;;
    Linux-x86_64)
        ACTIONLINT_ASSET=linux_amd64
        ACTIONLINT_SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
        SHELLCHECK_ASSET=linux.x86_64
        SHELLCHECK_SHA256=b7af85e41cc99489dcc21d66c6d5f3685138f06d34651e6d34b42ec6d54fe6f6
        NINJA_ASSET=ninja-linux.zip
        NINJA_SHA256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
        LLVM_ASSET=LLVM-$LLVM_VERSION-Linux-X64.tar.xz
        LLVM_SHA256=25d814fda50d5fb0084b42be09605d8b1595d49d5e2e03a3345d7e112ff6f9f3
        ;;
    *)
        echo "install_tools.sh: no pins for $(uname -s) $(uname -m)" >&2
        exit 1
        ;;
esac

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

# Prints the path of the verified download. The caller removes its folder.
fetch() {
    local url=$1 expected=$2 folder file actual
    folder=$(mktemp -d "$TOOLS/download.XXXXXX")
    file=$folder/$(basename "$url")
    curl --fail --silent --show-error --location --retry 3 --output "$file" "$url"
    actual=$(sha256_of "$file")
    if [[ $actual != "$expected" ]]; then
        echo "install_tools.sh: SHA-256 mismatch for $url" >&2
        echo "  expected $expected" >&2
        echo "  actual   $actual" >&2
        rm -rf "$folder"
        return 1
    fi
    echo "$file"
}

install_actionlint() {
    local file
    file=$(fetch "https://github.com/rhysd/actionlint/releases/download/v$ACTIONLINT_VERSION/actionlint_${ACTIONLINT_VERSION}_$ACTIONLINT_ASSET.tar.gz" \
                 "$ACTIONLINT_SHA256")
    tar -xzf "$file" -C "$(dirname "$file")" actionlint
    mv "$(dirname "$file")/actionlint" "$TOOLS/actionlint"
    rm -rf "$(dirname "$file")"
    "$TOOLS/actionlint" -version | head -n 1
}

# actionlint runs shellcheck over the workflow's shell, if it can find it.
install_shellcheck() {
    local file member=shellcheck-v$SHELLCHECK_VERSION/shellcheck
    file=$(fetch "https://github.com/koalaman/shellcheck/releases/download/v$SHELLCHECK_VERSION/shellcheck-v$SHELLCHECK_VERSION.$SHELLCHECK_ASSET.tar.gz" \
                 "$SHELLCHECK_SHA256")
    tar -xzf "$file" -C "$(dirname "$file")" "$member"
    mv "$(dirname "$file")/$member" "$TOOLS/shellcheck"
    rm -rf "$(dirname "$file")"
    "$TOOLS/shellcheck" --version | sed -n 2p
}

install_ninja() {
    local file
    file=$(fetch "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/$NINJA_ASSET" \
                 "$NINJA_SHA256")
    unzip -q -o "$file" ninja -d "$(dirname "$file")"
    mv "$(dirname "$file")/ninja" "$TOOLS/ninja"
    rm -rf "$(dirname "$file")"
    "$TOOLS/ninja" --version
}

# The part of the LLVM release the build and the checks use: the compiler with its headers
# and runtimes, clang-tidy, lld, the coverage tools, llvm-symbolizer for sanitizer reports,
# the binutils CMake looks for beside the compiler, macOS's static libc++, and the libLTO
# clang names to Apple's linker. Unpacked whole, the Linux release is 12 GB, and it filled
# a hosted runner's disk.
LLVM_KEEP='^[^/]+/(include/|lib/clang/|lib/[^/]+-unknown-linux-gnu/|lib/lib(c\+\+|c\+\+abi|unwind)[.a-z0-9]*$|lib/libLTO\.dylib$|bin/(clang|clang\+\+|clang-[0-9]+|clang-tidy|clang-scan-deps|lld|ld\.lld|ld64\.lld|llvm-(profdata|cov|cxxfilt|symbolizer|addr2line|ar|ranlib|dlltool|lib|nm|objcopy|strip|install-name-tool|objdump|otool|readobj|readelf))$)'
LLVM_TOOLS=(clang++ clang-tidy ld.lld ld64.lld llvm-profdata llvm-cov llvm-cxxfilt llvm-symbolizer
            llvm-ar llvm-nm)

install_llvm() {
    local file unpacked members tool
    file=$(fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/$LLVM_ASSET" \
                 "$LLVM_SHA256")
    members=$(dirname "$file")/members
    tar -tJf "$file" | grep -E "$LLVM_KEEP" >"$members"
    unpacked=$(dirname "$file")/llvm
    mkdir "$unpacked"
    # Every member is named, directories included, so none is reached through its parent:
    # GNU tar would then call the member's own name not found.
    tar -xJf "$file" -C "$unpacked" --strip-components=1 --no-recursion -T "$members"
    for tool in "${LLVM_TOOLS[@]}"; do
        if [[ ! -x $unpacked/bin/$tool ]]; then
            echo "install_tools.sh: the LLVM release has no bin/$tool" >&2
            rm -rf "$(dirname "$file")"
            return 1
        fi
    done
    rm -rf "$TOOLS/llvm"
    mv "$unpacked" "$TOOLS/llvm"
    rm -rf "$(dirname "$file")"
    "$TOOLS/llvm/bin/clang++" --version | head -n 1
}

if [[ $# -eq 0 ]]; then
    sed -n '2,/^# Exit 1/s/^# \{0,1\}//p' "${BASH_SOURCE[0]}" >&2
    exit 2
fi

mkdir -p "$TOOLS"
for tool in "$@"; do
    case $tool in
        actionlint) install_actionlint ;;
        shellcheck) install_shellcheck ;;
        ninja) install_ninja ;;
        llvm) install_llvm ;;
        *) echo "install_tools.sh: unknown tool: $tool" >&2; exit 2 ;;
    esac
done
