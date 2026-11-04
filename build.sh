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
#   --check-dist   only verify that dist/ was built from the current sources
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

version=$(sed -n 's/^ *public const string Version = "\([0-9.]*\)";.*/\1/p' Glimpse.cs)
[ -n "$version" ] || { echo "cannot read the version from Glimpse.cs" >&2; exit 1; }

# Files every package is built from; their hashes go into dist/BUILDINFO.txt.
sources=(Glimpse.cs Glimpse.ps1 Glimpse.manifest Glimpse.ico Glimpse.ini build.cmd build/Glimpse.csproj LICENSE docs/README-dist.txt
         build-native.sh build-native.cmd tools/build-mingw-toolchain.sh)
while IFS= read -r f; do sources+=("$f"); done < <(ls lang/*.txt deploy/*.cmd src/* 2>/dev/null | LC_ALL=C sort)

buildinfo() {
  echo "Glimpse $version"
  echo "Built by build.sh from these sources (sha256):"
  for f in "${sources[@]}"; do
    [ -f "$f" ] && printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"
  done
}

if [ "$check_only" = 1 ]; then
  if diff <(buildinfo) dist/BUILDINFO.txt >/dev/null 2>&1; then
    echo "dist/ matches the sources"; exit 0
  fi
  echo "dist/ is stale: run ./build.sh and commit dist/" >&2
  diff <(buildinfo) dist/BUILDINFO.txt >&2 || true
  exit 1
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
rm -rf dist
mkdir -p dist
python3 - "$version" "$out" <<'PY'
import os, sys, zipfile, hashlib
version, out = sys.argv[1], sys.argv[2]
stamp = (2026, 1, 1, 0, 0, 0)          # fixed, so the same sources give the same zips

def add(z, src, arc):
    with open(src, 'rb') as f:
        data = f.read()
    info = zipfile.ZipInfo('Glimpse/' + arc, stamp)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)

langs = sorted(f for f in os.listdir('lang') if f.endswith('.txt'))
common = [('Glimpse.ini', 'Glimpse.ini'), ('LICENSE', 'LICENSE.txt'), ('docs/README-dist.txt', 'README.txt')] + \
         [('lang/' + f, 'lang/' + f) for f in langs]

packages = {
    'Glimpse-%s-win-x64.zip' % version: [(out + '/x64/Glimpse.exe', 'Glimpse.exe'), ('deploy/Start-Glimpse.cmd', 'deploy/Start-Glimpse.cmd')],
    'Glimpse-%s-win-arm64.zip' % version: [(out + '/arm64/Glimpse.exe', 'Glimpse.exe'), ('deploy/Start-Glimpse.cmd', 'deploy/Start-Glimpse.cmd')],
    'Glimpse-%s-native-win-x64.zip' % version: [(out + '/native-x64/Glimpse.exe', 'Glimpse.exe'), ('deploy/Start-Glimpse.cmd', 'deploy/Start-Glimpse.cmd')],
    'Glimpse-%s-native-win-arm64.zip' % version: [(out + '/native-arm64/Glimpse.exe', 'Glimpse.exe'), ('deploy/Start-Glimpse.cmd', 'deploy/Start-Glimpse.cmd')],
    'Glimpse-%s-ps1.zip' % version: [('Glimpse.ps1', 'Glimpse.ps1'), ('Glimpse.cs', 'Glimpse.cs'),
                                    ('build.cmd', 'build.cmd'), ('Glimpse.manifest', 'Glimpse.manifest'), ('Glimpse.ico', 'Glimpse.ico'),
                                    ('deploy/Start-Glimpse-ps1.cmd', 'deploy/Start-Glimpse-ps1.cmd'), ('deploy/Start-Glimpse.cmd', 'deploy/Start-Glimpse.cmd')],
}
sums = []
for name, files in packages.items():
    path = os.path.join('dist', name)
    with zipfile.ZipFile(path, 'w') as z:
        for src, arc in sorted(files + common, key=lambda t: t[1]):
            if os.path.exists(src):
                add(z, src, arc)
            else:
                print('warning: missing ' + src, file=sys.stderr)
    sums.append((name, path))
for label, path in [('win-x64/Glimpse.exe', out + '/x64/Glimpse.exe'), ('win-arm64/Glimpse.exe', out + '/arm64/Glimpse.exe'),
                    ('native-win-x64/Glimpse.exe', out + '/native-x64/Glimpse.exe'), ('native-win-arm64/Glimpse.exe', out + '/native-arm64/Glimpse.exe')]:
    sums.append((label, path))
with open('dist/SHA256SUMS.txt', 'w', newline='\n') as f:
    for label, path in sorted(sums):
        f.write('%s  %s\n' % (hashlib.sha256(open(path, 'rb').read()).hexdigest(), label))
PY
buildinfo > dist/BUILDINFO.txt
ls -la dist
