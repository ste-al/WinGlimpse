#!/usr/bin/env python3
"""Package existing Windows builds, or verify a committed distribution (stdlib only)."""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
from pathlib import Path
import re
import sys
import tempfile
import zipfile


STAMP = (2026, 1, 1, 0, 0, 0)
ROOT = Path(__file__).resolve().parents[1]
BASE_SOURCES = (
    "Glimpse.cs", "Glimpse.ps1", "Glimpse.manifest", "Glimpse.ico", "Glimpse.ini",
    "build.cmd", "build/Glimpse.csproj", "LICENSE", "docs/README-dist.txt",
    "build-native.sh", "build-native.cmd", "tools/build-mingw-toolchain.sh",
    "build.sh", "tools/package.py",
)
BINARIES = {
    "win-x64": "x64", "win-arm64": "arm64",
    "native-win-x64": "native-x64", "native-win-arm64": "native-arm64",
}


def digest(stream):
    result = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        result.update(block)
    return result.hexdigest()


def file_hash(path):
    with path.open("rb") as stream:
        return digest(stream)


def source_paths(root):
    extra = sorted(p.relative_to(root).as_posix()
                   for pattern in ("lang/*.txt", "deploy/*.cmd", "src/*")
                   for p in root.glob(pattern) if p.is_file())
    return [root / name for name in BASE_SOURCES] + [root / name for name in extra]


def require_files(paths):
    missing = sorted({str(p) for p in paths if not p.is_file()})
    if missing:
        raise ValueError("missing required inputs:\n  " + "\n  ".join(missing))


def read_version(root):
    source = (root / "Glimpse.cs").read_text(encoding="utf-8-sig")
    versions = re.findall(r'^\s*public const string Version = "([0-9.]+)";', source, re.M)
    if len(versions) != 1:
        raise ValueError("cannot read the version from Glimpse.cs")
    return versions[0]


def build_info(root, version):
    paths = source_paths(root)
    require_files(paths)
    lines = ["Glimpse " + version, "Packaged from these sources (sha256):"]
    lines.extend("%s  %s" % (file_hash(p), p.relative_to(root).as_posix()) for p in paths)
    return "\n".join(lines) + "\n"


def package_files(root, out, version):
    common = [(root / "Glimpse.ini", "Glimpse.ini"), (root / "LICENSE", "LICENSE.txt"),
              (root / "docs/README-dist.txt", "README.txt")]
    require_files([root / "lang/en.txt"])
    common.extend((p, "lang/" + p.name) for p in sorted((root / "lang").glob("*.txt")))
    packages = {
        "Glimpse-%s-%s.zip" % (version, label): [
            (out / arch / "Glimpse.exe", "Glimpse.exe"),
            (root / "deploy/Start-Glimpse.cmd", "deploy/Start-Glimpse.cmd")]
        for label, arch in BINARIES.items()
    }
    scripts = ("Glimpse.ps1", "Glimpse.cs", "build.cmd", "Glimpse.manifest", "Glimpse.ico",
               "deploy/Start-Glimpse-ps1.cmd", "deploy/Start-Glimpse.cmd")
    packages["Glimpse-%s-ps1.zip" % version] = [(root / p, p) for p in scripts]
    return {name: sorted(files + common, key=lambda item: item[1])
            for name, files in packages.items()}


def package(root, out, dist):
    version = read_version(root)
    packages = package_files(root, out, version)
    # Reject incomplete builds before creating or changing any distribution files.
    require_files([p for files in packages.values() for p, _ in files])
    manifest = build_info(root, version)
    dist.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".glimpse-dist-", dir=dist.parent) as temporary:
        stage = Path(temporary)
        sums = {}
        for name, files in packages.items():
            with zipfile.ZipFile(stage / name, "w") as archive:
                for source, target in files:
                    info = zipfile.ZipInfo("Glimpse/" + target, STAMP)
                    info.create_system = 3  # Unix metadata, consistently on every build host.
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info.external_attr = 0o644 << 16
                    archive.writestr(info, source.read_bytes())
            sums[name] = file_hash(stage / name)
        for label, arch in BINARIES.items():
            sums[label + "/Glimpse.exe"] = file_hash(out / arch / "Glimpse.exe")
        (stage / "BUILDINFO.txt").write_text(manifest, encoding="utf-8", newline="\n")
        (stage / "SHA256SUMS.txt").write_text(
            "".join("%s  %s\n" % (value, label) for label, value in sorted(sums.items())),
            encoding="utf-8", newline="\n")
        # Assemble everything successfully before replacing the previous distribution.
        check(root, stage)
        dist.mkdir(parents=True, exist_ok=True)
        for generated in stage.iterdir():
            generated.replace(dist / generated.name)
        for old in dist.glob("Glimpse-*.zip"):
            if old.name not in packages:
                old.unlink()
    print("packaged %s into %s" % (version, dist))


def read_sums(path):
    sums = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match or match[2] in sums:
            raise ValueError("invalid or duplicate checksum in " + str(path))
        sums[match[2]] = match[1]
    return sums


def check(root, dist):
    version = read_version(root)
    expected_manifest = build_info(root, version)
    if (dist / "BUILDINFO.txt").read_text(encoding="utf-8") != expected_manifest:
        raise ValueError("dist/ is stale: source manifest differs; rebuild and commit dist/")
    # This validates committed packages without requiring an out/ build directory.
    packages = package_files(root, root / "out", version)
    names = set(packages)
    actual_names = {p.name for p in dist.glob("*.zip")}
    if actual_names != names:
        raise ValueError("archive set differs: missing %s; unexpected %s" %
                         (sorted(names - actual_names), sorted(actual_names - names)))
    sums = read_sums(dist / "SHA256SUMS.txt")
    labels = names | {label + "/Glimpse.exe" for label in BINARIES}
    if set(sums) != labels:
        raise ValueError("SHA256SUMS.txt must list exactly the expected archives and executables")
    for name, files in packages.items():
        if file_hash(dist / name) != sums[name]:
            raise ValueError("archive SHA256 mismatch: " + name)
        with zipfile.ZipFile(dist / name) as archive:
            targets = ["Glimpse/" + target for _, target in files]
            if sorted(archive.namelist()) != sorted(targets):
                raise ValueError("archive contents differ: " + name)
            for source, target in files:
                if target == "Glimpse.exe":
                    label = name[len("Glimpse-" + version + "-"):-4] + "/Glimpse.exe"
                    expected = sums[label]
                else:
                    expected = file_hash(source)
                with archive.open("Glimpse/" + target) as stream:
                    actual = digest(stream)
                if actual != expected:
                    raise ValueError("packaged SHA256 mismatch: " + name + ":" + target)
    print("dist/ matches sources, expected archives, and all packaged SHA256 hashes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify dist without rebuilding")
    parser.add_argument("--out", type=Path, default=Path("out"), help="build folder, relative to repository")
    parser.add_argument("--dist", type=Path, default=Path("dist"), help="distribution folder, relative to repository")
    args = parser.parse_args()
    try:
        if args.check:
            check(ROOT, ROOT / args.dist)
        else:
            package(ROOT, ROOT / args.out, ROOT / args.dist)
    except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as error:
        print("packaging error: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
