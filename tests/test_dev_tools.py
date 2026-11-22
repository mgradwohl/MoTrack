"""Tests for the developer setup tools: the toolchain pins, tools/dev_sdk.py's verified Qt fallback,
and the Bash setup and prerequisite scripts (no network; nothing is installed)."""

import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import dev_sdk

PINS = dev_sdk.toolchain()
# The Bash tests need a POSIX shell. On Windows they run only when MOTRACK_TEST_BASH names Git's bash.exe
# and MOTRACK_TEST_UTILITIES its usr\bin (a directory with grep and sed but no LLVM).
BASH = os.environ.get("MOTRACK_TEST_BASH") or (None if sys.platform == "win32" else shutil.which("bash"))

UPDATES = b"""<Updates>
 <PackageUpdate>
  <Name>qt.qt6.683.win64_msvc2022_64</Name>
  <Version>6.8.3-0-1</Version>
  <DownloadableArchives>qtbase.7z, d3d.7z, plain.7z</DownloadableArchives>
  <Operations>
   <Operation name="Extract"><Argument>@TargetDir@/6.8.3/msvc2022_64</Argument><Argument>qtbase.7z</Argument></Operation>
   <Operation name="Extract"><Argument>@TargetDir@/6.8.3/msvc2022_64/bin</Argument><Argument>d3d.7z</Argument></Operation>
  </Operations>
 </PackageUpdate>
 <PackageUpdate><Name>qt.qt6.683.addons.qtcharts.win64_msvc2022_64</Name><Version>x</Version></PackageUpdate>
</Updates>"""


