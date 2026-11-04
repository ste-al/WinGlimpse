#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the native Glimpse.exe (C++17, Win32, GDI+, DWM) for x64 and ARM64 with clang and mingw-w64
# on Linux. Statically linked: libc++ is inside the EXE, the C runtime is the Universal CRT that is
# part of Windows 10, 11 and Server 2016 and later.
# Needs the cross toolchain from tools/build-mingw-toolchain.sh (default /opt/glimpse-mingw, or set
# GLIMPSE_MINGW). On Windows, build-native.cmd builds the same sources with Visual Studio (MSVC).
#
# Usage: ./build-native.sh [--test]    --test: run the x64 self-test under Wine when it is installed
# Output: out/native-x64/Glimpse.exe and out/native-arm64/Glimpse.exe, with Glimpse.ini and lang/
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

want_test=0
for a in "$@"; do
  case "$a" in
    --test) want_test=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

MINGW="${GLIMPSE_MINGW:-/opt/glimpse-mingw}"
# the native sources are ASCII, like Glimpse.cs (texts with other characters use \uXXXX escapes)
if LC_ALL=C grep -nP '[^\x00-\x7F]' src/*.cpp src/*.h src/Glimpse.rc; then
  echo "non-ASCII characters found in src/ (use \\uXXXX escapes)" >&2; exit 1
fi

for pair in x86_64:x64 aarch64:arm64; do
  triple="${pair%%:*}-w64-mingw32"
  folder="out/native-${pair##*:}"
  cxx="$MINGW/bin/$triple-clang++"
  if [ ! -x "$cxx" ]; then
    echo "missing $cxx: build the toolchain with tools/build-mingw-toolchain.sh" >&2
    exit 1
  fi
  echo "== native ${pair##*:} ($triple)"
  mkdir -p "$folder"
  "$MINGW/bin/$triple-windres" --preprocessor="$MINGW/bin/$triple-clang" --preprocessor-arg=-E --preprocessor-arg=-xc \
    --preprocessor-arg=-DRC_INVOKED --preprocessor-arg=-Wno-unused-command-line-argument -I . -I src -O coff -i src/Glimpse.rc -o "$folder/Glimpse.res.o"
  "$cxx" -std=c++17 -O2 -municode -mwindows -static -s \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-command-line-argument \
    -ffile-prefix-map="$here"=. -Wl,--no-insert-timestamp \
    src/*.cpp "$folder/Glimpse.res.o" -o "$folder/Glimpse.exe" \
    -luser32 -lgdi32 -lgdiplus -ldwmapi -lshcore -lwtsapi32 -lshell32 -ladvapi32 -lole32 -lcomctl32
  rm -f "$folder/Glimpse.res.o"
  # the program folder: Glimpse.exe with Glimpse.ini and lang\ beside it
  cp Glimpse.ini "$folder/"
  mkdir -p "$folder/lang"
  cp lang/*.txt "$folder/lang/"
  ls -l "$folder/Glimpse.exe"
done

if [ "$want_test" = 1 ]; then
  if command -v wine >/dev/null 2>&1; then
    echo "== native self-test (Wine, x64)"
    WINEDEBUG=-all wine out/native-x64/Glimpse.exe /selftest:tests\\geometry-cases.txt | tr -d '\r' > out/native-selftest.txt || true
    grep -v '^ok' out/native-selftest.txt || true
    grep -q '^PASS' out/native-selftest.txt || { echo "native self-test failed" >&2; exit 1; }
    WINEDEBUG=-all wine out/native-x64/Glimpse.exe /checklang | tr -d '\r'
  else
    echo "warning: wine not found, native self-test skipped (CI runs it on Windows)" >&2
  fi
fi
