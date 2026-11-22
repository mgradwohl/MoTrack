#!/usr/bin/env python3
"""Tests for tools/check-text-apis.py."""

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "check-text-apis.py"
spec = importlib.util.spec_from_file_location("check_text_apis", SCRIPT)
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker  # dataclasses look their module up there
spec.loader.exec_module(checker)

MODELS = {"src/measurements.cpp", "src/measurements.hpp"}


def rules(findings) -> list[str]:
    return [finding.rule for finding in findings]


class CxxTest(unittest.TestCase):
    def check(self, text: str, path: str = "src/sample.cpp") -> list[str]:
        return rules(checker.check_cxx(str(REPO / path), text, MODELS))

    def test_only_the_locale_setup_changes_the_locale(self) -> None:
        self.assertEqual(self.check('std::setlocale(LC_ALL, "");\n'), ["locale-setup"])
        self.assertEqual(self.check("std::locale::global(std::locale(name));\n"), ["locale-setup"])
        self.assertEqual(self.check("_configthreadlocale(_ENABLE_PER_THREAD_LOCALE);\n"), ["locale-setup"])
        self.assertEqual(self.check("const char* name = std::setlocale(LC_NUMERIC, nullptr);\n"), [])
        self.assertEqual(self.check('std::setlocale(LC_ALL, ""); // text-api: allowed (the setup)\n'), [])
        self.assertEqual(self.check('// text-api: allowed (the setup)\nstd::setlocale(LC_ALL, "");\n'), [])
        self.assertEqual(self.check('// std::setlocale(LC_ALL, "") is what Qt does\n'), [])

    def test_an_application_object_needs_the_locale_setup(self) -> None:
        self.assertEqual(self.check("const QApplication app(argc, argv);\n"), ["locale-setup"])
        self.assertEqual(self.check("QCoreApplication application{argc, argv};\n"), ["locale-setup"])
        self.assertEqual(self.check("QGuiApplication app(argc, argv);\nmotrack::configureProcessLocale();\n"), [])
        self.assertEqual(self.check("QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);\n"), [])
        self.assertEqual(self.check("auto app = std::make_unique<QApplication>(argc, argv);\n"), ["locale-setup"])
        self.assertEqual(self.check("auto* app = new QCoreApplication(argc, argv);\n"), ["locale-setup"])

    def test_code_page_conversions_and_c_numbers_are_refused(self) -> None:
        self.assertEqual(self.check("auto bytes = name.toLocal8Bit();\n"), ["local8bit"])
        self.assertEqual(self.check("auto name = QString::fromLocal8Bit(bytes);\n"), ["local8bit"])
        for call in ("std::stod(text)", "strtod(text, nullptr)", "std::atof(text)", 'std::sscanf(text, "%lf", &x)',
                     'std::printf("%f", x)', 'snprintf(buffer, 8, "%g", x)'):
            with self.subTest(call=call):
                self.assertEqual(self.check(f"auto value = {call};\n"), ["c-number"])
        self.assertEqual(self.check("auto text = QString::asprintf(\"%d\", 1);\n"), [])
        self.assertEqual(self.check("std::fputs(text, stderr);\n"), [])
        self.assertEqual(self.check('auto text = std::format("{:L}", 1.5);\n'), ["locale-format"])
        self.assertEqual(self.check('auto text = std::format("{:.3f}", 1.5);\n'), [])

    def test_paths_go_through_the_helper(self) -> None:
        self.assertEqual(self.check("auto text = path.string();\n"), ["narrow-path"])
        self.assertEqual(self.check("auto path = std::filesystem::u8path(text);\n"), ["narrow-path"])
        self.assertEqual(self.check('auto* file = std::fopen(name, "rb");\n'), ["narrow-path"])
        self.assertEqual(self.check("std::filesystem::path path(text.toStdU16String());\n"), ["narrow-path"])
        self.assertEqual(self.check("return {text.toStdU16String()};\n", "src/filesystem_path.hpp"), [])
        self.assertEqual(self.check("auto file = _wfopen(name, L\"rb\");\n"), [])
        self.assertEqual(self.check("auto bytes = value.toStdString();\n"), [])
        self.assertEqual(self.check("QStringDecoder decoder(QStringConverter::System);\n"), ["system-codec"])

    def test_narrow_paths_are_refused(self) -> None:
        for line in ("auto text = path.generic_string();", 'std::filesystem::path path("x.trk");',
                     'auto path = std::filesystem::path(MOTRACK_FIXTURES) / "a.trk";',
                     "std::filesystem::path path(text.toStdString());", "auto path = std::filesystem::path{std::string(name)};"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line + "\n"), ["narrow-path"])
        for line in ('auto path = std::filesystem::path(u8"x.trk");', 'auto path = std::filesystem::path(L"x.trk");',
                     "auto path = std::filesystem::path{};", "const auto path = motrack::toFilesystemPath(text);"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line + "\n"), [])

    def test_floating_to_string_is_refused_but_integers_are_not(self) -> None:
        for line in ("auto text = std::to_string(1.5);", "auto text = std::to_string(static_cast<double>(x));",
                     "auto text = std::to_string(.5f);"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line + "\n"), ["c-number"])
        for line in ("auto text = std::to_string(index + 1);", "auto text = std::to_string(track.id);"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line + "\n"), [])

    def test_model_sources_do_not_format_for_display(self) -> None:
        self.assertEqual(self.check("const QLocale locale;\n", "src/measurements.cpp"), ["model-locale"])
        self.assertEqual(self.check('auto text = tr("%L1 m").arg(x);\n', "src/measurements.cpp"), ["model-locale"])
        self.assertEqual(self.check("const QLocale locale;\n", "src/main_window.cpp"), [])

    def test_model_sources_come_from_the_cmake_libraries(self) -> None:
        models = checker.model_sources()
        self.assertIn("src/tracker_writer.cpp", models)
        self.assertIn("src/tracker_writer.hpp", models)
        self.assertIn("src/video_reader.cpp", models)
        self.assertNotIn("src/main_window.cpp", models)
        self.assertNotIn("src/position_plot.cpp", models)