class ToolchainPinTests(unittest.TestCase):
    def test_pins_are_consistent(self):
        self.assertEqual(PINS["MOTRACK_LLVM_VERSION"].split(".")[0], PINS["MOTRACK_LLVM_MAJOR"])
        self.assertRegex(PINS["MOTRACK_LLVM_WIN64_SHA256"], r"^[0-9a-f]{64}$")
        self.assertRegex(PINS["MOTRACK_LLVM_APT_KEY_FINGERPRINT"], r"^[0-9A-F]{40}$")
        self.assertTrue(PINS["MOTRACK_QT_VERSION"].startswith(PINS["MOTRACK_QT_MIN"] + "."))
        self.assertTrue(PINS["MOTRACK_PYTHON_WINDOWS_VERSION"].startswith(PINS["MOTRACK_PYTHON_VERSION"] + "."))
        for key in ("CMAKE", "NINJA", "CCACHE"):
            pinned, floor = PINS[f"MOTRACK_{key}_VERSION"], PINS[f"MOTRACK_{key}_MIN"]
            self.assertGreaterEqual([int(p) for p in pinned.split(".")], [int(p) for p in floor.split(".")], key)
        self.assertEqual(dev_sdk.QT_VERSION, PINS["MOTRACK_QT_VERSION"])

    def test_every_line_is_a_comment_blank_or_pin(self):
        for line in (ROOT / "tools/toolchain.env").read_text(encoding="utf-8").splitlines():
            self.assertRegex(line, r"^(#.*|[A-Z0-9_]+=\S+)?$")

    def test_ci_reads_the_pins_instead_of_repeating_them(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        for key in ("MOTRACK_LLVM_VERSION", "MOTRACK_LLVM_WIN64_SHA256", "MOTRACK_QT_VERSION"):
            self.assertTrue(PINS[key] not in workflow, f"ci.yml repeats {key}; read it from tools/toolchain.env")
        self.assertNotRegex(workflow, r"\b(clang|llvm|clang-format|clang-tidy)-1\d\b")
        self.assertIn("tools/toolchain.env", workflow)


class QtArchiveListTests(unittest.TestCase):
    def test_extract_operations_place_archives_and_default_to_the_prefix(self):
        archives = dev_sdk.essential_archives(UPDATES, "qt.qt6.683.win64_msvc2022_64", "6.8.3", "msvc2022_64")
        self.assertEqual(archives, [("6.8.3-0-1qtbase.7z", "6.8.3/msvc2022_64"),
                                    ("6.8.3-0-1d3d.7z", "6.8.3/msvc2022_64/bin"),
                                    ("6.8.3-0-1plain.7z", "6.8.3/msvc2022_64")])

    def test_targets_outside_the_prefix_and_missing_packages_are_rejected(self):
        for target in ("@TargetDir@/6.8.3/other", "@TargetDir@/6.8.3/msvc2022_64/../../x", "@TargetDir@"):
            updates = UPDATES.replace(b"@TargetDir@/6.8.3/msvc2022_64/bin", target.encode())
            with self.subTest(target=target), self.assertRaises(SystemExit):
                dev_sdk.essential_archives(updates, "qt.qt6.683.win64_msvc2022_64", "6.8.3", "msvc2022_64")
        with self.assertRaises(SystemExit):
            dev_sdk.essential_archives(UPDATES, "qt.qt6.683.linux_gcc_64", "6.8.3", "gcc_64")

    def test_metadata_is_read_only_from_download_qt_io(self):
        with self.assertRaises(SystemExit):
            dev_sdk.fetch_trusted("https://mirror.example.org/online/qtsdkrepository/Updates.xml")


class QtDirectInstallTests(unittest.TestCase):
    """install_qt_direct against a fake repository: archive contents are file lists keyed by name."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.archives = {
            "6.8.3-0-1qtbase.7z": {"lib/cmake/Qt6/Qt6Config.cmake": "", "mkspecs/qconfig.pri": "QT_EDITION = Enterprise\nQT_LICHECK = licheck.exe\nQT_ARCH = x86_64\n"},
            "6.8.3-0-1d3d.7z": {"d3dcompiler_47.dll": "dll"},
            "6.8.3-0-1plain.7z": {"bin/Qt6Svg.dll": "svg"},
        }
        self.checksums = {name: hashlib.sha256(name.encode()).hexdigest() for name in self.archives}
        self.downloads = []

    def tearDown(self):
        self.directory.cleanup()

    def fetch(self, url):
        base = dev_sdk.repository_url("windows_x86", "6.8.3")
        if url == base + "Updates.xml":
            return UPDATES
        if url == base + "Updates.xml.sha256":
            return f"{hashlib.sha256(UPDATES).hexdigest()}  Updates.xml\n".encode()
        raise AssertionError(f"only Updates.xml and its checksum come from the server; asked for {url}")

    def download(self, url, target):
        self.downloads.append(url)
        target.write_bytes(url.rsplit("/", 1)[1].encode())

    def extract(self, archive, destination):
        for relative, content in self.archives[archive.name].items():
            (destination / relative).parent.mkdir(parents=True, exist_ok=True)
            (destination / relative).write_text(content, encoding="utf-8")

    def install(self):
        pins = {f"windows_x86/qt.qt6.683.win64_msvc2022_64/{name}": checksum for name, checksum in self.checksums.items()}
        with patch.object(dev_sdk, "fetch_trusted", self.fetch), patch.object(dev_sdk, "download", self.download):
            dev_sdk.install_qt_direct(self.root, "win64_msvc2022_64", "msvc2022_64", "windows_x86", "6.8.3", self.extract,
                                      pins)

    def test_installs_verified_archives_into_the_prefix(self):
        stale = self.root / "qt/6.8.3/msvc2022_64/stale.txt"
        stale.parent.mkdir(parents=True)
        stale.write_text("from a failed aqt run", encoding="utf-8")
        self.install()
        prefix = self.root / "qt/6.8.3/msvc2022_64"
        self.assertEqual((prefix / "bin/qt.conf").read_text(encoding="utf-8"), "[Paths]\nPrefix=..\n")
        self.assertTrue((prefix / "bin/d3dcompiler_47.dll").is_file())
        self.assertTrue((prefix / "bin/Qt6Svg.dll").is_file())
        self.assertFalse(stale.exists())
        self.assertEqual((prefix / "mkspecs/qconfig.pri").read_text(encoding="utf-8"),
                         "QT_EDITION = OpenSource\nQT_LICHECK =\nQT_ARCH = x86_64\n")
        self.assertEqual(len(self.downloads), 3)
        self.assertTrue(all("/qt.qt6.683.win64_msvc2022_64/6.8.3-0-1" in url for url in self.downloads))
        self.assertFalse((self.root / "qt-downloads").exists())
        self.assertFalse((self.root / "qt.partial").exists())

    def test_a_cached_archive_that_still_matches_is_not_downloaded_again(self):
        downloads = self.root / "qt-downloads"
        downloads.mkdir()
        (downloads / "6.8.3-0-1qtbase.7z").write_bytes(b"6.8.3-0-1qtbase.7z")
        self.install()
        self.assertEqual(len(self.downloads), 2)

    def test_a_checksum_mismatch_stops_before_anything_is_installed(self):
        self.checksums["6.8.3-0-1d3d.7z"] = "0" * 64
        with self.assertRaisesRegex(SystemExit, "d3d.7z has SHA-256 .*qt-archives.sha256 pins 0{64}"):
            self.install()
        self.assertFalse((self.root / "qt").exists())
        self.assertFalse((self.root / "qt-downloads/6.8.3-0-1d3d.7z").exists())

    def test_archives_the_pins_do_not_list_are_refused_before_any_download(self):
        for change in ("extra", "missing"):
            with self.subTest(change=change):
                checksums = dict(self.checksums)
                if change == "extra":
                    checksums["6.8.3-0-1qtmultimedia.7z"] = "1" * 64
                else:
                    del checksums["6.8.3-0-1plain.7z"]
                with patch.object(self, "checksums", checksums), self.assertRaisesRegex(SystemExit, "differ from tools/qt-archives.sha256"):
                    self.install()
                self.assertEqual(self.downloads, [])

    def test_the_pinned_checksums_cover_every_platform_at_the_pinned_qt(self):
        pins = dev_sdk.pinned_archives()
        compact = dev_sdk.QT_VERSION.replace(".", "")
        for archive, _directory, repository_os in dev_sdk.QT_PACKAGES:
            names = [key for key in pins if key.startswith(f"{repository_os}/qt.qt6.{compact}.{archive}/")]
            self.assertTrue(names, repository_os)
            self.assertTrue(all(key.rsplit("/", 1)[1].startswith(dev_sdk.QT_VERSION + "-") for key in names), names)
            self.assertTrue(any("qtbase" in key for key in names), repository_os)
        self.assertEqual(len(pins), sum(1 for line in dev_sdk.QT_ARCHIVE_PINS.read_text(encoding="utf-8").splitlines()
                                        if line and not line.startswith("#")))

    def test_updates_xml_must_match_its_published_checksum(self):
        def fetch(url):
            return b"0" * 64 if url.endswith("Updates.xml.sha256") else self.fetch(url)
        with patch.object(dev_sdk, "fetch_trusted", fetch), self.assertRaisesRegex(SystemExit, "Updates.xml"):
            dev_sdk.install_qt_direct(self.root, "win64_msvc2022_64", "msvc2022_64", "windows_x86", "6.8.3", self.extract)

    def test_archives_without_qt6config_are_not_installed(self):
        del self.archives["6.8.3-0-1qtbase.7z"]["lib/cmake/Qt6/Qt6Config.cmake"]
        with self.assertRaisesRegex(SystemExit, "Qt6Config.cmake"):
            self.install()
        self.assertFalse((self.root / "qt").exists())

    def test_bsdtar_extracts_into_the_target(self):
        try:
            tar = dev_sdk.find_bsdtar()
        except SystemExit:
            self.skipTest("no bsdtar")
        archive = self.root / "sample.7z"
        with zipfile.ZipFile(archive, "w") as content:  # bsdtar detects the format from the content
            content.writestr("bin/tool.txt", "ok")
        subprocess.run([tar, "-xf", archive, "-C", self.root], check=True)
        self.assertEqual((self.root / "bin/tool.txt").read_text(encoding="utf-8"), "ok")


class AqtFallbackTests(unittest.TestCase):
    def test_a_failed_aqt_falls_back_to_the_direct_download_unless_aqt_was_requested(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            python = root / "venv" / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
            python.parent.mkdir(parents=True)
            python.write_text("", encoding="utf-8")
            failure = subprocess.CalledProcessError(1, ["pip"])
            with patch.object(dev_sdk, "install_qt_aqt", side_effect=failure), \
                    patch.object(dev_sdk, "install_qt_direct") as direct:
                with self.assertRaises(subprocess.CalledProcessError):
                    dev_sdk.install(root, "windows", "win64_msvc2022_64", "msvc2022_64", "windows_x86", "aqt")
                direct.assert_not_called()
                with self.assertRaises(SystemExit):  # the mock installs nothing
                    dev_sdk.install(root, "windows", "win64_msvc2022_64", "msvc2022_64", "windows_x86", "auto")
                direct.assert_called_once_with(root, "win64_msvc2022_64", "msvc2022_64", "windows_x86")


@unittest.skipUnless(BASH, "needs Bash")
class BashScriptTests(unittest.TestCase):
    """The Bash scripts' row functions, sourced with stub tools first on PATH."""

    # bash itself too: the stubs start with #!/usr/bin/env bash, which looks bash up on PATH.
    UTILITIES = ("awk", "bash", "cat", "cut", "dirname", "grep", "head", "sed", "sort", "tr")

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.stubs = Path(self.directory.name) / "stubs"
        self.utilities = Path(self.directory.name) / "utilities"
        self.stubs.mkdir()
        # Only the stubs and the text utilities the scripts use, so a real LLVM on PATH stays out.
        if sys.platform == "win32":
            self.utilities = Path(os.environ["MOTRACK_TEST_UTILITIES"])
        else:
            self.utilities.mkdir()
            for name in self.UTILITIES:
                (self.utilities / name).symlink_to(shutil.which(name))

    def tearDown(self):
        self.directory.cleanup()

    def stub(self, name, script):
        path = self.stubs / name
        path.write_text("#!/usr/bin/env bash\n" + textwrap.dedent(script), encoding="utf-8", newline="\n")
        path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return path

    def rows(self, commands, env=None):
        script = f'source tools/check-prereqs.sh\n{commands}\nprintf "%s\\n" "${{rows[@]}}"\n'
        environment = {key: value for key, value in os.environ.items() if key != "MOTRACK_GCC_INSTALL_DIR"}
        environment.update(env or {}, PATH=os.pathsep.join((str(self.stubs), str(self.utilities))))
        result = subprocess.run([BASH, "-c", script], cwd=ROOT, env=environment, capture_output=True, text=True, check=True)
        return [line.split("\x1f") for line in result.stdout.splitlines()]

    def test_llvm_tools_must_be_the_pinned_major(self):
        major = PINS["MOTRACK_LLVM_MAJOR"]
        self.stub("clang-format", f'echo "Ubuntu clang-format version {major}.1.6 (++20261101)"')
        self.stub("clang-tidy", 'printf "LLVM (http://llvm.org/):\\n  LLVM version 18.1.8\\n"')
        self.stub("llvm-cov", 'echo "no version here"')
        rows = self.rows("for t in clang-format clang-tidy llvm-cov llvm-profdata; do check_llvm_tool $t; done")
        self.assertEqual([r[:3] for r in rows], [["ok", "clang-format", f"{major}.1.6"], ["fail", "clang-tidy", "18.1.8"],
                                               ["fail", "llvm-cov", "unknown"], ["fail", "llvm-profdata", ""]])
        self.assertIn(f"builds with LLVM {major} only", rows[1][5])

    def test_libstdcxx_must_be_gcc_14_or_newer_and_never_libcxx(self):
        gcc = PINS["MOTRACK_GCC_MAJOR"]
        selected = f"/usr/lib/gcc/x86_64-linux-gnu/{gcc}"
        for release, env, status in ((gcc, {"MOTRACK_GCC_INSTALL_DIR": selected}, "ok"), (gcc, None, "warn"),
                                     ("13", {"MOTRACK_GCC_INSTALL_DIR": selected}, "fail"), ("libc++", None, "fail"),
                                     ("", None, "fail")):
            self.stub("clang++", f"""\
                case " $* " in
                    *" -v "*) echo "Selected GCC installation: {selected}" >&2 ;;
                    *" -P "*) cat >/dev/null; echo "namespace std {{ typedef unsigned long size_t; }}"; echo "MOTRACK_STDLIB={release}" ;;
                esac
                """)
            with self.subTest(release=release, env=env):
                row = self.rows("check_libstdcxx", env)[0]
                self.assertEqual(row[:3], [status, "libstdc++", release])

    def test_macos_compilers_and_coverage_tools_must_be_apple_clang(self):
        minimum = PINS["MOTRACK_APPLE_CLANG_MIN"]
        self.stub("clang++", 'echo "Apple clang version 17.0.0 (clang-1700.0.13.5)"')
        self.stub("clang", 'echo "Homebrew clang version 23.1.6"')
        self.stub("llvm-cov", 'printf "Apple LLVM version 16.0.0\\n"')
        self.stub("xcrun", 'echo /Applications/Xcode.app/usr/bin/clang++')
        rows = self.rows(f"check_apple_tool clang++ {minimum}; check_apple_tool clang {minimum}; check_apple_tool llvm-cov {minimum}")
        self.assertEqual([r[:3] for r in rows], [["ok", "clang++", "17.0.0"], ["fail", "clang", "23.1.6"], ["fail", "llvm-cov", "16.0.0"]])
        self.assertIn("system libc++", rows[1][5])

    def test_existing_apt_llvm_sources_are_found_in_both_formats(self):
        apt = Path(self.directory.name) / "apt"
        (apt / "sources.list.d").mkdir(parents=True)
        own = apt / "sources.list.d/motrack-llvm-23.list"
        cases = {
            "deb822.sources": ("Types: deb\nURIs: http://apt.llvm.org/noble/\nSuites: llvm-toolchain-noble-23\n"
                               "Components: main\nSigned-By: /etc/apt/keyrings/llvm.asc\n", True),
            "two-stanzas.sources": ("Types: deb\nURIs: http://archive.ubuntu.com/ubuntu\nSuites: noble noble-updates\n\n"
                                    "Types: deb\nURIs: https://apt.llvm.org/noble/\nSuites: llvm-toolchain-noble llvm-toolchain-noble-23\n", True),
            "other-major.sources": ("Types: deb\nURIs: http://apt.llvm.org/noble/\nSuites: llvm-toolchain-noble-22\n", False),
            "disabled.sources": ("Enabled: no\nTypes: deb\nURIs: http://apt.llvm.org/noble/\nSuites: llvm-toolchain-noble-23\n", False),
            "llvm.list": ("deb [arch=amd64] http://apt.llvm.org/noble/ llvm-toolchain-noble-23 main\n", True),
            "commented.list": ("# deb http://apt.llvm.org/noble/ llvm-toolchain-noble-23 main\n", False),
            "suffix.list": ("deb http://apt.llvm.org/noble/ llvm-toolchain-noble-230 main\n", False),
        }
        for name, (content, _) in cases.items():
            (apt / "sources.list.d" / name).write_text(content, encoding="utf-8", newline="\n")
        own.write_text("deb [signed-by=/usr/share/keyrings/motrack-apt-llvm-org.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-23 main\n",
                       encoding="utf-8", newline="\n")
        apt_posix = subprocess.run([BASH, "-c", 'cd "$1" && pwd', "bash", str(apt)], capture_output=True, text=True,
                                   check=True, env={**os.environ, "PATH": os.pathsep.join((str(self.stubs), str(self.utilities)))}).stdout.strip()
        script = 'source tools/add-llvm-apt-source.sh; existing_llvm_sources "$1" noble 23 "$1/sources.list.d/motrack-llvm-23.list"'
        result = subprocess.run([BASH, "-c", script, "bash", apt_posix], cwd=ROOT, capture_output=True, text=True, check=True,
                                env={**os.environ, "PATH": os.pathsep.join((str(self.stubs), str(self.utilities)))})
        found = sorted(Path(line).name for line in result.stdout.splitlines())
        self.assertEqual(found, sorted(name for name, (_, expected) in cases.items() if expected))

    def test_minimum_versions(self):
        rows = self.rows("check_minimum a /a 3.25.0 3.25 fix; check_minimum b /b 3.24.9 3.25 fix; "
                         "check_minimum c /c '' 3.25 fix; check_minimum d /d 4.8 4.9 fix optional; check_minimum e /e 1.13.2 1.11 fix")
        self.assertEqual([r[0] for r in rows], ["ok", "fail", "fail", "warn", "ok"])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Ubuntu only")
    def test_setup_dry_run_installs_the_pinned_packages(self):
        os_release = Path("/etc/os-release").read_text(encoding="utf-8")
        if 'VERSION_ID="24.04"' not in os_release or "ID=ubuntu" not in os_release:
            self.skipTest("setup-dev.sh targets Ubuntu 24.04")
        output = subprocess.run([BASH, "tools/setup-dev.sh", "--dry-run", "--build"], cwd=ROOT, capture_output=True,
                                text=True, check=True).stdout
        llvm, gcc = PINS["MOTRACK_LLVM_MAJOR"], PINS["MOTRACK_GCC_MAJOR"]
        for package in (f"clang-{llvm}", f"clang-format-{llvm}", f"clang-tidy-{llvm}", f"llvm-{llvm}",
                        f"libclang-rt-{llvm}-dev", f"g++-{gcc}", "libarchive-tools"):
            self.assertRegex(output, rf"\[dry-run\].* apt-get .*install -y .*\b{re.escape(package)}\b")
        self.assertNotRegex(output, r"libc\+\+")
        self.assertRegex(output, rf"llvm-toolchain-\w+-{llvm} main")
        self.assertIn(PINS["MOTRACK_LLVM_APT_KEY_FINGERPRINT"], output)
        self.assertIn("[dry-run] cmake --workflow --preset dev", output)

    @unittest.skipUnless(sys.platform == "darwin", "macOS only")
    def test_macos_setup_dry_run_installs_homebrew_llvm(self):
        output = subprocess.run([BASH, "tools/setup-dev.sh", "--dry-run"], cwd=ROOT, capture_output=True,
                                text=True, check=True).stdout
        self.assertRegex(output, rf"\[dry-run\] brew install .*llvm@{PINS['MOTRACK_LLVM_MAJOR']}\b")


if __name__ == "__main__":
    unittest.main()
