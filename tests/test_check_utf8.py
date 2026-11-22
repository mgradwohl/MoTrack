#!/usr/bin/env python3
"""Tests for tools/check-utf8.py.

Every non-ASCII sample is built from code points, so this file stays clean under the checker it tests.
"""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "check-utf8.py"
spec = importlib.util.spec_from_file_location("check_utf8", SCRIPT)
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker  # dataclasses look their module up there
spec.loader.exec_module(checker)

EM_DASH = chr(0x2014)
E_ACUTE = chr(0xE9)
# U+2014's UTF-8 bytes E2 80 94 read as Windows-1252, and as Latin-1 (two C1 controls).
EM_DASH_AS_1252 = "".join(map(chr, (0xE2, 0x20AC, 0x201D)))
EM_DASH_AS_LATIN1 = chr(0xE2) + "\u0080\u0094"
# U+00E9 encoded twice, as the archived-movie-tracks fixture's member name is (#276).
E_ACUTE_TWICE = "".join(map(chr, (0xC3, 0xA9)))
CJK = "".join(map(chr, (0x5B9E, 0x9A8C)))
EMOJI = chr(0x1F600)
ARABIC = "".join(map(chr, (0x062A, 0x062C, 0x0631, 0x0628, 0x0629)))


class FindMojibakeTest(unittest.TestCase):
    def test_double_encoded_characters_are_found_with_what_they_meant(self) -> None:
        for garbled in (EM_DASH_AS_1252, EM_DASH_AS_LATIN1):
            with self.subTest(garbled=garbled.encode("unicode_escape")):
                self.assertEqual(checker.find_mojibake(f"x {garbled} y"), [(3, garbled, EM_DASH)])
        self.assertEqual(checker.find_mojibake(f"notes {E_ACUTE_TWICE}.txt"), [(7, E_ACUTE_TWICE, E_ACUTE)])

    def test_correct_non_ascii_text_is_not_flagged(self) -> None:
        i_diaeresis, degree, a_tilde, half, a_circumflex = map(chr, (0xEF, 0xB0, 0xC3, 0xBD, 0xE2))
        for text in (EM_DASH, f"caf{E_ACUTE}", f"na{i_diaeresis}ve", f"{degree}C", f"{a_tilde} alone", f"1{half} {a_circumflex}",
                     CJK, EMOJI, ARABIC):
            with self.subTest(text=text.encode("unicode_escape")):
                self.assertEqual(checker.find_mojibake(text), [])


class CheckBytesTest(unittest.TestCase):
    def test_clean_utf8_in_every_script_passes(self) -> None:
        text = f"// {CJK} {EMOJI} {E_ACUTE}l{chr(0xE8)}ve {ARABIC} {EM_DASH}\nint x = 1;\n"
        self.assertEqual(checker.check_bytes("sample.cpp", text.encode("utf-8")), [])

    def test_invalid_utf8_is_reported_with_line_and_column(self) -> None:
        problems = checker.check_bytes("sample.cpp", b"ok\nab\xe9cd\n")
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0].startswith("sample.cpp:2:3: not valid UTF-8 (byte(s) e9"), problems[0])

    def test_double_encoded_text_is_reported(self) -> None:
        problems = checker.check_bytes("notes.md", f"ok\nsee {EM_DASH_AS_1252} here\n".encode("utf-8"))
        self.assertEqual(len(problems), 1)
        self.assertIn("notes.md:2:5: double-encoded text", problems[0])
        self.assertIn("U+2014", problems[0])

    def test_every_byte_order_mark_is_refused(self) -> None:
        for data in ("x\n".encode("utf-8-sig"), "x\n".encode("utf-16"), "x\n".encode("utf-32")):
            with self.subTest(data=data[:4]):
                problems = checker.check_bytes("sample.txt", data)
                self.assertEqual(len(problems), 1)
                self.assertIn("byte-order mark", problems[0])

    def test_a_ps1_with_non_ascii_text_needs_powershell_7(self) -> None:
        body = f'Write-Host "a {EM_DASH} b"\n'.encode("utf-8")
        bare = checker.check_bytes("tool.ps1", body)
        self.assertEqual(len(bare), 1)
        self.assertIn("tool.ps1:1:", bare[0])
        for header in (b"#Requires -Version 7\n", b"#requires -version 7.4\r\n"):
            with self.subTest(header=header):
                self.assertEqual(checker.check_bytes("tool.ps1", header + body), [])
        self.assertEqual(checker.check_bytes("tool.ps1", b'Write-Host "ok"\n'), [])
        self.assertEqual(len(checker.check_bytes("tool.ps1", b"#Requires -Version 5.1\n" + body)), 1)
        self.assertEqual(checker.check_bytes("notes.md", body), [])


class SelectionTest(unittest.TestCase):
    def test_parses_check_attr_output(self) -> None:
        output = "a.png\0binary\0set\0a.png\0text\0unset\0b.cpp\0binary\0unspecified\0b.cpp\0text\0auto\0".encode("utf-8")
        self.assertEqual(
            checker.parse_check_attr(output),
            {"a.png": {"binary": "set", "text": "unset"}, "b.cpp": {"binary": "unspecified", "text": "auto"}},
        )

    def test_binary_and_exact_byte_files_are_skipped(self) -> None:
        self.assertFalse(checker.is_checked({"binary": "set", "text": "unset"}))
        self.assertFalse(checker.is_checked({"binary": "unspecified", "text": "unset"}))
        self.assertTrue(checker.is_checked({"binary": "unspecified", "text": "auto"}))
        self.assertTrue(checker.is_checked({}))

    def test_repository_attributes_select_sources_and_skip_media_and_fixtures(self) -> None:
        selected = set(checker.tracked_text_files())
        for name in ("src/main.cpp", "tools/check-utf8.py", "CONTRIBUTING.md", ".gitattributes", "tests/fixtures/README.md"):
            self.assertIn(name, selected)
        for name in ("tests/fixtures/point-mass.trk", "tests/fixtures/archive.trz", "tests/fixtures/vfr.mp4",
                     "tests/corpus/fixtures/linked-images/images/frame0.png"):
            self.assertNotIn(name, selected)


class CommandLineTest(unittest.TestCase):
    def run_checker(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-I", "-X", "utf8", str(SCRIPT), *arguments],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            timeout=120,
        )

    def test_files_given_on_the_command_line(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            clean = Path(folder) / f"{CJK} {EMOJI}.md"
            clean.write_bytes(f"{ARABIC}\n".encode("utf-8"))
            bad = Path(folder) / "bad.cpp"
            bad.write_bytes(b"\xff\n")
            self.assertEqual(self.run_checker(str(clean)).returncode, 0)
            result = self.run_checker(str(clean), str(bad))
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("bad.cpp:1:1: not valid UTF-8", result.stdout)

    def test_the_repository_is_clean(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
