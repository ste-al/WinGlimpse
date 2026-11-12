#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds Glimpse with the .NET SDK on Linux or macOS (CI uses it too):
#   * Glimpse.exe for x64 (.NET Framework 4.7.2) and ARM64 (.NET Framework 4.8.1), C# 5 enforced
#   * self-test with Mono (geometry cases and unit tests), language-file check
#   * preview images with /render (Mono + Xvfb), when available
#   * the native C++ Glimpse.exe for x64 and ARM64 (build-native.sh), when the clang/mingw-w64
#     toolchain from tools/build-mingw-toolchain.sh is installed (required for dist/)
#   * dist/: one zip per build, SHA256SUMS.txt, BUILDINFO.txt
#
# Usage: ./build.sh [--no-dist] [--no-previews] [--no-native] [--check-dist]
#   --check-dist   verify source manifest, package contents, and distribution checksums
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

want_dist=1; want_previews=1; want_native=1; check_only=0
for a in "$@"; do
  case "$a" in
    --no-dist) want_dist=0 ;;
    --no-previews) want_previews=0 ;;
    --no-native) want_native=0 ;;
    --check-dist) check_only=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done

if [ "$check_only" = 1 ]; then
  exec python3 tools/package.py --check
fi

# The compiler that ships with Windows reads BOM-less files in the ANSI code page: keep them ASCII.
if LC_ALL=C grep -nP '[^\x00-\x7F]' Glimpse.cs Glimpse.ps1 build.cmd deploy/*.cmd; then
  echo "non-ASCII characters found (use \\uXXXX escapes)" >&2; exit 1
fi

out="$here/out"
rm -rf "$out"
mkdir -p "$out"

build_one() { # architecture folder
  echo "== build $1"
  dotnet build build/Glimpse.csproj -c Release -nologo -v q \
    -p:GlimpseArch="$1" -p:BaseIntermediateOutputPath="$out/obj-$2/" -o "$out/$2"
  rm -f "$out/$2/Glimpse.exe.config"
  # the program folder: Glimpse.exe with Glimpse.ini and lang\ beside it
  cp Glimpse.ini "$out/$2/"
  mkdir -p "$out/$2/lang"
  cp lang/*.txt "$out/$2/lang/"
}
build_one x64 x64
build_one arm64 arm64

if command -v mcs >/dev/null 2>&1; then
  # Glimpse.ps1 compiles Glimpse.cs with Add-Type and only the assemblies it lists: check that list
  echo "== PowerShell build references"
  refs=$(grep -o "ReferencedAssemblies [^-]*" Glimpse.ps1 | grep -o "'[^']*'" | tr -d "'" | sed 's/^/-r:/; s/$/.dll/' | tr '\n' ' ')
  # shellcheck disable=SC2086
  mcs -noconfig -nologo -target:library -langversion:5 -r:System.dll $refs -out:"$out/addtype-check.dll" Glimpse.cs > "$out/addtype-check.txt" 2>&1 \
    || { grep -v ' warning ' "$out/addtype-check.txt" >&2; echo "Glimpse.cs does not compile with the references Glimpse.ps1 gives Add-Type" >&2; exit 1; }
  rm -f "$out/addtype-check.dll"
fi

if command -v mono >/dev/null 2>&1; then
  echo "== self-test (Mono)"
  mono "$out/x64/Glimpse.exe" /selftest:tests/geometry-cases.txt | tee "$out/selftest.txt" | grep -v '^ok' || true
  grep -q '^PASS' "$out/selftest.txt" || { echo "self-test failed" >&2; exit 1; }
  echo "== language files"
  mono "$out/x64/Glimpse.exe" /checklang | tee "$out/checklang.txt"
  echo "== defaults"
  mono "$out/x64/Glimpse.exe" /dumpini:"$out/Glimpse.default.ini" >/dev/null
  cmp -s "$out/Glimpse.default.ini" Glimpse.ini || { echo "Glimpse.ini differs from the built-in defaults (Settings.DefaultIniText)" >&2; exit 1; }
  if command -v mcs >/dev/null 2>&1; then
    # crowded 1920x1080 desktops through the real resolver: the rules hold in every mode, and the outcomes
    # become test cases the native build must reproduce (below, under Wine)
    echo "== crowded desktops (tests/simulate.cs)"
    mkdir -p "$out/sim"
    mcs -nologo -langversion:5 -main:GlimpseSim.Program -r:System.Windows.Forms.dll -r:System.Drawing.dll -r:System.Core.dll \
      -r:Accessibility.dll -out:"$out/sim/simulate.exe" Glimpse.cs tests/simulate.cs > "$out/sim/build.txt" 2>&1 \
      || { grep -v ' warning ' "$out/sim/build.txt" >&2; echo "tests/simulate.cs does not compile" >&2; exit 1; }
    mono "$out/sim/simulate.exe" "$out/sim" 150 1 12 > /dev/null \
      || { grep '!!' "$out/sim/report.txt" | head -20 >&2; echo "crowded desktops: rules broken, see $out/sim/report.txt" >&2; exit 1; }
    grep -E "Arrange now: windows|RULES" "$out/sim/report.txt"
    mono "$out/x64/Glimpse.exe" /selftest:"$out/sim/sim-cases.txt" > "$out/sim/selftest.txt" || true
    grep -q '^PASS' "$out/sim/selftest.txt" || { grep '^not ok' "$out/sim/selftest.txt" | head >&2; echo "simulated cases fail in the C# build" >&2; exit 1; }
  fi
  if [ "$want_previews" = 1 ] && command -v xvfb-run >/dev/null 2>&1; then
    echo "== previews"
    for lang in en de qps; do
      xvfb-run -a -s "-screen 0 1920x1080x24" mono "$out/x64/Glimpse.exe" /render:"$out/previews/$lang" /renderlang:$lang \
        || echo "warning: previews for $lang failed" >&2
    done
  fi
else
  echo "warning: mono not found, self-test skipped" >&2
fi

# the native C++ build needs the cross toolchain; dist/ always contains it
native=0
if [ "$want_native" = 1 ]; then
  if [ -x "${GLIMPSE_MINGW:-/opt/glimpse-mingw}/bin/x86_64-w64-mingw32-clang++" ]; then
    ./build-native.sh --test
    native=1
    if [ -f "$out/sim/sim-cases.txt" ] && command -v wine >/dev/null 2>&1; then
      echo "== crowded desktops, native build (Wine): the same decisions as the C# build"
      WINEDEBUG=-all wine out/native-x64/Glimpse.exe /selftest:out\\sim\\sim-cases.txt | tr -d '\r' > "$out/sim/native-selftest.txt" || true
      grep -q '^PASS' "$out/sim/native-selftest.txt" \
        || { grep '^not ok' "$out/sim/native-selftest.txt" | head >&2; echo "the native build decides differently on simulated desktops" >&2; exit 1; }
      grep '^PASS' "$out/sim/native-selftest.txt"
    fi
  elif [ "$want_dist" = 1 ]; then
    echo "the native build needs the clang/mingw-w64 toolchain: run tools/build-mingw-toolchain.sh (or ./build.sh --no-dist)" >&2
    exit 1
  else
    echo "note: clang/mingw-w64 toolchain not found, native build skipped (CI builds it with MSVC on Windows)" >&2
  fi
fi

[ "$want_dist" = 1 ] || exit 0
[ "$native" = 1 ] || { echo "dist/ needs the native build (drop --no-native)" >&2; exit 1; }

echo "== dist"
python3 tools/package.py --out "$out"
ls -la dist
