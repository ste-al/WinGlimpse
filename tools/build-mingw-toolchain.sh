#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the cross toolchain used for the native Glimpse.exe on Linux: clang/lld 18 from the
# distribution plus, for x86_64-w64-mingw32 and aarch64-w64-mingw32, the mingw-w64 headers and
# CRT (UCRT), compiler-rt builtins, libunwind, libc++abi and libc++ (static). This is the same
# recipe as llvm-mingw, without rebuilding LLVM itself. Sources come from the Ubuntu archive.
#
# Usage: tools/build-mingw-toolchain.sh [prefix]          (default /opt/glimpse-mingw)
# Needs: clang-18 lld-18 llvm-18 (Ubuntu 24.04), cmake, ninja, make, curl, xz, bzip2.
# Then:  ./build-native.sh   (uses <prefix>/bin/{x86_64,aarch64}-w64-mingw32-clang++)
set -euo pipefail

PREFIX="${1:-/opt/glimpse-mingw}"
WORK="${WORK:-/tmp/glimpse-toolchain}"
LLVM_VERSION=18.1.3
MINGW_VERSION=12.0.0
LLVMBIN=/usr/lib/llvm-18/bin
JOBS="$(nproc)"
ARCHS="${ARCHS:-x86_64 aarch64}"

mkdir -p "$WORK" "$PREFIX/bin"
cd "$WORK"

fetch() { [ -f "$2" ] || curl -fsSL --retry 4 -o "$2.part" "$1" && { [ -f "$2" ] || mv "$2.part" "$2"; }; }
fetch "http://archive.ubuntu.com/ubuntu/pool/universe/m/mingw-w64/mingw-w64_${MINGW_VERSION}.orig.tar.bz2" mingw.tar.bz2
fetch "http://archive.ubuntu.com/ubuntu/pool/main/l/llvm-toolchain-18/llvm-toolchain-18_${LLVM_VERSION}.orig.tar.xz" llvm.tar.xz

if [ ! -d mingw-src ]; then
  mkdir mingw-src && tar -xjf mingw.tar.bz2 -C mingw-src --strip-components=1
fi
if [ ! -d llvm-src ]; then
  top="$( (tar -tJf llvm.tar.xz 2>/dev/null || true) | head -1 | cut -d/ -f1)"
  mkdir llvm-src
  tar -xJf llvm.tar.xz -C llvm-src --strip-components=1 \
    "$top/cmake" "$top/compiler-rt" "$top/libcxx" "$top/libcxxabi" "$top/libunwind" \
    "$top/runtimes" "$top/llvm/cmake" "$top/llvm/utils/llvm-lit" "$top/third-party"
fi

# private clang resource directory: the compiler-rt builtins for Windows go here
RES="$PREFIX/lib/clang/18"
mkdir -p "$RES/lib/windows"
[ -d "$RES/include" ] || cp -r /usr/lib/llvm-18/lib/clang/18/include "$RES/include"

for arch in $ARCHS; do
  triple="$arch-w64-mingw32"
  for tool in clang clang++; do
    cat > "$PREFIX/bin/$triple-$tool" <<EOF
#!/bin/sh
exec "$LLVMBIN/$tool" --target=$triple --sysroot="$PREFIX/$triple" -resource-dir="$RES" \\
  -rtlib=compiler-rt -unwindlib=libunwind -stdlib=libc++ -fuse-ld=lld "\$@"
EOF
    chmod +x "$PREFIX/bin/$triple-$tool"
  done
  for tool in ar ranlib dlltool rc windres strip objcopy nm; do ln -sf "$LLVMBIN/llvm-$tool" "$PREFIX/bin/$triple-$tool"; done
done

