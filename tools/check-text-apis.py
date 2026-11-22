#!/usr/bin/env python3
"""tools/check-text-apis.py -- fail on code that makes text or numbers depend on the locale or code page.

Usage:
    python -I -X utf8 tools/check-text-apis.py            # every tracked C++, Python and PowerShell file
    python -I -X utf8 tools/check-text-apis.py FILE...    # just these files

The rules are CONTRIBUTING.md's "Text encoding and locale". Each finding is `file:line: rule: explanation`.

C++ (src/, tests/, benchmarks/):
  locale-setup     setlocale (except a query with nullptr), std::locale::global and _configthreadlocale belong to
                   src/app_locale.cpp alone; every Q*Application must be followed by motrack::configureProcessLocale()
  local8bit        toLocal8Bit/fromLocal8Bit convert through the system code page; Qt 6 QString <-> std::string is UTF-8
  c-number         std::stod/stof/strtod/atof/sscanf and the printf family follow LC_NUMERIC; use std::from_chars,
                   std::to_chars, QString::number/toDouble or QLocale::c()
  locale-format    std::format's {:L} follows the global C++ locale; displayed numbers use QLocale
  narrow-path      path::string(), std::filesystem::u8path and UTF-16 path conversions outside src/filesystem_path.hpp,
                   and narrow fopen; use motrack::toFilesystemPath/fromFilesystemPath, QFile or std::fstream(path)
  system-codec     QStringConverter::System decodes in the system code page
  model-locale     QLocale and %L placeholders in the science, model and media libraries (motrack_expression,
                   motrack_ode, motrack_project, motrack_input, motrack_media, motrack_image_package): machine-readable
Python (tools/, tests/):
  python-encoding  open() in text mode, read_text/write_text and text-mode subprocess calls must pass encoding=
PowerShell (tools/):
  ps-encoding      Set-Content, Add-Content and Out-File must pass -Encoding

A line ending with (or preceded by a line holding) `text-api: allowed (reason)` is exempt; give the reason.
"""

from __future__ import annotations

import ast
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOW = "text-api: allowed"
MODEL_LIBRARIES = ("motrack_expression", "motrack_ode", "motrack_project", "motrack_input", "motrack_media", "motrack_image_package")


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule}: {self.message}"


_COMMENT = re.compile(r"//.*$")
_STRING = re.compile(r'"(?:\\.|[^"\\])*"')
_SETLOCALE = re.compile(r"\b(?:_w)?setlocale\s*\(\s*[^,()]+,\s*([^)]*)\)")
_LOCALE_GLOBAL = re.compile(r"\b(?:std::)?locale::global\s*\(|\b_configthreadlocale\s*\(")
_APPLICATION = re.compile(r"\bQ(?:Core|Gui)?Application\s+\w+\s*[({]")
_CXX_RULES = (
    ("local8bit", re.compile(r"\b(?:to|from)Local8Bit\b"), "converts through the system code page; use toStdString/fromStdString (UTF-8)"),
    (
        "c-number",
        re.compile(r"\b(?:std::)?(?:stod|stof|stold|strtod|strtof|strtold|atof|sscanf|scanf|printf|fprintf|sprintf|snprintf)\s*\("),
        "follows LC_NUMERIC; use std::from_chars/std::to_chars, QString::number/toDouble or QLocale::c()",
    ),
    ("narrow-path", re.compile(r"\.string\(\)|\bu8path\s*\(|(?<![\w:])(?:std::)?fopen\s*\("), "goes through the ANSI code page on Windows"),
    ("system-codec", re.compile(r"QStringConverter::System\b"), "decodes in the system code page; files are UTF-8"),
)
_UTF16_PATH = re.compile(r"\b(?:toStdU16String|fromStdU16String|u16string)\s*\(")
_LOCALE_FORMAT = re.compile(r"\{[^{}]*:[^{}]*L[^{}]*\}")
_MODEL_LOCALE = re.compile(r"\bQLocale\b")
_PERCENT_L = re.compile(r"%L\d")


def _allowed(lines: list[str], index: int) -> bool:
    return ALLOW in lines[index] or (index > 0 and ALLOW in lines[index - 1])


def model_sources(cmake: Path = ROOT / "CMakeLists.txt") -> set[str]:
    """The sources (and their headers) of the model libraries, from CMakeLists.txt's add_library calls."""
    text = cmake.read_text(encoding="utf-8")
    sources: set[str] = set()
    for library in MODEL_LIBRARIES:
        match = re.search(rf"add_library\(\s*{library}\s+STATIC\s+([^)]*)\)", text)
        if match:
            for source in match.group(1).split():
                sources.add(source)
                sources.add(re.sub(r"\.cpp$", ".hpp", source))
    return sources