class PythonTest(unittest.TestCase):
    def check(self, text: str) -> list[str]:
        return rules(checker.check_python(str(REPO / "tools/sample.py"), text))

    def test_text_io_names_its_encoding(self) -> None:
        self.assertEqual(self.check("open(path)\n"), ["python-encoding"])
        self.assertEqual(self.check("open(path, 'w')\n"), ["python-encoding"])
        self.assertEqual(self.check("open(path, mode='rt')\n"), ["python-encoding"])
        self.assertEqual(self.check("path.read_text()\n"), ["python-encoding"])
        self.assertEqual(self.check("path.write_text(text)\n"), ["python-encoding"])
        self.assertEqual(self.check("subprocess.run(command, text=True)\n"), ["python-encoding"])
        self.assertEqual(self.check("subprocess.check_output(command, universal_newlines=True)\n"), ["python-encoding"])
        for clean in ("open(path, 'rb')\n", "open(path, encoding='utf-8')\n", "path.read_text(encoding='utf-8')\n",
                      "subprocess.run(command, capture_output=True)\n",
                      "subprocess.run(command, text=True, encoding='utf-8', errors='replace')\n",
                      "archive.open(member)\n", "path.read_text()  # text-api: allowed (ASCII only)\n"):
            with self.subTest(clean=clean):
                self.assertEqual(self.check(clean), [])

    def test_text_mode_path_open_names_its_encoding(self) -> None:
        for line in ("path.open()\n", "path.open('w')\n", "path.open(mode='a')\n", "path.open('rt')\n"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line), ["python-encoding"])
        for line in ("path.open('rb')\n", "path.open('r', encoding='utf-8')\n", "archive.open(name)\n",
                     "zipfile.ZipFile(source).open('member.txt')\n", "archive.open(info, 'r')\n"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line), [])


class PowerShellTest(unittest.TestCase):
    def check(self, text: str) -> list[str]:
        return rules(checker.check_powershell(str(REPO / "tools/sample.ps1"), text))

    def test_writes_name_their_encoding(self) -> None:
        self.assertEqual(self.check("Set-Content -LiteralPath $path -Value $text\n"), ["ps-encoding"])
        self.assertEqual(self.check("$text | Out-File $path\n"), ["ps-encoding"])
        self.assertEqual(self.check("Add-Content $path $line -Encoding utf8\n"), [])
        self.assertEqual(self.check("# Set-Content is not used here\n"), [])

    def test_output_is_not_redirected_to_files(self) -> None:
        for line in ('"text" > out.txt\n', "& git log >> history.txt\n", "& tool *> all.log\n"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line), ["ps-encoding"])
        for line in ("& tool 2>&1\n", "& tool > $null\n", "& tool 2>$null\n", "if ($count -gt 1) { }\n",
                     'Write-Host "a > b"\n', "# redirect > here in a comment\n"):
            with self.subTest(line=line):
                self.assertEqual(self.check(line), [])


class RepositoryTest(unittest.TestCase):
    def test_the_repository_follows_the_rules(self) -> None:
        result = subprocess.run(
            [sys.executable, "-I", "-X", "utf8", str(SCRIPT)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            timeout=120,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