for arch in $ARCHS; do
  triple="$arch-w64-mingw32"
  sysroot="$PREFIX/$triple"
  export CC="$PREFIX/bin/$triple-clang" CXX="$PREFIX/bin/$triple-clang++"
  export AR="$LLVMBIN/llvm-ar" RANLIB="$LLVMBIN/llvm-ranlib" DLLTOOL="$LLVMBIN/llvm-dlltool"

  if [ ! -f "$sysroot/include/windows.h" ]; then
    echo "== $triple: mingw-w64 headers"
    rm -rf "build-headers-$arch" && mkdir "build-headers-$arch" && cd "build-headers-$arch"
    ../mingw-src/mingw-w64-headers/configure --host="$triple" --prefix="$sysroot" \
      --enable-idl --with-default-win32-winnt=0x601 --with-default-msvcrt=ucrt > /dev/null
    make install > /dev/null
    cd "$WORK"
  fi

  if [ ! -f "$sysroot/lib/libkernel32.a" ]; then
    echo "== $triple: mingw-w64 CRT"
    case "$arch" in
      x86_64) crtflags="--disable-lib32 --enable-lib64 --disable-libarm32 --disable-libarm64" ;;
      aarch64) crtflags="--disable-lib32 --disable-lib64 --disable-libarm32 --enable-libarm64" ;;
    esac
    rm -rf "build-crt-$arch" && mkdir "build-crt-$arch" && cd "build-crt-$arch"
    ../mingw-src/mingw-w64-crt/configure --host="$triple" --prefix="$sysroot" --with-sysroot="$sysroot" \
      --with-default-msvcrt=ucrt $crtflags > /dev/null
    make -j"$JOBS" > /dev/null
    make install > /dev/null
    cd "$WORK"
  fi

  if [ ! -f "$RES/lib/windows/libclang_rt.builtins-$arch.a" ]; then
    echo "== $triple: compiler-rt builtins"
    rm -rf "build-builtins-$arch"
    cmake -G Ninja -S llvm-src/compiler-rt/lib/builtins -B "build-builtins-$arch" -Wno-dev \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$RES" \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_ASM_COMPILER="$CC" \
      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_AR="$AR" -DCMAKE_RANLIB="$RANLIB" \
      -DCMAKE_C_COMPILER_TARGET="$arch-w64-windows-gnu" -DCMAKE_ASM_COMPILER_TARGET="$arch-w64-windows-gnu" \
      -DCMAKE_C_COMPILER_WORKS=1 -DCMAKE_CXX_COMPILER_WORKS=1 -DCMAKE_ASM_COMPILER_WORKS=1 \
      -DCOMPILER_RT_DEFAULT_TARGET_ONLY=TRUE -DCOMPILER_RT_USE_BUILTINS_LIBRARY=TRUE \
      -DCOMPILER_RT_BUILD_BUILTINS=TRUE -DCOMPILER_RT_OS_DIR=windows -DLLVM_CONFIG_PATH="" \
      -DCMAKE_FIND_ROOT_PATH="$sysroot" -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
      -DSANITIZER_CXX_ABI=libc++ > /dev/null
    ninja -C "build-builtins-$arch" install > /dev/null
    # the clang driver looks for lib/windows/libclang_rt.builtins-<arch>.a
    f="$(find "$RES/lib" -name "libclang_rt.builtins*.a" -newer mingw.tar.bz2 | grep -i "$arch" | head -1)"
    [ -f "$RES/lib/windows/libclang_rt.builtins-$arch.a" ] || cp "$f" "$RES/lib/windows/libclang_rt.builtins-$arch.a"
  fi

  if [ ! -f "$sysroot/lib/libc++.a" ]; then
    echo "== $triple: libunwind, libc++abi, libc++"
    rm -rf "build-runtimes-$arch"
    cmake -G Ninja -S llvm-src/runtimes -B "build-runtimes-$arch" -Wno-dev \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$sysroot" \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_C_COMPILER_TARGET="$arch-w64-windows-gnu" -DCMAKE_CXX_COMPILER_TARGET="$arch-w64-windows-gnu" \
      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CROSSCOMPILING=TRUE \
      -DCMAKE_C_COMPILER_WORKS=TRUE -DCMAKE_CXX_COMPILER_WORKS=TRUE \
      -DCMAKE_AR="$AR" -DCMAKE_RANLIB="$RANLIB" \
      -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
      -DLIBUNWIND_USE_COMPILER_RT=TRUE -DLIBUNWIND_ENABLE_SHARED=FALSE -DLIBUNWIND_ENABLE_STATIC=TRUE \
      -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXX_ENABLE_SHARED=FALSE -DLIBCXX_ENABLE_STATIC=TRUE \
      -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=TRUE -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_LIBDIR_SUFFIX="" \
      -DLIBCXX_INCLUDE_TESTS=FALSE -DLIBCXX_INCLUDE_BENCHMARKS=FALSE -DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=FALSE \
      -DLIBCXXABI_USE_COMPILER_RT=ON -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_ENABLE_SHARED=OFF \
      -DLIBCXXABI_LIBDIR_SUFFIX="" > /dev/null
    ninja -C "build-runtimes-$arch" -j"$JOBS" install > /dev/null
  fi

  echo "== $triple: smoke test"
  printf '#include <regex>\n#include <string>\n#include <windows.h>\nint main(){std::wregex r(L"^a.c$", std::regex::ECMAScript);return std::regex_match(std::wstring(L"abc"), r) ? 0 : 1;}\n' > "smoke-$arch.cpp"
  "$CXX" -O2 -static "smoke-$arch.cpp" -o "smoke-$arch.exe"
done
echo "toolchain ready in $PREFIX"