def check_cxx(path: str, text: str, models: set[str]) -> list[Finding]:
    relative = Path(path).resolve().relative_to(ROOT).as_posix() if Path(path).resolve().is_relative_to(ROOT) else path
    lines = text.split("\n")
    findings: list[Finding] = []
    constructs_application = False
    for index, raw in enumerate(lines):
        code = _COMMENT.sub("", raw)
        unquoted = _STRING.sub('""', code)
        number = index + 1
        allowed = _allowed(lines, index)
        if _APPLICATION.search(unquoted):
            constructs_application = True
        if allowed:
            continue
        for match in _SETLOCALE.finditer(unquoted):
            if match.group(1).strip() not in ("nullptr", "NULL", "0"):
                findings.append(Finding(relative, number, "locale-setup", "only src/app_locale.cpp sets the C locale"))
        if _LOCALE_GLOBAL.search(unquoted):
            findings.append(Finding(relative, number, "locale-setup", "only src/app_locale.cpp may change the process locale"))
        for rule, pattern, message in _CXX_RULES:
            if pattern.search(unquoted):
                findings.append(Finding(relative, number, rule, message))
        if relative != "src/filesystem_path.hpp" and _UTF16_PATH.search(unquoted):
            findings.append(
                Finding(relative, number, "narrow-path", "convert QString <-> std::filesystem::path with src/filesystem_path.hpp")
            )
        for literal in _STRING.findall(code):
            if "format" in code and _LOCALE_FORMAT.search(literal):
                findings.append(Finding(relative, number, "locale-format", "{:L} follows the global C++ locale; use QLocale"))
        if relative in models and (_MODEL_LOCALE.search(unquoted) or any(_PERCENT_L.search(s) for s in _STRING.findall(code))):
            findings.append(
                Finding(relative, number, "model-locale", "model text is machine-readable; format numbers classically, display in the UI")
            )
    if constructs_application and "configureProcessLocale(" not in text:
        findings.append(
            Finding(relative, 1, "locale-setup", "constructs a Q*Application without calling motrack::configureProcessLocale() after it")
        )
    return findings


def _keyword(call: ast.Call, name: str) -> ast.expr | None:
    return next((keyword.value for keyword in call.keywords if keyword.arg == name), None)


def _true(node: ast.expr | None) -> bool:
    return isinstance(node, ast.Constant) and node.value is True


def check_python(path: str, text: str) -> list[Finding]:
    relative = Path(path).resolve().relative_to(ROOT).as_posix() if Path(path).resolve().is_relative_to(ROOT) else path
    lines = text.split("\n")
    findings: list[Finding] = []
    for node in ast.walk(ast.parse(text, filename=path)):
        if not isinstance(node, ast.Call) or _keyword(node, "encoding") is not None or _allowed(lines, node.lineno - 1):
            continue
        function = node.func
        name = function.id if isinstance(function, ast.Name) else function.attr if isinstance(function, ast.Attribute) else ""
        problem = None
        if isinstance(function, ast.Name) and name == "open":
            mode = node.args[1] if len(node.args) > 1 else _keyword(node, "mode")
            if not (isinstance(mode, ast.Constant) and isinstance(mode.value, str) and "b" in mode.value):
                problem = "open() in text mode"
        elif isinstance(function, ast.Attribute) and name in ("read_text", "write_text"):
            problem = f"{name}()"
        elif isinstance(function, ast.Attribute) and name in ("run", "check_output", "Popen", "call", "check_call"):
            if _true(_keyword(node, "text")) or _true(_keyword(node, "universal_newlines")):
                problem = f"subprocess.{name}(text=True)"
        if problem:
            findings.append(Finding(relative, node.lineno, "python-encoding", f"{problem} needs encoding=\"utf-8\""))
    return findings


_PS_WRITE = re.compile(r"\b(?:Set-Content|Add-Content|Out-File)\b", re.IGNORECASE)


def check_powershell(path: str, text: str) -> list[Finding]:
    relative = Path(path).resolve().relative_to(ROOT).as_posix() if Path(path).resolve().is_relative_to(ROOT) else path
    lines = text.split("\n")
    return [
        Finding(relative, index + 1, "ps-encoding", "name the encoding: -Encoding utf8 (UTF-8 without a BOM in PowerShell 7)")
        for index, line in enumerate(lines)
        if _PS_WRITE.search(line.split("#", 1)[0]) and not re.search(r"-Encoding\b", line, re.IGNORECASE) and not _allowed(lines, index)
    ]


CXX_DIRECTORIES = ("src/", "tests/", "benchmarks/")  # tools/prereqs/probe.cpp is a toolchain probe, not MoTrack code


def check_file(path: str, models: set[str]) -> list[Finding]:
    suffix = Path(path).suffix.lower()
    text = Path(path).read_bytes().decode("utf-8", errors="replace")
    resolved = Path(path).resolve()
    inside = resolved.relative_to(ROOT).as_posix() if resolved.is_relative_to(ROOT) else None
    if suffix in (".cpp", ".hpp", ".h", ".cc") and (inside is None or inside.startswith(CXX_DIRECTORIES)):
        return check_cxx(path, text, models)
    if suffix == ".py":
        return check_python(path, text)
    if suffix == ".ps1":
        return check_powershell(path, text)
    return []


def tracked_files(root: Path = ROOT) -> list[str]:
    listed = subprocess.run(
        ["git", "-c", "core.quotePath=false", "-C", str(root), "ls-files", "-z", "--", "src", "tests", "benchmarks", "tools"],
        capture_output=True,
        check=True,
        timeout=60,
    ).stdout
    return [str(root / name) for name in listed.decode("utf-8").split("\0") if name and (root / name).is_file()]


def main(argv: list[str]) -> int:
    models = model_sources()
    findings = [finding for path in (argv or tracked_files()) for finding in check_file(path, models)]
    for finding in findings:
        print(finding)
    if findings:
        print("See CONTRIBUTING.md, 'Text encoding and locale'. Mark a deliberate exception with "
              f"`{ALLOW} (reason)` on the line or the line before.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
