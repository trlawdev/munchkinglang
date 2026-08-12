#!/usr/bin/env bash
# Extensive release build for munxc (no C++ exceptions / RTTI).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

: "${BUILD_TYPE:=release}"
: "${OUTPUT:=munxc}"

# Prefer clang when both are installed; honour an explicit CXX override.
pick_cxx() {
    if [[ -n "${CXX:-}" ]]; then
        echo "$CXX"
        return
    fi
    if command -v clang++ >/dev/null 2>&1; then
        echo "clang++"
    elif command -v g++ >/dev/null 2>&1; then
        echo "g++"
    else
        echo "compile.sh: no C++ compiler found (need clang++ or g++)" >&2
        exit 1
    fi
}

CXX="$(pick_cxx)"

detect_compiler_family() {
    if "$CXX" --version 2>&1 | grep -qiE '(clang|LLVM)'; then
        echo "clang"
    else
        echo "gcc"
    fi
}

# Return 0 when @p flag is accepted for a minimal compile.
flag_supported() {
    local flag=$1
    "$CXX" $flag -c -x c++ - -o /dev/null 2>/dev/null <<<'int main(){return 0;}'
}

COMPILER_FAMILY="$(detect_compiler_family)"

common=(
    -std=c++20
    -Iinclude
    -pthread
    -fno-exceptions
    -fno-rtti
    -ffunction-sections
    -fdata-sections
    -fstack-protector-strong
    -pipe
    -march=native
    -ffast-math
    -fno-math-errno
    -fno-trapping-math
    -fno-semantic-interposition
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-plt
    -Wl,-O3
    -Wl,--gc-sections
    -Wl,--as-needed
)

warnings=(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wnull-dereference
    -Wundef
    -Wcast-align
    -Wunused
    -Wwrite-strings
)

compiler_flags=()
linker_flags=()

case "$COMPILER_FAMILY" in
    clang)
        compiler_flags=(
            -flto=thin
        )
        ;;
    gcc)
        compiler_flags=(
            -flto=auto
            -fipa-pta
            -fdevirtualize-at-ltrans
            -fdevirtualize-speculatively
            -fversion-loops-for-strides
            -funswitch-loops
            -fwhole-program
        )
        ;;
esac

if command -v mold >/dev/null 2>&1 && flag_supported -fuse-ld=mold; then
    linker_flags+=(-fuse-ld=mold -Wl,--icf=safe)
elif command -v ld.lld >/dev/null 2>&1 && flag_supported -fuse-ld=lld; then
    linker_flags+=(-fuse-ld=lld)
fi

case "$BUILD_TYPE" in
    release)
        opt=(-O3 -DNDEBUG -fomit-frame-pointer)
        if [[ "$COMPILER_FAMILY" == "gcc" ]]; then
            opt+=(-flto)
        fi
        ld=(-Wl,-O1)
        ;;
    debug)
        opt=(-O0 -g3 -DDEBUG -fno-omit-frame-pointer)
        ld=()
        compiler_flags=()
        linker_flags=()
        ;;
    sanitize)
        opt=(-O1 -g3 -DDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
        ld=(-fsanitize=address,undefined)
        compiler_flags=()
        linker_flags=()
        ;;
    *)
        echo "Unknown BUILD_TYPE=$BUILD_TYPE (use release|debug|sanitize)" >&2
        exit 1
        ;;
esac

arch="$(uname -m)"
simd=()
if [[ "$arch" == "x86_64" || "$arch" == "amd64" ]]; then
    simd=(-mavx2 -mfma)
fi

# Native AOT backends compiled into munxc (custom MIR→C, llvm MIR→IR text).
MUNX_NATIVE_BACKEND="${MUNX_NATIVE_BACKEND:-custom}"
native_flags=("-DMUNX_NATIVE_RUNTIME_DIR=\"$root/native/runtime\"")
case "$MUNX_NATIVE_BACKEND" in
    custom)
        native_flags+=(-DMUNX_NATIVE_CUSTOM=1 -DMUNX_NATIVE_LLVM=0)
        ;;
    llvm)
        native_flags+=(-DMUNX_NATIVE_CUSTOM=0 -DMUNX_NATIVE_LLVM=1)
        ;;
    both)
        native_flags+=(-DMUNX_NATIVE_CUSTOM=1 -DMUNX_NATIVE_LLVM=1)
        ;;
    *)
        echo "MUNX_NATIVE_BACKEND must be custom, llvm, or both" >&2
        exit 1
        ;;
esac

# POSIX dynamic loading (`dlopen` / `dlsym`) for `load_library`.
extra_libs=()
if [[ "$(uname -s)" != "Darwin" && "$(uname -s)" != *MINGW* && "$(uname -s)" != *MSYS* ]]; then
    extra_libs+=(-ldl)
fi

echo "Compiling $OUTPUT ($BUILD_TYPE, $CXX [$COMPILER_FAMILY], native=$MUNX_NATIVE_BACKEND, exceptions disabled)..."
"$CXX" "${common[@]}" "${compiler_flags[@]}" "${warnings[@]}" "${opt[@]}" "${simd[@]}" \
    "${native_flags[@]}" \
    src/main.cpp -o "$OUTPUT" "${ld[@]}" "${linker_flags[@]}" "${extra_libs[@]}"
echo "Built ./$OUTPUT"
