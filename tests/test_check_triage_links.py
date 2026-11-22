#!/usr/bin/env python3
"""Offline tests for tools/check_triage_links.py: canned comments and rows, no `gh` and no network."""

import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / "tools" / "check_triage_links.py"
spec = importlib.util.spec_from_file_location("check_triage_links", SCRIPT)
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker  # dataclasses look their module up there
spec.loader.exec_module(checker)

Ref = checker.UpstreamRef


def osp(number: int) -> str:
    return f"https://github.com/OpenSourcePhysics/osp/issues/{number}"


def tracker(number: int) -> str:
    return f"https://github.com/OpenSourcePhysics/tracker/issues/{number}"


def motrack(number: int) -> str:
    return f"[MoTrack #{number}](https://github.com/mgradwohl/MoTrack/issues/{number})"


DOC = f"""# Triage

A paragraph links {motrack(9)}, which is not a table row.

| Observation | Evidence | MoTrack tracking |
| --- | --- | --- |
| Filed and linked | Filed as [osp#20]({osp(20)}). | {motrack(193)} |
| Filed, not linked | Related to [#230](https://github.com/mgradwohl/MoTrack/issues/230). | {motrack(216)} |
| Two issues, one linked | Filed as [tracker#97]({tracker(97)}). | {motrack(252)}, [#288](https://github.com/mgradwohl/MoTrack/issues/288) |
"""

COMMENTS = {
    193: [f"Filed upstream as {osp(20)} (2026-11-21, with the maintainer's approval)."],
    216: [f"Filed upstream as {osp(27)} (2026-11-22).", "Upstream: OpenSourcePhysics/osp#27."],
    230: [f"Filed upstream (2026-11-22) as two root causes:\n- {osp(37)}\n- {osp(38)}"],
    252: [f"Filed upstream as {tracker(97)}."],
    288: ["Reproduced under Xvfb.", f"Filed upstream as {tracker(96)} (2026-11-22)."],
}


class UpstreamRefsTest(unittest.TestCase):
    def test_issue_urls_and_owner_repo_numbers_are_found(self) -> None:
        text = f"see {osp(20)}, {tracker(105)}#issuecomment-1 and OpenSourcePhysics/osp#10; opensourcephysics/OSP#12"
        self.assertEqual(checker.upstream_refs(text), {Ref("osp", 20), Ref("tracker", 105), Ref("osp", 10), Ref("osp", 12)})

    def test_other_links_and_bare_short_forms_are_not_refs(self) -> None:
        text = (
            "osp#22, tracker#78, mgradwohl/MoTrack#274, https://github.com/mgradwohl/MoTrack/issues/274, "
            "https://github.com/OpenSourcePhysics/osp/blob/d49c6ec/src/JarTool.java#L803 and "
            "https://github.com/OpenSourcePhysics/tracker/pull/12"
        )
        self.assertEqual(checker.upstream_refs(text), set())

    def test_refs_print_as_owner_repo_number(self) -> None:
        self.assertEqual(str(Ref("tracker", 105)), "OpenSourcePhysics/tracker#105")


class FilingTest(unittest.TestCase):
    def test_filing_phrases_count(self) -> None:
        for comment in (
            f"Filed upstream as {osp(10)}.",
            f"Item 2 filed upstream as {tracker(95)}.",
            f"1. Momentum angle: filed as {tracker(83)}.",
            f"Resize part filed upstream as {osp(36)}.",
            f"Same root cause, so it was grouped into the existing upstream issue {osp(33)}.",
            f"2. Sample rate: **not filed**.\n3. Help formula: filed as {tracker(84)}.",
            f"Reported upstream as {osp(16)}.",
            f"Opened {tracker(105)} (2026-11-22, by the maintainer).",
            "Opened upstream as OpenSourcePhysics/osp#57.",
            f"Could not reproduce it headless; filed as {osp(46)} from source.",
        ):
            with self.subTest(comment=comment):
                self.assertTrue(checker.records_filing(comment))

    def test_not_filed_and_plain_mentions_do_not_count(self) -> None:
        for comment in (
            "Not filed upstream (checked 2026-11-22 against the Tracker 6.3.5 JAR).",
            "Checked; **not filed upstream** because it could not be reproduced headless.",
            f"Reported separately in OpenSourcePhysics/osp (not yet filed); compare {osp(37)}.",
            f"It has not been filed; see {osp(37)}.",
            f"The report will be filed after review; related: {osp(37)}.",
            f"Never filed. Related {osp(37)}.",
            f"It wasn't filed upstream, unlike {osp(37)}.",
            f"Not reported upstream; compare {osp(37)}.",
            f"Upstream: OpenSourcePhysics/osp#20, reproduced against the 6.3.5 JAR ({osp(20)}).",
            "No corresponding upstream filing is known.",
            f"Opened the experiment in Tracker 6.3.5; it differs from {tracker(14)}.",
        ):
            with self.subTest(comment=comment):
                self.assertFalse(checker.records_filing(comment))

    def test_only_filing_comments_supply_refs(self) -> None:
        comments = [
            f"Not filed upstream; it does not share {osp(37)}'s root cause.",
            f"Filed upstream as two issues:\n- {osp(51)}\n- {osp(52)}",
        ]
        self.assertEqual(checker.filed_refs(comments), {Ref("osp", 51), Ref("osp", 52)})


