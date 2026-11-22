#!/usr/bin/env python3
"""tools/check-utf8.py -- fail when a text file is not strict UTF-8 without a BOM, or holds double-encoded text.

Usage:
    python -I -X utf8 tools/check-utf8.py            # every tracked text file
    python -I -X utf8 tools/check-utf8.py FILE...    # just these files

Each file is read as bytes and decoded strictly as UTF-8 one line at a time, so every bad line is reported as
`file:line:column: reason`. A byte-order mark is an error: UTF-8 files have none, and UTF-16/UTF-32 files are not
allowed at all. Double-encoded ("mojibake") text is valid UTF-8 too, so it is looked for separately: UTF-8 that was
read as Windows-1252 or Latin-1 and saved again, such as an e acute turned into U+00C3 U+00A9. A run of characters
is flagged when it maps back, byte for byte, to the UTF-8 encoding of one non-ASCII character. A .ps1 with
non-ASCII text must start with `#Requires -Version 7`, because Windows PowerShell 5.1 reads a file without a BOM
in the ANSI code page.

Without arguments, the tracked files come from git. Files .gitattributes marks `binary` (media, archives) or
`-text` (test inputs that keep their exact bytes, which may be another encoding on purpose) are skipped. Adapted
from TaskSmack's tools/check-utf8.py; see CONTRIBUTING.md, "Text encoding and locale". Exits 1 on any problem.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

_BOMS = (
    (b"\xef\xbb\xbf", "UTF-8"),
    (b"\xff\xfe\x00\x00", "UTF-32 LE"),
    (b"\x00\x00\xfe\xff", "UTF-32 BE"),
    (b"\xff\xfe", "UTF-16 LE"),
    (b"\xfe\xff", "UTF-16 BE"),
)


def _single_byte_chars(byte: int) -> list[str]:
    """The characters `byte` reads as in Latin-1 and in Windows-1252 (the same one from A0 up)."""
    chars = [bytes([byte]).decode("latin-1")]
    try:
        chars.append(bytes([byte]).decode("cp1252"))
    except UnicodeDecodeError:
        pass  # 81, 8D, 8F, 90 and 9D are undefined in 1252
    return chars


# A UTF-8 lead byte (C2-F4) read as one character, then one to three continuation bytes (80-BF) read the same way:
# the shape a non-ASCII character takes once its UTF-8 bytes are decoded as 1252 or Latin-1.
_CHAR_TO_BYTE = {char: byte for byte in range(0x80, 0x100) for char in _single_byte_chars(byte)}
_LEADS = "".join(char for byte in range(0xC2, 0xF5) for char in _single_byte_chars(byte))
_CONTINUATIONS = "".join(char for byte in range(0x80, 0xC0) for char in _single_byte_chars(byte))
_MOJIBAKE = re.compile(f"[{re.escape(_LEADS)}][{re.escape(_CONTINUATIONS)}]{{1,3}}")

_REQUIRES_PWSH7 = re.compile(rb"(?im)^#Requires\s+-Version\s+(?:[7-9]|\d\d)(?:\.\d+)*\s*$")
_NON_ASCII = re.compile(rb"[\x80-\xff]")


def find_mojibake(text: str) -> list[tuple[int, str, str]]:
    """(column, garbled text, intended character) for each double-encoded character in `text`."""
    found: list[tuple[int, str, str]] = []
    for match in _MOJIBAKE.finditer(text):
        raw = bytes(_CHAR_TO_BYTE[char] for char in match.group())
        try:
            meant = raw.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            continue
        found.append((match.start() + 1, match.group(), meant))
    return found


def powershell_5_1_problem(path: str, data: bytes) -> list[str]:
    """One message when a .ps1 holds non-ASCII text but does not require PowerShell 7."""
    if Path(path).suffix.lower() != ".ps1" or _REQUIRES_PWSH7.search(data):
        return []
    for line_number, line in enumerate(data.split(b"\n"), start=1):
        found = _NON_ASCII.search(line)
        if found:
            return [
                f"{path}:{line_number}:{found.start() + 1}: non-ASCII text in a .ps1 without `#Requires -Version 7`; "
                "Windows PowerShell 5.1 reads it in the ANSI code page. Write it as [char]0x... or add the #Requires line"
            ]
    return []


def check_bytes(path: str, data: bytes) -> list[str]:
    """One message per problem in `data`, the contents of `path` (empty when it is singly encoded UTF-8)."""
    for bom, name in _BOMS:
        if data.startswith(bom):
            return [f"{path}:1:1: starts with a {name} byte-order mark; save the file as UTF-8 without a BOM"]
    problems = powershell_5_1_problem(path, data)
    for line_number, line in enumerate(data.split(b"\n"), start=1):
        try:
            text = line.decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            bad = line[error.start : error.end].hex(" ")
            problems.append(f"{path}:{line_number}:{error.start + 1}: not valid UTF-8 (byte(s) {bad}: {error.reason})")
            continue
        for column, garbled, meant in find_mojibake(text):
            problems.append(
                f"{path}:{line_number}:{column}: double-encoded text (bytes {garbled.encode('utf-8').hex(' ')} are "
                f"U+{ord(meant):04X} encoded twice; write {meant.encode('utf-8').hex(' ')})"
            )
    return problems


def check_file(path: str) -> list[str]:
    try:
        data = Path(path).read_bytes()
    except OSError as error:
        return [f"{path}: cannot read: {error.strerror}"]
    return check_bytes(path, data)


def parse_check_attr(output: bytes) -> dict[str, dict[str, str]]:
    """`git check-attr -z` output (path NUL attribute NUL value NUL ...) as {path: {attribute: value}}."""
    fields = output.decode("utf-8").split("\0")
    attributes: dict[str, dict[str, str]] = {}
    for index in range(0, len(fields) - 2, 3):
        attributes.setdefault(fields[index], {})[fields[index + 1]] = fields[index + 2]
    return attributes


def is_checked(attributes: dict[str, str]) -> bool:
    """Whether a file with these `binary`/`text` attributes is a text file this tool checks."""
    return attributes.get("binary") != "set" and attributes.get("text") != "unset"


def tracked_text_files(root: Path = ROOT) -> list[str]:
    """Every tracked file except those .gitattributes marks binary or -text, relative to `root`."""
    git = ["git", "-c", "core.quotePath=false", "-C", str(root)]
    listed = subprocess.run([*git, "ls-files", "-z"], capture_output=True, check=True, timeout=60).stdout
    names = [name for name in listed.decode("utf-8").split("\0") if name]
    attributes = subprocess.run(
        [*git, "check-attr", "-z", "--stdin", "binary", "text"],
        input="\0".join(names).encode("utf-8"),
        capture_output=True,
        check=True,
        timeout=60,
    ).stdout
    selected = parse_check_attr(attributes)
    return [name for name in names if is_checked(selected.get(name, {})) and (root / name).is_file()]


def main(argv: list[str]) -> int:
    if argv:
        paths = argv
    else:
        paths = [str(ROOT / name) for name in tracked_text_files()]
    problems = [problem for path in paths for problem in check_file(path)]
    for problem in problems:
        print(problem)
    if problems:
        print("Save these files as UTF-8 without a BOM and re-type double-encoded text "
              "(CONTRIBUTING.md, 'Text encoding and locale').")
        return 1
    if not argv:
        print(f"check-utf8: {len(paths)} tracked text files are UTF-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
