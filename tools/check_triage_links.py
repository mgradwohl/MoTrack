#!/usr/bin/env python3
"""tools/check_triage_links.py -- report upstream filings that the Tracker issue triage does not link.

Usage:
    python -I -X utf8 tools/check_triage_links.py                 # docs/upstream-issues.md
    python -I -X utf8 tools/check_triage_links.py path/to/doc.md

Each Markdown table row of the triage names its MoTrack issues in its last cell (the tracking column). For every
such issue, the comments are fetched with `gh issue view` and searched for filing records: a comment that says
something was filed ("Filed upstream as <url>", "Item 2 filed upstream as <url>", "filed as <url>"), reported
upstream ("Reported upstream as <url>"), grouped into an existing upstream issue, or opened ("Opened <url>"). A
phrase is negated when "not", "never", "will" or a word ending in "n't" comes at most two words before it in the
same clause ("not yet filed", "has not been filed", "will be filed", "Never filed"); a comment whose filing phrases
are all negated is skipped. Every OpenSourcePhysics issue a filing comment names,
as an issue URL or as `OpenSourcePhysics/<repo>#<n>`, must be linked from the same row; each missing one is
reported as `doc:line: MoTrack #N: filed upstream as OpenSourcePhysics/<repo>#<n>, not linked from this row`.
Links outside table rows are not checked, and neither are issue bodies.

It needs the network and an authenticated `gh`, so it is a manual tool for triage sweeps (CONTRIBUTING.md, "Pull
requests and review"), not part of the test suite; tests/test_check_triage_links.py covers its matching offline.
Exits 1 when a filing is missing from its row and 2 when `gh` is missing, fails or returns something other than
the expected JSON.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DOC = ROOT / "docs" / "upstream-issues.md"
MOTRACK_REPO = "mgradwohl/MoTrack"
UPSTREAM_OWNER = "OpenSourcePhysics"

_MOTRACK_ISSUE = re.compile(r"https://github\.com/mgradwohl/MoTrack/issues/(\d+)\b", re.IGNORECASE)
_UPSTREAM_URL = re.compile(rf"https?://github\.com/{UPSTREAM_OWNER}/([\w.-]+)/issues/(\d+)\b", re.IGNORECASE)
_UPSTREAM_SHORT = re.compile(rf"(?<![\w/]){UPSTREAM_OWNER}/([\w.-]+)#(\d+)\b", re.IGNORECASE)
_REF_START = rf"(?:https?://github\.com/{UPSTREAM_OWNER}/|{UPSTREAM_OWNER}/)"
_FILING = re.compile(
    rf"\b(?:filed|grouped\s+into|reported\s+upstream|opened(?=(?:\s+upstream)?(?:\s+as)?\s+{_REF_START}))\b",
    re.IGNORECASE,
)
# A negating word, then at most two more words separated only by spaces (no punctuation ends the clause), then the
# phrase; `*` and `_` allow Markdown emphasis such as "**not filed**".
_NEGATION = re.compile(r"\b(?:not|never|will|\w+n['’]t)(?:\s+\w+){0,2}[\s*_]*$", re.IGNORECASE)
_CELL_SEPARATOR = re.compile(r"(?<!\\)\|")


@dataclass(frozen=True, order=True)
class UpstreamRef:
    """An upstream issue; the repository name is compared without regard to case."""

    repo: str
    number: int

    def __str__(self) -> str:
        return f"{UPSTREAM_OWNER}/{self.repo}#{self.number}"


@dataclass(frozen=True)
class Row:
    """A table row: its 1-based line number, its text and the MoTrack issues in its last cell."""

    line: int
    text: str
    issues: tuple[int, ...]


@dataclass(frozen=True)
class Missing:
    line: int
    issue: int
    ref: UpstreamRef

    def __str__(self) -> str:
        return f"{self.line}: MoTrack #{self.issue}: filed upstream as {self.ref}, not linked from this row"


def upstream_refs(text: str) -> set[UpstreamRef]:
    """Every OpenSourcePhysics issue `text` names as an issue URL or as `OpenSourcePhysics/<repo>#<n>`."""
    found = {UpstreamRef(repo.lower(), int(number)) for repo, number in _UPSTREAM_URL.findall(text)}
    found.update(UpstreamRef(repo.lower(), int(number)) for repo, number in _UPSTREAM_SHORT.findall(text))
    return found


def records_filing(comment: str) -> bool:
    """True when `comment` has a filing phrase that is not negated ("not filed", "will be filed", "Never filed")."""
    return any(not _NEGATION.search(comment, 0, match.start()) for match in _FILING.finditer(comment))


def filed_refs(comments: Iterable[str]) -> set[UpstreamRef]:
    """The upstream issues named by the comments that record a filing."""
    found: set[UpstreamRef] = set()
    for comment in comments:
        if records_filing(comment):
            found |= upstream_refs(comment)
    return found


def table_rows(markdown: str) -> list[Row]:
    """The table rows whose last cell links a MoTrack issue."""
    rows: list[Row] = []
    for line_number, line in enumerate(markdown.splitlines(), start=1):
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = _CELL_SEPARATOR.split(stripped.strip("|"))
        issues = tuple(dict.fromkeys(int(number) for number in _MOTRACK_ISSUE.findall(cells[-1])))
        if issues:
            rows.append(Row(line_number, line, issues))
    return rows


def missing_filings(rows: Iterable[Row], comments_of: Callable[[int], list[str]]) -> list[Missing]:
    """Each upstream filing recorded in a row's issues that the row does not link."""
    missing: list[Missing] = []
    for row in rows:
        linked = upstream_refs(row.text)
        for issue in row.issues:
            missing.extend(Missing(row.line, issue, ref) for ref in sorted(filed_refs(comments_of(issue)) - linked))
    return missing


class GhError(RuntimeError):
    """`gh` could not be run, failed, or returned something other than the issue's comments."""


def gh_comments(issue: int) -> list[str]:
    """The comment bodies of a MoTrack issue, read with `gh`."""
    try:
        result = subprocess.run(
            ["gh", "issue", "view", str(issue), "--repo", MOTRACK_REPO, "--json", "comments"],
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as error:
        raise GhError(f"cannot run gh for issue {issue}: {error}") from error
    if result.returncode != 0:
        raise GhError(f"gh issue view {issue} failed: {result.stderr.strip()}")
    try:
        return [comment["body"] for comment in json.loads(result.stdout)["comments"]]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise GhError(f"gh issue view {issue} returned unexpected output: {error}") from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Report upstream filings that the Tracker issue triage does not link.")
    parser.add_argument("doc", nargs="?", type=Path, default=DEFAULT_DOC, help="triage document (default: %(default)s)")
    args = parser.parse_args(argv)

    rows = table_rows(args.doc.read_text(encoding="utf-8"))
    cache: dict[int, list[str]] = {}

    def comments_of(issue: int) -> list[str]:
        if issue not in cache:
            cache[issue] = gh_comments(issue)
        return cache[issue]

    try:
        missing = missing_filings(rows, comments_of)
    except GhError as error:
        print(f"check_triage_links: {error}", file=sys.stderr)
        return 2
    doc = args.doc.resolve()
    shown = doc.relative_to(ROOT) if doc.is_relative_to(ROOT) else doc
    for item in missing:
        print(f"{shown.as_posix()}:{item}")
    print(f"check_triage_links: {len(cache)} issues in {len(rows)} rows, {len(missing)} filings not linked", file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