class RowsTest(unittest.TestCase):
    def test_rows_are_table_lines_whose_last_cell_links_issues(self) -> None:
        rows = checker.table_rows(DOC)
        self.assertEqual([(row.line, row.issues) for row in rows], [(7, (193,)), (8, (216,)), (9, (252, 288))])

    def test_an_issue_linked_twice_in_a_cell_is_listed_once(self) -> None:
        rows = checker.table_rows(f"| a | b | {motrack(83)}, {motrack(83)} |\n")
        self.assertEqual(rows[0].issues, (83,))

    def test_escaped_pipes_stay_inside_their_cell(self) -> None:
        rows = checker.table_rows(f"| a \\| b | {motrack(1)} \\| {motrack(2)} |\n")
        self.assertEqual(rows[0].issues, (1, 2))


class MissingFilingsTest(unittest.TestCase):
    def test_filings_missing_from_their_row_are_reported(self) -> None:
        missing = checker.missing_filings(checker.table_rows(DOC), lambda issue: COMMENTS.get(issue, []))
        self.assertEqual(
            [str(item) for item in missing],
            [
                "8: MoTrack #216: filed upstream as OpenSourcePhysics/osp#27, not linked from this row",
                "9: MoTrack #288: filed upstream as OpenSourcePhysics/tracker#96, not linked from this row",
            ],
        )

    def test_a_short_form_in_the_row_counts_as_linked(self) -> None:
        rows = checker.table_rows(f"| x | Upstream: OpenSourcePhysics/osp#27. | {motrack(216)} |\n")
        self.assertEqual(checker.missing_filings(rows, lambda issue: COMMENTS[issue]), [])


class GhCommentsTest(unittest.TestCase):
    def completed(self, stdout: str, returncode: int = 0, stderr: str = "") -> subprocess.CompletedProcess:
        return subprocess.CompletedProcess(["gh"], returncode, stdout=stdout, stderr=stderr)

    def test_comment_bodies_are_read_as_utf8_json(self) -> None:
        output = json.dumps({"comments": [{"body": "first"}, {"body": "second"}]})
        with mock.patch.object(checker.subprocess, "run", return_value=self.completed(output)) as run:
            self.assertEqual(checker.gh_comments(320), ["first", "second"])
        self.assertEqual(run.call_args.args[0], ["gh", "issue", "view", "320", "--repo", "mgradwohl/MoTrack", "--json", "comments"])
        self.assertEqual(run.call_args.kwargs["encoding"], "utf-8")

    def test_failures_become_gh_errors(self) -> None:
        for name, patch in (
            ("missing gh", {"side_effect": FileNotFoundError(2, "No such file or directory", "gh")}),
            ("gh exit status", {"return_value": self.completed("", 1, "HTTP 401: Bad credentials")}),
            ("not JSON", {"return_value": self.completed("<html>rate limited</html>")}),
            ("no comments field", {"return_value": self.completed(json.dumps({"title": "x"}))}),
        ):
            with self.subTest(name), mock.patch.object(checker.subprocess, "run", **patch):
                with self.assertRaises(checker.GhError):
                    checker.gh_comments(320)


class MainTest(unittest.TestCase):
    def run_main(self, comments_of) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as directory:
            doc = Path(directory) / "upstream-issues.md"
            doc.write_text(DOC, encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with (
                mock.patch.object(checker, "gh_comments", comments_of),
                contextlib.redirect_stdout(out),
                contextlib.redirect_stderr(err),
            ):
                code = checker.main([str(doc)])
        return code, out.getvalue(), err.getvalue()

    def test_reports_each_missing_filing_and_fetches_each_issue_once(self) -> None:
        fetched: list[int] = []

        def comments_of(issue: int) -> list[str]:
            fetched.append(issue)
            return COMMENTS.get(issue, [])

        code, out, err = self.run_main(comments_of)
        self.assertEqual(code, 1)
        self.assertEqual(sorted(fetched), [193, 216, 252, 288])
        self.assertEqual(len(out.splitlines()), 2)
        expected = "upstream-issues.md:8: MoTrack #216: filed upstream as OpenSourcePhysics/osp#27, not linked from this row"
        self.assertTrue(out.splitlines()[0].endswith(expected), out)
        self.assertIn("4 issues in 3 rows, 2 filings not linked", err)

    def test_a_gh_failure_exits_2(self) -> None:
        def failing(issue: int) -> list[str]:
            raise checker.GhError(f"gh issue view {issue} failed: not authenticated")

        code, out, err = self.run_main(failing)
        self.assertEqual((code, out), (2, ""))
        self.assertIn("not authenticated", err)

    def test_a_missing_gh_exits_2_without_a_traceback(self) -> None:
        with mock.patch.object(checker.subprocess, "run", side_effect=FileNotFoundError(2, "No such file", "gh")):
            code, out, err = self.run_main(checker.gh_comments)
        self.assertEqual((code, out), (2, ""))
        self.assertIn("cannot run gh", err)
        self.assertNotIn("Traceback", err)


if __name__ == "__main__":
    unittest.main()
