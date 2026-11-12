"""Distribution integrity regressions; run with python -m unittest discover -s tests -p test_package.py."""
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile


SCRIPT = Path(__file__).resolve().parents[1] / "tools/package.py"
SPEC = importlib.util.spec_from_file_location("glimpse_package", SCRIPT)
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.out = self.root / "out"
        self.dist = self.root / "dist"
        for name in package.BASE_SOURCES + (
                "lang/en.txt", "lang/de.txt", "deploy/Start-Glimpse.cmd",
                "deploy/Start-Glimpse-ps1.cmd", "src/main.cpp"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(("fixture " + name + "\n").encode())
        (self.root / "Glimpse.cs").write_text('public const string Version = "1.0.0";\n', encoding="utf-8")
        (self.root / "tools/package.py").write_bytes(SCRIPT.read_bytes())
        for label, arch in package.BINARIES.items():
            target = self.out / arch / "Glimpse.exe"
            target.parent.mkdir(parents=True)
            target.write_bytes(b"MZ\x00" + label.encode())

    def build(self):
        with contextlib.redirect_stdout(io.StringIO()):
            package.package(self.root, self.out, self.dist)

    def verify(self):
        with contextlib.redirect_stdout(io.StringIO()):
            package.check(self.root, self.dist)

    def snapshot(self):
        return {p.name: p.read_bytes() for p in self.dist.iterdir()}

    def test_roundtrip_determinism_and_cli_check_without_out(self):
        self.build()
        self.verify()
        before = self.snapshot()
        self.build()
        self.assertEqual(before, self.snapshot())
        (self.out / "x64/Glimpse.exe").unlink()
        result = subprocess.run([sys.executable, str(self.root / "tools/package.py"), "--check"],
                                cwd=self.root, capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("all packaged SHA256 hashes", result.stdout)

    def test_missing_input_preserves_existing_distribution(self):
        self.build()
        before = self.snapshot()
        for missing in (self.out / "arm64/Glimpse.exe", self.root / "deploy/Start-Glimpse.cmd"):
            with self.subTest(missing=missing):
                contents = missing.read_bytes()
                missing.unlink()
                try:
                    with self.assertRaisesRegex(ValueError, "missing required inputs"):
                        self.build()
                    self.assertEqual(before, self.snapshot())
                finally:
                    missing.write_bytes(contents)

    def test_source_change_rejected(self):
        self.build()
        with (self.root / "src/main.cpp").open("ab") as stream:
            stream.write(b"changed\n")
        with self.assertRaisesRegex(ValueError, "source manifest differs"):
            self.verify()

    def test_missing_archive_rejected(self):
        self.build()
        (self.dist / "Glimpse-1.0.0-win-arm64.zip").unlink()
        with self.assertRaisesRegex(ValueError, "archive set differs"):
            self.verify()

    def test_tampered_archive_rejected(self):
        self.build()
        with (self.dist / "Glimpse-1.0.0-win-x64.zip").open("ab") as stream:
            stream.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "archive SHA256 mismatch"):
            self.verify()

    def test_modified_executable_rejected_even_with_updated_zip_checksum(self):
        self.build()
        target = self.dist / "Glimpse-1.0.0-native-win-x64.zip"
        with zipfile.ZipFile(target) as archive:
            files = [(info, archive.read(info)) for info in archive.infolist()]
        with zipfile.ZipFile(target, "w") as archive:
            for info, contents in files:
                archive.writestr(info, b"modified EXE" if info.filename == "Glimpse/Glimpse.exe" else contents)
        sums = package.read_sums(self.dist / "SHA256SUMS.txt")
        sums[target.name] = package.file_hash(target)
        (self.dist / "SHA256SUMS.txt").write_text(
            "".join("%s  %s\n" % (value, label) for label, value in sorted(sums.items())), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "packaged SHA256 mismatch:.*Glimpse.exe"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
