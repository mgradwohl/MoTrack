"""Integrity and failure-path tests for the compatibility corpus runner (no network)."""

import hashlib
import copy
from contextlib import nullcontext, redirect_stderr, redirect_stdout
import io
import json
import math
import os
import re
import shutil
import struct
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import warnings
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import corpus


class Download(io.BytesIO):
    def __init__(self, data, url="https://example.org/sample.trk"):
        super().__init__(data)
        self.url = url

    def geturl(self):
        return self.url


class MatchOracleTests(unittest.TestCase):
    """The template-matching kernel oracle (tools/reference/MatchOracle.java) without running Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()
        self.fixture = json.loads(corpus.MATCH_REFERENCE.read_text(encoding="utf-8"))

    def check(self, document, *, record=False, commands=None):
        """Run check_match_oracle with a fake Java process that emits `document`."""
        def run(command):
            if commands is not None:
                commands.append(command)
            Path(command[-1]).write_text(json.dumps(document), encoding="utf-8")

        with patch.object(corpus, "verify_file") as verify, patch.object(corpus, "run", side_effect=run), \
                patch("sys.stdout", new_callable=io.StringIO) as output:
            corpus.check_match_oracle(self.manifest, self.root, "java", record=record)
        verify.assert_called_once_with(self.root / self.manifest["reference"]["file"], self.manifest["reference"])
        return output.getvalue()

    def test_committed_fixture_covers_every_case_family_in_its_canonical_layout(self):
        corpus.validate_match_reference(self.fixture, self.manifest)
        self.assertEqual([entry["alpha"] for entry in self.fixture["composite"]], list(range(1, 256)))
        self.assertTrue({"ellipse-empty", "ellipse-key-default", "rectangle-9", "ellipse-max"} <= {m["id"] for m in self.fixture["masks"]})
        ids = {case["id"] for case in self.fixture["matches"]}
        self.assertTrue({"exact", "uniform", "outside-right", "line(0.0)", "line-misses-area", "line-outside-trimmed-area"} <= ids)
        self.assertTrue(any(case["id"].startswith("beyond(") for case in self.fixture["matches"]))
        self.assertEqual({s["id"] for s in self.fixture["sequences"]} & {"clean", "occlusion", "evolving", "line-tilted"},
                         {"clean", "occlusion", "evolving", "line-tilted"})
        # The reference defects the C++ tests rely on are recorded, not hidden.
        outside = next(case for case in self.fixture["matches"] if case["id"] == "line-outside-trimmed-area")
        self.assertEqual(outside["result"]["exception"], "ArrayIndexOutOfBoundsException")
        self.assertIn("NegativeArraySizeException", {row.get("exception") for row in self.fixture["search_points"]})
        self.assertEqual(corpus.MATCH_REFERENCE.read_bytes().decode("utf-8"), corpus.match_reference_text(self.fixture))
        self.assertLess(corpus.MATCH_REFERENCE.stat().st_size, 256 * 1024)

    def test_check_runs_the_pinned_jar_privately_and_accepts_an_identical_regeneration(self):
        commands = []
        count = sum(len(self.fixture[section]) for section in corpus.MATCH_LISTS) + 1
        self.assertIn(f"{count} Java template-matching cases match tests/science/match-reference.json",
                      self.check(self.fixture, commands=commands))
        [command] = commands
        self.assertEqual(command[0], "java")
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertIn("-Xmx256m", command)
        self.assertEqual(command[command.index("--class-path") + 1], str(self.root / self.manifest["reference"]["file"]))
        self.assertEqual(Path(command[-2]), corpus.ROOT / "tools/reference/MatchOracle.java")
        home = next(arg for arg in command if arg.startswith("-Duser.home=")).split("=", 1)[1]
        self.assertFalse(Path(home).is_relative_to(self.root))
        self.assertFalse(Path(home).exists())

    def test_check_rejects_hash_score_position_and_case_drift(self):
        import math  # local: keeps this class independent of the module's shared import lines

        def first_point(document, line):
            return next(case for case in document["matches"]
                        if (case["line"] is not None) == line and case["result"].get("point"))["result"]["point"]

        for mutation in ["composite", "mask", "evolution", "points", "score", "position", "line-position", "sequence", "missing"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "composite":
                document["composite"][7]["opaque"] = "0" * 16
            elif mutation == "mask":
                document["masks"][0]["template"]["trim"] = [0, 0]
            elif mutation == "evolution":
                document["evolution"]["steps"][0]["template"]["hash"] = "f" * 16
            elif mutation == "points":
                next(row for row in document["search_points"] if row.get("points"))["points"][0][0] += 1
            elif mutation == "score":
                next(case for case in document["matches"] if case["id"] == "shift(0.25,0.0)")["result"]["height"] *= 1 + 1e-15
            elif mutation == "position":
                point = first_point(document, False)
                point[0] = math.nextafter(point[0], math.inf)
            elif mutation == "line-position":
                first_point(document, True)[1] += 1e-6
            elif mutation == "sequence":
                document["sequences"][0]["steps"][3]["good"] = not document["sequences"][0]["steps"][3]["good"]
            else:
                document["matches"].pop()
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(document)

    def test_one_dimensional_positions_allow_last_bit_trigonometric_differences(self):
        document = copy.deepcopy(self.fixture)
        case = next(case for case in document["matches"] if case["id"] == "line(0.3)")
        case["result"]["point"][0] += 1e-12
        self.check(document)

    def test_record_writes_the_canonical_fixture_only_when_requested(self):
        destination = self.root / "science" / "match-reference.json"
        with patch.object(corpus, "MATCH_REFERENCE", destination):
            self.assertIn("recorded", self.check(self.fixture, record=True))
        self.assertEqual(destination.read_bytes(), corpus.match_reference_text(self.fixture).encode("utf-8"))
        with patch.object(sys, "argv", ["corpus.py", "session", "--record"]), patch.object(corpus, "check_sessions") as sessions, \
                self.assertRaises(SystemExit), patch("sys.stderr", new_callable=io.StringIO):
            corpus.main()
        sessions.assert_not_called()
        with patch.object(sys, "argv", ["corpus.py", "match-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_match_oracle") as oracle:
            corpus.main()
        oracle.assert_called_once()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])

    def test_validation_rejects_malformed_or_incomplete_documents(self):
        for mutation in ["version", "schema", "section", "empty", "alphas", "hash", "duplicate", "no-result", "evolution",
                         "frames", "not-object"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "version": document["reference_version"] = "6.3.4"
            elif mutation == "schema": document["schema_version"] = 2
            elif mutation == "section": document["extra"] = []
            elif mutation == "empty": document["masks"] = []
            elif mutation == "alphas": document["composite"].pop(0)
            elif mutation == "hash": document["composite"][0]["translucent"] = "xyz"
            elif mutation == "duplicate": document["matches"].append(document["matches"][0])
            elif mutation == "no-result": del document["matches"][0]["result"]
            elif mutation == "evolution": document["evolution"]["steps"] = []
            elif mutation == "frames": document["sequences"][0]["steps"].pop()
            else: document["sequences"][0] = "sequence"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.validate_match_reference(document, self.manifest)


class CorpusTests(unittest.TestCase):
    def test_multitab_manifest_requires_independent_bounded_member_expectations(self):
        original = corpus.load_manifest()
        for mutation in ["not-archive", "not-list", "one-tab", "too-many", "duplicate", "escape", "wrong-shape",
                         "missing-template", "unaudited-template", "known-issue", "nested-archive", "selected-mismatch"]:
            manifest = copy.deepcopy(original)
            case = next(c for c in manifest["cases"] if c["id"] == "multitab-images")
            secondary = next(c for c in manifest["cases"] if c["id"] == "secondary-images")
            tabs = case["session_archive_tabs"]
            if mutation == "not-archive": case["session_archive"] = False
            elif mutation == "not-list": case["session_archive_tabs"] = {}
            elif mutation == "one-tab": tabs.pop()
            elif mutation == "too-many": case["session_archive_tabs"] = tabs * 5
            elif mutation == "duplicate": tabs[1]["member"] = tabs[0]["member"]
            elif mutation == "escape": tabs[1]["member"] = "../outside.trk"
            elif mutation == "wrong-shape": tabs[1]["extra"] = "unreviewed"
            elif mutation == "missing-template": tabs[1]["case"] = "missing"
            elif mutation == "unaudited-template": del secondary["session_tracks"]
            elif mutation == "known-issue": secondary["session_known_issue"] = "known loss"
            elif mutation == "nested-archive": secondary["session_archive"] = True
            elif mutation == "selected-mismatch": tabs[0]["case"] = "secondary-images"
            path = self.root / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.load_manifest(path)

    def test_multitab_sessions_open_the_archive_and_independently_check_every_tab(self):
        for case_id in ["multitab-images", "archived-movie"]:
            with self.subTest(case=case_id):
                self.check_multitab_session_case(case_id)

    def test_grouped_sessions_check_both_edited_tabs_and_native_reimports(self):
        for case_id in ["multitab-images", "archived-movie"]:
            with self.subTest(case=case_id):
                self.check_multitab_session_case(case_id, grouped=True)

    def check_multitab_session_case(self, case_id, grouped=False):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == case_id)
        templates = [next(c for c in manifest["cases"] if c["id"] == tab["case"])
                     for tab in case["session_archive_tabs"]]
        snapshots, desired = [], []
        for index, template in enumerate(templates):
            expected = json.loads(corpus.expected_path(template).read_text(encoding="utf-8"))["project"]
            if grouped:
                expected = (corpus.linked_media_roundtrip_expected(template) if index == 0
                            else corpus.secondary_archive_roundtrip_expected(template))
            desired.append(expected)
            mass = expected["point_masses"][0]
            snapshots.append({"tracks": [template["session_tracks"][0], {
                **template["session_tracks"][1], "mass": mass["mass"],
                **({"key_frames": [0, 1, 2]} if grouped else {}),
                "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                            for sample in mass["samples"]]}],
                "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                "media": copy.deepcopy(template["session_media"]), "frame_duration_ms": 125.0,
                "axes": copy.deepcopy(template["session_axes"]), "calibration": copy.deepcopy(template["session_calibration"]),
                "time_samples": copy.deepcopy(template["session_time_samples"]),
                "name_references": {"selected_track": mass["name"]}, **corpus.session_units(template, expected)})
        (self.root / "reports").mkdir(exist_ok=True)
        for mutation in [None, "missing", "duplicate", "non-object", "second-mass", "second-pixels", "second-origin",
                         "first-bytes", "second-bytes", "cache-policy"]:
            output = self.root / (case_id + ("-group" if grouped else "") + "-" + (mutation or "valid"))
            output.mkdir()
            calls, homes = [], []
            observed = copy.deepcopy(snapshots)
            if mutation == "second-mass": observed[1]["tracks"][1]["mass"] = 8.0
            elif mutation == "second-pixels": observed[1]["media"]["frames"][0]["rgb_sha256"] = "0" * 64
            elif mutation == "second-origin": observed[1]["calibration"]["keys"][0]["origin"][0] += 1

            def session(command, *, display=None):
                if grouped and Path(command[0]).name == "native-probe":
                    self.assertIsNone(display)
                    if command[1].startswith("--export-"):
                        self.assertEqual(command[1], "--export-archive-group-forward")
                        Path(command[3]).write_bytes(corpus.local_fixture_path(case).read_bytes())
                    else:
                        index = int(Path(command[1]).stem.rsplit("-", 1)[1])
                        Path(command[2]).write_text(json.dumps(desired[index]), encoding="utf-8")
                    return
                self.assertEqual(display, ":77")
                self.assertIn("-Xmx256m", command)
                for template in templates:
                    for dependency in template.get("reference_dependencies", []):
                        self.assertIn(str(self.root / "runtime" / dependency), command[command.index("-cp") + 1])
                args = command[command.index("TrackerSessionReference") + 1:]
                source, destination, stage = Path(args[0]), Path(args[1]), args[2]
                home = Path(next(arg.split("=", 1)[1] for arg in command if arg.startswith("-Duser.home=")))
                self.assertTrue(home.is_dir())
                self.assertTrue(home.is_relative_to(output))
                homes.append(home)
                calls.append((source, stage))
                report = {"reference_version": "6.3.5", "verification_level": "tracker_session_" + stage,
                          "view_preparation": "expand_visit_custom_tables_restore_selection", "loader_callbacks": 2,
                          "cache_policy": "none" if mutation == "cache-policy" else "private_persistent_osp_cache"}
                if stage == "save-tabs":
                    self.assertEqual(source.parent, output if grouped else output / "input")
                    if not grouped:
                        self.assertEqual(list(source.parent.iterdir()), [source])
                    self.assertEqual(source.read_bytes(), corpus.local_fixture_path(case).read_bytes())
                    self.assertEqual([Path(arg).name for arg in args[3:]], ["experiment.trk", "second.trk"])
                    for i, template in enumerate(templates):
                        data = corpus.local_fixture_path(template).read_bytes()
                        self.assertEqual(Path(args[3 + i]).read_bytes(), data)
                        (destination / f"tracker-resaved-{i}.trk").write_bytes(data)
                        directory = destination / "images" if template["session_media"]["class"].endswith(".ImageVideo") else destination
                        directory.mkdir(exist_ok=True)
                        for asset in template["assets"]:
                            payload = corpus.local_fixture_path(asset).read_bytes()
                            (directory / asset["file"]).write_bytes(payload)
                    if mutation in {"first-bytes", "second-bytes"}:
                        damaged_template = templates[0 if mutation == "first-bytes" else 1]
                        directory = destination / "images" if damaged_template["session_media"]["class"].endswith(".ImageVideo") else destination
                        damaged = directory / damaged_template["assets"][0]["file"]
                        damaged.write_bytes(damaged.read_bytes()[::-1])
                    report["tabs"] = [{"member": tab["member"], "loaded": observed[i], "after_save": observed[i]}
                                      for i, tab in enumerate(case["session_archive_tabs"])]
                    if mutation == "missing": report["tabs"].pop()
                    elif mutation == "duplicate": report["tabs"][1]["member"] = "experiment.trk"
                    elif mutation == "non-object": report["tabs"][1] = None
                else:
                    self.assertEqual(stage, "reopen")
                    index = int(source.stem.rsplit("-", 1)[1])
                    self.assertEqual(source, output / f"tracker-resaved-{index}.trk")
                    report["loaded"] = observed[index]
                (destination / f"session-{stage}.json").write_text(json.dumps(report), encoding="utf-8")

            with self.subTest(mutation=mutation), patch.object(corpus, "run", side_effect=session), \
                    (patch.object(corpus, "verify_archive_media_export") if grouped else nullcontext()):
                arguments = {"probe": "native-probe", "mode": "archive-group-forward"} if grouped else {}
                if mutation:
                    with self.assertRaisesRegex(ValueError, "checksum" if mutation.endswith("-bytes") else "."):
                        corpus.check_archive_tabs(case, manifest, self.root, "java", self.root / "classes",
                                                  self.root / "reference.jar", output, ":77", **arguments)
                else:
                    corpus.check_archive_tabs(case, manifest, self.root, "java", self.root / "classes",
                                              self.root / "reference.jar", output, ":77", **arguments)
                    self.assertEqual([stage for _, stage in calls], ["save-tabs", "reopen", "reopen"])
                    self.assertEqual(len(set(homes)), 3)
                    label = (f"{case_id}-archive-group-forward-motrack_archive_tabs_roundtrip" if grouped
                             else f"{case_id}-tracker_archive_tabs_resave")
                    report = json.loads((self.root / "reports" / f"{label}.json").read_text())
                    self.assertEqual([tab["member"] for tab in report["tabs"]], ["experiment.trk", "second.trk"])
                    self.assertTrue(all(set(tab["tracker_session"]["saved_media_inventories"]) == {"save", "reopen"}
                                        for tab in report["tabs"]))
                    self.assertTrue(all(set(tab["tracker_session"]["cache_policy"].values()) == {"private_persistent_osp_cache"}
                                        for tab in report["tabs"]))

    def test_multitab_session_refuses_an_omitted_or_changed_experiment(self):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == "multitab-images")
        original_path = corpus.local_fixture_path
        for mutation in ["missing", "changed"]:
            damaged = self.root / (mutation + ".trz")
            with zipfile.ZipFile(original_path(case)) as source, zipfile.ZipFile(damaged, "w") as output:
                for entry in source.infolist():
                    if entry.filename == "second.trk" and mutation == "missing":
                        continue
                    data = source.read(entry)
                    if entry.filename == "second.trk":
                        self.assertIn(b">3.5<", data)
                        data = data.replace(b">3.5<", b">4.5<")
                    output.writestr(entry, data)
            with self.subTest(mutation=mutation), \
                    patch.object(corpus, "local_fixture_path", side_effect=lambda value: damaged if value["id"] == case["id"] else original_path(value)), \
                    patch.object(corpus, "run") as run, self.assertRaisesRegex(ValueError, "independent"):
                corpus.check_archive_tabs(case, manifest, self.root, "java", self.root / "classes",
                                          self.root / "reference.jar", self.root / "output", ":77")
            run.assert_not_called()

    def test_private_display_waits_for_a_cold_start_and_reports_xvfb_diagnostics(self):
        output = {"text": b"(EE) Fatal server error: Cannot establish any listening sockets"}

        class Server:
            def __init__(self, command, *, pass_fds, stdout, stderr):
                self.command = command
                stderr.write(output["text"])
                stderr.flush()

            def poll(self):
                return None

            def terminate(self):
                pass

            def wait(self, timeout=None):
                return 0

        timeouts = []

        def never_ready(readers, writers, errors, timeout):
            timeouts.append(timeout)
            return [], [], []

        with patch.object(corpus.sys, "platform", "linux"), patch.object(corpus.shutil, "which", return_value="Xvfb"), \
                patch.object(corpus.subprocess, "Popen", Server), patch.object(corpus.select, "select", never_ready):
            with self.assertRaisesRegex(ValueError, r"within 30 s: \(EE\) Fatal server error"):
                with corpus.private_display():
                    self.fail("display must not be yielded")
        self.assertEqual(timeouts, [corpus.XVFB_READY_SECONDS])

        # A server that answers with something other than a display number is reported with its diagnostics.
        with patch.object(corpus.sys, "platform", "linux"), patch.object(corpus.shutil, "which", return_value="Xvfb"), \
                patch.object(corpus.subprocess, "Popen", Server), \
                patch.object(corpus.select, "select", lambda readers, *_: (readers, [], [])), \
                patch.object(corpus.os, "read", return_value=b"not a display\n"):
            with self.assertRaisesRegex(ValueError, r"failed to start: \(EE\) Fatal server error"):
                with corpus.private_display():
                    self.fail("display must not be yielded")

        # The fatal line comes last after a long preamble of warnings; the error keeps that tail.
        output["text"] = b"Warning: Could not resolve keysym XF86Example\n" * 80 + b"(EE) Fatal server error: real cause"
        self.assertGreater(len(output["text"]), corpus.XVFB_LOG_BYTES)
        with patch.object(corpus.sys, "platform", "linux"), patch.object(corpus.shutil, "which", return_value="Xvfb"), \
                patch.object(corpus.subprocess, "Popen", Server), patch.object(corpus.select, "select", never_ready):
            with self.assertRaises(ValueError) as raised:
                with corpus.private_display():
                    self.fail("display must not be yielded")
        message = str(raised.exception)
        self.assertTrue(message.endswith("(EE) Fatal server error: real cause"), message[-200:])
        self.assertLessEqual(len(message), corpus.XVFB_LOG_BYTES + 100)

        # Without diagnostics the message has no dangling separator.
        output["text"] = b""
        with patch.object(corpus.sys, "platform", "linux"), patch.object(corpus.shutil, "which", return_value="Xvfb"), \
                patch.object(corpus.subprocess, "Popen", Server), patch.object(corpus.select, "select", never_ready):
            with self.assertRaisesRegex(ValueError, r"^Private Xvfb display did not become ready within 30 s$"):
                with corpus.private_display():
                    self.fail("display must not be yielded")

    def test_failed_multitab_session_cleans_its_private_home_and_outputs(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [case for case in manifest["cases"] if case["id"] in
                             {"multitab-images", "linked-images", "secondary-images"}]
        calls = []

        def fail_session(command, *, display=None):
            if "TrackerSessionReference" not in command:
                return
            self.assertEqual(display, ":77")
            args = command[command.index("TrackerSessionReference") + 1:]
            calls.append(args)
            self.assertEqual(args[2], "save-tabs")
            home = Path(next(arg.split("=", 1)[1] for arg in command if arg.startswith("-Duser.home=")))
            self.assertTrue(home.is_dir())
            (home / "preferences.xml").write_text("private", encoding="utf-8")
            raise ValueError("multi-tab loader failure")

        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=fail_session):
            with self.assertRaisesRegex(ValueError, "multi-tab loader failure"):
                corpus.check_sessions(manifest, self.root, "java")
        self.assertEqual(len(calls), 1)
        self.assertEqual(list(self.root.glob("session-*")), [])

    def test_archive_oracle_checks_opaque_payloads_metadata_and_edited_xml(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "archived-images")
        fixture = corpus.local_fixture_path(case)
        original, exported, selected = (self.root / name for name in ["original.trz", "edited.trz", "selected.trk"])

        def build(path, edited, mutation=None):
            with zipfile.ZipFile(fixture) as source, zipfile.ZipFile(path, "w") as output:
                output.comment = b"changed" if mutation == "comment" else source.comment
                for entry in source.infolist():
                    info, data = copy.copy(entry), source.read(entry)
                    info.compress_type = zipfile.ZIP_STORED
                    if info.filename == "experiment.trk" and edited:
                        root = ET.fromstring(data)
                        mass = root.find(".//object[@class='org.opensourcephysics.cabrillo.tracker.PointMass']")
                        mass.find("property[@name='mass']").text = "6" if mutation == "xml" else "5"
                        frames = mass.find("property[@name='framedata']")
                        step = ET.Element("property", name="[1]", type="object")
                        obj = ET.SubElement(step, "object", {"class": "org.opensourcephysics.cabrillo.tracker.PointMass$FrameData"})
                        for name, value in [("x", "8"), ("y", "12")]:
                            ET.SubElement(obj, "property", name=name, type="double").text = value
                        frames.insert(1, step)
                        keys = mass.find("property[@name='keyFrames']")
                        ET.SubElement(keys, "property", name="array", type="string").text = "{0,1,2}"
                        data = ET.tostring(root, encoding="utf-8")
                    if info.filename == "support/opaque.bin":
                        if mutation == "payload":
                            data += b"lost"
                        elif mutation == "entry-comment":
                            info.comment = b"lost"
                        elif mutation == "compression":
                            info.compress_type = zipfile.ZIP_DEFLATED
                    output.writestr(info, data)
                if mutation == "extra-member":
                    output.writestr("unexpected.txt", "extra")

        build(original, False)
        build(exported, True)
        corpus.verify_archive_media_export(original, exported, selected)
        self.assertTrue(selected.is_file())
        for mutation in ["comment", "entry-comment", "payload", "compression", "extra-member", "xml"]:
            build(exported, True, mutation)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.verify_archive_media_export(original, exported, selected)

    def test_grouped_archive_oracle_requires_distinct_edits_and_preserves_all_other_bytes(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "multitab-images")
        original, exported, selected = (self.root / name for name in ["original.trz", "edited.trz", "selected.trk"])
        # Normalize both fixture copies to stored entries so Python's independent ZIP writer
        # preserves untouched local records exactly; native tests also use Java-deflated records.
        def build(path, edited=False, mutation=None):
            with zipfile.ZipFile(corpus.local_fixture_path(case)) as source, zipfile.ZipFile(path, "w") as output:
                output.comment = source.comment
                for entry in source.infolist():
                    info, data = copy.copy(entry), source.read(entry)
                    info.compress_type = zipfile.ZIP_STORED
                    if edited and entry.filename in {"experiment.trk", "second.trk"}:
                        with corpus.minidom.parseString(data) as document:
                            if entry.filename == "experiment.trk":
                                corpus.patch_expected_linked_media(document, relocate=False)
                            elif mutation != "second-unedited":
                                corpus.patch_expected_secondary_archive_member(document)
                            if mutation == entry.filename:
                                mass = next(obj for obj in document.getElementsByTagName("object")
                                            if obj.getAttribute("class").endswith(".PointMass"))
                                prop = next(node for node in mass.childNodes if node.nodeType == node.ELEMENT_NODE
                                            and node.getAttribute("name") == "mass")
                                prop.firstChild.data = "999"
                            data = document.toxml(encoding="utf-8")
                    if mutation == "opaque" and entry.filename == "support/opaque.bin":
                        data += b"changed"
                    output.writestr(info, data)
        build(original)
        build(exported, True)
        corpus.verify_archive_media_export(original, exported, selected, member_count=8, grouped=True)
        for mutation in ["experiment.trk", "second.trk", "second-unedited", "opaque"]:
            build(exported, True, mutation)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.verify_archive_media_export(original, exported, selected, member_count=8, grouped=True)
        secondary = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "secondary-images")
        expected = corpus.secondary_archive_roundtrip_expected(secondary)
        self.assertEqual(expected["point_masses"][0]["mass"], 7)
        self.assertEqual(expected["point_masses"][0]["samples"][1],
                         {"frame": 1, "image": [9, 11], "world": [3.5, 2.25]})
        with self.assertRaises(ValueError):
            corpus.secondary_archive_roundtrip_expected(case)

    def test_image_package_oracle_checks_inventory_media_and_all_xml(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "linked-images")
        original = corpus.local_fixture_path(case)
        exported = self.root / "packaged.trz"
        selected = self.root / "selected.trk"
        with corpus.minidom.parse(str(original)) as document:
            corpus.patch_expected_linked_media(document, relocate=False)
            xml = document.toxml()
        for index in range(4):
            xml = xml.replace(f"images/frame{index}.png", f"images/frame-{index:06d}.png")

        def build(mutation=None):
            with zipfile.ZipFile(exported, "w", compression=(zipfile.ZIP_STORED if mutation == "stored"
                                                          else zipfile.ZIP_DEFLATED)) as package:
                data = xml
                if mutation == "xml":
                    data = data.replace('name="xorigin" type="double">2.0', 'name="xorigin" type="double">3.0')
                elif mutation == "path":
                    data = data.replace("images/frame-000000.png", "images/frame-000001.png", 1)
                package.writestr("experiment.trk", data)
                for index, asset in enumerate(case["assets"]):
                    if mutation == "missing" and index == 0:
                        continue
                    payload = corpus.local_fixture_path(asset).read_bytes()
                    if mutation == "pixels" and index == 0:
                        payload = bytes([payload[0] ^ 1]) + payload[1:]
                    package.writestr(f"images/frame-{index:06d}.png", payload)
                if mutation == "extra":
                    package.writestr("unexpected.txt", "unexpected")

        build()
        corpus.verify_media_package_export(original, exported, selected, case)
        self.assertEqual(selected.read_text(encoding="utf-8"), xml)
        for mutation in ["xml", "path", "pixels", "missing", "extra", "stored"]:
            build(mutation)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.verify_media_package_export(original, exported, selected, case)

    def test_movie_package_oracle_checks_complete_media_xml_and_clip_ranges(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "linked-mov")
        self.assertTrue(case["native_movie_package_roundtrip"])
        original = corpus.local_fixture_path(case)
        exported = self.root / "packaged-movie.trz"
        selected = self.root / "selected-movie.trk"
        payload = corpus.local_fixture_path(case["assets"][0]).read_bytes()
        for clip_mode in [None, "stride", "single"]:
            with corpus.minidom.parse(str(original)) as document:
                corpus.patch_expected_linked_media(document, movie=True, relocate=False, clip_mode=clip_mode)
                xml = document.toxml().replace("constant-rate.mov", "media/movie.mov")
            for mutation in [None, "xml", "path", "movie", "missing", "extra", "stored", "crc-xml", "crc-movie", "not-zip"]:
                with self.subTest(clip_mode=clip_mode, mutation=mutation):
                    with zipfile.ZipFile(exported, "w", compression=(zipfile.ZIP_STORED if mutation == "stored"
                                                                  else zipfile.ZIP_DEFLATED)) as package:
                        data = xml
                        if mutation == "xml":
                            data = data.replace('name="xorigin" type="double">2.0', 'name="xorigin" type="double">3.0')
                        elif mutation == "path":
                            data = data.replace("media/movie.mov", "../external.mov")
                        package.writestr("experiment.trk", data)
                        if mutation != "missing":
                            package.writestr("media/movie.mov", payload[:-1] + bytes([payload[-1] ^ 1]) if mutation == "movie" else payload)
                        if mutation == "extra":
                            package.writestr("unexpected.txt", "unexpected")
                    if mutation in {"crc-xml", "crc-movie"}:
                        damaged = bytearray(exported.read_bytes())
                        with zipfile.ZipFile(exported) as package:
                            central = package.start_dir
                            if mutation == "crc-movie":
                                first = package.infolist()[0]
                                central += 46 + len(first.filename.encode("utf-8")) + len(first.extra) + len(first.comment)
                        self.assertEqual(damaged[central:central + 4], b"PK\x01\x02")
                        damaged[central + 16] ^= 1  # Central-directory CRC, leaving decoded bytes unchanged.
                        exported.write_bytes(damaged)
                    elif mutation == "not-zip":
                        exported.write_bytes(b"not a ZIP archive")
                    if mutation:
                        previous = selected.read_bytes()
                        message = "ZIP integrity" if mutation in {"crc-xml", "crc-movie", "not-zip"} else ".*"
                        with self.assertRaisesRegex(ValueError, message):
                            corpus.verify_media_package_export(original, exported, selected, case, clip_mode, movie=True)
                        self.assertEqual(selected.read_bytes(), previous)
                    else:
                        corpus.verify_media_package_export(original, exported, selected, case, clip_mode, movie=True)
                        self.assertEqual(selected.read_text(encoding="utf-8"), xml)

    def test_movie_calibration_xml_oracles_reject_unexpected_geometry_and_units(self):
        for packaged in [False, True]:
            case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == ("linked-mov" if packaged else "linked-movie"))
            original = corpus.local_fixture_path(case)
            exported = self.root / ("calibrated.trz" if packaged else "calibrated.trk")
            selected = self.root / "selected-calibrated.trk"
            with corpus.minidom.parse(str(original)) as document:
                corpus.patch_expected_linked_media(document, movie=True, packaged=packaged)
                frame = next(node for node in document.getElementsByTagName("object")
                             if node.getAttribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem$FrameData")
                for name, value in [("xorigin", "10"), ("yorigin", "18"), ("angle", "90"), ("xscale", "4"), ("yscale", "8")]:
                    prop = next(node for node in frame.getElementsByTagName("property") if node.getAttribute("name") == name)
                    prop.firstChild.data = value
                unit = next(node for node in document.documentElement.childNodes
                            if node.nodeType == node.ELEMENT_NODE and node.getAttribute("name") == "length_unit")
                unit.firstChild.data = "cm"
                xml = document.toxml()
            payload = corpus.local_fixture_path(case["assets"][0]).read_bytes()
            if not packaged:
                media = self.root / "média & movie"
                media.mkdir()
                (media / "constant-rate.avi").write_bytes(payload)
            for mutation in [None, "xorigin", "yorigin", "angle", "xscale", "yscale", "length_unit", "mass"]:
                with self.subTest(packaged=packaged, mutation=mutation):
                    with corpus.minidom.parseString(xml) as document:
                        if mutation:
                            prop = next(node for node in document.getElementsByTagName("property") if node.getAttribute("name") == mutation)
                            prop.firstChild.data = "m" if mutation == "length_unit" else "999"
                        changed = document.toxml()
                    if packaged:
                        with zipfile.ZipFile(exported, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                            archive.writestr("experiment.trk", changed)
                            archive.writestr("media/movie.mov", payload)
                    else:
                        exported.write_text(changed, encoding="utf-8")
                    def verify():
                        if packaged:
                            corpus.verify_media_package_export(original, exported, selected, case, movie=True, calibrated=True)
                        else:
                            corpus.verify_linked_movie_export_xml(original, exported, case, calibrated=True)
                    if mutation:
                        with self.assertRaises(ValueError):
                            verify()
                    else:
                        verify()

    def test_linked_movie_export_oracle_checks_all_xml_and_movie_bytes(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "linked-movie")
        original = corpus.local_fixture_path(case)
        exported = self.root / "movie.trk"
        media = self.root / "média & movie" / "constant-rate.avi"
        media.parent.mkdir()
        movie_bytes = corpus.local_fixture_path(case["assets"][0]).read_bytes()
        media.write_bytes(movie_bytes)
        with corpus.minidom.parse(str(original)) as document:
            corpus.patch_expected_linked_media(document, movie=True)
            xml = document.toxml()
        exported.write_text(xml, encoding="utf-8")
        corpus.verify_linked_movie_export_xml(original, exported, case)
        expected = corpus.linked_media_roundtrip_expected(case)
        self.assertEqual(expected["point_masses"][0]["mass"], 5.0)
        self.assertEqual(expected["point_masses"][0]["samples"][1],
                         {"frame": 1, "image": [8.0, 12.0], "world": [3.0, 2.0]})
        for before, after in [('name="mass" type="double">5', 'name="mass" type="double">7'),
                              ('name="xorigin" type="double">2.0', 'name="xorigin" type="double">3.0'),
                              ('name="delta_t" type="double">125.0', 'name="delta_t" type="double">250.0'),
                              ('média &amp; movie', 'different-folder'),
                              ('name="x" type="double">8', 'name="x" type="double">9')]:
            self.assertIn(before, xml)
            exported.write_text(xml.replace(before, after, 1), encoding="utf-8")
            with self.subTest(change=before), self.assertRaises(ValueError):
                corpus.verify_linked_movie_export_xml(original, exported, case)
        exported.write_text(xml, encoding="utf-8")
        media.write_bytes(movie_bytes[:-1] + bytes([movie_bytes[-1] ^ 1]))
        with self.assertRaises(ValueError):
            corpus.verify_linked_movie_export_xml(original, exported, case)

    def test_movie_clip_oracle_preserves_excluded_marks_timing_and_complete_movie(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "linked-movie")
        original = corpus.local_fixture_path(case)
        media = self.root / "média & movie" / "constant-rate.avi"
        media.parent.mkdir()
        payload = corpus.local_fixture_path(case["assets"][0]).read_bytes()
        media.write_bytes(payload)
        output = self.root / "clip.trk"
        for single in [False, True]:
            mode = "single" if single else "stride"
            expected, clip_case = corpus.linked_media_clip_expected(case, single)
            self.assertEqual([sample["frame"] for sample in expected["point_masses"][0]["samples"]], [0, 1, 2])
            self.assertEqual(expected["clip"]["first_frame"], 2 if single else 1)
            self.assertEqual(clip_case["session_media"]["frame_count"], 4)
            with corpus.minidom.parse(str(original)) as document:
                corpus.patch_expected_linked_media(document, movie=True, clip_mode=mode)
                correct = document.toxml()
            output.write_text(correct, encoding="utf-8")
            corpus.verify_linked_movie_export_xml(original, output, case, mode)
            for name, value in [("startframe", "0"), ("stepsize", "3"), ("stepcount", "4"), ("starttime", "0")]:
                root = ET.fromstring(correct)
                root.find(f"property[@name='videoclip']/object/property[@name='{name}']").text = value
                ET.ElementTree(root).write(output, encoding="utf-8")
                with self.subTest(mode=mode, field=name), self.assertRaises(ValueError):
                    corpus.verify_linked_movie_export_xml(original, output, case, mode)
            output.write_text(correct.replace("{0,1,2}", "{1,2}"), encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_linked_movie_export_xml(original, output, case, mode)
            output.write_text(correct, encoding="utf-8")
            media.write_bytes(payload[::-1])
            with self.assertRaises(ValueError):
                corpus.verify_linked_movie_export_xml(original, output, case, mode)
            media.write_bytes(payload)

    def test_saved_movie_oracle_checks_reference_and_all_bytes_after_clipped_resave(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "linked-movie")
        asset = case["assets"][0]
        movie = self.root / "média & movie" / "constant-rate.avi"
        movie.parent.mkdir()
        payload = corpus.local_fixture_path(asset).read_bytes()
        movie.write_bytes(payload)
        saved = self.root / "tracker-resaved.trk"
        with corpus.minidom.parse(str(corpus.local_fixture_path(case))) as document:
            corpus.patch_expected_linked_media(document, movie=True, clip_mode="single")
            valid = document.toxml()
        for reference in ["média & movie/constant-rate.avi", str(movie), movie.as_uri()]:
            root = ET.fromstring(valid)
            root.find(".//property[@name='path']").text = reference
            ET.ElementTree(root).write(saved, encoding="utf-8")
            observed = corpus.verify_saved_movie_asset(saved, asset)
            self.assertEqual(observed, {"reference": reference, "bytes": asset["bytes"], "sha256": asset["sha256"]})
        for mutation in ["network", "network-file", "network-share", "archive", "missing", "duplicate", "type", "class", "nested"]:
            root = ET.fromstring(valid)
            video = root.find("property[@name='videoclip']/object/property[@name='video']/object")
            path = video.find("property[@name='path']")
            if mutation == "network": path.text = "https://example.invalid/movie.avi"
            elif mutation == "network-file": path.text = "file://example.invalid/movie.avi"
            elif mutation == "network-share": path.text = "//example.invalid/movie.avi"
            elif mutation == "archive": path.text = "archive.trz!/movie.avi"
            elif mutation == "missing": video.remove(path)
            elif mutation == "duplicate": video.append(copy.deepcopy(path))
            elif mutation == "type": path.set("type", "object")
            elif mutation == "class": video.set("class", "UnknownVideo")
            else: ET.SubElement(path, "object")
            ET.ElementTree(root).write(saved, encoding="utf-8")
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.verify_saved_movie_asset(saved, asset)
        saved.write_text(valid, encoding="utf-8")
        for corrupted in [payload[:-1], payload[::-1], payload + b"extra"]:
            movie.write_bytes(corrupted)
            with self.assertRaisesRegex(ValueError, "checksum"):
                corpus.verify_saved_movie_asset(saved, asset)
        movie.unlink()
        with self.assertRaises(OSError):
            corpus.verify_saved_movie_asset(saved, asset)

    def test_saved_archived_movie_oracle_rejects_changed_missing_and_ambiguous_media(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "linked-movie")
        asset = case["assets"][0]
        payload = corpus.local_fixture_path(asset).read_bytes()
        archive = self.root / "movie.trz"
        saved = self.root / "saved.trk"
        root = ET.parse(corpus.local_fixture_path(case)).getroot()
        path = root.find(".//property[@name='path']")
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr("constant-rate.avi", payload)
        for reference in ["movie.trz!/constant-rate.avi", str(archive) + "!/constant-rate.avi",
                          "jar:" + archive.as_uri() + "!/constant-rate.avi"]:
            path.text = reference
            ET.ElementTree(root).write(saved, encoding="utf-8")
            observed = corpus.verify_saved_movie_asset(saved, asset, allow_archive=True)
            self.assertEqual(observed["sha256"], asset["sha256"])
            with self.assertRaises(ValueError):
                corpus.verify_saved_movie_asset(saved, asset)
        for reference in ["movie.trz!/../constant-rate.avi", "movie.trz!//constant-rate.avi", "movie.trz!/",
                          "movie.trz!/nested.trz!/constant-rate.avi", "jar:https://example.invalid/movie.trz!/constant-rate.avi",
                          "file://example.invalid/movie.trz!/constant-rate.avi"]:
            path.text = reference
            ET.ElementTree(root).write(saved, encoding="utf-8")
            with self.subTest(reference=reference), self.assertRaises(ValueError):
                corpus.verify_saved_movie_asset(saved, asset, allow_archive=True)
        path.text = "movie.trz!/constant-rate.avi"
        ET.ElementTree(root).write(saved, encoding="utf-8")
        for mutation in ["changed", "size", "missing", "duplicate", "oversized"]:
            with warnings.catch_warnings(), zipfile.ZipFile(archive, "w") as output:
                warnings.simplefilter("ignore", UserWarning)
                if mutation != "missing":
                    output.writestr("constant-rate.avi", payload[::-1] if mutation == "changed" else
                                    payload + b"extra" if mutation == "size" else payload)
                if mutation == "duplicate": output.writestr("constant-rate.avi", payload)
                if mutation == "oversized": output.writestr("padding.bin", b"x" * (1024 * 1024))
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.verify_saved_movie_asset(saved, asset, allow_archive=True)

    def test_movie_archive_modes_all_use_the_independent_multitab_checker(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [c for c in manifest["cases"] if c["id"] in {"archived-movie", "linked-mov", "secondary-images"}]
        for case in manifest["cases"]:
            if case["id"] != "archived-movie":
                for key in list(case):
                    if key.startswith("native_"):
                        del case[key]
        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run"), \
                patch.object(corpus, "check_archive_tabs") as check:
            corpus.check_sessions(manifest, self.root, "java", "native-probe")
        self.assertEqual([call.args[-1] for call in check.call_args_list],
                         ["archive-group-forward", "archive-group-reverse",
                          "archive-movie", "archive-movie-clip", "archive-movie-clip-single", "paired-archive-movie",
                          "archive-movie-calibration", "paired-archive-movie-calibration"])
        self.assertTrue(all(call.args[0]["id"] == "archived-movie" for call in check.call_args_list))

    def test_all_keyed_movie_archive_variants_use_the_multitab_checker(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [c for c in manifest["cases"] if c.get("native_keyed_movie_archive_roundtrip")]
        self.assertEqual([c["movie_calibration_flags"] for c in manifest["cases"]], list(range(7)))
        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run"), \
                patch.object(corpus, "check_archive_tabs") as check:
            corpus.check_sessions(manifest, self.root, "java", "native-probe")
        expected = [(f"archived-keyed-movie-{flags}", mode) for flags in range(7)
                    for mode in ["archive-movie-keyed-calibration", "paired-archive-movie-keyed-calibration"]]
        self.assertEqual([(call.args[0]["id"], call.args[-1]) for call in check.call_args_list], expected)

    def test_keyed_movie_coordinates_match_independent_boundary_values(self):
        worlds = [
            [[1, 1], [-0.5, 1], [-1, 0], [-1, 0]],
            [[-1, -0.5], [-0.5, 1], [-0.25, -0.125], [-0.25, -0.125]],
            [[-1, 1], [-0.5, 1], [0, -1], [0, -1]],
            [[0.5, -1], [-0.5, 1], [0.125, -0.25], [0.125, -0.25]],
            [[0.25, 0.25], [-0.125, 0.25], [-1, 0], [-1, 0]],
            [[-0.25, -0.125], [-0.125, 0.25], [-0.25, -0.125], [-0.25, -0.125]],
            [[-0.25, 0.25], [-0.125, 0.25], [0, -1], [0, -1]]]
        manifest = corpus.load_manifest()
        for flags, desired in enumerate(worlds):
            case = next(c for c in manifest["cases"] if c["id"] == f"keyed-movie-{flags}")
            expected, edited = corpus.keyed_movie_roundtrip_expected(case)
            with self.subTest(flags=flags):
                self.assertEqual([s["world"] for s in expected["point_masses"][0]["samples"]], desired)
                self.assertEqual([k["frame"] for k in edited["session_calibration"]["keys"]], [0, 2] if flags == 3 else [0, 1, 2])
                self.assertEqual(expected["length_unit"], "cm")

    def test_keyed_movie_xml_rejects_wrong_keys_flags_padding_and_untouched_fields(self):
        manifest = corpus.load_manifest()
        media = self.root / "média & movie"
        media.mkdir()
        for flags in range(7):
            case = next(c for c in manifest["cases"] if c["id"] == f"keyed-movie-{flags}")
            original = corpus.local_fixture_path(case)
            (media / "constant-rate.mov").write_bytes(corpus.local_fixture_path(case["assets"][0]).read_bytes())
            with corpus.minidom.parse(str(original)) as document:
                corpus.patch_expected_linked_media(document, movie=True, keyed_case=case)
                xml = document.toxml()
            exported = self.root / "mutated.trk"
            for mutation in ["key-frame", "fixedorigin", "padding", "length_unit", "starttime", "mass"]:
                with self.subTest(flags=flags, mutation=mutation), corpus.minidom.parseString(xml) as document:
                    if mutation == "key-frame":
                        frame = next(n for n in document.getElementsByTagName("object")
                                     if n.getAttribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem$FrameData"
                                     and n.parentNode.getAttribute("name") == "[2]")
                        frame.parentNode.setAttribute("name", "[3]")
                    else:
                        name = "[3]" if mutation == "padding" else mutation
                        prop = next(n for n in document.getElementsByTagName("property") if n.getAttribute("name") == name)
                        prop.firstChild.data = ("false" if flags & 1 else "true") if mutation == "fixedorigin" else "wrong"
                    exported.write_text(document.toxml(), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        corpus.verify_linked_movie_export_xml(original, exported, case, keyed=True)

    def test_linked_image_export_oracle_checks_edited_geometry_all_xml_and_relocated_assets(self):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == "linked-images")
        original = corpus.local_fixture_path(case)
        expected = corpus.linked_media_roundtrip_expected(case)
        self.assertEqual(expected["point_masses"][0]["mass"], 5.0)
        self.assertEqual(expected["point_masses"][0]["samples"][1],
                         {"frame": 1, "image": [8.0, 12.0], "world": [3.0, 2.0]})
        self.assertEqual(expected["clip"]["start_ms"], -250.0)
        with self.assertRaises(ValueError):
            corpus.linked_media_roundtrip_expected({**case, "id": "unrelated"})
        output = self.root / "exported.trk"
        media = self.root / "media é & frames"
        media.mkdir()
        for asset in case["assets"]:
            (media / asset["file"]).write_bytes(corpus.local_fixture_path(asset).read_bytes())
        with corpus.minidom.parse(str(original)) as document:
            corpus.patch_expected_linked_media(document)
            correct = document.toxml()
        output.write_text(correct, encoding="utf-8")
        corpus.verify_linked_image_export_xml(original, output, case)
        for old, new in [('name="mass" type="double">5', 'name="mass" type="double">6'),
                         ('name="xorigin" type="double">2.0', 'name="xorigin" type="double">3.0'),
                         ('name="delta_t" type="double">125.0', 'name="delta_t" type="double">250.0'),
                         ('media é &amp; frames/frame0.png', 'media é &amp; frames/frame1.png'),
                         ('{0,1,2}', '{0,2}')]:
            self.assertIn(old, correct)
            output.write_text(correct.replace(old, new, 1), encoding="utf-8")
            with self.subTest(old=old), self.assertRaisesRegex(ValueError, "beyond"):
                corpus.verify_linked_image_export_xml(original, output, case)
        output.write_text(correct, encoding="utf-8")
        frame = media / case["assets"][0]["file"]
        data = frame.read_bytes()
        frame.write_bytes(bytes([data[0] ^ 1]) + data[1:])
        with self.assertRaisesRegex(ValueError, "Checksum"):
            corpus.verify_linked_image_export_xml(original, output, case)
        frame.unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            corpus.verify_linked_image_export_xml(original, output, case)

    def test_image_clip_oracle_checks_range_all_marks_and_excluded_image_bytes(self):
        manifest = corpus.load_manifest()
        original_case = next(c for c in manifest["cases"] if c["id"] == "linked-images")
        original = corpus.local_fixture_path(original_case)
        media = self.root / "media é & frames"
        media.mkdir()
        for asset in original_case["assets"]:
            (media / asset["file"]).write_bytes(corpus.local_fixture_path(asset).read_bytes())
        output = self.root / "clip.trk"
        for single in [False, True]:
            mode = "single" if single else "stride"
            expected, case = corpus.linked_media_clip_expected(original_case, single)
            self.assertEqual(expected["clip"], {"first_frame": 2 if single else 1, "step_size": 1 if single else 2,
                                               "step_count": 1 if single else 2, "start_ms": -250.0})
            self.assertEqual([sample["frame"] for sample in expected["point_masses"][0]["samples"]], [0, 1, 2])
            self.assertEqual(case["session_media"]["frame_count"], 4)
            self.assertEqual([frame["frame"] for frame in case["session_media"]["frames"]], [2] if single else [1, 2, 3])
            self.assertEqual(case["session_time_samples"], [{"frame": 2, "time_ms": -250.0}, {"frame": 2, "time_ms": -250.0}]
                             if single else [{"frame": 1, "time_ms": -250.0}, {"frame": 3, "time_ms": 0.0}])
            with corpus.minidom.parse(str(original)) as document:
                corpus.patch_expected_linked_media(document, clip_mode=mode)
                correct = document.toxml()
            output.write_text(correct, encoding="utf-8")
            corpus.verify_linked_image_export_xml(original, output, case, mode)
            for name, value in [("startframe", "0"), ("stepsize", "3"), ("stepcount", "4"), ("starttime", "0")]:
                with corpus.minidom.parseString(correct) as document:
                    clip = next(node for node in document.getElementsByTagName("object")
                                if node.getAttribute("class") == "org.opensourcephysics.media.core.VideoClip")
                    field = next(node for node in clip.childNodes if node.nodeType == node.ELEMENT_NODE
                                 and node.getAttribute("name") == name)
                    field.firstChild.data = value
                    output.write_text(document.toxml(), encoding="utf-8")
                with self.subTest(mode=mode, field=name), self.assertRaisesRegex(ValueError, "beyond"):
                    corpus.verify_linked_image_export_xml(original, output, case, mode)
            output.write_text(correct, encoding="utf-8")
            excluded = media / "frame0.png"
            image = excluded.read_bytes()
            excluded.write_bytes(image[:-1] + bytes([image[-1] ^ 1]))
            with self.assertRaisesRegex(ValueError, "Checksum"):
                corpus.verify_linked_image_export_xml(original, output, case, mode)
            excluded.write_bytes(image)
        self.assertEqual(original_case["session_media"]["frames"][0]["frame"], 0)
        with self.assertRaisesRegex(ValueError, "pinned"):
            corpus.linked_media_clip_expected({**original_case, "id": "unrelated"})
        with corpus.minidom.parse(str(original)) as document, self.assertRaisesRegex(ValueError, "Unknown"):
            corpus.patch_expected_linked_media(document, clip_mode="unreviewed")

    def saved_image_inventory_fixture(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "linked-images")
        media = self.root / "media é & frames"
        media.mkdir()
        for asset in case["assets"]:
            (media / asset["file"]).write_bytes(corpus.local_fixture_path(asset).read_bytes())
        output = self.root / "tracker-resaved.trk"
        with corpus.minidom.parse(str(corpus.local_fixture_path(case))) as document:
            corpus.patch_expected_linked_media(document, clip_mode="single")
            output.write_text(document.toxml(), encoding="utf-8")
        return case["assets"], output, media

    def test_saved_image_inventory_rejects_changes_to_excluded_pixels_and_references(self):
        assets, saved, media = self.saved_image_inventory_fixture()
        valid = saved.read_bytes()
        observed = corpus.verify_saved_image_inventory(saved, assets)
        self.assertEqual([item["frame"] for item in observed], [0, 1, 2, 3])
        self.assertEqual([item["sha256"] for item in observed], [asset["sha256"] for asset in assets])
        excluded = media / "frame0.png"
        original = excluded.read_bytes()
        excluded.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        with self.assertRaisesRegex(ValueError, "checksum.*frame 0"):
            corpus.verify_saved_image_inventory(saved, assets)
        excluded.write_bytes(original)
        for mutation in ["redirect", "missing", "reorder", "duplicate-index", "network", "network-file", "network-share", "wrong-array"]:
            root = ET.fromstring(valid)
            paths = root.find(".//property[@name='paths']")
            if mutation == "redirect":
                paths[0].text = paths[1].text
            elif mutation == "missing":
                paths.remove(paths[0])
            elif mutation == "reorder":
                paths[:] = [paths[1], paths[0], *list(paths)[2:]]
            elif mutation == "duplicate-index":
                paths[0].set("name", "[1]")
            elif mutation == "network":
                paths[0].text = "https://example.invalid/frame0.png"
            elif mutation == "network-file":
                paths[0].text = "file://example.invalid/share/frame0.png"
            elif mutation == "network-share":
                paths[0].text = "//example.invalid/share/frame0.png"
            else:
                paths.set("class", "wrong")
            ET.ElementTree(root).write(saved, encoding="utf-8")
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.verify_saved_image_inventory(saved, assets)
        saved.write_bytes(valid)
        excluded.unlink()
        with self.assertRaises(OSError):
            corpus.verify_saved_image_inventory(saved, assets)

    def test_saved_image_inventory_resolves_local_and_archive_references_after_resaving(self):
        assets, saved, media = self.saved_image_inventory_fixture()
        valid = saved.read_bytes()
        package = self.root / "archive é & images.trz"
        with zipfile.ZipFile(package, "w") as archive:
            for asset in assets:
                archive.writestr("images/" + asset["file"], (media / asset["file"]).read_bytes())
        for kind in ["relative", "absolute", "file-url", "archive-relative", "archive-file-url"]:
            root = ET.fromstring(valid)
            paths = root.find(".//property[@name='paths']")
            for entry, asset in zip(paths, assets, strict=True):
                if kind == "absolute":
                    entry.text = str(media / asset["file"])
                elif kind == "file-url":
                    entry.text = (media / asset["file"]).as_uri()
                elif kind.startswith("archive"):
                    entry.text = (package.name if kind == "archive-relative" else "jar:" + package.as_uri()) + "!/images/" + asset["file"]
            ET.ElementTree(root).write(saved, encoding="utf-8")
            with self.subTest(kind=kind):
                actual = corpus.verify_saved_image_inventory(saved, assets)
                self.assertEqual([item["sha256"] for item in actual], [asset["sha256"] for asset in assets])
        # Frame zero is outside the single-frame clip. A valid archive with changed bytes must fail.
        with zipfile.ZipFile(package, "w") as archive:
            for index, asset in enumerate(assets):
                payload = (media / asset["file"]).read_bytes()
                archive.writestr("images/" + asset["file"], payload[::-1] if index == 0 else payload)
        with self.assertRaisesRegex(ValueError, "checksum.*frame 0"):
            corpus.verify_saved_image_inventory(saved, assets)

    def test_media_clip_session_checks_strided_and_single_frame_bounds(self):
        for case_id in ["linked-images", "linked-movie"]:
            for mode in ["stride", "single"]:
                with self.subTest(case_id=case_id, mode=mode):
                    self.check_media_session_oracle(case_id, mode)

    def test_media_session_requires_independent_pixels_order_and_timing(self):
        for case_id in ["linked-images", "linked-movie"]:
            with self.subTest(case=case_id):
                self.check_media_session_oracle(case_id)

    def test_movie_calibration_session_requires_independent_geometry_and_units(self):
        for case_id in ["linked-movie", "linked-mov", "archived-movie"]:
            with self.subTest(case=case_id):
                self.check_media_session_oracle(case_id, calibrated=True)

    def test_keyed_movie_manual_keys_distinguish_source_xml_from_loaded_tracker_state(self):
        cases = [c for c in corpus.load_manifest()["cases"] if "movie_calibration_flags" in c]
        self.assertEqual(len(cases), 14)
        for case in cases:
            with self.subTest(case=case["id"]):
                self.assertEqual(case["session_key_frames"], [[0, 1, 2, 3]])
                if case.get("session_archive"):
                    with zipfile.ZipFile(corpus.local_fixture_path(case)) as archive:
                        xml = archive.read(case["project_member"])
                else:
                    xml = corpus.local_fixture_path(case).read_bytes()
                mass = next(node for node in ET.fromstring(xml).iter("object")
                            if node.get("class") == "org.opensourcephysics.cabrillo.tracker.PointMass")
                raw_keys = mass.find("property[@name='keyFrames']")
                self.assertIsNotNone(raw_keys)
                self.assertEqual(list(raw_keys), [])

    def test_media_vector_sessions_reject_consistent_geometry_physics_and_identity_loss(self):
        manifest = corpus.load_manifest()
        for source in [case for case in manifest["cases"] if case.get("native_media_vector_roundtrip")]:
            kind = source["media_vector_kind"]
            for scenario in corpus.media_vector_scenarios(source):
                expected, case = corpus.media_vector_roundtrip_expected(source, scenario)
                tracks = [copy.deepcopy(case["session_tracks"][0])]
                for vector, data in zip(expected["vectors"], case["session_vector_data"], strict=True):
                    tracks.append({"class": "org.opensourcephysics.cabrillo.tracker.Vector", "name": vector["name"],
                                   "vector_samples": vector["samples"], "calibrated_vector_samples": data["samples"],
                                   "appearance": case.get("session_track_appearances", {}).get(vector["name"], {})})
                before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                          "media": case["session_media"], "frame_duration_ms": 125.0, "axes": case["session_axes"],
                          "calibration": case["session_calibration"], "time_samples": case["session_time_samples"],
                          "name_references": {"selected_track": case["session_selected_track"]}, **corpus.session_units(case, expected)}
                valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                         "view_preparation": "expand_visit_custom_tables_restore_selection",
                         **{stage: copy.deepcopy(before) for stage in ["before", "after_save", "after"]}}
                corpus.validate_session(case, valid, manifest, expected)
                mutations = [lambda row: row["name_references"].update(selected_track="wrong target"),
                             lambda row: row["media"].update(frame_count=3),
                             lambda row: row["media"]["frames"][0].update(rgb_sha256="0" * 64),
                             lambda row: row["media"]["frames"][1].update(time_ms=999),
                             lambda row: row["tracks"].append(copy.deepcopy(row["tracks"][0])),
                             lambda row: row["clip"].update(step_count=3),
                             lambda row: row["calibration"]["keys"][-1].update(scale=[999, 999]),
                             lambda row: row["calibration"]["keys"][-1].update(origin=[999, 999]),
                             lambda row: row["calibration"].update(fixed_origin=not row["calibration"]["fixed_origin"]),
                             lambda row: row["time_samples"][0].update(time_ms=999)]
                if expected["vectors"]:
                    mutations.append(lambda row: row["tracks"][1].update(name="wrong identity"))
                if scenario not in {"empty", "delete-all"}:
                    mutations.extend([lambda row: row["tracks"][1]["vector_samples"][0].update(tail=[999, 999]),
                                      lambda row: row["tracks"][1]["vector_samples"][0].update(components=[999, 999]),
                                      lambda row: next(track["calibrated_vector_samples"] for track in row["tracks"]
                                                       if track.get("calibrated_vector_samples"))[0].update(magnitude=999)])
                if case.get("session_track_appearances"):
                    mutations.append(lambda row: row["tracks"][-1].update(appearance={}))
                for i, mutate in enumerate(mutations):
                    for stages in [["after"], ["before", "after_save", "after"]]:
                        with self.subTest(kind=kind, scenario=scenario, mutation=i, stages=stages):
                            bad = copy.deepcopy(valid)
                            for stage in stages:
                                mutate(bad[stage])
                            with self.assertRaises(ValueError):
                                corpus.validate_session(case, bad, manifest, expected)

    def test_media_vector_archive_modes_visit_both_experiments(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [c for c in manifest["cases"] if c.get("native_media_vector_archive_roundtrip")]
        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run"), \
                patch.object(corpus, "check_archive_tabs") as check:
            corpus.check_sessions(manifest, self.root, "java", "native-probe")
        expected = [(f"archived-vector-{kind}", f"media-vectors-{kind}-{route}-{scenario}") for kind in ["movie", "images"]
                    for route in ["archive", "paired"] for scenario in corpus.MEDIA_VECTOR_SCENARIOS]
        expected += [(f"archived-keyed-vector-{kind}-{flags}", f"media-vectors-{kind}-{route}-keyed-calibration")
                     for kind in ["images", "movie"] for flags in range(7) for route in ["archive", "paired"]]
        self.assertEqual([(call.args[0]["id"], call.args[-1]) for call in check.call_args_list], expected)

    def test_keyed_vectors_match_independent_components_and_reject_invalid_fixture_selection(self):
        components = [[[2, -1], [-0.25, 0.25], [0, -1.5]], [[2, -1], [-0.25, 0.25], [0, -1.5]],
                      [[1, 2], [-0.25, -0.25], [0, -1.5]], [[1, 2], [-0.25, -0.25], [0, -1.5]],
                      [[0.5, -0.25], [-0.25, 0.25], [0, -0.375]], [[0.5, -0.25], [-0.25, 0.25], [0, -0.375]],
                      [[0.25, 0.5], [-0.25, -0.25], [0, -0.375]]]
        cases = [case for case in corpus.load_manifest()["cases"] if "vector_calibration_flags" in case]
        self.assertEqual(len(cases), 28)
        for case in cases:
            # TrackerSessionReference.snapshot samples experiment time at the two clip endpoints.
            # Intermediate source timestamps are independently checked in the complete media inventory.
            self.assertEqual(case["session_time_samples"], [{"frame": 0, "time_ms": 0.0}, {"frame": 3, "time_ms": 375.0}])
            self.assertEqual([row["time_ms"] for row in case["session_media"]["frames"]], [0.0, 125.0, 250.0, 375.0])
            expected, edited = corpus.media_vector_roundtrip_expected(case, "keyed-calibration")
            samples = edited["session_vector_data"][0]["samples"] + edited["session_vector_data"][1]["samples"]
            self.assertEqual([sample["components"] for sample in samples], components[case["vector_calibration_flags"]] + [[0, 0]])
            self.assertEqual([sample["frame"] for sample in samples], [0, 3, 1, 3])
            self.assertEqual([key["frame"] for key in edited["session_calibration"]["keys"]],
                             [0, 2] if case["vector_calibration_flags"] == 3 else [0, 1, 2])
            self.assertEqual(expected["vectors"][0]["samples"][1]["tail"], [32, 0])
            for changes in [{"vector_calibration_flags": flag} for flag in [True, None, -1, 7]] + [
                    {"id": "wrong"}, {"media_vector_kind": "unknown"}]:
                with self.subTest(case=case["id"], changes=changes), self.assertRaises(ValueError):
                    corpus.keyed_vector_calibration({**case, **changes})

    def test_negative_time_vector_defect_is_retained_and_never_accepted(self):
        manifest = corpus.load_manifest()
        for kind in ["images", "movie"]:
            case = next(c for c in manifest["cases"] if c["id"] == "vector-" + kind + "-negative-time")
            expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
            self.assertEqual(expected["clip"]["start_ms"], -250.0)
            self.assertFalse(any(key.startswith("native_") for key in case))
            corpus.verify_file(corpus.local_fixture_path(case), case)
            tracks = [copy.deepcopy(case["session_tracks"][0])]
            for vector, data in zip(expected["vectors"], case["session_vector_data"], strict=True):
                tracks.append({"class": "org.opensourcephysics.cabrillo.tracker.Vector", "name": vector["name"],
                               "vector_samples": copy.deepcopy(vector["samples"]),
                               "calibrated_vector_samples": copy.deepcopy(data["samples"])})
            before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      "media": case["session_media"], "frame_duration_ms": 125.0, "axes": case["session_axes"],
                      "calibration": case["session_calibration"], "time_samples": case["session_time_samples"],
                      "name_references": {"selected_track": case["session_selected_track"]}, **corpus.session_units(case, expected)}
            valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                     "view_preparation": "expand_visit_custom_tables_restore_selection",
                     **{stage: copy.deepcopy(before) for stage in ["before", "after_save", "after"]}}
            corpus.validate_session(case, valid, manifest)
            defect = copy.deepcopy(valid)
            for stage in ["before", "after_save", "after"]:
                for index in [1, 2]:
                    del defect[stage]["tracks"][index]["calibrated_vector_samples"][0]
            with self.assertRaisesRegex(ValueError, "calibrated_vectors"):
                corpus.validate_session(case, defect, manifest)
            result = corpus.validate_known_session_issue(case, defect, manifest, corpus.local_fixture_path(case))
            self.assertFalse(result["compatibility_accepted"])
            self.assertEqual(result["expected_vector_data"], case["session_vector_data"])
            self.assertEqual(result["verification_level"], "tracker_session_known_issue")
            # A fixed Tracker, additional missing rows, wrong raw data or wrong times must fail
            # this narrow diagnosis, including identical corruption at every snapshot stage.
            mutations = [lambda row: row["tracks"][1].update(calibrated_vector_samples=case["session_vector_data"][0]["samples"]),
                         lambda row: row["tracks"][1].update(calibrated_vector_samples=[]),
                         lambda row: row["tracks"][2]["calibrated_vector_samples"][0].update(frame=1),
                         lambda row: row["tracks"][1]["calibrated_vector_samples"][0].update(components=[0.0, 0.0]),
                         lambda row: row["tracks"][1]["vector_samples"].pop(0),
                         lambda row: row["tracks"][1]["vector_samples"][0].update(tail=[0.0, 0.0]),
                         lambda row: row["time_samples"][0].update(time_ms=0.0),
                         lambda row: row["clip"].update(start_ms=0.0),
                         lambda row: row["media"].update(frame_count=3)]
            for index, mutate in enumerate(mutations):
                for stages in [["after"], ["before", "after_save", "after"]]:
                    with self.subTest(kind=kind, mutation=index, stages=stages):
                        invalid = copy.deepcopy(defect)
                        for stage in stages:
                            mutate(invalid[stage])
                        with self.assertRaises(ValueError):
                            corpus.validate_known_session_issue(case, invalid, manifest, corpus.local_fixture_path(case))
            for invalid_case in [{**case, "id": "vector-" + kind}, {**case, "session_known_issue": "other"}]:
                with self.assertRaises(ValueError):
                    corpus.validate_known_session_issue(invalid_case, defect, manifest, corpus.local_fixture_path(case))
            with self.assertRaises(ValueError):
                corpus.validate_known_session_issue(case, defect, {**manifest, "reference": {"version": "6.3.6"}}, corpus.local_fixture_path(case))

    def test_media_vector_xml_oracle_rejects_unrequested_edits(self):
        for case in [c for c in corpus.load_manifest()["cases"] if c.get("native_media_vector_roundtrip")]:
            kind = case["media_vector_kind"]
            original = corpus.local_fixture_path(case)
            directory = self.root / case["id"]
            media = directory / "media é & vectors"
            media.mkdir(parents=True)
            for asset in case["assets"]:
                (media / asset["file"]).write_bytes(corpus.local_fixture_path(asset).read_bytes())
            output = directory / "edited.trk"
            for scenario in corpus.media_vector_scenarios(case):
                with corpus.minidom.parse(str(original)) as document:
                    corpus.patch_expected_linked_media(document, movie=kind == "movie", vector_scenario=scenario, vector_case=case)
                    valid = document.toxml()
                output.write_text(valid, encoding="utf-8")
                corpus.verify_linked_vector_export_xml(original, output, case, scenario)
                step_count = 2 if scenario == "geometry" else 4
                for before, after in [('vector A and vector B are words', 'renamed words'),
                                      ('<property name="mass_unit" type="string">kg', '<property name="mass_unit" type="string">g'),
                                      (f'<property name="stepcount" type="int">{step_count}', '<property name="stepcount" type="int">3')]:
                    with self.subTest(kind=kind, scenario=scenario, mutation=before):
                        self.assertIn(before, valid)
                        output.write_text(valid.replace(before, after), encoding="utf-8")
                        with self.assertRaises(ValueError):
                            corpus.verify_linked_vector_export_xml(original, output, case, scenario)
                if scenario == "keyed-calibration":
                    for mutation in ["fixedorigin", "fixedangle", "fixedscale", "xorigin", "yscale", "padding"]:
                        with self.subTest(case=case["id"], mutation=mutation), corpus.minidom.parseString(valid) as document:
                            coords = next(n for n in document.getElementsByTagName("object")
                                          if n.getAttribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem")
                            name = "[3]" if mutation == "padding" else mutation
                            prop = next(n for n in coords.getElementsByTagName("property") if n.getAttribute("name") == name)
                            prop.firstChild.data = "wrong"
                            output.write_text(document.toxml(), encoding="utf-8")
                            with self.assertRaises(ValueError):
                                corpus.verify_linked_vector_export_xml(original, output, case, scenario)

    def test_movie_authoring_sessions_reject_consistent_data_identity_and_media_loss(self):
        manifest = corpus.load_manifest()
        source = next(case for case in manifest["cases"] if case["id"] == "movie-tracks")
        for scenario in corpus.MOVIE_TRACK_SCENARIOS:
            expected, case = corpus.movie_tracks_roundtrip_expected(source, scenario)
            tracks = [copy.deepcopy(case["session_tracks"][0])]
            for mass, keys in zip(expected["point_masses"], case["session_key_frames"], strict=True):
                tracks.append({"class": "org.opensourcephysics.cabrillo.tracker.PointMass", "name": mass["name"],
                               "mass": mass["mass"], "key_frames": keys,
                               "appearance": case.get("session_track_appearances", {}).get(mass["name"], {}),
                               "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                                           for sample in mass["samples"]]})
            before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      "media": case["session_media"], "frame_duration_ms": 125.0, "axes": case["session_axes"],
                      "calibration": case["session_calibration"], "time_samples": case["session_time_samples"],
                      "name_references": {"selected_track": case["session_selected_track"]}, **corpus.session_units(case, expected)}
            valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                     "view_preparation": "expand_visit_custom_tables_restore_selection",
                     **{stage: copy.deepcopy(before) for stage in ["before", "after_save", "after"]}}
            corpus.validate_session(case, valid, manifest, expected)
            mutations = [lambda row: row["name_references"].update(selected_track="wrong target"),
                         lambda row: row["media"].update(frame_count=3),
                         lambda row: row["media"]["frames"][0].update(rgb_sha256="0" * 64),
                         lambda row: row["tracks"].append(copy.deepcopy(row["tracks"][0])),
                         lambda row: row["clip"].update(step_count=3)]
            if expected["point_masses"]:
                mutations.extend([lambda row: row["tracks"][1].update(mass=99),
                                  lambda row: row["tracks"][1].update(name="other identity"),
                                  lambda row: row["tracks"][1].update(key_frames=[99]),
                                  lambda row: row["tracks"][1].update(samples=[])])
            if case.get("session_track_appearances"):
                mutations.append(lambda row: row["tracks"][-1].update(appearance={}))
            for i, mutate in enumerate(mutations):
                for stages in [["after"], ["before", "after_save", "after"]]:
                    with self.subTest(scenario=scenario, mutation=i, stages=stages):
                        bad = copy.deepcopy(valid)
                        for stage in stages:
                            mutate(bad[stage])
                        with self.assertRaises(ValueError):
                            corpus.validate_session(case, bad, manifest, expected)

    def test_movie_authoring_archive_modes_visit_both_experiments(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [c for c in manifest["cases"] if c["id"] == "archived-movie-tracks"]
        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run"), \
                patch.object(corpus, "check_archive_tabs") as check:
            corpus.check_sessions(manifest, self.root, "java", "native-probe")
        self.assertEqual([call.args[-1] for call in check.call_args_list],
                         [prefix + "-movie-tracks-" + scenario for prefix in ["archive", "paired-archive"]
                          for scenario in corpus.MOVIE_TRACK_SCENARIOS])

    def test_movie_authoring_xml_oracle_rejects_unrequested_mutations(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "movie-tracks")
        original = corpus.local_fixture_path(case)
        media = self.root / "média & movie"
        media.mkdir()
        asset = corpus.local_fixture_path(case["assets"][0])
        (media / asset.name).write_bytes(asset.read_bytes())
        output = self.root / "edited.trk"
        for scenario in corpus.MOVIE_TRACK_SCENARIOS:
            with corpus.minidom.parse(str(original)) as document:
                corpus.patch_expected_linked_media(document, movie=True, authoring=scenario)
                valid = document.toxml()
            output.write_text(valid, encoding="utf-8")
            corpus.verify_linked_movie_export_xml(original, output, case, authoring=scenario)
            for before, after in [('movie mass and other mass are words', 'renamed words'),
                                  ('<property name="mass_unit" type="string">kg', '<property name="mass_unit" type="string">g'),
                                  ('<property name="stepcount" type="int">4', '<property name="stepcount" type="int">3')]:
                with self.subTest(scenario=scenario, mutation=before):
                    self.assertIn(before, valid)
                    output.write_text(valid.replace(before, after), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        corpus.verify_linked_movie_export_xml(original, output, case, authoring=scenario)

    def test_keyed_movie_sessions_reject_corrupted_keys_at_every_stage(self):
        for flags in range(7):
            for archived in [False, True]:
                case_id = ("archived-" if archived else "") + f"keyed-movie-{flags}"
                with self.subTest(case=case_id):
                    self.check_media_session_oracle(case_id, keyed=True)

    def check_media_session_oracle(self, case_id, clip_mode=None, *, calibrated=False, keyed=False):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == case_id)
        expected = json.loads(corpus.expected_path(case).read_text())["project"]
        if clip_mode is not None:
            expected, case = corpus.linked_media_clip_expected(case, clip_mode == "single")
        if calibrated:
            expected, case = corpus.linked_movie_calibration_expected(case)
        keys = [[0, 1, 2]] if clip_mode is not None or calibrated else None
        if keyed:
            expected, case = corpus.keyed_movie_roundtrip_expected(case)
            keys = case["session_key_frames"]
        mass = expected["point_masses"][0]
        before = {"tracks": [case["session_tracks"][0], {
            **case["session_tracks"][1], "mass": mass["mass"], "key_frames": keys[0] if keys else [],
            "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                        for sample in mass["samples"]]}],
            "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
            "media": copy.deepcopy(case["session_media"]), "frame_duration_ms": 125.0,
            "axes": copy.deepcopy(case["session_axes"]), "calibration": copy.deepcopy(case["session_calibration"]),
            "time_samples": copy.deepcopy(case["session_time_samples"]),
            "name_references": {"selected_track": mass["name"]}, **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                 "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest, expected, None if keyed else keys)
        for stages in [["after"], ["before", "after_save", "after"]]:
            mutations = ["missing", "pixels", "order", "time", "dimensions", "count", "origin", "scale", "axes_grid", "engine",
                         "clip-range", "missing-mark", "display-time", "angle", "world", "unit"]
            if keyed:
                mutations += ["key-frame", "missing-key", "fixed_origin", "fixed_angle", "fixed_scale", "last-world",
                              "manual-keys-empty", "manual-keys-missing", "manual-keys-extra", "manual-keys-order"]
            for mutation in mutations:
                invalid = copy.deepcopy(valid)
                for stage in stages:
                    media = invalid[stage]["media"]
                    if mutation == "missing":
                        invalid[stage]["media"] = None
                    elif mutation == "engine":
                        media["class"] = "UnreviewedVideoEngine"
                    elif mutation == "pixels":
                        media["frames"][0]["rgb_sha256"] = "0" * 64
                    elif mutation == "order":
                        media["frames"][0]["frame"] = -1
                    elif mutation == "time":
                        media["frames"][0]["time_ms"] = 200.0
                    elif mutation == "dimensions":
                        media["frames"][0]["width"] = 31
                    elif mutation == "origin":
                        invalid[stage]["calibration"]["keys"][0]["origin"] = [0.0, 0.0]
                    elif mutation == "scale":
                        invalid[stage]["calibration"]["keys"][0]["scale"] = [1.0, 1.0]
                    elif mutation == "angle":
                        invalid[stage]["calibration"]["keys"][0]["angle"] += 5.0
                    elif mutation == "world":
                        invalid[stage]["tracks"][1]["samples"][0]["world"][0] += 1.0
                    elif mutation == "unit":
                        invalid[stage]["length_unit"] = "wrong"
                    elif mutation == "key-frame":
                        invalid[stage]["calibration"]["keys"][1]["frame"] += 1
                    elif mutation == "missing-key":
                        invalid[stage]["calibration"]["keys"].pop(1)
                    elif mutation in {"fixed_origin", "fixed_angle", "fixed_scale"}:
                        invalid[stage]["calibration"][mutation] = not invalid[stage]["calibration"][mutation]
                    elif mutation.startswith("manual-keys-"):
                        invalid[stage]["tracks"][1]["key_frames"] = {
                            "manual-keys-empty": [], "manual-keys-missing": [0, 1, 2],
                            "manual-keys-extra": [0, 1, 2, 3, 4], "manual-keys-order": [0, 2, 1, 3]}[mutation]
                    elif mutation == "last-world":
                        invalid[stage]["tracks"][1]["samples"][-1]["world"][1] += 1.0
                    elif mutation == "axes_grid":
                        invalid[stage]["axes"][0]["grid"]["visible"] = True
                    elif mutation == "clip-range":
                        invalid[stage]["clip"]["step_count"] += 1
                    elif mutation == "missing-mark":
                        invalid[stage]["tracks"][1]["samples"].pop(0)
                    elif mutation == "display-time":
                        invalid[stage]["time_samples"][0]["time_ms"] += 1.0
                    else:
                        media["frame_count"] = 3
                with self.subTest(stages=stages, mutation=mutation), self.assertRaises(ValueError):
                    corpus.validate_session(case, invalid, manifest, expected, None if keyed else keys)
        unaudited = {key: value for key, value in case.items() if key != "session_media"}
        with self.assertRaisesRegex(ValueError, "independent pixel"):
            corpus.validate_session(unaudited, valid, manifest, expected, keys)

    def test_reference_runtime_dependencies_are_pinned_bounded_and_selected_per_case(self):
        original = corpus.load_manifest()
        dependencies = original["reference"]["runtime_dependencies"]
        movie = next(case for case in original["cases"] if case["id"] == "linked-movie")
        self.assertEqual(len(dependencies), 2)
        entries = list(corpus.artifacts(original, self.root))
        for dependency in dependencies:
            self.assertIn((self.root / "runtime" / dependency["file"], dependency), entries)
        base = str(self.root / original["reference"]["file"])
        self.assertEqual(corpus.java_classpath(original, self.root), base)
        self.assertEqual(corpus.java_classpath(original, self.root, classes=self.root / "classes",
                                               dependencies=movie["reference_dependencies"]),
                         os.pathsep.join([str(self.root / "classes"), base,
                                          *(str(self.root / "runtime" / item["file"]) for item in dependencies)]))
        with self.assertRaisesRegex(ValueError, "Unknown"):
            corpus.java_classpath(original, self.root, dependencies=["../unknown.jar"])
        for mutation in ["not-list", "too-many", "non-object", "duplicate", "reference-name", "local", "not-jar",
                         "escape", "http", "hash", "size", "size-type", "missing-pin"]:
            manifest = copy.deepcopy(original)
            items = manifest["reference"]["runtime_dependencies"]
            if mutation == "not-list": manifest["reference"]["runtime_dependencies"] = {}
            elif mutation == "too-many": manifest["reference"]["runtime_dependencies"] = items * 5
            elif mutation == "non-object": items[0] = None
            elif mutation == "duplicate": items.append(copy.deepcopy(items[0]))
            elif mutation == "reference-name": items[0]["file"] = manifest["reference"]["file"]
            elif mutation == "local": items[0]["local_file"] = "fixtures/runtime.jar"
            elif mutation == "not-jar": items[0]["file"] = "runtime.exe"
            elif mutation == "escape": items[0]["file"] = "../runtime.jar"
            elif mutation == "http": items[0]["url"] = "http://example.org/runtime.jar"
            elif mutation == "hash": items[0]["sha256"] = "bad"
            elif mutation == "size": items[0]["bytes"] = 129 * 1024 * 1024
            elif mutation == "size-type": items[0]["bytes"] = "46328130"
            elif mutation == "missing-pin": del items[0]["sha256"]
            path = self.root / "runtime-manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.load_manifest(path)
        for required in ["runtime.jar", ["unknown.jar"], [movie["reference_dependencies"][0]] * 2, [None], [[]]]:
            manifest = copy.deepcopy(original)
            manifest["cases"][0]["reference_dependencies"] = required
            path = self.root / "runtime-case.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.subTest(required=required), self.assertRaises(ValueError):
                corpus.load_manifest(path)

    def test_reference_commands_only_load_the_cases_selected_movie_dependencies(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [case for case in manifest["cases"] if case["id"] in {"linked-images", "linked-movie"}]
        commands = []

        def reference(command):
            case = next(case for case in manifest["cases"] if corpus.project_path(case, self.root) == Path(command[-2]))
            Path(command[-1]).write_text(corpus.expected_path(case).read_text(encoding="utf-8"), encoding="utf-8")
            commands.append(command)
            self.assertIn("-Djava.awt.headless=true", command)
            self.assertIn(f"-Djava.io.tmpdir={self.root / 'java-home'}", command)
            classpath = command[command.index("--class-path") + 1].split(os.pathsep)
            expected = [str(self.root / manifest["reference"]["file"])]
            expected.extend(str(self.root / "runtime" / name) for name in case.get("reference_dependencies", []))
            self.assertEqual(classpath, expected)

        with patch.object(corpus, "verify_file"), patch.object(corpus, "run", side_effect=reference):
            corpus.check_cases(manifest, self.root, java="java")
        self.assertEqual(len(commands), 2)

    def test_media_assets_are_pinned_bounded_and_cannot_escape_fixture_root(self):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == "linked-images")
        self.assertEqual(len(case["assets"]), 4)
        artifacts = list(corpus.artifacts(manifest, self.root))
        for asset in case["assets"]:
            path = corpus.local_fixture_path(asset)
            self.assertIn((path, asset), artifacts)
            corpus.verify_file(path, asset)
            with self.assertRaisesRegex(ValueError, "Checksum"):
                corpus.verify_file(path, {**asset, "sha256": "0" * 64})
        for assets in ["bad", [None], [{}], [{"url": "https://example.org/frame.png"}], [{}] * 10001,
                       [{**case["assets"][0], "local_file": "../outside.png"}],
                       [{**case["assets"][0], "bytes": 129 * 1024 * 1024}]]:
            invalid = copy.deepcopy(manifest)
            invalid["cases"][0]["assets"] = assets
            path = self.root / "media-manifest.json"
            path.write_text(json.dumps(invalid), encoding="utf-8")
            with self.subTest(assets=str(assets)[:80]), self.assertRaises(ValueError):
                corpus.load_manifest(path)

    def test_unsupported_avi_archive_stays_pinned_outside_application_acceptance(self):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == "archived-avi")
        corpus.verify_file(corpus.local_fixture_path(case), case)
        self.assertEqual(case["sha256"], "aaf84ffdd4e637c93490149d173a4c99d803531e8f239fb8ea736b40bab05b64")
        self.assertEqual(case["session_exclusion"]["issue"], "https://github.com/mgradwohl/MoTrack/issues/140")
        for field, value in [("session_tracks", []), ("native_archive_movie_roundtrip", True)]:
            invalid = copy.deepcopy(manifest)
            next(c for c in invalid["cases"] if c["id"] == "archived-avi")[field] = value
            path = self.root / "excluded-avi.json"
            path.write_text(json.dumps(invalid), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "archived-avi: session_exclusion conflicts"):
                corpus.load_manifest(path)

    def test_known_null_table_subset_remains_pinned_and_excluded_from_acceptance(self):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == "table-owners-subset")
        corpus.verify_file(corpus.local_fixture_path(case), case)
        self.assertEqual(case["session_exclusion"]["issue"], "https://github.com/mgradwohl/MoTrack/issues/64")
        root = ET.parse(corpus.local_fixture_path(case)).getroot()
        columns = root.find(".//property[@name='track_columns']/property[@name='[2]']")
        self.assertEqual([cell.text for cell in columns], ["mass C", "t", "x"])
        for field, value in [("session_tracks", []), ("native_delete_table_roundtrip", True)]:
            invalid = copy.deepcopy(manifest)
            rejected = next(c for c in invalid["cases"] if c["id"] == "table-owners-subset")
            rejected[field] = value
            path = self.root / "excluded-manifest.json"
            path.write_text(json.dumps(invalid), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "table-owners-subset: session_exclusion conflicts"):
                corpus.load_manifest(path)

    def test_default_column_table_loss_stays_pinned_outside_acceptance(self):
        case = next(c for c in corpus.load_manifest()["cases"] if c["id"] == "table-owners-defaults")
        corpus.verify_file(corpus.local_fixture_path(case), case)
        self.assertNotIn("session_tracks", case)
        self.assertEqual(case["session_exclusion"]["issue"], "https://github.com/mgradwohl/MoTrack/issues/64")
        root = ET.parse(corpus.local_fixture_path(case)).getroot()
        rows = root.find(".//property[@name='track_columns']")
        self.assertEqual([[cell.text for cell in row] for row in rows],
                         [["mass A", "t", "x", "y"], ["mass B", "t", "y", "x"], ["mass C", "t", "x", "y"]])

    def test_deletion_oracle_keeps_untouched_measurements_names_and_views(self):
        manifest = corpus.load_manifest()
        for mode, case_id in [("delete-mass", "protractor"), ("delete-vector", "independent-vectors"),
                              ("delete-all-vectors", "independent-vectors"), ("delete-selected-mass", "protractor"),
                              ("delete-all-masses", "protractor"), ("delete-plot-owner", "plot-guests"),
                              ("delete-plot-guest", "plot-guests"), ("delete-first-plot-guest", "plot-guests"),
                              ("delete-first-table-owner", "table-default-format-owners"), ("delete-selected-table-owner", "table-default-format-owners"),
                              ("delete-last-table-owner", "table-default-format-owners"), ("delete-all-table-owners", "table-default-format-owners")]:
            original = next(c for c in manifest["cases"] if c["id"] == case_id)
            unchanged = copy.deepcopy(original)
            expected, case = corpus.deletion_roundtrip_expected(original, mode)
            tracks = []
            for identity in case["session_tracks"]:
                mass = next((v for v in expected["point_masses"] if v["name"] == identity["name"]), None)
                vector = next((v for v in expected["vectors"] if v["name"] == identity["name"]), None)
                calibrated = next((v for v in case.get("session_vector_data", []) if v["name"] == identity["name"]), None)
                keys = ([s["frame"] for s in mass["samples"]] if mass and mode in corpus.PLOT_DELETION_NAMES.keys() | corpus.TABLE_DELETION_NAMES.keys()
                        else [0] if mass else None)
                tracks.append({**identity, "mass": mass["mass"] if mass else None, "key_frames": keys,
                               "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                           for s in mass["samples"]] if mass else [],
                               "vector_samples": vector["samples"] if vector else [],
                               "calibrated_vector_samples": calibrated["samples"] if calibrated else []})
            before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      "name_references": case.get("session_names"), "saved_views": case.get("session_views"),
                      **corpus.session_units(case, expected)}
            report = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                      "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                      "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            keys = [] if mode == "delete-all-masses" else [[0]] if "mass" in mode else None
            if mode in corpus.PLOT_DELETION_NAMES:
                keys = [[0, 2], [0, 2]] if mode == "delete-first-plot-guest" else [[0, 2], [1, 3]]
            elif mode in corpus.TABLE_DELETION_NAMES:
                keys = [] if mode == "delete-all-table-owners" else [[0, 2], [0, 2]] if mode == "delete-last-table-owner" else [[0, 2], [1, 3]]
            corpus.validate_session(case, report, manifest, expected, keys)
            for mutation in ["resurrect", "lose-survivor", "alter-calibration"]:
                invalid = copy.deepcopy(report)
                for stage in ["before", "after_save", "after"]:
                    if mutation == "resurrect":
                        invalid[stage]["tracks"].append({"class": "org.opensourcephysics.cabrillo.tracker.Vector", "name": "vector A"})
                    elif mutation == "lose-survivor":
                        invalid[stage]["tracks"].pop()
                    else:
                        invalid[stage]["coordinate_samples"][0]["world"][0] += 10
                with self.subTest(mode=mode, mutation=mutation), self.assertRaises(ValueError):
                    corpus.validate_session(case, invalid, manifest, expected, keys)
            self.assertEqual(original, unchanged)
            if mode == "delete-mass":
                self.assertEqual([m["name"] for m in expected["point_masses"]], ["mass B"])
                self.assertEqual(case["session_names"], original["session_names"])
                self.assertEqual(case["session_views"], original["session_views"])
            elif mode in {"delete-selected-mass", "delete-all-masses"}:
                fallback = "protractor A" if mode == "delete-all-masses" else "mass A"
                self.assertEqual([m["name"] for m in expected["point_masses"]], [] if mode == "delete-all-masses" else ["mass A"])
                self.assertIsNone(case["session_names"]["selected_track"])
                self.assertEqual(case["session_names"]["view_tracks"], [[fallback, None], [None, fallback], [None, None], [None, None]])
                self.assertEqual(case["session_views"]["tables"][0]["track_columns"], original["session_views"]["tables"][0]["track_columns"])
                for mutation in ["panel", "fallback", "columns"]:
                    invalid = copy.deepcopy(report)
                    for stage in ["before", "after_save", "after"]:
                        if mutation == "panel":
                            invalid[stage]["name_references"]["selected_track"] = "mass B"
                        elif mutation == "fallback":
                            invalid[stage]["name_references"]["view_tracks"][0][0] = "axes"
                        else:
                            invalid[stage]["saved_views"]["tables"][0]["track_columns"] = []
                    with self.subTest(mode=mode, mutation=mutation), self.assertRaises(ValueError):
                        corpus.validate_session(case, invalid, manifest, expected, keys)
            elif mode in corpus.TABLE_DELETION_NAMES:
                removed = corpus.TABLE_DELETION_NAMES[mode]
                first = next((name for name in ["mass A", "mass B", "mass C"] if name not in removed), None)
                self.assertEqual(case["session_names"]["view_tracks"][0][0], first)
                self.assertEqual(case["session_names"]["view_tracks"][1][1], first)
                if mode == "delete-all-table-owners":
                    self.assertEqual(case["session_views"]["tables"], [])
                    self.assertEqual(case["session_names"]["view_tracks"], [[None, None]] * 4)
                else:
                    table = case["session_views"]["tables"][0]
                    self.assertEqual([row[0] for row in table["track_columns"]], [name for name in ["mass A", "mass B", "mass C"] if name not in removed])
                    self.assertTrue(all(row[0] not in removed for row in table["column_formats"]))
                for mutation in ["columns", "formats", "selection"]:
                    invalid = copy.deepcopy(report)
                    for stage in ["before", "after_save", "after"]:
                        if mutation == "selection":
                            invalid[stage]["name_references"]["view_tracks"][0][1] = "axes"
                        elif mode == "delete-all-table-owners":
                            invalid[stage]["saved_views"] = copy.deepcopy(original["session_views"])
                        elif mutation == "columns":
                            invalid[stage]["saved_views"]["tables"][0]["track_columns"][0][1:] = ["lost"]
                        else:
                            invalid[stage]["saved_views"]["tables"][0]["column_formats"][0][2] = "0.999"
                    with self.subTest(mode=mode, mutation=mutation), self.assertRaises(ValueError):
                        corpus.validate_session(case, invalid, manifest, expected, keys)
            elif mode in corpus.PLOT_DELETION_NAMES:
                self.assertEqual(case["session_names"]["view_tracks"][1][1], "mass B" if mode == "delete-plot-owner" else "mass A")
                if mode == "delete-plot-owner":
                    self.assertEqual(case["session_views"]["plots"], [])
                    self.assertIsNone(case["session_names"]["selected_track"])
                    self.assertEqual(case["session_names"]["view_tracks"][0][0], "mass B")
                else:
                    plots = case["session_views"]["plots"][0]["track_views"][0]["plots"]
                    self.assertEqual([plot["guests"] for plot in plots], [["mass C"], []] if mode == "delete-plot-guest" else [["mass B"], ["mass B"]])
                for mutation in ["links", "selection", "axis"]:
                    invalid = copy.deepcopy(report)
                    for stage in ["before", "after_save", "after"]:
                        if mutation == "links":
                            invalid[stage]["saved_views"] = copy.deepcopy(original["session_views"])
                        elif mutation == "selection":
                            invalid[stage]["name_references"]["view_tracks"][0][0] = "axes"
                        elif mode != "delete-plot-owner":
                            invalid[stage]["saved_views"]["plots"][0]["track_views"][0]["plots"][0]["x_var"] = "y"
                        else:
                            invalid[stage]["tracks"][1]["samples"][0]["world"][0] += 1
                    with self.subTest(mode=mode, mutation=mutation), self.assertRaises(ValueError):
                        corpus.validate_session(case, invalid, manifest, expected, keys)
            else:
                self.assertEqual([v["name"] for v in expected["vectors"]], [] if mode == "delete-all-vectors" else ["vector B"])
        for mode, case_id in [("invalid", "protractor"), ("delete-mass", "independent-vectors"), ("delete-vector", "protractor")]:
            original = next(c for c in manifest["cases"] if c["id"] == case_id)
            with self.assertRaises(ValueError):
                corpus.deletion_roundtrip_expected(original, mode)

    def test_deletion_xml_oracle_allows_only_exact_original_object_removal(self):
        original, exported = self.root / "deletion-source.trk", self.root / "deletion-output.trk"
        for mode in ["delete-mass", "delete-vector", "delete-all-vectors"]:
            kind, prefix = ("PointMass", "mass") if mode == "delete-mass" else ("Vector", "vector")
            items = [f'<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.{kind}">'
                     f'<property name="name" type="string">{prefix} {name}</property></object></property>' for name in ["A", "B"]]
            opening = ('<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">'
                       '<property name="description" type="string">preserved</property>'
                       '<property name="tracks" type="collection" class="java.util.ArrayList">')
            closing = '</property></object>'
            source = opening + "".join(items) + closing
            remaining = "" if mode == "delete-all-vectors" else items[1]
            valid = opening + remaining + closing
            original.write_text(source, encoding="utf-8")
            exported.write_text(valid, encoding="utf-8")
            corpus.verify_deletion_export_xml(original, exported, mode)
            for invalid in [source, valid.replace("preserved", "lost"), opening + items[0] + closing]:
                exported.write_text(invalid, encoding="utf-8")
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    corpus.verify_deletion_export_xml(original, exported, mode)
            original.write_text(source.replace(f">{prefix} A<", ">missing<"), encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_deletion_export_xml(original, exported, mode)
        with self.assertRaises(ValueError):
            corpus.verify_deletion_export_xml(original, exported, "invalid")

    def test_selection_deletion_xml_oracle_preserves_unrelated_content(self):
        original, exported = self.root / "selected-source.trk", self.root / "selected-output.trk"
        mass_a = ('<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.PointMass">'
                  '<property name="name" type="string">mass A</property></object></property>')
        mass_b = mass_a.replace("mass A", "mass B")
        tool = mass_a.replace("PointMass", "Protractor").replace("mass A", "protractor A")
        opening = ('<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">'
                   '<property name="description" type="string">preserved</property>'
                   '<property name="tracks" type="collection" class="java.util.ArrayList">')
        panel = '<property name="selectedtrack" type="string">mass B</property>'
        selections = ('<property name="selected_track_views" type="string">mass B,null;null,mass B;null,null;null,null</property>'
                      '<property name="views"><object class="org.opensourcephysics.cabrillo.tracker.TableTView">'
                      '<property name="selected_track" type="string">mass B</property>'
                      '<property name="untouched">custom</property></object></property></object>')
        source = opening + mass_a + tool + mass_b + '</property>' + panel + selections
        for mode, fallback, survivors in [("delete-selected-mass", "mass A", mass_a + tool),
                                          ("delete-all-masses", "protractor A", tool)]:
            valid = opening + survivors + '</property>' + selections.replace("mass B", fallback)
            original.write_text(source, encoding="utf-8")
            exported.write_text(valid, encoding="utf-8")
            corpus.verify_deletion_export_xml(original, exported, mode)
            for invalid in [source, valid.replace("preserved", "lost"), valid.replace("custom", "lost"),
                            valid.replace(f'>{fallback},null;', '>axes,null;'), valid.replace('</object>', panel + '</object>', 1)]:
                exported.write_text(invalid, encoding="utf-8")
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    corpus.verify_deletion_export_xml(original, exported, mode)
            for broken in [source.replace(panel, ''), source.replace('mass B,null;', 'missing,null;'),
                           source.replace('type="string">mass B</property>', 'type="string"><object/></property>')]:
                original.write_text(broken, encoding="utf-8")
                with self.assertRaises(ValueError):
                    corpus.verify_deletion_export_xml(original, exported, mode)

    def test_plot_deletion_xml_oracle_rejects_stale_links_and_unrelated_changes(self):
        source = corpus.CORPUS / "fixtures/plot-guests.trk"
        exported = self.root / "plot-deletion.trk"
        for mode, name in corpus.PLOT_DELETION_NAMES.items():
            tree = ET.parse(source)
            root = tree.getroot()
            tracks = root.find("property[@name='tracks']")
            for item in list(tracks):
                if item.find("object/property[@name='name']").text == name:
                    tracks.remove(item)
            view = root.find(".//object[@class='org.opensourcephysics.cabrillo.tracker.PlotTView']")
            owners = view.find("property[@name='track_views']")
            if mode == "delete-plot-owner":
                root.remove(root.find("property[@name='selectedtrack']"))
                view.find("property[@name='selected_track']").text = "mass B"
                owners.remove(list(owners)[0])
            else:
                for panel in owners.findall(".//object[@class='org.opensourcephysics.cabrillo.tracker.TrackPlottingPanel']"):
                    guests = panel.find("property[@name='guests']")
                    for guest in list(guests):
                        if guest.text == name:
                            guests.remove(guest)
                    if len(guests) == 0:
                        panel.remove(guests)
                    else:
                        for i, guest in enumerate(guests):
                            guest.set("name", f"[{i}]")
            tree.write(exported, encoding="utf-8")
            corpus.verify_deletion_export_xml(source, exported, mode)
            good = exported.read_text(encoding="utf-8")
            invalids = [source.read_text(encoding="utf-8"), good.replace('>kg<', '>lost<')]
            if mode == "delete-plot-owner":
                invalids.append(good.replace('name="selected_track" type="string">mass B', 'name="selected_track" type="string">axes'))
            else:
                invalids.append(good.replace('name="x_var" type="string">t', 'name="x_var" type="string">x'))
                invalids.append(good.replace('name="[0]" type="string">mass', 'name="[1]" type="string">mass'))
            for invalid in invalids:
                self.assertNotEqual(invalid, good)
                exported.write_text(invalid, encoding="utf-8")
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    corpus.verify_deletion_export_xml(source, exported, mode)

    def test_table_deletion_xml_oracle_preserves_surviving_groups_and_formats(self):
        source = corpus.CORPUS / "fixtures/table-default-format-owners.trk"
        exported = self.root / "table-deletion.trk"
        for mode, removed in corpus.TABLE_DELETION_NAMES.items():
            tree = ET.parse(source)
            root = tree.getroot()
            tracks = root.find("property[@name='tracks']")
            for item in list(tracks):
                if item.find("object/property[@name='name']").text in removed:
                    tracks.remove(item)
            table = root.find(".//object[@class='org.opensourcephysics.cabrillo.tracker.TableTView']")
            for field in ["track_columns", "column_formats"]:
                array = table.find(f"property[@name='{field}']")
                for group in list(array):
                    owner = group.find("property").text if field == "track_columns" else group.find("property/property").text
                    if owner in removed:
                        array.remove(group)
                if len(array) == 0:
                    table.remove(array)
                else:
                    for i, group in enumerate(array):
                        group.set("name", f"[{i}]")
            if "mass B" in removed:
                root.remove(root.find("property[@name='selectedtrack']"))
                selected = table.find("property[@name='selected_track']")
                if mode == "delete-all-table-owners":
                    table.remove(selected)
                    empty_views = ET.tostring(root.find("property[@name='views']"), encoding="unicode")
                    root.remove(root.find("property[@name='views']"))
                else:
                    selected.text = "mass A"
            remaining = [name for name in ["mass A", "mass B", "mass C"] if name not in removed]
            first = remaining[0] if remaining else "null"
            a = "mass A" if "mass A" in remaining else first
            b = "mass B" if "mass B" in remaining else first
            root.find("property[@name='selected_track_views']").text = f"{a},{b};null,{a};null,null;null,null"
            tree.write(exported, encoding="utf-8")
            corpus.verify_deletion_export_xml(source, exported, mode)
            good = exported.read_text(encoding="utf-8")
            invalids = [source.read_text(encoding="utf-8"), good.replace('>kg<', '>lost<')]
            if mode != "delete-all-table-owners":
                invalids += [good.replace('type="string">x<', 'type="string">lost<'),
                             good.replace('type="string">0<', 'type="string">9<')]
            else:
                before, after = good.rsplit("</object>", 1)
                invalids.append(before + empty_views + "</object>" + after)
            for invalid in invalids:
                self.assertNotEqual(invalid, good)
                exported.write_text(invalid, encoding="utf-8")
                with self.subTest(mode=mode), self.assertRaises(ValueError):
                    corpus.verify_deletion_export_xml(source, exported, mode)

    def test_created_vector_oracle_requires_original_and_new_geometry_and_appearance(self):
        manifest = corpus.load_manifest()
        original = next(case for case in manifest["cases"] if case["id"] == "independent-vectors")
        baseline = copy.deepcopy(original)
        for empty in [False, True]:
            expected, case = corpus.create_vector_roundtrip_expected(original, empty)
            self.assertEqual(expected["vectors"][-1]["samples"], [] if empty else [
                {"frame": 1, "tail": [100, 120], "components": [4, -8]},
                {"frame": 6, "tail": [200, 180], "components": [0, 0]},
                {"frame": 8, "tail": [160, 100], "components": [-6, 12]}])
            tracks = []
            for identity in case["session_tracks"]:
                vector = next((v for v in expected["vectors"] if v["name"] == identity["name"]), None)
                calibrated = next((v for v in case["session_vector_data"] if v["name"] == identity["name"]), None)
                tracks.append({**identity, "samples": [], "vector_samples": vector["samples"] if vector else [],
                               "calibrated_vector_samples": copy.deepcopy(calibrated["samples"]) if calibrated else [],
                               "appearance": copy.deepcopy(corpus.CREATED_VECTOR_APPEARANCE)})
            before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      **corpus.session_units(case, expected)}
            report = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                      "view_preparation": "expand_visit_custom_tables_restore_selection",
                      "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            corpus.validate_session(case, report, manifest, expected)
            for field, value in [("name", "wrong"), ("vector_samples", [{"frame": 1, "tail": [101, 120], "components": [4, -8]}]),
                                 ("appearance", {**corpus.CREATED_VECTOR_APPEARANCE, "visible": False}),
                                 ("appearance", {**corpus.CREATED_VECTOR_APPEARANCE, "footprint": "Footprint.Diamond"})]:
                invalid = copy.deepcopy(report)
                for stage in ["before", "after_save", "after"]:
                    invalid[stage]["tracks"][-1][field] = value
                with self.subTest(empty=empty, field=field), self.assertRaises(ValueError):
                    corpus.validate_session(case, invalid, manifest, expected)
            self.assertEqual([s["components"] for s in case["session_vector_data"][-1]["samples"]],
                             [] if empty else [[2, 2], [0, 0], [-3, -3]])
            for target in [1, len(tracks) - 1]:
                for field in ["components", "magnitude", "frame"]:
                    invalid = copy.deepcopy(report)
                    for stage in ["before", "after_save", "after"]:
                        values = invalid[stage]["tracks"][target]["calibrated_vector_samples"]
                        if values:
                            values[0][field] = [4, -8] if field == "components" else 123
                        else:
                            values.append({"frame": 1, "components": [2, 2], "magnitude": 8 ** 0.5})
                    with self.subTest(empty=empty, target=target, field=field), self.assertRaisesRegex(ValueError, "calibrated_vectors"):
                        corpus.validate_session(case, invalid, manifest, expected)
        self.assertEqual(original, baseline)
        with self.assertRaises(ValueError):
            corpus.create_vector_roundtrip_expected({**original, "id": "wrong"})

    def test_created_vector_xml_oracle_preserves_original_tracks_and_rejects_extra_keys(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "independent-vectors")
        source = corpus.local_fixture_path(case).read_text(encoding="utf-8")
        original, exported = self.root / "vector-creation-source.trk", self.root / "vector-creation-export.trk"
        original.write_text(source, encoding="utf-8")
        new = ('<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.Vector">'
               '<property name="name" type="string"><![CDATA[vecteur β & <C>]]></property>'
               '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
               '<property name="locked" type="boolean">false</property><property name="footprint" type="string"><![CDATA[Footprint.BoldArrow]]></property>'
               '<property name="color" type="object"><object class="java.awt.Color">'
               '<property name="red" type="int">255</property><property name="green" type="int">0</property>'
               '<property name="blue" type="int">255</property><property name="alpha" type="int">255</property></object></property>'
               '</object></property>')
        point = source.rfind('</property>')
        valid = source[:point] + new + source[point:]
        exported.write_text(valid, encoding="utf-8")
        corpus.verify_create_vector_export_xml(original, exported, True)
        with self.assertRaises(ValueError):
            corpus.verify_create_vector_export_xml(original, exported, False)
        for old, replacement in [('>vector A<', '>lost original<'), ('<![CDATA[vecteur β & <C>]]>', '<![CDATA[wrong]]>'),
                                 ('name="xcomponent" type="double">3.0', 'name="xcomponent" type="double">7.0'),
                                 ('<![CDATA[Footprint.BoldArrow]]>', '<![CDATA[Footprint.Arrow]]>'),
                                 ('<![CDATA[vecteur β & <C>]]></property>',
                                  '<![CDATA[vecteur β & <C>]]></property><property name="keyFrames" type="array" class="[I"/>')]:
            self.assertIn(old, valid)
            exported.write_text(valid.replace(old, replacement, 1), encoding="utf-8")
            with self.subTest(change=old), self.assertRaises(ValueError):
                corpus.verify_create_vector_export_xml(original, exported, True)

    def test_created_mass_expected_values_and_appearance_are_independent(self):
        manifest = corpus.load_manifest()
        original = next(case for case in manifest["cases"] if case["id"] == "protractor")
        baseline = copy.deepcopy(original)
        for empty in [False, True]:
            expected, case = corpus.create_mass_roundtrip_expected(original, empty)
            self.assertEqual(expected["point_masses"][-1]["mass"], 3)
            self.assertEqual([s["world"] for s in expected["point_masses"][-1]["samples"]],
                             [] if empty else [[10, 20], [20, 30], [40, 10]])
            self.assertEqual(case["session_names"], original["session_names"])
            tracks = []
            keys = [[0], [0], [] if empty else [1, 4, 8]]
            for identity in case["session_tracks"]:
                i = next((i for i, mass in enumerate(expected["point_masses"]) if mass["name"] == identity["name"]), None)
                mass = expected["point_masses"][i] if i is not None else None
                tracks.append({**identity, "mass": mass["mass"] if mass else None,
                               "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                           for s in mass["samples"]] if mass else [],
                               "key_frames": keys[i] if mass else None,
                               "appearance": copy.deepcopy(corpus.CREATED_MASS_APPEARANCE)})
            before = {"saved_views": copy.deepcopy(case["session_views"]), "name_references": copy.deepcopy(case["session_names"]),
                      "tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      **corpus.session_units(case, expected)}
            report = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                      "view_preparation": "expand_visit_custom_tables_restore_selection",
                      "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            corpus.validate_session(case, report, manifest, expected, keys)
            for field, value in [("mass", 1), ("name", "wrong"), ("key_frames", [2]),
                                 ("appearance", {**corpus.CREATED_MASS_APPEARANCE, "visible": False}),
                                 ("appearance", {**corpus.CREATED_MASS_APPEARANCE, "locked": True})]:
                invalid = copy.deepcopy(report)
                for stage in ["before", "after_save", "after"]:
                    invalid[stage]["tracks"][-1][field] = value
                with self.subTest(empty=empty, field=field), self.assertRaises(ValueError):
                    corpus.validate_session(case, invalid, manifest, expected, keys)
        self.assertEqual(original, baseline)
        with self.assertRaises(ValueError):
            corpus.create_mass_roundtrip_expected({**original, "id": "wrong"})

    def test_created_mass_xml_oracle_preserves_original_content_and_rejects_wrong_defaults(self):
        source = ('<!-- keep --><object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">'
                  '<property name="description" type="string">old words</property>'
                  '<property name="tracks" type="collection" class="java.util.ArrayList"></property></object>')
        new = ('<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.PointMass">'
               '<property name="name" type="string"><![CDATA[nouvelle β & <C>]]></property><property name="mass" type="double">3</property>'
               '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
               '<property name="locked" type="boolean">false</property><property name="footprint" type="string"><![CDATA[Footprint.Diamond]]></property>'
               '<property name="color" type="object"><object class="java.awt.Color">'
               '<property name="red" type="int">255</property><property name="green" type="int">0</property>'
               '<property name="blue" type="int">0</property><property name="alpha" type="int">255</property></object></property>'
               '</object></property>')
        original, exported = self.root / "creation-source.trk", self.root / "creation-export.trk"
        original.write_text(source, encoding="utf-8")
        valid = source.replace('class="java.util.ArrayList">', 'class="java.util.ArrayList">' + new)
        exported.write_text(valid, encoding="utf-8")
        corpus.verify_create_mass_export_xml(original, exported, True)
        with self.assertRaises(ValueError):
            corpus.verify_create_mass_export_xml(original, exported, False)
        for old, replacement in [('old words', 'lost words'), ('<!-- keep -->', ''), ('>3</property>', '>1</property>'),
                                 ('name="visible" type="boolean">true', 'name="visible" type="boolean">false'),
                                 ('<property name="item"', new + '<property name="item"')]:
            exported.write_text(valid.replace(old, replacement, 1), encoding="utf-8")
            with self.subTest(change=old), self.assertRaises(ValueError):
                corpus.verify_create_mass_export_xml(original, exported, True)

    def test_rename_oracle_preserves_unrelated_text_and_every_selection(self):
        def source(first="mass A", second="mass B", selected="mass B"):
            return f"""<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>
<property name='description' type='string'>mass A and mass B stay in this note</property>
<property name='tracks' type='collection' class='java.util.ArrayList'>
<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>
<property name='name' type='string'><![CDATA[{first}]]></property></object></property>
<property name='item' type='object'><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>
<property name='name' type='string'><![CDATA[{second}]]></property></object></property></property>
<property name='selectedtrack' type='string'><![CDATA[{selected}]]></property>
<property name='selected_track_views' type='string'><![CDATA[{selected},null;null,{selected};null,null;null,null]]></property>
<property name='views' type='array'><property name='[1]' type='object'>
<object class='org.opensourcephysics.cabrillo.tracker.TableTView'>
<property name='selected_track' type='string'><![CDATA[{selected}]]></property>
<property name='track_columns' type='array'><property name='[0]' type='array' class='[Ljava.lang.String;'>
<property name='[0]' type='string'><![CDATA[{selected}]]></property>
<property name='[1]' type='string'>mass B</property></property></property>
</object></property></property></object>"""
        original = self.root / "original-name.trk"
        exported = self.root / "exported-name.trk"
        original.write_text(source(), encoding="utf-8")
        for mode in ["name", "unselected-name"]:
            text = source(second=corpus.RENAME_LABEL, selected=corpus.RENAME_LABEL) if mode == "name" else source(first=corpus.RENAME_LABEL)
            exported.write_text(text, encoding="utf-8")
            corpus.verify_rename_export_xml(original, exported, mode)
            for before, after in [("mass A and mass B stay", "renamed words stay"),
                                  (">mass B</property>", ">wrong label</property>"),
                                  (";null,null;null,null", ";null,wrong;null,null"),
                                  ("<property name='selectedtrack'", "<property name='wrong_selection'"),
                                  ("</object>", "<property name='unexpected' type='string'>new</property></object>")]:
                exported.write_text(text.replace(before, after, 1), encoding="utf-8")
                with self.subTest(mode=mode, change=before), self.assertRaises(ValueError):
                    corpus.verify_rename_export_xml(original, exported, mode)

    def test_rename_expectations_use_original_identity_and_independent_baseline(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "protractor")
        original = copy.deepcopy(case)
        expected, selected = corpus.rename_roundtrip_expected(case, "name")
        self.assertEqual(expected["point_masses"][1]["name"], "élève β & <B>")
        self.assertEqual(selected["session_names"]["selected_track"], "élève β & <B>")
        self.assertEqual(selected["session_names"]["view_tracks"][1], [None, "élève β & <B>"])
        self.assertEqual(selected["session_views"]["tables"][0]["selected_track"], "élève β & <B>")
        self.assertEqual(selected["session_views"]["tables"][0]["track_columns"], [["protractor A", "t", "θ"]])
        expected, unselected = corpus.rename_roundtrip_expected(case, "unselected-name")
        self.assertEqual(expected["point_masses"][0]["name"], "élève β & <B>")
        self.assertEqual(unselected["session_names"], case["session_names"])
        self.assertEqual(case, original)
        with self.assertRaises(ValueError):
            corpus.rename_roundtrip_expected({**case, "id": "other"}, "name")
        with self.assertRaises(ValueError):
            corpus.rename_roundtrip_expected(case, "wrong")

    def test_vector_rename_oracle_preserves_geometry_null_extent_and_notes(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "independent-vectors")
        source = corpus.local_fixture_path(case).read_text(encoding="utf-8")
        before, after = source.rsplit("</object>", 1)
        source = (before + '<property name="selectedtrack" type="string">vector B</property>\n'
                  '<property name="selected_track_views" type="string">vector B,null;null,vector B;null,null;null,null</property>\n'
                  '<property name="description" type="string">vector A and vector B stay in this note</property>\n</object>' + after)
        original, exported = self.root / "vector-name-source.trk", self.root / "vector-name-export.trk"
        original.write_text(source, encoding="utf-8")
        text = source.replace('name="name" type="string">vector B</property>',
                              'name="name" type="string"><![CDATA[' + corpus.RENAME_LABEL + ']]></property>')
        text = text.replace('name="selectedtrack" type="string">vector B</property>',
                            'name="selectedtrack" type="string"><![CDATA[' + corpus.RENAME_LABEL + ']]></property>')
        packed = corpus.RENAME_LABEL + ',null;null,' + corpus.RENAME_LABEL + ';null,null;null,null'
        text = text.replace('name="selected_track_views" type="string">vector B,null;null,vector B;null,null;null,null</property>',
                            'name="selected_track_views" type="string"><![CDATA[' + packed + ']]></property>')
        exported.write_text(text, encoding="utf-8")
        corpus.verify_rename_export_xml(original, exported, "vector-name")
        for old, new in [('type="object">null', 'type="object"></property><property name="extra" type="object">null'),
                         ('vector A and vector B stay', 'replaced text'),
                         ('name="xcomponent" type="double">3.0', 'name="xcomponent" type="double">4.0'),
                         (';null,null;null,null', ';null,wrong;null,null')]:
            self.assertIn(old, text)
            exported.write_text(text.replace(old, new, 1), encoding="utf-8")
            with self.subTest(change=old), self.assertRaises(ValueError):
                corpus.verify_rename_export_xml(original, exported, "vector-name")
        expected, updated = corpus.rename_roundtrip_expected(case, "vector-name")
        baseline = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        self.assertEqual(expected["vectors"][1]["name"], "élève β & <B>")
        self.assertEqual(expected["vectors"][1]["samples"], baseline["vectors"][1]["samples"])
        self.assertEqual(updated["session_names"]["selected_track"], "élève β & <B>")
        self.assertEqual(updated["session_names"]["view_tracks"][0], ["élève β & <B>", None])
        self.assertNotIn("session_names", case)

    def test_plot_rename_oracle_changes_only_the_owner_or_ordered_guests(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "plot-guests")
        source = corpus.local_fixture_path(case).read_text(encoding="utf-8")
        before, after = source.rsplit("</object>", 1)
        source = before + '<property name="description" type="string">mass B</property>\n</object>' + after
        original, exported = self.root / "plot-source.trk", self.root / "plot-export.trk"
        original.write_text(source, encoding="utf-8")
        for mode in ["plot-owner-name", "plot-guest-name"]:
            old = "mass A" if mode == "plot-owner-name" else "mass B"
            fields = ["name", "selectedtrack", "selected_track", "track"] if mode == "plot-owner-name" else ["name", "[0]", "[1]"]
            text = source
            for field in fields:
                text = text.replace(f'name="{field}" type="string">{old}</property>',
                                    f'name="{field}" type="string"><![CDATA[{corpus.RENAME_LABEL}]]></property>')
            exported.write_text(text, encoding="utf-8")
            corpus.verify_rename_export_xml(original, exported, mode)
            for old_text, new_text in [('name="description" type="string">mass B', 'name="description" type="string">changed'),
                                       ('name="x_var" type="string">t', 'name="x_var" type="string">wrong'),
                                       ('name="guests" type="array"', 'name="lost_guests" type="array"'),
                                       ('name="[0]" type="string">mass C', 'name="[0]" type="string">wrong')]:
                self.assertIn(old_text, text)
                exported.write_text(text.replace(old_text, new_text, 1), encoding="utf-8")
                with self.subTest(mode=mode, change=old_text), self.assertRaises(ValueError):
                    corpus.verify_rename_export_xml(original, exported, mode)

    def test_plot_rename_requires_independent_live_owner_guest_order_and_selection(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "plot-guests")
        for mode in ["plot-owner-name", "plot-guest-name"]:
            expected, updated = corpus.rename_roundtrip_expected(case, mode)
            tracks = []
            for identity in updated["session_tracks"]:
                mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
                tracks.append({**identity, "mass": mass["mass"] if mass else None,
                               "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                           for s in mass["samples"]] if mass else []})
            before = {"tracks": tracks, "saved_views": copy.deepcopy(updated["session_views"]),
                      "name_references": {"selected_track": updated["session_selected_track"]},
                      "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      **corpus.session_units(updated, expected)}
            valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                     "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                     "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            corpus.validate_session(updated, valid, manifest, expected)
            plots = before["saved_views"]["plots"][0]["track_views"][0]["plots"]
            self.assertEqual(plots[0]["guests"], ["mass C", corpus.RENAME_LABEL if mode == "plot-guest-name" else "mass B"])
            changes = [(["name_references", "selected_track"], "wrong"),
                       (["name_references"], None),
                       (["saved_views", "plots", 0, "selected_track"], "wrong"),
                       (["saved_views", "plots", 0, "track_views", 0, "track"], "wrong"),
                       (["saved_views", "plots", 0, "track_views", 0, "plots", 0, "guests"], list(reversed(plots[0]["guests"]))),
                       (["saved_views", "plots", 0, "track_views", 0, "plots", 1, "guests"], []),
                       (["saved_views", "plots", 0, "track_views", 0, "plots", 0, "lines"], False),
                       (["saved_views", "plots", 0, "track_views", 0, "plots", 1, "points"], False)]
            for path, value in changes:
                for states in [["before", "after_save", "after"], ["after"]]:
                    invalid = copy.deepcopy(valid)
                    for state in states:
                        target = invalid[state]
                        for key in path[:-1]:
                            target = target[key]
                        target[path[-1]] = value
                    with self.subTest(mode=mode, path=path, states=states), self.assertRaises(ValueError):
                        corpus.validate_session(updated, invalid, manifest, expected)

    def test_center_member_rename_preserves_complete_source_and_independent_physics(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "center-of-mass")
        baseline = copy.deepcopy(case)
        expected, updated = corpus.rename_roundtrip_expected(case, "center-member-name")
        self.assertEqual(case, baseline)
        center = updated["session_centers"][0]
        self.assertEqual(center["members"], [corpus.RENAME_LABEL, "mass B"])
        self.assertEqual(center["mass"], 5.0)
        self.assertEqual(center["samples"], case["session_centers"][0]["samples"])
        source = corpus.local_fixture_path(case).read_text(encoding="utf-8")
        before, after = source.rsplit("</object>", 1)
        source = before + '<property name="description" type="string">mass A</property>\n</object>' + after
        text = source
        for field in ["name", "item"]:
            text = text.replace(f'name="{field}" type="string">mass A</property>',
                                f'name="{field}" type="string"><![CDATA[{corpus.RENAME_LABEL}]]></property>')
        original, exported = self.root / "center-source.trk", self.root / "center-export.trk"
        original.write_text(source, encoding="utf-8")
        exported.write_text(text, encoding="utf-8")
        corpus.verify_rename_export_xml(original, exported, "center-member-name")
        for old, new in [('name="description" type="string">mass A', 'name="description" type="string">changed'),
                         ('name="mass" type="double">5.0', 'name="mass" type="double">4.0'),
                         ('name="item" type="string">mass B', 'name="item" type="string">wrong'),
                         ('name="selectedtrack" type="string">center', 'name="selectedtrack" type="string">mass B')]:
            self.assertIn(old, text)
            exported.write_text(text.replace(old, new, 1), encoding="utf-8")
            with self.subTest(change=old), self.assertRaises(ValueError):
                corpus.verify_rename_export_xml(original, exported, "center-member-name")

        tracks = []
        for identity in updated["session_tracks"]:
            mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                       for s in mass["samples"]] if mass else []})
        before = {"tracks": tracks, "centers_of_mass": copy.deepcopy(updated["session_centers"]),
                  "name_references": {"selected_track": "center"}, "clip": expected["clip"],
                  "coordinate_samples": expected["coordinate_samples"], **corpus.session_units(updated, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                 "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(updated, valid, manifest, expected)
        for path, value in [(["centers_of_mass", 0, "members"], ["mass A", "mass B"]),
                            (["centers_of_mass", 0, "members"], [corpus.RENAME_LABEL]),
                            (["centers_of_mass", 0, "mass"], 4.0),
                            (["centers_of_mass", 0, "samples", 0, "world"], [0.0, 0.0]),
                            (["centers_of_mass"], []), (["name_references", "selected_track"], "mass B")]:
            for states in [["before", "after_save", "after"], ["after"]]:
                invalid = copy.deepcopy(valid)
                for state in states:
                    target = invalid[state]
                    for key in path[:-1]:
                        target = target[key]
                    target[path[-1]] = value
                with self.subTest(path=path, states=states), self.assertRaises(ValueError):
                    corpus.validate_session(updated, invalid, manifest, expected)

    def test_attachment_rename_oracles_keep_targets_empty_slots_and_independent_lengths(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "attachments")
        original_case = copy.deepcopy(case)
        source = corpus.local_fixture_path(case).read_text(encoding="utf-8")
        before, after = source.rsplit("</object>", 1)
        source = before + '<property name="description" type="string">mass A and mass B</property>\n</object>' + after
        original, exported = self.root / "attachment-source.trk", self.root / "attachment-export.trk"
        original.write_text(source, encoding="utf-8")
        for mode in ["attachment-first-name", "attachment-last-name"]:
            old = "mass A" if mode == "attachment-first-name" else "mass B"
            text = source
            for field in ["name", "[0]", "[1]"]:
                text = text.replace(f'name="{field}" type="string">{old}</property>',
                                    f'name="{field}" type="string"><![CDATA[{corpus.RENAME_LABEL}]]></property>')
            exported.write_text(text, encoding="utf-8")
            corpus.verify_rename_export_xml(original, exported, mode)
            for old_text, new_text in [('name="description" type="string">mass A', 'name="description" type="string">changed'),
                                       ('name="[1]" type="object">null', 'name="[1]" type="string">mass A'),
                                       ('name="x1" type="double">100.0', 'name="x1" type="double">99.0'),
                                       ('name="worldlengths"', 'name="lost_lengths"')]:
                self.assertIn(old_text, text)
                exported.write_text(text.replace(old_text, new_text, 1), encoding="utf-8")
                with self.subTest(mode=mode, change=old_text), self.assertRaises(ValueError):
                    corpus.verify_rename_export_xml(original, exported, mode)
            expected, updated = corpus.rename_roundtrip_expected(case, mode)
            for before_tape, after_tape in zip(case["session_attachments"], updated["session_attachments"]):
                self.assertEqual(after_tape["samples"], before_tape["samples"])
                self.assertEqual(after_tape["targets"], [corpus.RENAME_LABEL if name == old else name for name in before_tape["targets"]])
            tracks = []
            for identity in updated["session_tracks"]:
                mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
                tracks.append({**identity, "mass": mass["mass"] if mass else None,
                               "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                           for s in mass["samples"]] if mass else []})
            before = {"tracks": tracks, "attachments": copy.deepcopy(updated["session_attachments"]),
                      "name_references": {"selected_track": "both ends"}, "clip": expected["clip"],
                      "coordinate_samples": expected["coordinate_samples"], **corpus.session_units(updated, expected)}
            valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                     "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                     "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            corpus.validate_session(updated, valid, manifest, expected)
            for path, value in [(["attachments", 0, "targets"], ["mass A", "mass B"]),
                                (["attachments", 1, "targets", 1], "mass B"),
                                (["attachments", 2, "targets", 0], "mass A"),
                                (["attachments", 0, "samples", 1, "world_length"], 5.0),
                                (["attachments", 0, "samples", 0, "endpoints"], [[0, 0], [0, 0]]),
                                (["attachments", 1, "samples", 0, "attached"], [True, True]),
                                (["attachments"], []), (["name_references", "selected_track"], "mass B")]:
                for states in [["before", "after_save", "after"], ["after"]]:
                    invalid = copy.deepcopy(valid)
                    for state in states:
                        target = invalid[state]
                        for key in path[:-1]:
                            target = target[key]
                        target[path[-1]] = value
                    with self.subTest(mode=mode, path=path, states=states), self.assertRaises(ValueError):
                        corpus.validate_session(updated, invalid, manifest, expected)
        self.assertEqual(case, original_case)

    def test_model_booster_rename_changes_only_the_label_and_booster_reference(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "model-dependencies")
        baseline = copy.deepcopy(case)
        source = corpus.local_fixture_path(case).read_text(encoding="utf-8")
        before, after = source.rsplit("</object>", 1)
        source = before + '<property name="description" type="string">mass A</property>\n</object>' + after
        original, exported = self.root / "model-source.trk", self.root / "model-export.trk"
        original.write_text(source, encoding="utf-8")
        text = source
        for field in ["name", "booster"]:
            text = text.replace(f'name="{field}" type="string">mass A</property>',
                                f'name="{field}" type="string"><![CDATA[{corpus.RENAME_LABEL}]]></property>')
        exported.write_text(text, encoding="utf-8")
        corpus.verify_rename_export_xml(original, exported, "model-booster-name")
        for old, new in [('name="description" type="string">mass A', 'name="description" type="string">changed'),
                         ('name="function" type="string">100', 'name="function" type="string">0'),
                         ('name="expression" type="string">0', 'name="expression" type="string">1'),
                         ('name="[1]" type="string">particle B', 'name="[1]" type="string">particle A'),
                         ('name="selectedtrack" type="string">system', 'name="selectedtrack" type="string">mass A')]:
            self.assertIn(old, text)
            exported.write_text(text.replace(old, new, 1), encoding="utf-8")
            with self.subTest(change=old), self.assertRaises(ValueError):
                corpus.verify_rename_export_xml(original, exported, "model-booster-name")
        expected, updated = corpus.rename_roundtrip_expected(case, "model-booster-name")
        self.assertEqual(expected["point_masses"][0]["name"], corpus.RENAME_LABEL)
        self.assertEqual(updated["session_models"][1]["booster"], corpus.RENAME_LABEL)
        self.assertEqual(updated["session_models"][1]["samples"], case["session_models"][1]["samples"])
        self.assertEqual(updated["session_models"][2], case["session_models"][2])
        self.assertEqual(case, baseline)

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="motrack-corpus-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.data = b"reference data\n"
        self.artifact = {"file": "sample.trk", "url": "https://example.org/sample.trk",
                         "bytes": len(self.data), "sha256": hashlib.sha256(self.data).hexdigest()}

    def test_download_is_verified_before_publication(self):
        path = self.root / "sample.trk"
        with patch.object(corpus.urllib.request, "urlopen", return_value=Download(self.data)):
            corpus.fetch_file(path, self.artifact)
        corpus.verify_file(path, self.artifact)
        self.assertEqual(path.read_bytes(), self.data)
        self.assertEqual(list(self.root.glob("*.part")), [])

    def test_corrupt_existing_file_is_preserved_without_network(self):
        path = self.root / "sample.trk"
        path.write_bytes(b"x" * len(self.data))
        with patch.object(corpus.urllib.request, "urlopen") as request:
            with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
                corpus.fetch_file(path, self.artifact)
            request.assert_not_called()
        self.assertEqual(path.read_bytes(), b"x" * len(self.data))

    def test_bad_downloads_leave_no_partial_or_published_file(self):
        for data, url in [(b"x" * len(self.data), "https://example.org/a"),
                          (self.data + b"too long", "https://example.org/a"),
                          (b"short", "https://example.org/a"),
                          (self.data, "http://example.org/a")]:
            with self.subTest(data=data, url=url):
                with patch.object(corpus.urllib.request, "urlopen", return_value=Download(data, url)):
                    with self.assertRaises(ValueError):
                        corpus.fetch_file(self.root / "sample.trk", self.artifact)
                self.assertEqual(list(self.root.iterdir()), [])

    def test_compare_uses_exact_identity_and_explicit_numeric_tolerance(self):
        tolerance = {"absolute": 1e-10, "relative": 1e-12}
        corpus.compare({"frame": 123, "world": [1.0, None]}, {"frame": 123, "world": [1.0 + 1e-11, None]}, tolerance)
        for actual in [{"frame": 124, "world": [1.0, None]},
                       {"frame": 123, "world": [1.001, None]},
                       {"frame": 123, "world": [float("nan"), None]},
                       {"frame": 123, "world": [1.0, 0]},
                       {"frame": 123, "world": [1.0]},
                       {"frame": 123, "world": [1.0, None], "extra": 0}]:
            with self.subTest(actual=actual), self.assertRaises(ValueError):
                corpus.compare({"frame": 123, "world": [1.0, None]}, actual, tolerance)

    def test_archive_member_is_read_without_extracting_its_path(self):
        (self.root / "fixtures").mkdir()
        source = self.root / "fixtures/sample.trz"
        with zipfile.ZipFile(source, "w") as archive:
            archive.writestr("../outside.trk", b"<object/>")
            archive.writestr("unrelated.txt", b"untouched")
        case = {"id": "example", "file": "sample.trz", "project_member": "../outside.trk"}
        result = corpus.project_path(case, self.root)
        self.assertEqual(result, self.root / "projects/example.trk")
        self.assertEqual(result.read_bytes(), b"<object/>")
        self.assertFalse((self.root / "outside.trk").exists())
        self.assertFalse((self.root / "unrelated.txt").exists())
        with patch.object(corpus, "MAX_XML_BYTES", 3), self.assertRaisesRegex(ValueError, "oversized"):
            corpus.project_path(case, self.root)

    def test_missing_archive_member_fails(self):
        (self.root / "fixtures").mkdir()
        with zipfile.ZipFile(self.root / "fixtures/sample.trz", "w") as archive:
            archive.writestr("actual.trk", b"<object/>")
        with self.assertRaisesRegex(ValueError, "Missing"):
            corpus.project_path({"id": "example", "file": "sample.trz", "project_member": "missing.trk"}, self.root)

    def test_duplicate_archive_member_fails(self):
        (self.root / "fixtures").mkdir()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.root / "fixtures/sample.trz", "w") as archive:
                archive.writestr("project.trk", b"first")
                archive.writestr("project.trk", b"second")
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            corpus.project_path({"id": "example", "file": "sample.trz", "project_member": "project.trk"}, self.root)

    def test_manifest_rejects_duplicate_ids_and_escaping_paths(self):
        missing_version = corpus.load_manifest()
        del missing_version["cases"][0]["tracker_version"]
        missing_path = self.root / "missing-version.json"
        missing_path.write_text(json.dumps(missing_version), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "original Tracker version"):
            corpus.load_manifest(missing_path)
        manifest = corpus.load_manifest()
        manifest["cases"].append(manifest["cases"][0])
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            corpus.load_manifest(path)
        with self.assertRaisesRegex(ValueError, "inside"):
            corpus.expected_path({"expected": "../../outside.json"})
        manifest = corpus.load_manifest()
        manifest["cases"][0]["native_reference_index"] = "../../outside.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "inside"):
            corpus.load_manifest(path)

    def test_native_reference_case_requires_exact_source_targets(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "protractor")
        manifest["cases"] = [case]
        project = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        expected = json.loads(corpus.expected_path({"expected": case["native_reference_index"]}).read_text(encoding="utf-8"))
        corrupt = False
        def probe(command):
            if "--references" in command:
                report = copy.deepcopy(expected)
                if corrupt:
                    report["references"][0]["targets"] = [1]
            else:
                report = project
            Path(command[-1]).write_text(json.dumps(report), encoding="utf-8")
        with patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "project_path", return_value=self.root / "unused.trk"), \
                patch.object(corpus, "run", side_effect=probe) as run:
            corpus.check_cases(manifest, self.root, probe="unused-probe")
            self.assertIn("--references", run.call_args.args[0])
            corrupt = True
            with self.assertRaisesRegex(ValueError, "reference_index"):
                corpus.check_cases(manifest, self.root, probe="unused-probe")

    def test_session_validation_checks_loaded_content_and_java_resave(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "protractor")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                                       for sample in mass["samples"]] if mass else []})
        before = {"saved_views": copy.deepcopy(case["session_views"]),
                  "name_references": copy.deepcopy(case["session_names"]),
                  "tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        edited_expected = corpus.mass_roundtrip_expected(case)
        edited = copy.deepcopy(valid)
        for key in ["before", "after_save", "after"]:
            edited[key]["tracks"][1]["mass"] *= 2.5
        corpus.validate_session(case, edited, manifest, edited_expected)
        with self.assertRaises(ValueError):
            corpus.validate_session(case, valid, manifest, edited_expected)
        invalid = copy.deepcopy(valid)
        invalid["after"]["tracks"][1]["mass"] = 2.0
        with self.assertRaises(ValueError):
            corpus.validate_session(case, invalid, manifest)
        invalid = copy.deepcopy(valid)
        for key in ["before", "after_save", "after"]:
            invalid[key]["tracks"][2]["class"] = "MissingProtractor"
        with self.assertRaises(ValueError):
            corpus.validate_session(case, invalid, manifest)
        invalid = copy.deepcopy(valid)
        for key in ["before", "after_save", "after"]:
            invalid[key]["tracks"][1]["samples"][0]["points"][0][0] += 1.0
        with self.assertRaises(ValueError):
            corpus.validate_session(case, invalid, manifest)
        for key, value in [("reference_version", "6.3.4"), ("verification_level", "xml_and_coordinate_api"),
                           ("view_preparation", None), ("view_preparation", "unvisited")]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                corpus.validate_session(case, {**valid, key: value}, manifest)

    def test_session_name_references_reject_consistent_wrong_targets(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "protractor")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                                       for sample in mass["samples"]] if mass else []})
        before = {"saved_views": copy.deepcopy(case["session_views"]),
                  "name_references": copy.deepcopy(case["session_names"]), "tracks": tracks,
                  "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        for field, value in [("selected_track", "mass A"), ("selected_track", None),
                             ("view_tracks", [["mass A", None], [None, "mass B"], [None, None], [None, None]]),
                             ("view_tracks", [["mass B", None], [None, "mass A"], [None, None], [None, None]]),
                             ("packed_view_tracks", "mass A,null;null,mass B;null,null;null,null")]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state]["name_references"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)
        for mode in ["missing", "changed_after_save", "changed_after_reopen"]:
            invalid = copy.deepcopy(valid)
            if mode == "missing":
                for state in ["before", "after_save", "after"]:
                    del invalid[state]["name_references"]
            else:
                state = "after_save" if mode == "changed_after_save" else "after"
                invalid[state]["name_references"]["selected_track"] = "mass A"
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

    def test_saved_views_require_original_owners_columns_and_formats(self):
        manifest = corpus.load_manifest()
        for case_id in ["protractor", "dynamic-sine", "plot-guests", "table-owners", "table-default-format-owners"]:
            case = next(case for case in manifest["cases"] if case["id"] == case_id)
            if "rejected_session_case" in case:
                case = json.loads((corpus.CORPUS / case["rejected_session_case"]).read_text(encoding="utf-8"))
            expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
            tracks = []
            for identity in case["session_tracks"]:
                mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
                tracks.append({**identity, "mass": mass["mass"] if mass else None,
                               "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                                           for sample in mass["samples"]] if mass else []})
            before = {"saved_views": copy.deepcopy(case["session_views"]), "tracks": tracks,
                      "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      **corpus.session_units(case, expected)}
            if "session_names" in case:
                before["name_references"] = copy.deepcopy(case["session_names"])
            valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                     "view_preparation": "expand_visit_custom_tables_restore_selection",
                     "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            corpus.validate_session(case, valid, manifest)
            if case_id == "protractor":
                changes = [
                    (["tables", 0, "chooser"], 0),
                    (["tables", 0, "selected_track"], "protractor A"),
                    (["tables", 0, "track_columns", 0, 0], "mass B"),
                    (["tables", 0, "track_columns", 0], ["protractor A", "x", "t"]),
                    (["tables", 0, "track_columns", 0], ["protractor A", "\u03b8", "t"]),
                    (["tables", 0, "column_formats", 0, 0], "mass B"),
                    (["tables", 0, "column_formats", 0, 1], "x"),
                    (["tables", 0, "column_formats", 0, 2], "0.00"),
                    (["tables", 0, "dataset_indices"], [["protractor A", "1"]]),
                    (["tables"], []),
                ]
            elif case_id.startswith("table-"):
                changes = [
                    (["tables", 0, "selected_track"], "mass A"),
                    (["tables", 0, "track_columns", 1], ["mass B", "t", "x", "y", "frame"]),
                    (["tables", 0, "column_formats"], []),
                    (["tables"], []),
                ]
                if case_id == "table-owners":
                    changes.append((["tables", 0, "column_formats", 3, 2], ""))
            else:
                changes = [
                    (["plots", 0, "chooser"], 1),
                    (["plots", 0, "selected_track"], "axes"),
                    (["plots", 0, "track_views", 0, "track"], "axes"),
                    (["plots", 0, "track_views", 0, "linked"], True),
                    (["plots", 0, "track_views", 0, "plots", 0, "x_var"], "x"),
                    (["plots", 0, "track_views", 0, "plots", 0, "y_var"], "invalid-variable"),
                    (["plots", 0, "track_views", 0, "plots", 0, "guests"], ["axes"]),
                    (["plots", 0, "track_views"], []),
                    (["plots"], []),
                ]
                if case_id == "plot-guests":
                    changes.extend([
                        (["plots", 0, "track_views", 0, "plots", 0, "guests"], []),
                        (["plots", 0, "track_views", 0, "plots", 0, "guests"], ["mass B", "mass C"]),
                        (["plots", 0, "track_views", 0, "plots", 0, "guests"], ["mass B"]),
                        (["plots", 0, "track_views", 0, "plots", 0, "guests"], ["mass C", "mass C"]),
                        (["plots", 0, "track_views", 0, "plots", 1, "guests"], ["mass C"]),
                    ])
            for path, value in changes:
                # Reject a consistently wrong load as well as changes during saving/reopening.
                for states in [["before", "after_save", "after"], ["after_save"], ["after"]]:
                    invalid = copy.deepcopy(valid)
                    for state in states:
                        target = invalid[state]["saved_views"]
                        for key in path[:-1]:
                            target = target[key]
                        target[path[-1]] = value
                    with self.subTest(case=case_id, path=path, states=states), self.assertRaises(ValueError):
                        corpus.validate_session(case, invalid, manifest)
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                del invalid[state]["saved_views"]
            with self.subTest(case=case_id, missing=True), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

    def test_center_of_mass_requires_members_and_independent_weighted_positions(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "center-of-mass")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                                       for sample in mass["samples"]] if mass else []})
        before = {"centers_of_mass": copy.deepcopy(case["session_centers"]), "tracks": tracks,
                  "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        for field, value in [("members", ["mass A"]), ("members", ["mass A", "mass A"]),
                             ("mass", 2.0), ("name", "different center"), ("samples", []),
                             ("samples", [{"frame": 0, "image": [25.0, 40.0], "world": [2.5, -5.0]},
                                          {"frame": 2, "image": [32.0, 52.0], "world": [6.0, -8.0]}])]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state]["centers_of_mass"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)
        for field, value in [("world", [0.0, 0.0]), ("image", [25.0, 40.0])]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state]["centers_of_mass"][0]["samples"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)
        invalid = copy.deepcopy(valid)
        for state in ["before", "after_save", "after"]:
            invalid[state]["centers_of_mass"][0]["samples"][1]["frame"] = 1
        with self.assertRaises(ValueError):
            corpus.validate_session(case, invalid, manifest)
        invalid = copy.deepcopy(valid)
        for state in ["before", "after_save", "after"]:
            del invalid[state]["centers_of_mass"]
        with self.assertRaises(ValueError):
            corpus.validate_session(case, invalid, manifest)

    def test_attachments_require_targets_null_slots_and_independent_measurements(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "attachments")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                       for s in mass["samples"]] if mass else []})
        before = {"attachments": copy.deepcopy(case["session_attachments"]), "tracks": tracks,
                  "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        changes = [([0, "name"], "wrong"), ([0, "targets"], []),
                   ([0, "targets"], ["mass B", "mass A"]), ([0, "targets"], ["mass A", "mass A"]),
                   ([1, "targets"], ["mass A", "null"]), ([1, "targets"], [None, "mass A"]),
                   ([1, "samples", 0, "attached"], [True, True]), ([0, "samples", 0, "attached"], [False, False]),
                   ([2, "samples", 0, "attached"], [True, True]), ([2, "targets"], ["mass B", None]),
                   ([0, "samples"], []), ([0, "samples", 0, "endpoints"], [[100, 100], [110, 100]]),
                   ([0, "samples", 1, "world_length"], 15.0), ([0, "samples", 2, "frame"], 3),
                   ([1, "samples", 2, "endpoints"], [[24, 40], [0, 0]])]
        for path, value in changes:
            for states in [["before", "after_save", "after"], ["after_save"], ["after"]]:
                invalid = copy.deepcopy(valid)
                for state in states:
                    target = invalid[state]["attachments"]
                    for key in path[:-1]:
                        target = target[key]
                    target[path[-1]] = value
                with self.subTest(path=path, states=states), self.assertRaises(ValueError):
                    corpus.validate_session(case, invalid, manifest)
        invalid = copy.deepcopy(valid)
        for state in ["before", "after_save", "after"]:
            del invalid[state]["attachments"]
        with self.assertRaises(ValueError):
            corpus.validate_session(case, invalid, manifest)

    def test_models_require_boosters_members_and_independent_positions(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "model-dependencies")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        original = copy.deepcopy(case)
        for renamed in [False, True]:
            if renamed:
                expected, case = corpus.rename_roundtrip_expected(original, "model-booster-name")
            tracks = []
            for identity in case["session_tracks"]:
                mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
                tracks.append({**identity, "mass": mass["mass"] if mass else None,
                               "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                           for s in mass["samples"]] if mass else []})
            before = {"models": copy.deepcopy(case["session_models"]), "tracks": tracks,
                      "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      **corpus.session_units(case, expected)}
            if renamed:
                before["name_references"] = {"selected_track": "system"}
            valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                     "view_preparation": "expand_visit_custom_tables_restore_selection",
                     "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            corpus.validate_session(case, valid, manifest, expected)
            changes = [([1, "booster"], None), ([1, "booster"], "wrong"), ([2, "particles"], []),
                       ([2, "particles"], ["particle B", "particle A"]), ([2, "particles"], ["particle A", "particle A"]),
                       ([2, "mass"], 0.0), ([0, "mass"], 3.0), ([1, "samples"], []),
                       ([1, "samples", 0, "world"], [100.0, 100.0]), ([1, "samples", 0, "image"], [220.0, -160.0]),
                       ([2, "samples", 2, "world"], [2.0, 3.0]), ([2, "samples", 2, "frame"], 3),
                       ([0, "functions", 0, "expression"], "1"), ([1, "functions", 1, "name"], "wrong"),
                       ([2, "functions"], [])]
            for path, value in changes:
                for states in [["before", "after_save", "after"], ["after_save"], ["after"]]:
                    invalid = copy.deepcopy(valid)
                    for state in states:
                        target = invalid[state]["models"]
                        for key in path[:-1]:
                            target = target[key]
                        target[path[-1]] = value
                    with self.subTest(path=path, states=states), self.assertRaises(ValueError):
                        corpus.validate_session(case, invalid, manifest, expected)
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                del invalid[state]["models"]
            with self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest, expected)

    def test_vector_sum_requires_members_recomputed_components_and_missing_frames(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "vector-sum")
        corpus.verify_file(corpus.local_fixture_path(case), case)
        root = ET.parse(corpus.local_fixture_path(case)).getroot()
        cached = root.find("property[@name='tracks']/property/object[@class='org.opensourcephysics.cabrillo.tracker.VectorSum']")
        self.assertIsNotNone(cached)
        cached_frames = cached.findall("property[@name='framedata']/property/object")
        self.assertEqual(len(cached_frames), 3)
        for frame in cached_frames:
            self.assertEqual(float(frame.find("property[@name='xcomponent']").text), 0.0)
            self.assertEqual(float(frame.find("property[@name='ycomponent']").text), 0.0)
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            vector = next((vector for vector in expected["vectors"] if vector["name"] == identity["name"]), None)
            tracks.append({**identity, "samples": [], "vector_samples": copy.deepcopy(vector["samples"]) if vector else []})
        before = {"vector_sums": copy.deepcopy(case["session_sums"]), "tracks": tracks,
                  "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        changes = [(["name"], "other sum"), (["members"], ["vector A"]),
                   (["members"], ["vector A", "vector A"]), (["members"], ["vector A", "missing"]),
                   (["samples"], []), (["samples", 0, "tail"], [0.0, 0.0]),
                   (["samples", 0, "components"], [0.0, 0.0]),
                   (["samples", 0, "world_components"], [5.0, 1.0]),
                   (["samples", 1, "components"], [1.0, 0.0]),
                   (["samples", 2, "frame"], 3),
                   (["samples"], case["session_sums"][0]["samples"] + [
                       {"frame": 1, "tail": [260.0, 270.0], "components": [5.0, 6.0], "world_components": [2.5, -1.5]}])]
        for path, value in changes:
            for states in [["before", "after_save", "after"], ["after_save"], ["after"]]:
                invalid = copy.deepcopy(valid)
                for state in states:
                    target = invalid[state]["vector_sums"][0]
                    for key in path[:-1]:
                        target = target[key]
                    target[path[-1]] = value
                with self.subTest(path=path, states=states), self.assertRaises(ValueError):
                    corpus.validate_session(case, invalid, manifest)
        invalid = copy.deepcopy(valid)
        for state in ["before", "after_save", "after"]:
            del invalid[state]["vector_sums"]
        with self.assertRaises(ValueError):
            corpus.validate_session(case, invalid, manifest)

    def test_known_sum_defect_is_explicit_and_never_roundtrip_acceptance(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "vector-sum")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            vector = next((v for v in expected["vectors"] if v["name"] == identity["name"]), None)
            tracks.append({**identity, "samples": [], "vector_samples": copy.deepcopy(vector["samples"]) if vector else []})
        before = {"vector_sums": [{"name": "resultant", "members": [], "samples": []}], "tracks": tracks,
                  "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        defect = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                  "view_preparation": "expand_visit_custom_tables_restore_selection",
                  "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        saved = self.root / "known-defect.trk"
        xml = ('<object><property name="tracks"><property><object class="org.opensourcephysics.cabrillo.tracker.VectorSum">'
               '<property name="vectors" type="collection" class="java.util.ArrayList"/></object></property></property></object>')
        saved.write_text(xml, encoding="utf-8")
        report = corpus.validate_known_session_issue(case, defect, manifest, saved)
        self.assertFalse(report["compatibility_accepted"])
        self.assertEqual(report["verification_level"], "tracker_session_known_issue")
        self.assertEqual(report["expected_vector_sums"], case["session_sums"])
        with self.assertRaises(ValueError):
            corpus.validate_session(case, defect, manifest)
        changes = [(["vector_sums", 0, "members"], ["vector A"]),
                   (["vector_sums", 0, "samples"], case["session_sums"][0]["samples"]),
                   (["tracks", 3, "samples"], [{"frame": 0}]),
                   (["tracks", 3, "vector_samples"], [{"frame": 0}]),
                   (["tracks", 1, "vector_samples", 0, "components"], [0.0, 0.0]),
                   (["length_unit"], "wrong")]
        for path, value in changes:
            for stages in [["before", "after_save", "after"], ["after_save"], ["after"]]:
                invalid = copy.deepcopy(defect)
                for stage in stages:
                    target = invalid[stage]
                    for key in path[:-1]:
                        target = target[key]
                    target[path[-1]] = value
                with self.subTest(path=path, stages=stages), self.assertRaises(ValueError):
                    corpus.validate_known_session_issue(case, invalid, manifest, saved)
        for invalid_xml in [xml.replace('name="vectors"', 'name="other"'),
                            xml.replace('class="java.util.ArrayList"/>', 'class="java.util.ArrayList">'
                                        '<property name="item" type="string">vector A</property></property>'),
                            xml.replace('type="collection"', 'type="array"'),
                            xml.replace('class="java.util.ArrayList"/>', 'class="java.util.ArrayList">null</property>')]:
            saved.write_text(invalid_xml, encoding="utf-8")
            with self.subTest(xml=invalid_xml), self.assertRaises(ValueError):
                corpus.validate_known_session_issue(case, defect, manifest, saved)
        for invalid_case in [{**case, "id": "other"}, {**case, "session_known_issue": "other"}]:
            with self.assertRaises(ValueError):
                corpus.validate_known_session_issue(invalid_case, defect, manifest, saved)

    def test_partly_fixed_calibration_requires_independent_flags_keys_and_world_values(self):
        manifest = corpus.load_manifest()
        world = [
            [[-35, 17.5], [-17.5, -35], [-8.75, 4.375], [-8.75, 4.375], [-8.75, -35]],
            [[-55, -155], [-17.5, -35], [-7.5, -13.75], [-7.5, -13.75], [-13.75, -50]],
            [[-17.5, -35], [-17.5, -35], [-4.375, -8.75], [-4.375, -8.75], [-8.75, -35]],
            [[-38.75, 6.875], [-2.1875, -8.75], [-13.75, 3.75], [-13.75, 3.75], [-3.4375, -12.5]],
            [[-8.75, 2.1875], [-2.1875, -8.75], [-8.75, 2.1875], [-8.75, 2.1875], [-2.1875, -8.75]],
            [[-6.875, -38.75], [-2.1875, -8.75], [-3.75, -13.75], [-3.75, -13.75], [-3.4375, -12.5]]]
        for flags in range(1, 7):
            case = next(case for case in manifest["cases"] if case["id"] == f"mixed-calibration-{flags}")
            corpus.verify_file(corpus.local_fixture_path(case), case)
            for edited in [False, True]:
                expected = (corpus.mixed_calibration_roundtrip_expected(case) if edited else
                            json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"])
                calibration = corpus.mixed_calibration_expected(case, edited)
                mass = expected["point_masses"][0]
                if edited:
                    self.assertEqual([sample["world"] for sample in mass["samples"]], world[flags - 1])
                    self.assertEqual([key["frame"] for key in calibration["keys"]], [0, 4, 9] if flags == 3 else [0, 3, 4, 9])
                tracks = [{**case["session_tracks"][0], "samples": []},
                          {**case["session_tracks"][1], "mass": mass["mass"],
                           "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]} for s in mass["samples"]]}]
                before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                          "calibration": calibration, **corpus.session_units(case, expected)}
                valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                         "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                         "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
                corpus.validate_session(case, valid, manifest, expected, calibration=calibration)
                changes = [(["calibration", "fixed_origin"], not calibration["fixed_origin"]),
                           (["calibration", "fixed_angle"], not calibration["fixed_angle"]),
                           (["calibration", "fixed_scale"], not calibration["fixed_scale"]),
                           (["calibration", "keys", 0, "origin"], [999.0, 999.0]),
                           (["calibration", "keys", 0, "angle"], 45.0),
                           (["calibration", "keys", 0, "scale"], [999.0, 999.0]),
                           (["calibration", "keys"], []),
                           (["tracks", 1, "samples", 2, "world"], [999.0, 999.0])]
                for path, value in changes:
                    invalid = copy.deepcopy(valid)
                    for stage in ["before", "after_save", "after"]:
                        target = invalid[stage]
                        for part in path[:-1]:
                            target = target[part]
                        target[path[-1]] = value
                    with self.subTest(flags=flags, edited=edited, path=path), self.assertRaises(ValueError):
                        corpus.validate_session(case, invalid, manifest, expected, calibration=calibration)
        for invalid in [{"id": "other", "mixed_calibration_flags": 1},
                        *[{"id": f"mixed-calibration-{flags}", "mixed_calibration_flags": flags} for flags in [0, 7, None, True]]]:
            with self.assertRaises(ValueError):
                corpus.mixed_calibration_expected(invalid)

    def test_partly_fixed_xml_comparison_rejects_unrequested_changes(self):
        manifest = corpus.load_manifest()
        for flags in range(1, 7):
            case = next(case for case in manifest["cases"] if case["id"] == f"mixed-calibration-{flags}")
            source = corpus.local_fixture_path(case)
            tree = ET.parse(source)
            coords = tree.find("property[@name='coords']/object")
            data = coords.find("property[@name='framedata']")
            entries = {int(entry.attrib["name"][1:-1]): entry for entry in data}
            for key in corpus.mixed_calibration_expected(case, True, normalized=False)["keys"]:
                number = key["frame"]
                added_key = number not in entries
                if added_key:
                    entry = copy.deepcopy(entries[0]); entry.set("name", f"[{number}]")
                    data.insert(1, entry); entries[number] = entry
                obj = entries[number].find("object")
                values = key["origin"] + [key["angle"]] + key["scale"]
                nodes = [obj.find(f"property[@name='{name}']") for name in ["xorigin", "yorigin", "angle", "xscale", "yscale"]]
                if added_key or [float(node.text) for node in nodes] != values:
                    for node, value in zip(nodes, values):
                        node.text = format(value, ".17g")
            exported = self.root / "mixed.trk"
            tree.write(exported, encoding="utf-8", xml_declaration=True)
            corpus.verify_mixed_calibration_export_xml(case, source, exported)
            for xpath in ["property[@name='length_unit']", "property[@name='coords']/object/property[@name='fixedorigin']",
                          "property[@name='coords']/object/property[@name='framedata']/property/object/property[@name='angle']"]:
                invalid = copy.deepcopy(tree)
                invalid.find(xpath).text = "wrong"
                invalid.write(exported, encoding="utf-8", xml_declaration=True)
                with self.subTest(flags=flags, field=xpath), self.assertRaises(ValueError):
                    corpus.verify_mixed_calibration_export_xml(case, source, exported)

    def test_session_checks_reject_non_linux_hosts(self):
        with patch.object(corpus.sys, "platform", "win32"), self.assertRaisesRegex(ValueError, "Linux"):
            corpus.check_sessions({}, self.root, "java")

    def test_keyed_calibration_requires_independent_world_coordinates_at_each_boundary(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "keyed-calibration")
        corpus.verify_file(corpus.local_fixture_path(case), case)
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        mass = expected["point_masses"][0]
        corpus.compare([[s["frame"], *s["world"]] for s in mass["samples"]],
                       [[0, -155, 55], [3, -155, 55], [4, 12.5, 22.5], [8, 12.5, 22.5], [9, -13.75, -50]],
                       manifest["tolerance"])
        tracks = [{**case["session_tracks"][0], "samples": []},
                  {**case["session_tracks"][1], "mass": mass["mass"],
                   "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                               for s in mass["samples"]]}]
        before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        for index in range(len(mass["samples"])):
            invalid = copy.deepcopy(valid)
            for stage in ["before", "after_save", "after"]:
                invalid[stage]["tracks"][1]["samples"][index]["world"][0] += 1
            with self.subTest(frame=mass["samples"][index]["frame"]), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

    def test_keyed_calibration_export_preserves_other_keys_and_recorded_marks(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "keyed-calibration")
        original = corpus.local_fixture_path(case)
        xml = original.read_text(encoding="utf-8")
        edited = xml
        for name, before, after in [("xorigin", "100.0", "120"), ("yorigin", "120.0", "140"),
                                   ("angle", "90.0", "0"), ("xscale", "4.0", "8"), ("yscale", "8.0", "16")]:
            edited = edited.replace(f'name="{name}" type="double">{before}<', f'name="{name}" type="double">{after}<', 1)
        added = ('<property name="[3]" type="object">'
                 '<object class="org.opensourcephysics.media.core.ImageCoordSystem$FrameData">'
                 '<property name="xorigin" type="double">80</property><property name="yorigin" type="double">90</property>'
                 '<property name="angle" type="double">-90</property><property name="xscale" type="double">2</property>'
                 '<property name="yscale" type="double">4</property></object></property>\n')
        pos = edited.index('<property name="[4]"')
        edited = edited[:pos] + added + edited[pos:]
        output = self.root / "keyed.trk"
        output.write_text(edited, encoding="utf-8")
        corpus.verify_keyed_calibration_export_xml(original, output)
        for invalid in [xml, edited.replace('name="[3]"', 'name="[2]"', 1),
                        edited.replace('name="xorigin" type="double">110.0', 'name="xorigin" type="double">111'),
                        edited.replace('name="x" type="double">10.0', 'name="x" type="double">11', 1)]:
            output.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_keyed_calibration_export_xml(original, output)
        expected = corpus.keyed_calibration_roundtrip_expected(case)
        self.assertEqual([s["world"] for s in expected["point_masses"][0]["samples"]],
                         [[-155, 55], [-17.5, -35], [-13.75, 7.5], [-13.75, 7.5], [-13.75, -50]])

    def test_keyed_session_rejects_consistently_wrong_calibration_even_when_sample_positions_match(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "keyed-calibration")
        expected = corpus.keyed_calibration_roundtrip_expected(case)
        mass = expected["point_masses"][0]
        calibration = corpus.keyed_calibration_expected(edited=True)
        before = {"tracks": [{**case["session_tracks"][0]}, {**case["session_tracks"][1], "mass": mass["mass"],
                             "samples": [{"frame": s["frame"], "points": [s["image"]], "world": s["world"]}
                                         for s in mass["samples"]]}],
                  "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  "calibration": calibration, **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest, expected, calibration=calibration)
        for field, value in [("frame", 2), ("origin", [81, 90]), ("scale", [3, 4]), ("angle", 90)]:
            invalid = copy.deepcopy(valid)
            for stage in ["before", "after_save", "after"]:
                invalid[stage]["calibration"]["keys"][1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest, expected, calibration=calibration)
        for value in [True, "false"]:
            invalid = copy.deepcopy(valid)
            for stage in ["before", "after_save", "after"]:
                invalid[stage]["calibration"]["fixed_origin"] = value
            with self.subTest(flag=value), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest, expected, calibration=calibration)

    def test_local_vector_fixture_is_pinned_and_cannot_escape_the_corpus(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "independent-vectors")
        local = corpus.local_fixture_path(case)
        corpus.verify_file(local, case)
        self.assertEqual(corpus.project_path(case, self.root), local)
        self.assertIn((local, case), list(corpus.artifacts(manifest, self.root)))
        with self.assertRaisesRegex(ValueError, "inside"):
            corpus.local_fixture_path({**case, "local_file": "../outside.trk"})
        case["url"] = "https://example.org/conflicting-source.trk"
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Invalid corpus artifact"):
            corpus.load_manifest(path)

    def test_snapping_oracle_requires_the_actual_tracker_geometry_change(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "independent-vectors")
        for mode, track, before, after in [("snap-origin", 0, [321.0, 240.0], [320.0, 240.0]),
                                           ("snap-vector", 1, [14.0, 16.0], [13.0, 16.0])]:
            expected = corpus.snapping_roundtrip_expected(case, mode)
            self.assertEqual(expected["vectors"][track]["samples"][0]["tail"], after)
            wrong = copy.deepcopy(expected)
            wrong["vectors"][track]["samples"][0]["tail"] = before
            with self.assertRaises(ValueError):
                corpus.compare(expected, wrong, manifest["tolerance"])

    def test_vector_session_requires_loaded_tails_components_and_frames(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "independent-vectors")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            vector = next((v for v in expected["vectors"] if v["name"] == identity["name"]), None)
            tracks.append({**identity, "vector_samples": copy.deepcopy(vector["samples"]) if vector else []})
        before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        for field, value in [("tail", [11.0, 20.0]), ("components", [3.0, 4.0]), ("frame", 1)]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state]["tracks"][1]["vector_samples"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

        for field in ["length_unit", "mass_unit", "time_unit"]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state][field] = "wrong-unit"
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

    def test_time_unit_export_preserves_timing_measurements_and_unknown_content(self):
        original, exported = self.root / "time-source.trk", self.root / "time-export.trk"
        xml = ('<object><property name="starttime" type="double">-500.0</property>'
               '<property name="delta_t" type="double">100.0</property><!-- keep --></object>')
        original.write_text(xml, encoding="utf-8")
        field = '<property name="time_unit" type="string"><![CDATA[' + corpus.TIME_UNIT_LABEL + ']]></property>'
        edited = xml.replace('</object>', field + '</object>')
        exported.write_text(edited, encoding="utf-8")
        corpus.verify_time_unit_export_xml(original, exported)
        for invalid in [xml, edited.replace(corpus.TIME_UNIT_LABEL, "s"), edited.replace('>100.0<', '>1.0<'),
                        edited.replace('>-500.0<', '>0.0<'), edited.replace('<!-- keep -->', '')]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_time_unit_export_xml(original, exported)
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "protractor")
        baseline = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        self.assertEqual(corpus.time_unit_roundtrip_expected(case), {**baseline, "time_unit": corpus.TIME_UNIT_LABEL})

    def test_time_unit_session_requires_independent_label_and_unscaled_times(self):
        manifest = corpus.load_manifest()
        original = next(case for case in manifest["cases"] if case["id"] == "protractor")
        times = [{"frame": 0, "time_ms": 0.0}, {"frame": 9, "time_ms": 900.0}]
        case = {**original, "session_time_samples": times}
        expected = corpus.time_unit_roundtrip_expected(case)
        tracks = []
        for identity in case["session_tracks"]:
            mass = next((m for m in expected["point_masses"] if m["name"] == identity["name"]), None)
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": x["frame"], "points": [x["image"]], "world": x["world"]}
                                       for x in mass["samples"]] if mass else []})
        before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  "name_references": copy.deepcopy(case["session_names"]), "saved_views": copy.deepcopy(case["session_views"]),
                  "time_samples": copy.deepcopy(times), **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                 "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest, expected)
        for field, value in [("time_unit", "s"), ("time_samples", [{"frame": 0, "time_ms": 0.0}, {"frame": 9, "time_ms": 0.9}]),
                             ("time_samples", []), ("time_samples", [{"frame": 0, "time_ms": 0.0}, {"frame": 8, "time_ms": 900.0}])]:
            invalid = copy.deepcopy(valid)
            for stage in ["before", "after_save", "after"]:
                invalid[stage][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest, expected)

    def test_clip_timing_xml_oracle_preserves_unrelated_playback_and_content(self):
        original, exported = self.root / "timing-source.trk", self.root / "timing-export.trk"
        xml = ('<object><object class="org.opensourcephysics.media.core.VideoClip">'
               '<property name="startframe" type="int">0</property><property name="stepsize" type="int">1</property>'
               '<property name="stepcount" type="int">10</property><property name="starttime" type="double">0.0</property></object>'
               '<object class="org.opensourcephysics.media.core.StepperClipControl">'
               '<property name="rate" type="double">1.25</property><property name="frame" type="int">0</property>'
               '<property name="delta_t" type="double">100.0</property></object><!-- keep --></object>')
        original.write_text(xml, encoding="utf-8")
        for ranged in [False, True]:
            edited = xml.replace('>0.0<', '>-500<').replace('>100.0<', '>250<')
            if ranged:
                edited = edited.replace('name="startframe" type="int">0<', 'name="startframe" type="int">2<')
                edited = edited.replace('name="stepsize" type="int">1<', 'name="stepsize" type="int">2<')
                edited = edited.replace('name="stepcount" type="int">10<', 'name="stepcount" type="int">4<')
            exported.write_text(edited, encoding="utf-8")
            corpus.verify_clip_timing_export_xml(original, exported, ranged)
            for invalid in [xml, edited.replace('>250<', '>500<'), edited.replace('>-500<', '>0<'),
                            edited.replace('>1.25<', '>1<'), edited.replace('<!-- keep -->', ''),
                            edited.replace('name="frame" type="int">0<', 'name="frame" type="int">2<')]:
                with self.subTest(ranged=ranged, invalid=invalid):
                    exported.write_text(invalid, encoding="utf-8")
                    with self.assertRaises(ValueError):
                        corpus.verify_clip_timing_export_xml(original, exported, ranged)

    def test_clip_timing_requires_independent_live_and_native_intervals_and_times(self):
        manifest = corpus.load_manifest()
        original = next(case for case in manifest["cases"] if case["id"] == "protractor")
        for ranged in [False, True]:
            expected = corpus.clip_timing_roundtrip_expected(original, ranged)
            timing = corpus.clip_timing_expected(ranged)
            case = {**original, "session_frame_duration_ms": 250.0, "session_time_samples": timing["time_samples"]}
            tracks = []
            for identity in case["session_tracks"]:
                mass = next((m for m in expected["point_masses"] if m["name"] == identity["name"]), None)
                tracks.append({**identity, "mass": mass["mass"] if mass else None,
                               "samples": [{"frame": x["frame"], "points": [x["image"]], "world": x["world"]}
                                           for x in mass["samples"]] if mass else []})
            before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                      "name_references": copy.deepcopy(case["session_names"]), "saved_views": copy.deepcopy(case["session_views"]),
                      **copy.deepcopy(timing), **corpus.session_units(case, expected)}
            valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                     "view_preparation": "expand_visit_custom_tables_restore_selection", "before": before,
                     "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
            corpus.validate_session(case, valid, manifest, expected)
            mutations = [("frame_duration_ms", 100.0), ("frame_duration_ms", None), ("time_samples", []),
                         ("time_samples", [{"frame": 2 if ranged else 0, "time_ms": 0.0},
                                           {"frame": 8 if ranged else 9, "time_ms": 1500.0 if ranged else 2250.0}])]
            for field, value in mutations:
                invalid = copy.deepcopy(valid)
                for stage in ["before", "after_save", "after"]:
                    invalid[stage][field] = value
                with self.subTest(ranged=ranged, field=field), self.assertRaises(ValueError):
                    corpus.validate_session(case, invalid, manifest, expected)
            output = self.root / "native-timing.json"
            output.write_text(json.dumps(timing), encoding="utf-8")
            with patch.object(corpus, "run") as run:
                corpus.check_native_clip_timing("probe", self.root / "input.trk", output, manifest["tolerance"], ranged)
                self.assertEqual(run.call_args.args[0][1], "--timing")
                for field, value in mutations:
                    output.write_text(json.dumps({**timing, field: value}), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        corpus.check_native_clip_timing("probe", self.root / "input.trk", output, manifest["tolerance"], ranged)

    def test_mass_unit_export_preserves_mass_values_and_unknown_content(self):
        original = self.root / "original.trk"
        exported = self.root / "mass-unit.trk"
        xml = ('<object><property name="mass_unit" type="string">kg</property>'
               '<property name="mass" type="double">2.5</property><!-- keep --></object>')
        original.write_text(xml, encoding="utf-8")
        edited = xml.replace('>kg<', '><![CDATA[g]]><')
        exported.write_text(edited, encoding="utf-8")
        corpus.verify_mass_unit_export_xml(original, exported)
        for invalid in [xml, edited.replace('[g]', '[kg]'), edited.replace('>2.5<', '>3.5<'), edited.replace('<!-- keep -->', '')]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_mass_unit_export_xml(original, exported)
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "protractor")
        expected = corpus.mass_unit_roundtrip_expected(case)
        original_project = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        self.assertEqual(expected, {**original_project, "mass_unit": "g"})

    def test_length_unit_export_preserves_calibration_and_geometry(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "independent-vectors")
        original = corpus.local_fixture_path(case)
        exported = self.root / "unit.trk"
        xml = original.read_text(encoding="utf-8")
        pos = xml.rfind("</object>")
        edited = xml[:pos] + '<property name="length_unit" type="string"><![CDATA[\u00b5m]]></property>' + xml[pos:]
        exported.write_text(edited, encoding="utf-8")
        corpus.verify_length_unit_export_xml(original, exported)
        for invalid in [xml, edited.replace('\u00b5m', 'm'),
                        edited.replace('name="xscale" type="double">2.0', 'name="xscale" type="double">3.0'),
                        edited.replace('name="xtail" type="double">10.0', 'name="xtail" type="double">11.0')]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_length_unit_export_xml(original, exported)
        expected = corpus.length_unit_roundtrip_expected(case)
        self.assertEqual(expected["length_unit"], "\u00b5m")
        self.assertEqual(expected["coordinate_samples"], corpus.vector_roundtrip_expected(case)["coordinate_samples"])

    def test_vector_export_rejects_unrequested_geometry_and_unknown_content_changes(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "independent-vectors")
        original = corpus.local_fixture_path(case)
        exported = self.root / "exported.trk"
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        xml = original.read_text(encoding="utf-8")
        expected["vectors"][0]["samples"][0]["tail"][0] = 22.5
        edited = xml.replace('name="xtail" type="double">10.0', 'name="xtail" type="double">22.5', 1)
        for name, before, after in [("ytail", "20.0", "20"), ("xcomponent", "3.0", "3"), ("ycomponent", "-4.0", "-4")]:
            edited = edited.replace(f'name="{name}" type="double">{before}', f'name="{name}" type="double">{after}', 1)
        exported.write_text(edited, encoding="utf-8")
        corpus.verify_vector_export_xml(original, exported, expected)
        for invalid in [xml, edited.replace('>22.5<', '>23.5<', 1),
                        edited.replace('>Footprint.BoldArrow<', '>Footprint.Arrow<', 1),
                        edited.replace('name="[5]"', 'name="[6]"', 1)]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_vector_export_xml(original, exported, expected)

    def test_calibration_export_retains_geometry_timing_and_unknown_content(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "independent-vectors")
        original = corpus.local_fixture_path(case)
        exported = self.root / "calibration.trk"
        xml = original.read_text(encoding="utf-8")
        edited = xml
        for name, before, after in [("xorigin", "320.0", "100"), ("yorigin", "240.0", "120"),
                                     ("angle", "0.0", "90"), ("xscale", "2.0", "4"), ("yscale", "4.0", "8")]:
            edited = edited.replace(f'name="{name}" type="double">{before}', f'name="{name}" type="double">{after}', 1)
        exported.write_text(edited, encoding="utf-8")
        corpus.verify_calibration_export_xml(original, exported)
        for invalid in [xml, edited.replace('name="xtail" type="double">10.0', 'name="xtail" type="double">11.0'),
                        edited.replace('>Footprint.BoldArrow<', '>Footprint.Arrow<', 1)]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_calibration_export_xml(original, exported)
        expected = corpus.calibration_roundtrip_expected(case)
        self.assertEqual([sample["world"] for sample in expected["coordinate_samples"]], [[12.84375, 21.625]] * 2)

    def test_clip_export_retains_tracks_timing_and_unknown_content(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "independent-vectors")
        original = corpus.local_fixture_path(case)
        exported = self.root / "clip.trk"
        xml = original.read_text(encoding="utf-8")
        edited = xml
        for name, before, after in [("startframe", "0", "2"), ("stepsize", "1", "2"), ("stepcount", "10", "4")]:
            edited = edited.replace(f'name="{name}" type="int">{before}', f'name="{name}" type="int">{after}', 1)
        exported.write_text(edited, encoding="utf-8")
        corpus.verify_clip_export_xml(original, exported)
        for invalid in [xml, edited.replace('name="[0]"', 'name="[1]"'),
                        edited.replace('>Footprint.BoldArrow<', '>Footprint.Arrow<', 1),
                        edited.replace('name="starttime" type="double">0.0', 'name="starttime" type="double">10.0')]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_clip_export_xml(original, exported)
        expected = corpus.clip_roundtrip_expected(case)
        self.assertEqual(expected["clip"], {"first_frame": 2, "step_size": 2, "step_count": 4, "start_ms": 0.0})
        self.assertEqual([sample["frame"] for sample in expected["coordinate_samples"]], [2, 8])
        self.assertEqual([sample["frame"] for sample in expected["vectors"][0]["samples"]], [0, 5])
        boundary = corpus.clip_roundtrip_expected(case, boundary=True)
        self.assertEqual(boundary["clip"], {"first_frame": 500000, "step_size": 1, "step_count": 1, "start_ms": 0.0})
        self.assertEqual(boundary["vectors"], [])
        self.assertEqual([sample["frame"] for sample in boundary["coordinate_samples"]], [500000, 500000])

    def test_mark_session_requires_edited_samples_and_manual_keys(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "protractor")
        expected = corpus.mark_roundtrip_expected(case)
        tracks = []
        keys = [[0, 3], [7]]
        for identity in case["session_tracks"]:
            index = next((i for i, mass in enumerate(expected["point_masses"]) if mass["name"] == identity["name"]), None)
            mass = expected["point_masses"][index] if index is not None else None
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": sample["frame"], "points": [sample["image"]], "world": sample["world"]}
                                       for sample in mass["samples"]] if mass else [],
                           "key_frames": keys[index] if mass else None})
        before = {"saved_views": copy.deepcopy(case["session_views"]),
                  "name_references": copy.deepcopy(case["session_names"]),
                  "tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"],
                  **corpus.session_units(case, expected)}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "view_preparation": "expand_visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest, expected, keys)
        for field, value in [("key_frames", [0]), ("samples", tracks[1]["samples"][:1])]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state]["tracks"][1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest, expected, keys)

    def test_mark_export_checks_changes_and_preserves_unknown_content(self):
        case = next(case for case in corpus.load_manifest()["cases"] if case["id"] == "protractor")
        expected = corpus.mark_roundtrip_expected(case)
        point_class = "org.opensourcephysics.cabrillo.tracker.PointMass"
        def frame(number, x, y):
            return (f"<property name='[{number}]' type='object'><object class='{point_class}$FrameData'>"
                    f"<property name='x' type='double'>{x}</property><property name='y' type='double'>{y}</property>"
                    "</object></property>")
        def mass(frames, keys):
            values = "" if keys is None else f"<property name='array' type='string'>{{{keys}}}</property>"
            return (f"<property type='object'><object class='{point_class}'>"
                    f"<property name='framedata' type='array' class='[L{point_class}$FrameData;'>{frames}</property>"
                    f"<property name='keyFrames' type='array' class='[I'>{values}</property>"
                    "<property name='unknown'>  keep  </property></object></property>")
        def project(first, second):
            return "<!-- before --><object><property name='tracks' type='collection'>" + first + second + "</property></object>"
        original, exported = self.root / "original.trk", self.root / "exported.trk"
        original.write_text(project(mass(frame(0, 1, 2), "0"), mass(frame(0, 3, 4), "0")), encoding="utf-8")
        x, y = expected["point_masses"][0]["samples"][0]["image"]
        edited = project(mass(frame(0, format(x, '.17g'), format(y, '.17g')) + frame(3, 100.125, 200.5), "0,3"),
                         mass(frame(7, 50, 60), "7"))
        exported.write_text(edited, encoding="utf-8")
        corpus.verify_mark_export_xml(original, exported, expected)
        for invalid in [edited.replace("{0,3}", "{0}"), edited.replace("[7]", "[6]"),
                        edited.replace(">50<", ">51<"), edited.replace("  keep  ", "keep"),
                        edited.replace("<!-- before -->", "")]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_mark_export_xml(original, exported, expected)
        empty = project(mass("\n ", None), mass(frame(0, 3, 4), "0"))
        exported.write_text(empty, encoding="utf-8")
        corpus.verify_mark_export_xml(original, exported, corpus.mark_roundtrip_expected(case, "empty-marks"), "empty-marks")
        original.write_text(empty, encoding="utf-8")
        first = project(mass(frame(4, 11.25, 22.5), "4"), mass(frame(0, 3, 4), "0"))
        exported.write_text(first, encoding="utf-8")
        corpus.verify_mark_export_xml(original, exported, corpus.mark_roundtrip_expected(case, "first-mark"), "first-mark")
        exported.write_text(first.replace("{4}", "{}"), encoding="utf-8")
        with self.assertRaises(ValueError):
            corpus.verify_mark_export_xml(original, exported, corpus.mark_roundtrip_expected(case, "first-mark"), "first-mark")

    def test_mass_export_checks_unknown_content_and_the_requested_edit(self):
        original, exported = self.root / "original.trk", self.root / "exported.trk"
        xml = ("<!-- before --><?test keep?><object><property name='tracks'><property><object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
               "<property name='mass' type='double'>1</property></object></property></property>"
               "<!-- keep --><property name='unknown'>unchanged</property></object><!-- after -->")
        original.write_text(xml, encoding="utf-8")
        exported.write_text(xml.replace(">1<", ">2.5<"), encoding="utf-8")
        corpus.verify_mass_export_xml(original, exported)
        for invalid in [xml, xml.replace(">1<", ">2.5<").replace("<!-- keep -->", ""),
                        xml.replace(">1<", ">2.5<").replace("<!-- before -->", ""),
                        xml.replace(">1<", ">2.5<").replace("<!-- after -->", ""),
                        xml.replace(">1<", ">2.5<").replace("<?test keep?>", ""),
                        xml.replace(">1<", ">2.5<").replace("unchanged", "lost")]:
            exported.write_text(invalid, encoding="utf-8")
            with self.assertRaises(ValueError):
                corpus.verify_mass_export_xml(original, exported)

    def test_roundtrip_cannot_pass_without_an_applicable_case(self):
        with patch.object(corpus.sys, "platform", "linux"), self.assertRaisesRegex(ValueError, "No applicable"):
            corpus.check_sessions({"cases": []}, self.root, "java", "probe")

    def test_mass_export_preserves_scalar_mixed_and_explicit_content_whitespace(self):
        original, exported = self.root / "original.trk", self.root / "exported.trk"
        xml = ("<object><property name='tracks' type='collection'><property type='object'>"
               "<object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
               "<property name='mass' type='double'>1</property></object></property></property>"
               "<property name='unknown' type='string'>  keep  </property>"
               "<property name='blank' type='string'>   </property>"
               "<property name='mixed'> before <object/> after </property>"
               "<object xml:space='preserve'> <property name='nested'>value</property> </object></object>")
        original.write_text(xml, encoding="utf-8")
        edited = xml.replace(">1<", ">2.5<")
        exported.write_text(edited.replace("<object>", "<object>\n  ", 1), encoding="utf-8")
        corpus.verify_mass_export_xml(original, exported)
        for before, after in [("  keep  ", "keep"), (">   <", "><"), (" before ", "before"),
                              ("preserve'> <property", "preserve'><property")]:
            exported.write_text(edited.replace(before, after), encoding="utf-8")
            with self.subTest(before=before), self.assertRaises(ValueError):
                corpus.verify_mass_export_xml(original, exported)

    def test_mass_export_preserves_decoded_character_references(self):
        original, exported = self.root / "original.trk", self.root / "exported.trk"
        xml = ("<object><property name='tracks' type='collection'><property type='object'>"
               "<object class='org.opensourcephysics.cabrillo.tracker.PointMass'>"
               "<property name='mass' type='double'>1</property></object></property></property>"
               "<property name='unknown' type='string' custom='left&#13;right'>left&#13;right</property>"
               "<object>\u00a0<property name='nested'>value</property></object></object>")
        original.write_text(xml, encoding="utf-8")
        edited = xml.replace(">1<", ">2.5<")
        exported.write_text(edited.replace("&#13;", "&#xD;"), encoding="utf-8")
        corpus.verify_mass_export_xml(original, exported)
        for before, after in [("left&#13;right</", "left&#10;right</"),
                              ("custom='left&#13;right'", "custom='left&#10;right'"),
                              ("\u00a0", "")]:
            exported.write_text(edited.replace(before, after), encoding="utf-8")
            with self.subTest(before=before), self.assertRaises(ValueError):
                corpus.verify_mass_export_xml(original, exported)

    def test_ordinary_desktop_display_is_replaced_by_an_owned_server(self):
        server = Mock()
        server.poll.return_value = None

        def start(command, **kwargs):
            writer = int(command[command.index("-displayfd") + 1])
            self.assertEqual(kwargs["pass_fds"], (writer,))
            corpus.os.write(writer, b"77\n")
            return server

        with patch.object(corpus.sys, "platform", "linux"), \
                patch.dict(corpus.os.environ, DISPLAY=":0", WAYLAND_DISPLAY="wayland-0", XAUTHORITY="desktop-cookie"), \
                patch.object(corpus.shutil, "which", return_value="/usr/bin/Xvfb"), \
                patch.object(corpus.subprocess, "Popen", side_effect=start), \
                patch.object(corpus.select, "select", side_effect=lambda readers, *_: (readers, [], [])):
            with corpus.private_display() as display:
                self.assertEqual(display, ":77")
                self.assertEqual(corpus.os.environ["DISPLAY"], ":0")
                with patch.object(corpus.subprocess, "run", return_value=Mock(returncode=0)) as run:
                    corpus.run(["java"], display=display)
                    self.assertEqual(run.call_args.kwargs["env"]["DISPLAY"], ":77")
                    self.assertNotIn("WAYLAND_DISPLAY", run.call_args.kwargs["env"])
                    self.assertNotIn("XAUTHORITY", run.call_args.kwargs["env"])
        server.terminate.assert_called_once()
        server.wait.assert_called_once_with(timeout=5)

    def test_display_startup_timeout_terminates_only_its_owned_server(self):
        server = Mock()
        server.poll.return_value = None
        server.wait.side_effect = [corpus.subprocess.TimeoutExpired("Xvfb", 5), 0]
        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus.shutil, "which", return_value="/usr/bin/Xvfb"), \
                patch.object(corpus.subprocess, "Popen", return_value=server), \
                patch.object(corpus.select, "select", return_value=([], [], [])):
            with self.assertRaisesRegex(ValueError, "did not become ready"), corpus.private_display():
                self.fail("A display that never became ready must not be yielded")
        server.terminate.assert_called_once()
        server.kill.assert_called_once()

    def test_failed_archive_session_cleans_its_private_home_and_outputs(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [next(case for case in manifest["cases"] if case["id"] == "archived-images")]
        calls = []
        (self.root / "fixtures").mkdir()
        (self.root / "fixtures/protractor.trk").write_text("<object/>", encoding="utf-8")

        def fail_session(command, *, display=None):
            calls.append(command)
            if "TrackerSessionReference" in command:
                self.assertEqual(display, ":77")
                private_home = Path(next(arg.split("=", 1)[1] for arg in command if arg.startswith("-Duser.home=")))
                self.assertTrue(private_home.is_relative_to(self.root))
                self.assertTrue(private_home.is_dir())
                (private_home / "preferences.xml").write_text("private", encoding="utf-8")
                raise ValueError("session failure")

        with patch.object(corpus.sys, "platform", "linux"), patch.dict(corpus.os.environ, DISPLAY=":99"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=fail_session):
            with self.assertRaisesRegex(ValueError, "session failure") as raised:
                corpus.check_sessions(manifest, self.root, "java")
            self.assertIn('Tracker resave diagnostic (bounded): {"inspection_error":', str(raised.exception))
        self.assertEqual(len(calls), 3)
        self.assertEqual(calls[1][-1], "TrackerSessionDiagnosticsTest")
        arguments = calls[2][calls[2].index("TrackerSessionReference") + 1:]
        self.assertEqual(arguments[2], "save")
        self.assertEqual(Path(arguments[0]), corpus.local_fixture_path(manifest["cases"][0]))
        self.assertEqual(Path(arguments[3]).suffix, ".trk")
        self.assertNotEqual(arguments[0], arguments[3])
        self.assertEqual(list(self.root.glob("session-*")), [])

    def test_session_file_diagnostic_distinguishes_xml_errors_without_modifying_input(self):
        source = self.root / "diagnostic.trk"
        cases = [b'<object class="TrackerPanel"/>', b'<object>\n<property>&broken;</property>\n</object>',
                 b'<object>\n<property>', b""]
        for data in cases:
            with self.subTest(data=data):
                source.write_bytes(data)
                report = corpus.session_file_diagnostic(source)
                self.assertEqual(report["bytes"], len(data))
                self.assertEqual(report["sha256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(source.read_bytes(), data)
                self.assertEqual(report["xml"], "well-formed" if data == cases[0] else "malformed")
                if data == cases[0]:
                    self.assertEqual(report["root_class"], "TrackerPanel")
                elif data:
                    self.assertIn("error_line_excerpt", report)
        source.write_bytes(b"<object>" + b"x" * 3000 + b"&broken;</object>")
        report = corpus.session_file_diagnostic(source)
        self.assertIn("&broken;", report["error_line_excerpt"])
        self.assertLessEqual(len(report["error_line_excerpt"]), 512)

    def test_session_file_diagnostic_bounds_reads_and_handles_missing_input(self):
        source = self.root / "oversized.trk"
        source.write_bytes(b"x" * (1024 * 1024 + 1))
        self.assertEqual(corpus.session_file_diagnostic(source),
                         {"inspection": "skipped: input exceeds 1 MiB diagnostic limit"})
        self.assertIn("inspection_error", corpus.session_file_diagnostic(self.root / "missing.trk"))
        source.write_bytes(b'<?xml version="1.0" encoding="no-such-encoding"?><object/>')
        self.assertIn("inspection_error", corpus.session_file_diagnostic(source))

    def test_corrupt_byte_excerpts_cannot_truncate_parser_diagnostics(self):
        source = self.root / "corrupt.trk"
        for value in [b"<object>" + b"\0" * 800,
                      ('<object class="' + "\U0001f52c" * 256 + '">' + "\U0001f52c" * 800 + '</object>').encode("utf-8")]:
            with self.subTest(prefix=value[:15]):
                source.write_bytes(value)
                formatted = corpus.format_session_file_diagnostic(source)
                self.assertLessEqual(len(formatted), 3000)
                report = json.loads(formatted)
                self.assertEqual(report["bytes"], len(value))
                self.assertEqual(report["sha256"], hashlib.sha256(value).hexdigest())
                self.assertIn("xml", report)
                if b"\0" in value:
                    self.assertEqual(report["xml"], "malformed")
                    self.assertIn("parse_error", report)
                else:
                    self.assertEqual(report["xml"], "well-formed")
                    self.assertIn("root_class", report)
                self.assertEqual(source.read_bytes(), value)

    def test_session_failure_context_distinguishes_save_reopen_and_preserves_cleanup_without_retry(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [next(case for case in manifest["cases"] if case["id"] == "protractor")]
        (self.root / "fixtures").mkdir()
        (self.root / "fixtures/protractor.trk").write_text("<object/>", encoding="utf-8")
        for failing_stage in ["save", "reopen"]:
            for failure in [ValueError("loader callback timed out"), OSError("could not launch Java"),
                            corpus.subprocess.TimeoutExpired(["java"], 45)]:
                calls = []
                def session(command, *, display=None):
                    if "TrackerSessionReference" not in command:
                        return
                    source, output, stage = command[-3:]
                    calls.append(stage)
                    self.assertEqual(display, ":77")
                    if stage == failing_stage:
                        if stage == "save":
                            (Path(output) / "tracker-resaved.trk").write_bytes(b"")
                        raise failure
                    output = Path(output)
                    (output / "tracker-resaved.trk").write_text("<object>\n<property>", encoding="utf-8")
                    report = {"reference_version": "6.3.5", "verification_level": "tracker_session_save", "loader_callbacks": 2,
                              "view_preparation": "expand_visit_custom_tables_restore_selection", "loaded": {}, "after_save": {}}
                    (output / "session-save.json").write_text(json.dumps(report), encoding="utf-8")
                with self.subTest(stage=failing_stage, error=type(failure).__name__), \
                        patch.object(corpus.sys, "platform", "linux"), \
                        patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                        patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=session):
                    with self.assertRaisesRegex(ValueError, "protractor/session/" + failing_stage) as raised:
                        corpus.check_sessions(manifest, self.root, "java")
                    self.assertIs(raised.exception.__cause__, failure)
                    self.assertIn("protractor.trk" if failing_stage == "save" else "tracker-resaved.trk", str(raised.exception))
                    self.assertIn('Tracker input diagnostic (bounded):', str(raised.exception))
                    self.assertIn('"xml": "well-formed"' if failing_stage == "save" else '"xml": "malformed"',
                                  str(raised.exception))
                    if failing_stage == "save":
                        self.assertIn('Tracker resave diagnostic (bounded): {"bytes": 0,', str(raised.exception))
                        self.assertIn('"parse_error": "no element found:', str(raised.exception))
                    else:
                        self.assertNotIn('Tracker resave diagnostic', str(raised.exception))
                    self.assertEqual(calls, ["save"] if failing_stage == "save" else ["save", "reopen"])
                    self.assertEqual(list(self.root.glob("session-*")), [])

    def test_reopen_runs_in_a_separate_process_and_private_home(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [next(case for case in manifest["cases"] if case["id"] == "protractor")]
        (self.root / "fixtures").mkdir()
        (self.root / "fixtures/protractor.trk").write_text("<object/>", encoding="utf-8")
        calls = []
        diagnostics = []
        def session(command, *, display=None):
            if "TrackerSessionDiagnosticsTest" in command:
                diagnostics.append(command)
                self.assertIsNone(display)
            if "TrackerSessionReference" not in command:
                return
            source, output, stage = command[-3:]
            calls.append((source, next(arg for arg in command if arg.startswith("-Duser.home=")), stage))
            output = Path(output)
            if stage == "save":
                (output / "tracker-resaved.trk").write_text("<object/>", encoding="utf-8")
            else:
                self.assertEqual(Path(source).name, "tracker-resaved.trk")
                self.assertTrue(Path(source).is_file())
            report = {"reference_version": "6.3.5", "verification_level": "tracker_session_" + stage, "loader_callbacks": 2,
                      "view_preparation": "expand_visit_custom_tables_restore_selection",
                      "loaded": {"stage": stage}, "after_save": {"stage": "after_save"}}
            (output / ("session-" + stage + ".json")).write_text(json.dumps(report), encoding="utf-8")
        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=session), \
                patch.object(corpus, "validate_session") as validate:
            corpus.check_sessions(manifest, self.root, "java")
        self.assertEqual([call[2] for call in calls], ["save", "reopen"])
        self.assertEqual(len(diagnostics), 1)
        self.assertIn("-Djava.awt.headless=true", diagnostics[0])
        self.assertIn("-Xmx256m", diagnostics[0])
        self.assertTrue(any(arg.startswith("-Duser.home=") and arg.endswith("diagnostic-home") for arg in diagnostics[0]))
        self.assertNotEqual(calls[0][1], calls[1][1])
        report = validate.call_args.args[1]
        self.assertEqual(report["before"], {"stage": "save"})
        self.assertEqual(report["after_save"], {"stage": "after_save"})
        self.assertEqual(report["after"], {"stage": "reopen"})
        self.assertEqual(list(self.root.glob("session-*")), [])


TRACKER = "org.opensourcephysics.cabrillo.tracker."


def tracker_state(mass_name="mass A", members=("vector A",)):
    """A small, schema-complete Tracker dump: axes, one point mass and one vector sum."""
    columns = [("t", "T"), ("x", "L"), ("y", "L"), ("v_{x}", "L/T"), ("a_{x}", "L/TT"), ("θ_{v}", "A"),
               ("step", "I"), ("frame", "I")]
    rows = [[0.0, 1.0, 2.0, "NaN", "NaN", "NaN", 0.0, 0.0],
            [0.1, 1.5, 2.5, 5.0, "NaN", 0.0, 1.0, 1.0],
            [0.2, 2.0, 3.0, "NaN", "NaN", "NaN", 2.0, 2.0]]
    data = {"columns": [{"name": name, "dimensions": dims} for name, dims in columns], "constants": {"m": 2.0}, "rows": rows}
    sum_data = {"columns": [{"name": "t", "dimensions": "T"}, {"name": "x", "dimensions": "L"}, {"name": "frame", "dimensions": "I"}],
                "constants": {}, "rows": [] if not members else [[0.0, 5.0, 0.0]]}
    return {
        "schema_version": 1,
        "producer": {"kind": "tracker", "version": "6.3.5", "loading": "tracker_session",
                     "view_preparation": corpus.STATE_PREPARATION},
        "panel": {
            "units": {"length": "m", "mass": "kg", "time": "s", "angles_in_radians": False},
            "selected_track": mass_name,
            "properties": {"@class": TRACKER + "TrackerPanel", "width": 640.0, "length_unit": "m"},
            "clip": {"first_frame": 0, "step_size": 1, "step_count": 3, "end_frame": 2, "frame_count": 3, "frame_shift": 0,
                     "extra_frames": 0, "start_time_ms": 0.0, "frame_duration_ms": 100.0, "rate": 1.0, "looping": False,
                     "steps": [[0, 0, 0.0], [1, 1, 100.0], [2, 2, 200.0]]},
            "video": None,
            "coords": {"class": "org.opensourcephysics.media.core.ImageCoordSystem", "fixed_origin": True, "fixed_angle": True,
                       "fixed_scale": True, "locked": False, "key_frames": [],
                       "frames": [[frame, 0.0, 0.0, 0.0, 1.0, 1.0, [1.0, 0.0, 0.0, -1.0, 0.0, 0.0]] for frame in range(3)]},
            "tracks": [
                {"class": TRACKER + "CoordAxes", "name": "axes", "properties": {"name": "axes"}, "steps": [],
                 "data": {"columns": [], "constants": {}, "rows": []}},
                {"class": TRACKER + "PointMass", "name": mass_name,
                 "properties": {"mass": 2.0, "name": mass_name, "keyFrames": {"@array": "[I", "items": [0, 2]}},
                 "steps": [{"frame": frame, "position": [1.0 + frame / 2, -2.0 - frame / 2]} for frame in range(3)], "data": data},
                {"class": TRACKER + "VectorSum", "name": "resultant",
                 "properties": {"vectors": {"@collection": "java.util.ArrayList", "items": list(members)}},
                 "steps": [] if not members else [{"frame": 0, "tail": [1.0, 2.0], "tip": [6.0, 2.0], "components": [5.0, 0.0]}],
                 "data": sum_data}],
            "views": {"tables": [], "plots": []}}}


def motrack_state(tracker=None):
    """MoTrack's dump of the same experiment: units, clip, coordinates and the point mass only."""
    tracker = tracker or tracker_state()
    mass = tracker["panel"]["tracks"][1]
    keep = ["frame", "step", "t", "x", "y", "v_{x}"]
    indices = [i for i, column in enumerate(mass["data"]["columns"]) if column["name"] in keep]
    return {
        "schema_version": 1,
        "producer": {"kind": "motrack", "supported_classes": [TRACKER + "PointMass", TRACKER + "Vector"]},
        "panel": {
            "units": {"mass": "kg", "time": "s", "length": "m"},
            "clip": {"first_frame": 0, "step_size": 1, "step_count": 3, "end_frame": 2, "start_time_ms": 0,
                     "frame_duration_ms": 100, "steps": [[0, 0, 0], [1, 1, 100], [2, 2, 200]]},
            "coords": {"fixed_origin": True, "fixed_angle": True, "fixed_scale": True, "key_frames": [],
                       "frames": [row[:6] for row in tracker["panel"]["coords"]["frames"]]},
            "tracks": [{"class": mass["class"], "name": mass["name"], "properties": {"mass": 2},
                        "steps": copy.deepcopy(mass["steps"]),
                        "data": {"columns": [mass["data"]["columns"][i] for i in indices], "constants": {"m": 2},
                                 "rows": [[row[i] for i in indices] for row in mass["data"]["rows"]]}}]}}


def known_avi_failure(entry_id="avi"):
    return {"id": entry_id, "case": "archived-avi", "object": None, "path": None,
            "issue": "https://github.com/mgradwohl/MoTrack/issues/140", "upstream": None, "summary": "AVI is not extracted.",
            "expected": {"state_of": "linked-movie", "project_sha256": "0" * 64},
            "tracker": {"load_failure": ["waiting=application loader callback"]}}


AVI_FAILURE = "Probe failed (1):\nTracker state dump input=x waiting=application loader callback\njava.lang.IllegalStateException: timed out"


class TrackerStateTests(unittest.TestCase):
    """The loaded-state oracle (TrackerStateDump.java), differ, gap baseline and registry, without Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="motrack-state-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()

    def compare(self, expected, actual, subset=False):
        return corpus.compare_state(expected, actual, self.manifest, "case", subset=subset)

    def test_schema_validation_rejects_malformed_or_inconsistent_dumps(self):
        corpus.validate_state_dump(tracker_state(), self.manifest)
        corpus.validate_state_dump(motrack_state(), self.manifest, "motrack")
        untimed = motrack_state()
        untimed["panel"]["clip"]["steps"][1][2] = None  # A partial producer may not know a step time.
        corpus.validate_state_dump(untimed, self.manifest, "motrack")
        for mutation in ["version", "producer", "loading", "panel-keys", "track-keys", "duplicate-frame", "ragged-row",
                         "raw-nan", "duplicate-column", "clip-step", "coords-frame", "unsupported-class", "constants",
                         "tracker-null-time"]:
            document = tracker_state()
            kind = "tracker"
            panel = document["panel"]
            mass = panel["tracks"][1]
            if mutation == "version": document["schema_version"] = 2
            elif mutation == "producer": document["producer"]["version"] = "6.3.4"
            elif mutation == "loading": document["producer"]["loading"] = "headless_frameless"
            elif mutation == "panel-keys": del panel["views"]
            elif mutation == "track-keys": mass["extra"] = True
            elif mutation == "duplicate-frame": mass["steps"].append(mass["steps"][0])
            elif mutation == "ragged-row": mass["data"]["rows"][0].pop()
            elif mutation == "raw-nan": mass["data"]["rows"][0][1] = math.nan
            elif mutation == "duplicate-column": mass["data"]["columns"][1]["name"] = "t"
            elif mutation == "clip-step": panel["clip"]["steps"][0][1] = 0.5
            elif mutation == "coords-frame": panel["coords"]["frames"][0][0] = "0"
            elif mutation == "constants": mass["data"]["constants"]["m"] = None
            elif mutation == "tracker-null-time": panel["clip"]["steps"][0][2] = None
            else:
                document, kind = motrack_state(), "motrack"
                document["panel"]["tracks"][0]["class"] = TRACKER + "CenterOfMass"
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, "^named.json: invalid"):
                corpus.validate_state_dump(document, self.manifest, kind, "named.json")

    def test_dump_files_name_themselves_and_reject_bare_specials(self):
        path = self.root / "bare.json"
        path.write_text(json.dumps(tracker_state()).replace('"NaN"', "NaN", 1), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "bare.json: bare NaN is not JSON"):
            corpus.read_state(path, self.manifest)
        path.write_text(json.dumps({**tracker_state(), "schema_version": 3}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "bare.json: invalid tracker state dump: schema"):
            corpus.read_state(path, self.manifest)

    def test_canonical_layout_round_trips_with_one_row_per_line(self):
        document = tracker_state()
        text = corpus.state_text(document)
        self.assertEqual(json.loads(text), document)
        self.assertEqual(corpus.state_text(json.loads(text)), text)
        self.assertIn('\n            [0.1,1.5,2.5,5.0,"NaN",0.0,1.0,1.0],\n', text)
        self.assertIn('"θ_{v}"', text)

    def test_identical_dumps_match_and_every_quantity_class_is_checked(self):
        self.assertEqual(self.compare(tracker_state(), tracker_state()).failures, [])
        cases = {
            "name": (lambda d: d["panel"]["tracks"][1].update(name="mass B"), "track identities differ"),
            "class": (lambda d: d["panel"]["tracks"][0].update({"class": TRACKER + "OffsetOrigin"}), "track identities differ"),
            "frame": (lambda d: d["panel"]["tracks"][1]["steps"][2].update(frame=3), "steps[frame=2]: missing step"),
            "row": (lambda d: d["panel"]["tracks"][1]["data"]["rows"].pop(0), "rows[frame=0]: missing row"),
            "position": (lambda d: d["panel"]["tracks"][1]["data"]["rows"][1].__setitem__(1, 1.5 + 1e-9), "rows[frame=1].x"),
            "nan": (lambda d: d["panel"]["tracks"][1]["data"]["rows"][1].__setitem__(4, 0.0), "rows[frame=1].a_{x}"),
            "column": (lambda d: d["panel"]["tracks"][1]["data"]["columns"][3].update(name="vx"), "data.columns"),
            "dimensions": (lambda d: d["panel"]["tracks"][1]["data"]["columns"][3].update(dimensions="L"), "dimensions"),
            "time": (lambda d: d["panel"]["clip"]["steps"][2].__setitem__(2, 200.001), "clip.steps[2].time_ms"),
            "mapping": (lambda d: d["panel"]["clip"]["steps"][2].__setitem__(1, 3), "clip.steps[2]: frame 3 differs"),
            "extra-step": (lambda d: d["panel"]["clip"]["steps"].append([3, 3, 300.0]), "clip.steps[3]: unexpected step"),
            "coords": (lambda d: d["panel"]["coords"]["frames"][1][6].__setitem__(4, 1e-6), "coords.frames[1]"),
            "property": (lambda d: d["panel"]["tracks"][1]["properties"]["keyFrames"]["items"].append(3), "keyFrames.items"),
            "integer": (lambda d: d["panel"]["tracks"][1]["properties"]["keyFrames"]["items"].__setitem__(0, 0.0), None),
            "selection": (lambda d: d["panel"].update(selected_track=None), "selected_track"),
            "unit": (lambda d: d["panel"]["units"].update(length="cm"), "units.length"),
            "view": (lambda d: d["panel"]["views"]["tables"].append({"chooser": 0}), "views.tables"),
        }
        for name, (mutate, location) in cases.items():
            actual = tracker_state()
            mutate(actual)
            failures = [message for _, message in self.compare(tracker_state(), actual).failures]
            with self.subTest(mutation=name):
                if location is None:
                    self.assertEqual(failures, [])  # An equal double and int are the same OSP number.
                else:
                    self.assertTrue(any(location in message for message in failures), failures)

    def test_tolerances_come_from_the_manifest_table_by_column_dimensions(self):
        table = self.manifest["state_tolerance"]
        self.assertEqual(table["default"], self.manifest["tolerance"])
        self.assertGreater(table["dimensions"]["L/TT"]["absolute"], table["dimensions"]["L/T"]["absolute"])
        for column, value, accepted in [(1, 1.5 + 5e-11, True), (1, 1.5 + 5e-10, False), (3, 5.0 + 5e-9, False),
                                        (3, 5.0 + 4e-9, True)]:
            actual = tracker_state()
            actual["panel"]["tracks"][1]["data"]["rows"][1][column] = value
            with self.subTest(column=column, value=value):
                self.assertEqual(not self.compare(tracker_state(), actual).failures, accepted)
        second = tracker_state()
        second["panel"]["tracks"][1]["data"]["rows"][1][4] = 3.0
        actual = copy.deepcopy(second)
        actual["panel"]["tracks"][1]["data"]["rows"][1][4] = 3.0 + 5e-8  # within L/TT, beyond the default
        self.assertEqual(self.compare(second, actual).failures, [])
        actual["panel"]["tracks"][1]["data"]["rows"][1][4] = 3.0 + 5e-7
        self.assertTrue(self.compare(second, actual).failures)

    def test_partial_producer_reports_unmodeled_content_as_gaps_and_its_own_values_as_failures(self):
        comparison = self.compare(tracker_state(), motrack_state(), subset=True)
        self.assertEqual(comparison.failures, [])
        for gap in ["track class CoordAxes", "track class VectorSum", "PointMass.data.a_{x}", "PointMass.data.θ_{v}",
                    "PointMass.properties.name", "selected_track", "views", "video", "properties", "coords.frames.transform",
                    "units.angles_in_radians", "clip.rate"]:
            self.assertIn(gap, comparison.gaps)
        untimed = motrack_state()
        untimed["panel"]["clip"]["steps"][2][2] = None
        self.assertIn("clip.steps.time", self.compare(tracker_state(), untimed, subset=True).gaps)
        shorter = motrack_state()
        shorter["panel"]["coords"]["frames"].pop()
        self.assertIn("coords.frames after 1", self.compare(tracker_state(), shorter, subset=True).gaps)
        for mutation, location in [("value", "rows[frame=1].v_{x}"), ("invented", "columns.speed"), ("missing-track", "identities"),
                                   ("extra-step", "steps[frame=7]"), ("mass", "properties.mass"), ("unit", "units.time"),
                                   ("coords", "coords.frames[3]"), ("row", "rows[frame=2]"), ("gap-frames", "contiguous"),
                                   ("empty-frames", "contiguous"), ("key-frames", "coords.key_frames"),
                                   ("clip-step", "clip.steps[2]")]:
            actual = motrack_state()
            track = actual["panel"]["tracks"][0]
            if mutation == "value": track["data"]["rows"][1][3] = 5.5
            elif mutation == "invented": track["data"]["columns"][0]["name"] = "speed"
            elif mutation == "missing-track": actual["panel"]["tracks"] = []
            elif mutation == "extra-step": track["steps"].append({"frame": 7, "position": [0.0, 0.0]})
            elif mutation == "mass": track["properties"]["mass"] = 3.0
            elif mutation == "unit": actual["panel"]["units"]["time"] = "ms"
            elif mutation == "coords": actual["panel"]["coords"]["frames"].append([3, 0.0, 0.0, 0.0, 1.0, 1.0])
            elif mutation == "gap-frames": del actual["panel"]["coords"]["frames"][1]
            elif mutation == "empty-frames": actual["panel"]["coords"]["frames"] = []
            elif mutation == "key-frames": actual["panel"]["coords"]["key_frames"] = [0]
            elif mutation == "clip-step": actual["panel"]["clip"]["steps"].pop()
            else: track["data"]["rows"].pop()
            failures = [message for _, message in self.compare(tracker_state(), actual, subset=True).failures]
            with self.subTest(mutation=mutation):
                self.assertTrue(any(location in message for message in failures), failures)

    def test_gaps_name_their_reason(self):
        tracker, motrack = tracker_state(), motrack_state()
        untimed = copy.deepcopy(motrack)
        del untimed["panel"]["clip"]["frame_duration_ms"]
        movie = copy.deepcopy(tracker)
        movie["panel"]["video"] = {"class": "XuggleVideo"}
        uncalibrated = copy.deepcopy(motrack)
        del uncalibrated["panel"]["coords"]
        for gap, tracker_doc, motrack_doc, reason in [
                ("track class CoordAxes", tracker, motrack, "unmodeled-class"),
                ("PointMass.data.θ_{v}", tracker, motrack, "unmodeled-column"),
                ("PointMass.data.v_{x}", tracker, untimed, "default-timing"),
                ("PointMass.data.t", movie, untimed, "probe-media-timing"),
                ("clip.steps.time", movie, untimed, "probe-media-timing"),
                ("PointMass.data.x", tracker, uncalibrated, "uncalibrated"),
                ("units.length", tracker, motrack, "default-length-unit"),
                ("coords.frames after 12", tracker, motrack, "coords-range"),
                ("known defect x-row.t", movie, untimed, "probe-media-timing"),
                ("known defect x-step-11[2]", tracker, untimed, "default-timing"),
                ("known defect x", tracker, motrack, "known-defect-unmodeled"),
                ("views", tracker, motrack, "unmodeled-field")]:
            with self.subTest(gap=gap):
                self.assertEqual(corpus.classify_gap(gap, tracker_doc, motrack_doc), reason)
        self.assertEqual(set(corpus.STATE_GAP_REASONS), {"unmodeled-class", "unmodeled-field", "unmodeled-column",
                                                         "probe-media-timing", "default-timing", "default-length-unit",
                                                         "uncalibrated", "coords-range", "known-defect-unmodeled"})

    def registry(self, *entries):
        return {"schema_version": 1, "reference_version": "6.3.5", "entries": list(entries)}

    def entry(self, **changes):
        entry = {"id": "sum-members", "case": "vector-sum", "object": {"track": "resultant"}, "path": "properties.vectors",
                 "issue": "https://github.com/mgradwohl/MoTrack/issues/63", "upstream": None, "summary": "Members stay pending.",
                 "expected": {"items": ["vector A", "vector B"]}, "tracker": {"@collection": "java.util.ArrayList", "items": []}}
        entry.update(changes)
        return entry

    def write_registry(self, registry):
        path = self.root / "known-defects.json"
        path.write_text(json.dumps(registry), encoding="utf-8")
        return path

    def test_committed_registry_is_valid_and_seeded_from_unaccepted_cases(self):
        registry = corpus.load_known_defects(self.manifest)
        issues = {entry["issue"].rsplit("/", 1)[1] for entry in registry["entries"]}
        self.assertTrue({"63", "64", "140", "146", "209"} <= issues)
        cases = {entry["case"] for entry in registry["entries"]}
        # Each rejected session case of the corpus guide is represented, never waived as a group.
        excluded = {case["id"] for case in self.manifest["cases"] if "session_exclusion" in case or case.get("session_known_issue")}
        self.assertTrue(excluded <= cases, excluded - cases)

    def test_committed_dumps_are_canonical_cover_the_corpus_and_reproduce_the_registry(self):
        registry = corpus.load_known_defects(self.manifest)
        whole = {entry["case"]: entry for entry in registry["entries"] if corpus.is_whole_file(entry)}
        dumps, size = {}, 0
        for case in self.manifest["cases"]:
            path = corpus.state_path(case["id"])
            if case["id"] in whole:
                self.assertFalse(path.exists(), case["id"])
                dumps[case["id"]] = corpus.STATE_LOAD_FAILURE
                continue
            document = corpus.read_state(path, self.manifest)
            self.assertEqual(path.read_bytes().decode("utf-8"), corpus.state_text(document), case["id"])
            dumps[case["id"]] = document
            size += path.stat().st_size
        self.assertEqual(set(corpus.STATE_DIR.glob("*.json")),
                         {corpus.state_path(case["id"]) for case in self.manifest["cases"] if case["id"] not in whole})
        self.assertLess(size, 1536 * 1024)
        failures = []
        errors = {case_id: "\n".join(entry["tracker"]["load_failure"]) for case_id, entry in whole.items()}
        reproduced = corpus.check_known_defects(registry, self.manifest, dumps, failures, errors)
        self.assertEqual(failures, [])
        self.assertEqual(len(reproduced), len(registry["entries"]))
        for case_id in ["protractor", "ball-toss", "dynamic-sine"]:
            self.assertEqual(corpus.compare_state(dumps[case_id], dumps[case_id], self.manifest, case_id).failures, [])
        for path in corpus.STATE_DIR.glob("*.json"):
            self.assertNotRegex(path.read_text(encoding="utf-8"), r"/home/|/tmp/|[A-Za-z]:[\\/]|/Users/")

    def test_registry_values_are_derived_independently_from_the_saved_projects(self):
        # BallToss is fetched into the reference cache; its derivation runs in state-oracle/state-check.
        manifest = copy.deepcopy(self.manifest)
        manifest["cases"] = [case for case in manifest["cases"] if case["id"] != "ball-toss"]
        registry = corpus.load_known_defects(self.manifest)
        values, checks = corpus.derive_known_defect_values(manifest, self.root)
        derivable = [entry for entry in registry["entries"] if entry["case"] != "ball-toss" and not corpus.is_whole_file(entry)]
        self.assertLessEqual({entry["id"] for entry in derivable}, set(values))
        for entry in derivable:
            mismatches, missing = corpus.registered_check(values[entry["id"]], entry["expected"], {"absolute": 1e-9, "relative": 1e-9})
            with self.subTest(entry=entry["id"]):
                self.assertEqual((mismatches, missing), ([], []))
        self.assertEqual(len(checks), 4)
        dumps = {case["id"]: corpus.read_state(corpus.state_path(case["id"]), self.manifest) for case in manifest["cases"]
                 if corpus.state_path(case["id"]).exists()}
        for case_id, track, path, value in checks:
            actual = corpus.resolve_state_path(dumps[case_id]["panel"], {"track": track}, path)
            self.assertTrue(corpus.state_matches(value, actual, {"absolute": 1e-9, "relative": 1e-9}), (case_id, path))
        avi = next(entry for entry in registry["entries"] if entry["id"] == "archived-avi-load")
        by_id = {case["id"]: case for case in self.manifest["cases"]}
        for case_id in ("archived-avi", "linked-movie"):
            digest = hashlib.sha256(corpus.project_path(by_id[case_id], self.root).read_bytes()).hexdigest()
            self.assertEqual(digest, avi["expected"]["project_sha256"], case_id)
        # A tampered registry value no longer matches its derivation.
        tampered = copy.deepcopy(registry)
        entry = next(item for item in tampered["entries"] if item["id"] == "vector-images-negative-time-a")
        entry["expected"]["x"] = 1.25
        entry["expected"]["extra"] = 0.0
        failures = []
        with patch.object(corpus, "derive_ball_toss", return_value=[]):
            corpus.check_known_defect_derivations(tampered, manifest, self.root, dumps, failures)
        self.assertTrue(any("vector-images-negative-time-a" in failure and "registered value differs" in failure
                            for failure in failures), failures)

    def test_media_sample_tables_give_independent_frame_times(self):
        movie = corpus.CORPUS / "fixtures/vector-movie/constant-rate.mov"
        self.assertEqual(corpus.iso_video_frames(movie.read_bytes()), [0.0, 125.0, 250.0, 375.0])
        with self.assertRaises(ValueError):
            corpus.iso_video_frames(b"\x00\x00\x00\x10moovxxxx\x00\x00\x00\x04")

    def test_registry_rejects_cross_file_wildcard_duplicate_or_non_deviating_entries(self):
        whole = known_avi_failure()
        corpus.load_known_defects(self.manifest, self.write_registry(self.registry(self.entry(), whole)))
        mutations = {
            "unknown-case": self.entry(case="not-a-case"),
            "case-list": self.entry(case=["vector-sum", "center-of-mass"]),
            "wildcard": self.entry(path="properties.*"),
            "bad-segment": self.entry(path="properties..vectors"),
            "upstream-issue": self.entry(issue="https://github.com/OpenSourcePhysics/tracker/issues/1"),
            "non-deviating": self.entry(tracker={"items": ["vector A", "vector B"]}),
            "missing-summary": self.entry(summary=""),
            "extra-key": {**self.entry(), "waive": True},
            "whole-object": {**whole, "case": "vector-sum"},
            "self-reference": {**whole, "expected": {**whole["expected"], "state_of": "archived-avi"}},
            "unknown-state-of": {**whole, "expected": {**whole["expected"], "state_of": "missing"}},
            "no-hash": {**whole, "expected": {"state_of": "linked-movie"}},
            "no-signature": {**whole, "tracker": {"load_failure": []}},
            "generic-failure": {**whole, "tracker": "@load_failure"},
        }
        for name, entry in mutations.items():
            entries = [entry, self.entry(id="second", path="steps", expected=[{"frame": 0}], tracker=[])] \
                if name == "whole-object" else [entry]
            with self.subTest(mutation=name), self.assertRaises(ValueError):
                corpus.load_known_defects(self.manifest, self.write_registry(self.registry(*entries)))
        with self.assertRaises(ValueError):
            corpus.load_known_defects(self.manifest, self.write_registry(self.registry(self.entry(), self.entry(id="again"))))
        with self.assertRaises(ValueError):
            corpus.load_known_defects(self.manifest, self.write_registry({**self.registry(), "reference_version": "6.3.6"}))

    def test_registry_paths_select_objects_rows_and_absent_values(self):
        panel = tracker_state()["panel"]
        self.assertEqual(corpus.resolve_state_path(panel, {"track": "resultant"}, "properties.vectors.items"), ["vector A"])
        self.assertEqual(corpus.resolve_state_path(panel, {"track": "mass A"}, "data.rows[frame=1]")["v_{x}"], 5.0)
        self.assertEqual(corpus.resolve_state_path(panel, {"track": "mass A"}, "steps[frame=2].position[0]"), 2.0)
        self.assertEqual(corpus.resolve_state_path(panel, None, "clip.steps[1]"), [1, 1, 100.0])
        for target, path in [({"track": "mass A"}, "data.rows[frame=9]"), ({"track": "missing"}, "steps"),
                             ({"track": "mass A"}, "data.missing"), (None, "clip.steps[9]")]:
            self.assertEqual(corpus.resolve_state_path(panel, target, path), corpus.STATE_ABSENT)

    def test_known_defects_must_persist_and_stale_or_changed_entries_fail(self):
        registry = self.registry(self.entry(), known_avi_failure())
        broken = tracker_state(members=())
        failures = []
        reproduced = corpus.check_known_defects(registry, self.manifest,
                                                {"vector-sum": broken, "archived-avi": corpus.STATE_LOAD_FAILURE}, failures,
                                                {"archived-avi": AVI_FAILURE})
        self.assertEqual(failures, [])
        self.assertEqual([entry["id"] for entry in reproduced], ["sum-members", "avi"])
        for dumps, errors, message in [
                ({"vector-sum": tracker_state(members=("vector A", "vector B")), "archived-avi": corpus.STATE_LOAD_FAILURE},
                 {"archived-avi": AVI_FAILURE}, "STALE known defect sum-members"),
                ({"vector-sum": tracker_state(members=("vector C",)), "archived-avi": corpus.STATE_LOAD_FAILURE},
                 {"archived-avi": AVI_FAILURE}, "CHANGED known defect sum-members"),
                ({"vector-sum": broken, "archived-avi": tracker_state()}, {}, "STALE known defect avi"),
                ({"vector-sum": broken, "archived-avi": corpus.STATE_LOAD_FAILURE}, {"archived-avi": "Probe failed: OutOfMemoryError"},
                 "CHANGED known defect avi"),
                ({"vector-sum": corpus.STATE_LOAD_FAILURE, "archived-avi": corpus.STATE_LOAD_FAILURE}, {"archived-avi": AVI_FAILURE},
                 "Tracker failed to load"),
                ({"archived-avi": corpus.STATE_LOAD_FAILURE}, {"archived-avi": AVI_FAILURE}, "was not dumped")]:
            failures = []
            corpus.check_known_defects(registry, self.manifest, dumps, failures, errors)
            with self.subTest(message=message):
                self.assertTrue(any(message in failure for failure in failures), failures)

    def test_motrack_must_match_the_independent_value_without_waiving_unlisted_keys(self):
        entry = self.entry(id="mass-velocity", case="case", object={"track": "mass A"}, path="data.rows[frame=1]",
                           expected={"v_{x}": 6.0}, tracker={"v_{x}": 5.0})

        def check(actual, registry_entry=entry, expected=None):
            comparison = self.compare(expected or tracker_state(), actual, subset=True)
            corpus.apply_known_defects(comparison, self.registry(registry_entry), "case", actual, expected or tracker_state())
            return comparison
        # MoTrack reproducing Tracker's deviating value fails; the independent value passes.
        for value, failing in [(5.0, True), (6.0, False)]:
            actual = motrack_state()
            actual["panel"]["tracks"][0]["data"]["rows"][1][3] = value
            comparison = check(actual)
            with self.subTest(value=value):
                self.assertEqual(bool(comparison.failures), failing, comparison.failures)
                if failing:
                    self.assertIn("independent value of known defect mass-velocity", comparison.failures[0][1])
        # An unlisted key in the same row is still compared with Tracker, not waived.
        actual = motrack_state()
        actual["panel"]["tracks"][0]["data"]["rows"][1][3] = 6.0
        actual["panel"]["tracks"][0]["data"]["rows"][1][1] = 9.0
        self.assertTrue(any("rows[frame=1].x" in message for _, message in check(actual).failures))
        # Where Tracker has no row, every key MoTrack reports must be listed; listed unreported keys are gaps.
        absent = self.entry(id="extra-row", case="case", object={"track": "mass A"}, path="data.rows[frame=3]",
                            expected={"frame": 3, "x": 2.5, "y": 3.5, "v_{x}": 1.0}, tracker="@absent")
        actual = motrack_state()
        actual["panel"]["tracks"][0]["data"]["rows"].append([0.3, 2.5, 3.5, "NaN", 3.0, 3.0])
        comparison = check(actual, absent)
        self.assertTrue(any("t: not part of the independent value" in message for _, message in comparison.failures),
                        comparison.failures)
        absent["expected"]["t"] = 0.3
        absent["expected"]["step"] = 3
        actual["panel"]["tracks"][0]["data"]["rows"][-1][3] = 1.0
        comparison = check(actual, absent)
        self.assertEqual(comparison.failures, [])
        del actual["panel"]["tracks"][0]["data"]["columns"][0]
        for row in actual["panel"]["tracks"][0]["data"]["rows"]:
            del row[0]
        self.assertIn("known defect extra-row.t", check(actual, absent).gaps)
        unmodeled = self.entry(case="case")
        comparison = check(motrack_state(), unmodeled)
        self.assertEqual(comparison.failures, [])
        self.assertIn("known defect sum-members", comparison.gaps)

    def oracle_cases(self):
        by_id = {case["id"]: case for case in self.manifest["cases"]}
        manifest = copy.deepcopy(self.manifest)
        manifest["cases"] = [copy.deepcopy(by_id[name]) for name in ["independent-vectors", "vector-sum", "archived-avi", "linked-movie"]]
        return manifest

    def run_oracle(self, manifest, registry, outputs, *, record=False, commands=None, logs=None):
        """Run check_state_oracle with fake javac/Java processes; `outputs` maps input names to dumps or errors."""
        state_dir = self.root / "state"

        def run(command, display=None):
            if commands is not None:
                commands.append((command, display))
            if "TrackerStateDump" not in command:
                return ""
            index = command.index("TrackerStateDump")
            source, output = Path(command[index + 1]), Path(command[index + 2])
            polls = next(argument for argument in command if argument.startswith("-Dmotrack.tableVisitPolls=")).split("=")[1]
            result = outputs.get((source.name, polls), outputs[source.name])
            if isinstance(result, Exception):
                raise result
            output.write_text(json.dumps(result), encoding="utf-8")
            return (logs or {}).get(source.name, "")

        with patch.object(corpus.sys, "platform", "linux"), patch.object(corpus, "STATE_DIR", state_dir), \
                patch.object(corpus, "KNOWN_DEFECTS", self.write_registry(registry)), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=run), \
                patch.object(corpus, "check_known_defect_derivations"), redirect_stdout(io.StringIO()) as printed:
            corpus.check_state_oracle(manifest, self.root, "java", record=record)
        return printed.getvalue()

    def oracle_registry(self):
        return self.registry(self.entry(), known_avi_failure())

    def oracle_outputs(self):
        return {"independent-vectors.trk": tracker_state(), "vector-sum.trk": tracker_state(members=()),
                "archived-movie.trz": ValueError(AVI_FAILURE), "linked-movie.trk": tracker_state()}

    def test_state_oracle_records_then_rejects_drift_and_keeps_generated_dumps(self):
        manifest = self.oracle_cases()
        outputs = self.oracle_outputs()
        commands = []
        printed = self.run_oracle(manifest, self.oracle_registry(), outputs, record=True, commands=commands)
        self.assertIn("recorded 3 Tracker dumps", printed)
        self.assertIn("archived-avi: Tracker load failed (whole-file known defect): ", printed)
        self.assertIn("waiting=application loader callback | java.lang.IllegalStateException: timed out", printed)
        self.assertIn("KNOWN TRACKER DEFECT reproduced: avi", printed)
        self.assertEqual(sorted(path.name for path in (self.root / "state").glob("*.json")),
                         ["independent-vectors.json", "linked-movie.json", "vector-sum.json"])
        self.assertEqual((self.root / "state" / "vector-sum.json").read_text(encoding="utf-8"),
                         corpus.state_text(tracker_state(members=())))
        javac, *java = [command for command, _ in commands]
        self.assertTrue(Path(javac[0]).name.startswith("javac"))
        self.assertEqual([Path(argument).name for argument in javac[-3:]], list(corpus.STATE_JAVA_SOURCES))
        for command, display in commands[1:]:
            self.assertEqual(display, ":77")
            self.assertIn("-Xmx256m", command)
            self.assertIn(f"-Dmotrack.tableVisitPolls={corpus.STATE_TABLE_POLLS}", command)
            home = next(argument for argument in command if argument.startswith("-Duser.home=")).split("=", 1)[1]
            self.assertTrue(Path(home).is_relative_to(self.root) and not Path(home).exists())
        archive = next(command for command, _ in commands if "archived-movie.trz" in " ".join(command))
        self.assertEqual(archive[-1], "experiment.trk")
        self.assertNotEqual(Path(archive[-3]).parent, corpus.local_fixture_path(manifest["cases"][2]).parent)

        self.assertIn("3 Tracker loaded-state dumps match", self.run_oracle(manifest, self.oracle_registry(), outputs))
        drifted = tracker_state()
        drifted["panel"]["tracks"][1]["data"]["rows"][2][2] = 3.5
        with self.assertRaisesRegex(ValueError, r"independent-vectors: .*rows\[frame=2\]\.y"):
            self.run_oracle(manifest, self.oracle_registry(), {**outputs, "independent-vectors.trk": drifted})
        generated = sorted(path.name for path in (self.root / "state-generated").glob("*.json"))
        self.assertEqual(generated, ["independent-vectors.json", "linked-movie.json", "vector-sum.json"])

    def test_bounded_table_visits_are_logged_and_confirmed_with_a_long_bound(self):
        manifest = self.oracle_cases()
        outputs = self.oracle_outputs()
        self.run_oracle(manifest, self.oracle_registry(), outputs, record=True)
        logs = {"independent-vectors.trk": "noise\nTrackerStateDump table visit owner=mass A result=bounded polls=40\n"
                                           "TrackerStateDump table visit owner=mass B result=populated polls=3\n"}
        commands = []
        printed = self.run_oracle(manifest, self.oracle_registry(), outputs, logs=logs, commands=commands)
        self.assertIn("independent-vectors: table visits mass A: bounded (40 polls), mass B: populated (3 polls)", printed)
        self.assertIn("independent-vectors: bounded table visit confirmed with 400 polls", printed)
        longer = [command for command, _ in commands if f"-Dmotrack.tableVisitPolls={corpus.STATE_LONG_TABLE_POLLS}" in command]
        self.assertEqual(len(longer), 1)
        changed = tracker_state()
        changed["panel"]["views"]["tables"].append({"chooser": 0, "track_columns": [["mass A", "t"]]})
        with self.assertRaisesRegex(ValueError, "400-poll table visit changes the dump"):
            self.run_oracle(manifest, self.oracle_registry(),
                            {**outputs, ("independent-vectors.trk", str(corpus.STATE_LONG_TABLE_POLLS)): changed}, logs=logs)

    def test_state_oracle_fails_for_stale_or_unexpected_load_results(self):
        manifest = self.oracle_cases()
        outputs = self.oracle_outputs()
        self.run_oracle(manifest, self.oracle_registry(), outputs, record=True)
        for changed, message in [({"vector-sum.trk": tracker_state(members=("vector A", "vector B"))}, "STALE known defect sum-members"),
                                 ({"archived-movie.trz": tracker_state()}, "STALE known defect avi"),
                                 ({"archived-movie.trz": ValueError("Probe failed (1): crash")}, "CHANGED known defect avi"),
                                 ({"independent-vectors.trk": ValueError("Probe failed (1): crash")}, "Tracker state dump failed")]:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                self.run_oracle(manifest, self.oracle_registry(), {**outputs, **changed})
        (self.root / "state" / "archived-avi.json").write_text(corpus.state_text(tracker_state()), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, r"unexpected \['archived-avi.json'\]"):
            self.run_oracle(manifest, self.oracle_registry(), outputs)
        (self.root / "state" / "archived-avi.json").unlink()
        (self.root / "state" / "independent-vectors.json").unlink()
        with self.assertRaisesRegex(ValueError, "no committed dump"):
            self.run_oracle(manifest, self.oracle_registry(), outputs)
        with patch.object(corpus.sys, "platform", "win32"), self.assertRaisesRegex(ValueError, "requires Linux"):
            corpus.check_state_oracle(manifest, self.root, "java")

    def native_check(self, manifest, native, *, record=False, probes=None, gaps_path=None):
        state_dir = self.root / "state"

        def run(command, display=None):
            if probes is not None:
                probes.append(command)
            self.assertEqual(command[1], "--state")
            case_id = next(name for name in native if Path(command[2]).stem == name)
            result = native[case_id]
            if isinstance(result, Exception):
                raise result
            Path(command[3]).write_text(json.dumps(result), encoding="utf-8")
            return ""

        with patch.object(corpus, "STATE_DIR", state_dir), patch.object(corpus, "STATE_GAPS", gaps_path or self.root / "gaps.json"), \
                patch.object(corpus, "KNOWN_DEFECTS", self.write_registry(self.oracle_registry())), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=run), \
                patch.object(corpus, "project_path", side_effect=lambda case, cache: self.root / (case["id"] + ".trk")), \
                patch.object(corpus, "check_known_defect_derivations"), redirect_stdout(io.StringIO()) as printed:
            corpus.check_state_native(manifest, self.root, "probe", record=record)
        return printed.getvalue()

    def test_native_state_check_ratchets_gaps_and_uses_independent_whole_file_expectations(self):
        manifest = self.oracle_cases()
        state_dir = self.root / "state"
        state_dir.mkdir()
        for name in ["independent-vectors", "vector-sum", "linked-movie"]:
            (state_dir / (name + ".json")).write_text(corpus.state_text(tracker_state()), encoding="utf-8")
        native = {name: motrack_state() for name in ["independent-vectors", "vector-sum", "archived-avi", "linked-movie"]}
        probes = []
        self.assertIn("recorded the gaps of 4 cases", self.native_check(manifest, native, record=True, probes=probes))
        self.assertEqual(len(probes), 4)
        baseline = corpus.load_gap_baseline(manifest, self.root / "gaps.json")
        self.assertEqual(baseline["vector-sum"]["track class VectorSum"], "unmodeled-class")
        self.assertIn("unmodeled-field", json.loads((self.root / "gaps.json").read_text(encoding="utf-8"))["all_cases"])
        printed = self.native_check(manifest, native)
        self.assertIn("in 4 of 4 cases, and every gap equals the committed baseline", printed)
        self.assertIn("unmodeled-class 8", printed)
        # A value MoTrack stops reporting is a new gap, so it fails even though nothing is compared there.
        for name, mutate, message in [
                ("calibration-lost", lambda d: d["panel"].pop("coords"), "new gap (unmodeled-field): MoTrack no longer"),
                ("timing-lost", lambda d: d["panel"]["clip"].pop("frame_duration_ms"), "gap clip.frame_duration_ms"),
                ("mass-lost", lambda d: d["panel"]["tracks"][0].pop("properties"), "gap PointMass.properties"),
                ("steps-lost", lambda d: d["panel"]["tracks"][0].pop("steps"), "gap PointMass.steps"),
                ("column-lost", lambda d: d["panel"]["tracks"][0]["data"]["columns"].pop(3) and
                 [row.pop(3) for row in d["panel"]["tracks"][0]["data"]["rows"]], "gap PointMass.data.v_{x}"),
                ("frames-lost", lambda d: d["panel"]["coords"]["frames"].pop(), "gap coords.frames after 1")]:
            changed = copy.deepcopy(native)
            mutate(changed["vector-sum"])
            with self.subTest(mutation=name), self.assertRaisesRegex(ValueError, re.escape(message)):
                self.native_check(manifest, changed)
        # A closed gap must be removed from the baseline explicitly.
        richer = copy.deepcopy(native)
        richer["vector-sum"]["panel"]["selected_track"] = "mass A"
        with self.assertRaisesRegex(ValueError, "gap closed: MoTrack now reports this value"):
            self.native_check(manifest, richer)
        native["archived-avi"]["panel"]["tracks"][0]["steps"][0]["position"] = [9.0, 9.0]
        with self.assertRaisesRegex(ValueError, r"archived-avi: .*steps\[frame=0\]\.position"):
            self.native_check(manifest, native)
        native["archived-avi"] = ValueError("Probe failed (3): crash")
        with self.assertRaisesRegex(ValueError, "archived-avi: MoTrack state probe failed"):
            self.native_check(manifest, native)
        native["archived-avi"] = motrack_state()
        (state_dir / "archived-avi.json").write_text(corpus.state_text(tracker_state()), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, r"unexpected \['archived-avi.json'\]"):
            self.native_check(manifest, native)

    def test_committed_gap_baseline_covers_every_case_with_named_reasons(self):
        baseline = corpus.load_gap_baseline(self.manifest)
        self.assertEqual(set(baseline), {case["id"] for case in self.manifest["cases"]})
        self.assertEqual(baseline["ball-toss"]["PointMass.data.t"], "probe-media-timing")
        self.assertEqual(baseline["center-of-mass"]["PointMass.data.t"], "default-timing")
        self.assertEqual(baseline["independent-vectors"]["units.length"], "default-length-unit")
        self.assertNotIn("PointMass.data.x", baseline["ball-toss"])  # Calibrated positions are compared.
        self.assertEqual(corpus.STATE_GAPS.read_bytes().decode("utf-8"),
                         corpus.gap_baseline_text(corpus.gap_baseline(baseline)))

    def test_state_diff_command_compares_two_files_and_rejects_misuse(self):
        expected, actual = self.root / "tracker.json", self.root / "motrack.json"
        expected.write_text(json.dumps(tracker_state()), encoding="utf-8")
        actual.write_text(json.dumps(motrack_state()), encoding="utf-8")
        with patch.object(sys, "argv", ["corpus.py", "state-diff", str(expected), str(actual)]), \
                redirect_stdout(io.StringIO()) as printed:
            corpus.main()
        self.assertIn("matches", printed.getvalue())
        self.assertIn("gaps: ", printed.getvalue())
        broken = motrack_state()
        broken["panel"]["tracks"][0]["properties"]["mass"] = 1.0
        actual.write_text(json.dumps(broken), encoding="utf-8")
        with patch.object(sys, "argv", ["corpus.py", "state-diff", str(expected), str(actual)]), \
                self.assertRaisesRegex(ValueError, r"properties\.mass"), redirect_stdout(io.StringIO()):
            corpus.main()
        for argv in [["state-diff", str(expected)], ["state-check"], ["check", "--probe", "p", str(expected)],
                     ["state-diff", str(expected), str(actual), "--record"]]:
            with self.subTest(argv=argv), patch.object(sys, "argv", ["corpus.py", *argv]), \
                    self.assertRaises(SystemExit), patch("sys.stderr", new_callable=io.StringIO):
                corpus.main()
        with patch.object(sys, "argv", ["corpus.py", "state-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_state_oracle") as oracle:
            corpus.main()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])
        with patch.object(sys, "argv", ["corpus.py", "state-check", "--probe", "p", "--record"]), \
                patch.object(corpus, "check_state_native") as native:
            corpus.main()
        self.assertTrue(native.call_args.kwargs["record"])


class ExpressionOracleTests(unittest.TestCase):
    """The expression kernel fixture and its Java regeneration check (no Java needed)."""

    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads(corpus.EXPRESSION_REFERENCE.read_text(encoding="utf-8"))

    def case(self, report, case_id):
        return next(case for case in report["cases"] if case["id"] == case_id)

    def test_fixture_is_bounded_unique_and_documents_every_defect(self):
        self.assertEqual(self.fixture["reference"]["tracker_version"], "6.3.5")
        ids = [case["id"] for case in self.fixture["cases"]]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue(all(re.fullmatch(r"[a-z0-9-]+", case_id) for case_id in ids))
        self.assertGreater(len(ids), 600)
        corpus.compare_expression_reference(self.fixture, copy.deepcopy(self.fixture))
        for case in self.fixture["cases"]:
            values = [evaluation for evaluation in case["evaluations"] if "result_bits" in evaluation or "intended_bits" in evaluation]
            if case["defect"] and case["defect"] != "tracker-code-limit":
                # Each annotated defect is evidenced by at least one value that differs from Tracker's.
                self.assertTrue(any("intended_bits" in evaluation for evaluation in values), case["id"])
            if case["error"] or case["exception"]:
                self.assertFalse(any("result_bits" in evaluation for evaluation in values), case["id"])

    def test_exact_cases_require_identical_bits_and_signed_zeros(self):
        actual = copy.deepcopy(self.fixture)
        evaluation = self.case(actual, "precedence-negative-zero")["evaluations"][0]
        self.assertEqual(evaluation["result_bits"], "8000000000000000")
        evaluation["result_bits"] = "0000000000000000"
        evaluation["result"] = "0.0"
        with self.assertRaisesRegex(ValueError, "precedence-negative-zero"):
            corpus.compare_expression_reference(self.fixture, actual)

    def test_transcendental_cases_accept_only_the_declared_ulp_bound(self):
        name = "function-sin"
        index = next(i for i, e in enumerate(self.case(self.fixture, name)["evaluations"]) if e["values"] == ["0.5"])
        original = corpus.expression_value(self.case(self.fixture, name)["evaluations"][index]["result_bits"])
        for ulps, accepted in [(1, True), (self.fixture["tolerance"]["ulp"], True), (self.fixture["tolerance"]["ulp"] + 1, False)]:
            actual = copy.deepcopy(self.fixture)
            evaluation = self.case(actual, name)["evaluations"][index]
            shifted = struct.unpack(">d", struct.pack(">q", struct.unpack(">q", struct.pack(">d", original))[0] + ulps))[0]
            evaluation["result_bits"] = struct.pack(">d", shifted).hex()
            evaluation["result"] = repr(shifted)
            with self.subTest(ulps=ulps):
                if accepted:
                    corpus.compare_expression_reference(self.fixture, actual)
                else:
                    with self.assertRaisesRegex(ValueError, "ulp"):
                        corpus.compare_expression_reference(self.fixture, actual)

    def test_signed_zeros_are_one_ulp_value_and_nan_payloads_are_equal(self):
        self.assertEqual(corpus.expression_ulps(0.0, -0.0), 0)
        self.assertEqual(corpus.expression_ulps(-5e-324, 5e-324), 2)
        expected = {"values": [], "result": "NaN", "result_bits": "7ff8000000000000"}
        corpus.compare_expression_values(expected, dict(expected, result_bits="fff8000000000001"), "exact", 4, "nan")
        with self.assertRaises(ValueError):
            corpus.compare_expression_values(expected, dict(expected, result_bits="0000000000000000"), "ulp", 4, "nan")
        with self.assertRaises(ValueError):
            corpus.compare_expression_values(expected, dict(expected, result_bits="not-hex-bits0000"), "ulp", 4, "bits")

    def test_definitions_errors_and_display_text_must_match(self):
        mutations = {
            "expression": lambda c: c.__setitem__("expression", c["expression"] + " "),
            "error position": lambda c: c["error"].__setitem__("position", c["error"]["position"] + 1),
            "evaluation count": lambda c: c["evaluations"].pop(),
            "missing field": lambda c: c.pop("defect"),
            "display text": lambda c: c["evaluations"][0].__setitem__("result", "3"),
            "random range": lambda c: c["evaluations"][0].__setitem__("range", ["0.0", "2.0"]),
        }
        targets = {"expression": "constant-pi", "error position": "syntax-unclosed", "evaluation count": "division-variable",
                   "missing field": "constant-pi", "display text": "constant-pi", "random range": "random-offset"}
        for name, mutate in mutations.items():
            actual = copy.deepcopy(self.fixture)
            mutate(self.case(actual, targets[name]))
            with self.subTest(mutation=name), self.assertRaises(ValueError):
                corpus.compare_expression_reference(self.fixture, actual)
        for key in ["function_names", "cases"]:
            actual = copy.deepcopy(self.fixture)
            actual[key].pop()
            with self.subTest(header=key), self.assertRaises(ValueError):
                corpus.compare_expression_reference(self.fixture, actual)
        with self.assertRaises(ValueError):
            corpus.compare_expression_reference(self.fixture, dict(self.fixture, tolerance={"ulp": 0, "random_samples": 2000}))

    def test_oracle_runs_the_pinned_jar_in_a_private_home_and_can_record(self):
        manifest = corpus.load_manifest()
        root = Path(tempfile.mkdtemp(prefix="motrack-expression-test-"))
        self.addCleanup(shutil.rmtree, root)
        commands = []
        fixture = corpus.EXPRESSION_REFERENCE.read_text(encoding="utf-8")

        def oracle(command):
            commands.append(command)
            Path(command[-1]).write_text(fixture, encoding="utf-8")

        with patch.object(corpus, "verify_file"), patch.object(corpus, "run", side_effect=oracle):
            corpus.check_expression_oracle(manifest, root, "java")
            recorded = root / "recorded.json"
            with patch.object(corpus, "EXPRESSION_REFERENCE", recorded):
                corpus.check_expression_oracle(manifest, root, "java", record=True)
        self.assertEqual(json.loads(recorded.read_text(encoding="utf-8")), self.fixture)
        self.assertEqual(len(commands), 2)
        command = commands[0]
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertEqual(command[command.index("--class-path") + 1], str(root / manifest["reference"]["file"]))
        self.assertEqual(command[-2], str(corpus.EXPRESSION_ORACLE))
        home = Path(command[-1]).parent
        self.assertIn(f"-Duser.home={home}", command)
        self.assertFalse(home.exists())
        self.assertEqual(list(root.iterdir()), [recorded])

    def test_command_line_accepts_recording_only_for_reference_commands(self):
        with patch.object(sys, "argv", ["corpus.py", "session", "--record"]), redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit):
            corpus.main()
        with patch.object(sys, "argv", ["corpus.py", "expression-oracle", "--record", "--java", "jdk"]), \
                patch.object(corpus, "check_expression_oracle") as check:
            corpus.main()
        self.assertEqual(check.call_args.args[2], "jdk")
        self.assertTrue(check.call_args.kwargs["record"])

class XmlOracleTests(unittest.TestCase):
    """O1/O2 runner: inputs, comparison and reports, with the Java and probe processes mocked."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="motrack-xml-oracle-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.committed = self.root / "committed"
        self.osp = self.root / "osp"
        for directory in (self.committed, self.osp, self.root / "fixtures"):
            directory.mkdir()
        (self.committed / "fixture.trk").write_bytes(b'<object class="a"/>\n')
        (self.committed / "notes.txt").write_bytes(b"not xml")
        (self.osp / "quirk.xml").write_bytes(b'<object class="q">\n</object>\n')
        (self.osp / "fail-quirk.xml").write_bytes(b"fail\n")
        (self.osp / "generated").mkdir()
        self.generated = b'<?xml version="1.0" encoding="UTF-8"?>\n<object class="java.lang.Object"/>'
        (self.osp / "generated" / "demo.xml").write_bytes(self.generated)
        (self.root / "fixtures" / "experiment.trk").write_bytes(b'<object class="e"/>\r\n')
        with zipfile.ZipFile(self.root / "fixtures" / "archive.trz", "w") as archive:
            archive.writestr("one.trk", '<object class="one"/>')
            archive.writestr("nested/two.xml", '<object class="two"/>')
            archive.writestr("duplicate.trk", '<object class="one"/>')
            archive.writestr("image.png", b"\x89PNG")
            archive.writestr("folder/", "")
        self.manifest = {"reference": {"file": "tracker.jar"},
                         "cases": [{"id": "experiment", "file": "experiment.trk"}, {"id": "archive", "file": "archive.trz"}]}
        for replacement in (patch.object(corpus, "COMMITTED_FIXTURES", self.committed), patch.object(corpus, "OSP_FIXTURES", self.osp),
                            patch.object(corpus, "verify_file")):
            replacement.start()
            self.addCleanup(replacement.stop)

    @staticmethod
    def dump(data):
        report = {"schema": corpus.XML_DUMP_SCHEMA, "read": b"fail" not in data}
        if report["read"]:
            report["root"] = {"class": "x", "version": None,
                              "properties": [{"name": "n", "type": "int", "content": "1", "int": 1}]}
        return report

    def runner(self, native=None, generated=None):
        """A run() replacement: the Java tool writes OSP's documents or every listed dump; the probe writes one."""
        commands = []

        def run(command, timeout=45):
            commands.append((command, timeout))
            if "--generate" in command:
                output = Path(command[-1])
                output.mkdir()
                for name, data in (generated if generated is not None else {"demo.xml": self.generated}).items():
                    (output / name).write_bytes(data)
                return
            if "--osp-dump" in command:
                source, output = Path(command[2]), Path(command[3])
                report = self.dump(source.read_bytes())
                if native is not None:
                    report = native(source.read_bytes(), report)
                output.write_text(json.dumps(report), encoding="utf-8")
                return
            for line in Path(command[-1]).read_text(encoding="utf-8").splitlines():
                source, output = line.split("\t")
                Path(output).write_text(json.dumps(self.dump(Path(source).read_bytes())), encoding="utf-8")

        return commands, run

    def test_inputs_cover_cases_archive_members_fixtures_and_line_ending_variants(self):
        documents = corpus.xml_oracle_inputs(self.manifest, self.root)
        labels = [label for label, _ in documents]
        self.assertEqual(labels, ["experiment", "archive!one.trk", "archive!nested/two.xml", "fixtures/fixture.trk",
                                  "osp/fail-quirk.xml[lf]", "osp/fail-quirk.xml[crlf]", "osp/fail-quirk.xml[cr]",
                                  "osp/fail-quirk.xml[bom-crlf]", "osp/quirk.xml[lf]", "osp/quirk.xml[crlf]", "osp/quirk.xml[cr]",
                                  "osp/quirk.xml[bom-crlf]", "osp/generated/demo.xml[lf]", "osp/generated/demo.xml[crlf]",
                                  "osp/generated/demo.xml[cr]", "osp/generated/demo.xml[bom-crlf]"])
        data = dict(documents)
        self.assertEqual(data["experiment"], b'<object class="e"/>\r\n')
        self.assertEqual(data["osp/quirk.xml[cr]"], b'<object class="q">\r</object>\r')
        self.assertEqual(data["osp/quirk.xml[bom-crlf]"], b'\xef\xbb\xbf<object class="q">\r\n</object>\r\n')
        self.assertEqual(corpus.xml_oracle_variants(b"a\r\nb\rc\n")["lf"], b"a\nb\nc\n")

    def test_inputs_are_bounded(self):
        with patch.object(corpus, "MAX_XML_BYTES", 10), self.assertRaisesRegex(ValueError, "experiment: document exceeds"):
            corpus.xml_oracle_inputs(self.manifest, self.root)
        archive_only = {**self.manifest, "cases": [self.manifest["cases"][1]]}
        with patch.object(corpus, "MAX_XML_BYTES", 15), self.assertRaisesRegex(ValueError, "one.trk.*exceeds"):
            corpus.xml_oracle_inputs(archive_only, self.root)
        with patch.object(corpus, "MAX_XML_ORACLE_DOCUMENTS", 2), self.assertRaisesRegex(ValueError, "Too many"):
            corpus.xml_oracle_inputs(self.manifest, self.root)
        with patch.object(corpus, "MAX_ARCHIVE_MEMBERS", 4), self.assertRaisesRegex(ValueError, "archive: archive has too many members"):
            corpus.xml_oracle_inputs(archive_only, self.root)
        # Two members with one name are ambiguous: which one a reader takes is not defined.
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            with zipfile.ZipFile(self.root / "fixtures" / "archive.trz", "a") as archive:
                archive.writestr("one.trk", '<object class="other"/>')
        with self.assertRaisesRegex(ValueError, "ambiguous archive member 'one.trk'"):
            corpus.xml_oracle_inputs(archive_only, self.root)

    def test_matching_dumps_pass_and_report_reads_and_shared_rejections(self):
        commands, run = self.runner()
        reports = self.root / "reports"
        with patch.object(corpus, "run", side_effect=run), patch("builtins.print") as printed:
            corpus.check_xml_oracle(self.manifest, self.root, "java", "probe", reports)
        java = [(command, timeout) for command, timeout in commands if "--osp-dump" not in command]
        self.assertEqual([command[-2] for command, _ in java][0], "--generate")
        self.assertEqual(len(java), 2)
        for command, timeout in java:
            self.assertEqual(timeout, 600)
            self.assertIn("-Djava.awt.headless=true", command)
            self.assertIn("-Xmx256m", command)
            self.assertEqual(command[command.index("--class-path") + 1], str(self.root / "tracker.jar"))
        self.assertTrue(java[1][0][-2].endswith("XmlControlDump.java"))
        self.assertEqual(len(commands) - 2, 16)
        summary = json.loads((reports / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary, {"schema": corpus.XML_DUMP_SCHEMA, "oracles": "O1+O2", "documents": 16, "read_by_both": 12,
                                   "deliberate_differences": [], "rejected_by_both": 4, "mismatches": []})
        self.assertIn("16 documents", printed.call_args.args[0])
        self.assertEqual(sorted(path.name for path in reports.iterdir()), ["summary.json"])

    def test_any_difference_fails_and_keeps_both_dumps(self):
        def native(data, report):
            if data == b'<object class="one"/>':
                report["root"]["properties"][0]["int"] = 2
            if b"fail" in data and b"\r" in data:
                report = {**report, "read": True, "root": {}}
            return report

        _, run = self.runner(native)
        reports = self.root / "reports"
        with patch.object(corpus, "run", side_effect=run), self.assertRaises(ValueError) as raised:
            corpus.check_xml_oracle(self.manifest, self.root, "java", "probe", reports)
        message = str(raised.exception)
        self.assertIn("O1+O2 failed for 4 of 16 documents", message)
        self.assertIn("archive!one.trk: $.root.properties[0].int (1 != 2)", message)
        self.assertIn("osp/fail-quirk.xml[crlf]: $ (keys ['root'])", message)
        summary = json.loads((reports / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(len(summary["mismatches"]), 4)
        self.assertEqual(len(list(reports.glob("*-java.json"))), 4)
        self.assertEqual(len(list(reports.glob("*-native.json"))), 4)

    def test_committed_osp_writer_documents_must_match_a_fresh_java_run(self):
        for generated, pattern in [({"demo.xml": b"changed"}, "differs from OSP's writer output"),
                                   ({}, "OSP writer documents differ"),
                                   ({"demo.xml": self.generated, "extra.xml": b"x"}, "OSP writer documents differ")]:
            commands, run = self.runner(generated=generated)
            with self.subTest(generated=sorted(generated)), patch.object(corpus, "run", side_effect=run), \
                    self.assertRaisesRegex(ValueError, pattern):
                corpus.check_xml_oracle(self.manifest, self.root, "java", "probe", self.root / "reports")
            self.assertEqual(len(commands), 1)

    def test_registered_rejections_still_compare_the_tree_and_others_fail(self):
        def native(data, report):
            if data == b'<object class="one"/>':
                report = {**report, "motrack_rejects": "truncated"}
            if data == b'<object class="two"/>':
                report = {**report, "motrack_rejects": "something else"}
            return report

        _, run = self.runner(native)
        reports = self.root / "reports"
        with patch.object(corpus, "run", side_effect=run), self.assertRaises(ValueError) as raised:
            corpus.check_xml_oracle(self.manifest, self.root, "java", "probe", reports)
        self.assertIn("archive!nested/two.xml: unregistered MoTrack rejection 'something else'", str(raised.exception))
        summary = json.loads((reports / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["deliberate_differences"], ["archive!one.trk: truncated"])
        self.assertEqual(len(summary["mismatches"]), 1)

        # A registered rejection must still produce Java's tree.
        def wrong_tree(data, report):
            if data == b'<object class="one"/>':
                report = {**report, "motrack_rejects": "truncated", "root": {"class": "other"}}
            return report

        _, run = self.runner(wrong_tree)
        with patch.object(corpus, "run", side_effect=run), self.assertRaisesRegex(ValueError, "archive!one.trk: \\$.root"):
            corpus.check_xml_oracle(self.manifest, self.root, "java", "probe", reports)

    def test_o2_only_runs_every_document_through_the_probe_without_java(self):
        commands, run = self.runner(lambda data, report: {**report, "motrack_rejects": "truncated"} if data == b'<object class="one"/>' else report)
        reports = self.root / "o2"
        with patch.object(corpus, "run", side_effect=run), patch("builtins.print") as printed:
            corpus.check_xml_oracle(self.manifest, self.root, None, "probe", reports, o2_only=True)
        self.assertTrue(all("--osp-dump" in command for command, _ in commands))
        self.assertEqual(len(commands), 16)
        summary = json.loads((reports / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual((summary["oracles"], summary["read_by_both"], summary["rejected_by_both"]), ("O2", 11, 4))
        self.assertEqual(summary["deliberate_differences"], ["archive!one.trk: truncated"])
        self.assertIn("--o2-only: 16 documents", printed.call_args.args[0])
        _, run = self.runner(lambda data, report: {**report, "schema": "other"})
        with patch.object(corpus, "run", side_effect=run), self.assertRaisesRegex(ValueError, "unexpected XML dump schema"):
            corpus.check_xml_oracle(self.manifest, self.root, None, "probe", reports, o2_only=True)

    def test_unexpected_dump_schema_is_rejected(self):
        _, run = self.runner(lambda data, report: {**report, "schema": "other"})
        with patch.object(corpus, "run", side_effect=run), self.assertRaisesRegex(ValueError, "unexpected XML dump schema"):
            corpus.check_xml_oracle(self.manifest, self.root, "java", "probe", self.root / "reports")

    def test_json_difference_requires_identical_types_keys_and_order(self):
        self.assertIsNone(corpus.first_json_difference({"a": [1, {"b": None}]}, {"a": [1, {"b": None}]}))
        self.assertEqual(corpus.first_json_difference({"a": True}, {"a": 1}), "$.a (type)")
        self.assertEqual(corpus.first_json_difference({"a": 1}, {"b": 1}), "$ (keys ['a', 'b'])")
        self.assertEqual(corpus.first_json_difference([1, 2], [1]), "$ (length 2 != 1)")
        self.assertEqual(corpus.first_json_difference([1, [2, "x"]], [1, [2, "y"]]), "$[1][1] ('x' != 'y')")

    def test_command_line_requires_a_probe_and_limits_reports_to_the_oracle(self):
        for arguments in (["corpus.py", "xml-oracle"], ["corpus.py", "check", "--probe", "p", "--reports", "r"],
                          ["corpus.py", "check", "--probe", "p", "--o2-only"]):
            with self.subTest(arguments=arguments), patch.object(sys, "argv", arguments), \
                    patch("sys.stderr", new_callable=io.StringIO), self.assertRaises(SystemExit):
                corpus.main()
        with patch.object(sys, "argv", ["corpus.py", "xml-oracle", "--probe", "p", "--java", "j", "--cache", str(self.root),
                                        "--reports", str(self.root / "out")]), \
                patch.object(corpus, "load_manifest", return_value=self.manifest), \
                patch.object(corpus, "check_xml_oracle") as check:
            corpus.main()
        check.assert_called_once_with(self.manifest, self.root, "j", "p", self.root / "out", False)


class OdeOracleTests(unittest.TestCase):
    """The ODE kernel oracle (tools/reference/OdeOracle.java) without running Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()
        self.fixture = json.loads(corpus.ODE_REFERENCE.read_text(encoding="utf-8"))

    def check(self, document, *, record=False, commands=None):
        """Run check_ode_oracle with a fake Java process that emits `document`."""
        def run(command):
            if commands is not None:
                commands.append(command)
            Path(command[-1]).write_text(json.dumps(document), encoding="utf-8")

        with patch.object(corpus, "verify_file") as verify, patch.object(corpus, "run", side_effect=run), \
                redirect_stdout(io.StringIO()) as output:
            corpus.check_ode_oracle(self.manifest, self.root, "java", record=record)
        verify.assert_called_once_with(self.root / self.manifest["reference"]["file"], self.manifest["reference"])
        return output.getvalue()

    def test_committed_fixture_covers_every_solver_in_its_canonical_layout(self):
        corpus.validate_ode_reference(self.fixture, self.manifest)
        classes = {solver["class"].rsplit(".", 1)[1] for solver in self.fixture["solvers"]}
        self.assertEqual(classes, {"Euler", "EulerRichardson", "Ralston2", "Heun3", "RK4", "Butcher5", "Fehlberg8", "Adams4",
                                   "Adams5", "Adams6", "Verlet", "LeapFrog", "CashKarp45", "DormandPrince45", "RK45",
                                   "RK45MultiStep", "ODEMultistepSolver"})
        self.assertEqual({system["id"] for system in self.fixture["systems"]},
                         {"oscillator", "drag", "kepler", "stiff", "forced", "nan-rate"})
        tracker = self.fixture["tracker"]
        self.assertEqual(tracker["default_solver"], "org.opensourcephysics.numerics.RK4")
        self.assertEqual(tracker["model_builder_solvers"][0], tracker["default_solver"])
        self.assertEqual((tracker["trace_points_per_step"], tracker["iterations_per_step"]), (10, 10))
        self.assertEqual(corpus.ODE_REFERENCE.read_bytes().decode("utf-8"), corpus.ode_reference_text(self.fixture))
        self.assertLess(corpus.ODE_REFERENCE.stat().st_size, 256 * 1024)

    def test_check_runs_the_pinned_jar_privately_and_accepts_an_identical_regeneration(self):
        commands = []
        self.assertIn(f"{len(self.fixture['cases'])} Java solver cases match tests/science/ode-reference.json",
                      self.check(self.fixture, commands=commands))
        [command] = commands
        self.assertEqual(command[0], "java")
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertIn("-Xmx256m", command)
        self.assertEqual(command[command.index("--class-path") + 1], str(self.root / self.manifest["reference"]["file"]))
        self.assertEqual(Path(command[-2]), corpus.ROOT / "tools/reference/OdeOracle.java")
        # A private, removed home: OSP preference files never land in the shared reference cache.
        home = next(arg for arg in command if arg.startswith("-Duser.home=")).split("=", 1)[1]
        self.assertFalse(Path(home).is_relative_to(self.root))
        self.assertFalse(Path(home).exists())

    def test_check_rejects_numerical_count_and_non_finite_drift(self):
        def first(document, predicate):
            return next(point for case in document["cases"] for point in case["checkpoints"] if predicate(case, point))

        for mutation in ["state", "rates", "error-code", "nan", "taken", "missing-case", "stiff-state", "tracker"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "state":
                point = first(document, lambda case, point: case["solver"] == "rk4" and point["n"] > 1)
                point["state"][0] = math.nextafter(point["state"][0], math.inf)
            elif mutation == "rates":
                first(document, lambda case, point: True)["rates"] += 1
            elif mutation == "error-code":
                first(document, lambda case, point: point["error_code"] == 1)["error_code"] = 0
            elif mutation == "nan":
                point = first(document, lambda case, point: "NaN" in point["state"])
                point["state"][point["state"].index("NaN")] = 0.0
            elif mutation == "taken":
                first(document, lambda case, point: case["solver"] == "dormandprince45" and point["n"] > 0)["taken"] *= 1 + 1e-7
            elif mutation == "missing-case":
                document["cases"].pop()
            elif mutation == "stiff-state":
                point = first(document, lambda case, point: case["system"] == "stiff" and case["solver"] == "rk45" and point["n"] > 1)
                point["state"][0] *= 1 + 1e-5
            else:
                document["tracker"]["trace_points_per_step"] = 5
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(document)

    def test_adaptive_cases_allow_amplified_last_bit_pow_differences(self):
        # Magnitudes observed with fdlibm's pow (HotSpot on arm64): 4e-10 on the Kepler orbit and
        # up to 6e-9 at the stiff system's stability limit.
        document = copy.deepcopy(self.fixture)
        kepler = next(point for case in document["cases"] if case["id"] == "kepler/dormandprince45/tol=1.0E-6"
                      for point in case["checkpoints"] if point["n"] > 1)
        kepler["taken"] *= 1 + 4e-10
        point = next(point for case in document["cases"] if case["system"] == "stiff" and case["solver"] == "dormandprince45"
                     for point in case["checkpoints"] if point["n"] > 1)
        point["state"][0] *= 1 + 1e-8
        self.check(document)
        # Fixed-step solvers on the same system must still match to the last bit.
        fixed = next(point for case in document["cases"] if case["system"] == "stiff" and case["solver"] == "rk4"
                     for point in case["checkpoints"] if point["n"] > 1)
        fixed["state"][1] = math.nextafter(fixed["state"][1], math.inf)
        with self.assertRaisesRegex(ValueError, "stiff/rk4"):
            self.check(document)

    def test_record_writes_the_canonical_fixture_only_when_requested(self):
        destination = self.root / "science" / "ode-reference.json"
        with patch.object(corpus, "ODE_REFERENCE", destination):
            self.assertIn(f"recorded {len(self.fixture['cases'])} Java solver cases", self.check(self.fixture, record=True))
        self.assertEqual(destination.read_bytes(), corpus.ode_reference_text(self.fixture).encode("utf-8"))
        with patch.object(sys, "argv", ["corpus.py", "check", "--probe", "probe", "--record"]), \
                patch.object(corpus, "check_cases") as check, self.assertRaises(SystemExit), \
                patch("sys.stderr", new_callable=io.StringIO):
            corpus.main()
        check.assert_not_called()
        with patch.object(sys, "argv", ["corpus.py", "ode-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_ode_oracle") as oracle:
            corpus.main()
        oracle.assert_called_once()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])

    def test_validation_rejects_malformed_or_incomplete_documents(self):
        for mutation in ["version", "schema", "no-cases", "duplicate-case", "unknown-solver", "unknown-system", "checkpoint",
                         "uncovered-solver", "duplicate-solver", "not-object"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "version": document["reference_version"] = "6.3.4"
            elif mutation == "schema": document["schema_version"] = 2
            elif mutation == "no-cases": document["cases"] = []
            elif mutation == "duplicate-case": document["cases"].append(document["cases"][0])
            elif mutation == "unknown-solver": document["cases"][0]["solver"] = "rk5"
            elif mutation == "unknown-system": document["cases"][0]["system"] = "pendulum"
            elif mutation == "checkpoint": document["cases"][0]["checkpoints"][-1]["n"] = document["cases"][0]["steps"] + 1
            elif mutation == "uncovered-solver": document["cases"] = [c for c in document["cases"] if c["solver"] != "leapfrog"]
            elif mutation == "duplicate-solver": document["solvers"].append(document["solvers"][0])
            else: document["cases"][0] = "case"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.validate_ode_reference(document, self.manifest)


class LibraryOracleTests(unittest.TestCase):
    """corpus.py library-oracle without Java or the probe: inputs, generated documents and the comparison."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="motrack-library-oracle-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.library = self.root / "library"
        (self.library / "compadre").mkdir(parents=True)
        (self.library / "generated").mkdir()
        self.collection = b'<?xml version="1.0" encoding="UTF-8"?>\n<object class="org.opensourcephysics.tools.LibraryCollection">\n</object>\n'
        (self.library / "known.xml").write_bytes(self.collection)
        (self.library / "my-library.xml").write_bytes(b'<object class="org.opensourcephysics.tools.Library">\n</object>\n')
        (self.library / "compadre" / "tree.rest").write_bytes(b"<x><Identify/></x>\n")
        (self.library / "compadre" / "records.rest").write_bytes(b"<records><record/></records>\n")
        (self.library / "notes.txt").write_bytes(b"ignored")
        self.generated = b'<object class="org.opensourcephysics.tools.LibraryCollection"/>'
        (self.library / "generated" / "osp-collection.xml").write_bytes(self.generated)
        for replacement in (patch.object(corpus, "LIBRARY_FIXTURES", self.library), patch.object(corpus, "verify_file"),
                            patch.object(corpus, "LIBRARY_DEEPEST_NESTING", 2)):
            replacement.start()
            self.addCleanup(replacement.stop)

    def test_kinds_follow_the_root_class_or_rest_elements(self):
        self.assertEqual(corpus.library_kind(self.collection), "collection")
        self.assertEqual(corpus.library_kind(b'<object class="org.opensourcephysics.tools.Library">'), "library")
        self.assertEqual(corpus.library_kind(b"<x><Identify/></x>"), "compadre-tree")
        self.assertEqual(corpus.library_kind(b"<records/>"), "compadre-records")

    def test_inputs_cover_fixtures_variants_extras_and_generated_documents(self):
        extra = self.root / "extra"
        extra.mkdir()
        (extra / "real.rest").write_bytes(b"<records></records>\n")
        documents = corpus.library_oracle_inputs(extra)
        labels = [label for _, label, _, _ in documents]
        self.assertIn("known.xml[lf]", labels)
        self.assertIn("known.xml[bom-crlf]", labels)
        self.assertIn("compadre/tree.rest[cr]", labels)
        self.assertIn("extra/real.rest", labels)
        self.assertNotIn("notes.txt", " ".join(labels))
        kinds = {label: kind for _, label, kind, _ in documents}
        self.assertEqual(kinds["my-library.xml[lf]"], "library")
        self.assertEqual(kinds["compadre/records.rest[lf]"], "compadre-records")
        self.assertEqual(kinds["synthetic/deep-nesting.xml"], "collection")
        deep = next(data for _, label, _, data in documents if label == "synthetic/deep-nesting.xml")
        self.assertEqual(deep.count(b"org.opensourcephysics.tools.LibraryCollection"), 2)
        self.assertIn("synthetic/large-index.xml", labels)
        self.assertEqual(len(labels), len(set(labels)))

    def run_oracle(self, java_dump, native_dump, generated=None):
        def run(command, timeout=45):
            if "--generate" in command:
                output = Path(command[-1])
                output.mkdir()
                for name, data in (generated or {"osp-collection.xml": self.generated}).items():
                    (output / name).write_bytes(data)
                return
            if "--library-dump" in command:
                kind, source, output = command[2], Path(command[3]), Path(command[4])
                Path(output).write_text(json.dumps(native_dump(kind, source.read_bytes())), encoding="utf-8")
                return
            for line in Path(command[-1]).read_text(encoding="utf-8").splitlines():
                kind, source, output = line.split("\t")
                Path(output).write_text(json.dumps(java_dump(kind, Path(source).read_bytes())), encoding="utf-8")

        manifest = {"reference": {"file": "tracker.jar"}, "cases": []}
        with patch.object(corpus, "run", run), redirect_stdout(io.StringIO()) as output:
            corpus.check_library_oracle(manifest, self.root, "java", "probe", self.root / "reports")
        return output.getvalue()

    @staticmethod
    def dump(kind, data, loaded=True):
        return {"schema": corpus.LIBRARY_DUMP_SCHEMA, "kind": kind, "read": True, "loaded": loaded, "error": "ignored"}

    def test_matching_dumps_pass_and_differences_fail_with_reports(self):
        summary = self.run_oracle(self.dump, lambda kind, data: {k: v for k, v in self.dump(kind, data).items() if k != "error"})
        self.assertIn("documents matched", summary)
        with self.assertRaisesRegex(ValueError, "Library oracle failed"):
            self.run_oracle(self.dump, lambda kind, data: self.dump(kind, data, loaded=False) | {"extra": 1})
        self.assertTrue(any((self.root / "reports").glob("*-native.json")))

    def test_generated_documents_must_equal_the_committed_copies(self):
        with self.assertRaisesRegex(ValueError, "differs from OSP's writer output"):
            self.run_oracle(self.dump, self.dump, generated={"osp-collection.xml": b"changed"})
        with self.assertRaisesRegex(ValueError, "OSP library documents differ"):
            self.run_oracle(self.dump, self.dump, generated={"other.xml": self.generated})

    def test_known_differences_must_still_differ(self):
        (self.library / "collection-resources-arraylist.xml").write_bytes(self.collection.replace(b"</object>", b"<!-- list --></object>"))
        java = lambda kind, data: self.dump(kind, data, loaded=b"list" not in data)
        native = lambda kind, data: {k: v for k, v in self.dump(kind, data).items() if k != "error"}
        # One fixture in four line-ending variants.
        self.assertIn("4 known differences", self.run_oracle(java, native))
        with self.assertRaisesRegex(ValueError, "stale known difference"):
            self.run_oracle(lambda kind, data: self.dump(kind, data), native)


class FitOracleTests(unittest.TestCase):
    """The curve-fit kernel oracle (tools/reference/FitOracle.java) without running Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()
        self.fixture = json.loads(corpus.FIT_REFERENCE.read_text(encoding="utf-8"))

    def check(self, document, *, record=False, commands=None):
        """Run check_fit_oracle with a fake Java process that emits `document`."""
        def run(command, **options):
            if commands is not None:
                commands.append((command, options))
            Path(command[-1]).write_text(json.dumps(document), encoding="utf-8")

        with patch.object(corpus, "verify_file") as verify, patch.object(corpus, "run", side_effect=run), \
                redirect_stdout(io.StringIO()) as output:
            corpus.check_fit_oracle(self.manifest, self.root, "java", record=record)
        verify.assert_called_once_with(self.root / self.manifest["reference"]["file"], self.manifest["reference"])
        return output.getvalue()

    def case(self, document, case_id):
        return next(case for case in document["cases"] if case["id"] == case_id)

    def test_committed_fixture_covers_every_fit_defect_and_agreement_class(self):
        corpus.validate_fit_reference(self.fixture, self.manifest)
        self.assertEqual([fit["id"] for fit in self.fixture["fits"]], corpus.FIT_BUILT_INS)
        self.assertEqual([fit["name"] for fit in self.fixture["fits"]],
                         ["Poly1", "Poly2", "Poly3", "Gaussian", "Exponential", "Sinusoid", "DampedSine", "Power", "Log"])
        cases = self.fixture["cases"]
        self.assertEqual({case["agreement"] for case in cases}, corpus.FIT_AGREEMENTS)
        self.assertEqual({case["defect"] for case in cases if "defect" in case}, corpus.FIT_DEFECTS)
        self.assertEqual({case["java"]["method"] for case in cases}, corpus.FIT_METHODS)
        for fit in corpus.FIT_BUILT_INS:
            groups = {case["group"] for case in cases if case["fit"] == fit}
            self.assertTrue({"exact", "noisy", "outlier", "duplicate-x", "fixed", "current", "manual", "random"} <= groups, fit)
        self.assertGreaterEqual(sum("expect" in case for case in cases), 70)
        self.assertEqual(len(self.fixture["minimizers"]), 14)
        self.assertEqual(corpus.FIT_REFERENCE.read_bytes().decode("utf-8"), corpus.fit_reference_text(self.fixture))
        self.assertLess(corpus.FIT_REFERENCE.stat().st_size, 400 * 1024)

    def test_check_runs_the_pinned_jar_privately_and_accepts_an_identical_regeneration(self):
        commands = []
        output = self.check(self.fixture, commands=commands)
        self.assertIn(f"{len(self.fixture['cases'])} Java curve-fit cases and 14 minimizer runs match", output)
        [(command, options)] = commands
        self.assertEqual(command[0], "java")
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertIn("-Xmx256m", command)
        self.assertGreater(options["timeout"], 45)
        self.assertEqual(command[command.index("--class-path") + 1], str(self.root / self.manifest["reference"]["file"]))
        self.assertEqual(Path(command[-2]), corpus.ROOT / "tools/reference/FitOracle.java")
        # A private, removed home: OSP preference files never land in the shared reference cache.
        home = next(arg for arg in command if arg.startswith("-Duser.home=")).split("=", 1)[1]
        self.assertFalse(Path(home).is_relative_to(self.root))
        self.assertFalse(Path(home).exists())

    def test_check_rejects_drift_beyond_each_agreement_class(self):
        def mutate(document, case_id, change):
            change(self.case(document, case_id))

        mutations = {
            "exact-parameter": ("line/noisy", lambda c: c["java"]["parameters"].__setitem__(
                0, math.nextafter(c["java"]["parameters"][0], math.inf))),
            "polynomial-parameter": ("cubic/noisy", lambda c: c["java"]["parameters"].__setitem__(
                1, math.nextafter(c["java"]["parameters"][1], math.inf))),
            "transcendental-parameter": ("gaussian/noisy", lambda c: c["java"]["parameters"].__setitem__(
                0, c["java"]["parameters"][0] * (1 + 1e-5))),
            "uncertainty": ("gaussian/noisy", lambda c: c["java"]["uncertainties"].__setitem__(
                0, c["java"]["uncertainties"][0] * (1 + 1e-2))),
            "band": ("sinusoid/noisy", lambda c: c["java"]["band"][0].__setitem__(0, c["java"]["band"][0][0] * 1.01)),
            "method": ("user-line/noisy", lambda c: c["java"].__setitem__("method", "levenberg-marquardt")),
            "points": ("line/nan-x", lambda c: c["java"].__setitem__("points", c["java"]["points"] - 1)),
            "correlation": ("gaussian/noisy", lambda c: c["java"].__setitem__(
                "correlation", math.nextafter(c["java"]["correlation"], 0))),
            "nan": ("power/negative-x", lambda c: c["java"].__setitem__("rms", 0.5)),
            "input": ("log/noisy", lambda c: c["y"].__setitem__(0, c["y"][0] + 1)),
            "expect": ("exponential/noisy", lambda c: c["expect"]["parameters"].__setitem__(
                0, c["expect"]["parameters"][0] * (1 + 1e-6))),
            "text": ("line/noisy", lambda c: c["java"]["formatted"].__setitem__(0, "1 ± 1")),
        }
        for name, (case_id, change) in mutations.items():
            document = copy.deepcopy(self.fixture)
            mutate(document, case_id, change)
            with self.subTest(mutation=name), self.assertRaises(ValueError):
                self.check(document)
        for name, change in {
            "minimizer": lambda d: d["minimizers"][0]["parameters"].__setitem__(0, d["minimizers"][0]["parameters"][0] + 1e-15),
            "minimizer-iterations": lambda d: d["minimizers"][1].__setitem__("iterations", 7),
            "fit-definition": lambda d: d["fits"][3].__setitem__("expression", "A*exp(-(x-B)^2/C)"),
            "format-probe": lambda d: d["format"][0].__setitem__("text", "2.0 ± 0.0"),
            "case-order": lambda d: d["cases"].reverse(),
        }.items():
            document = copy.deepcopy(self.fixture)
            change(document)
            with self.subTest(mutation=name), self.assertRaises(ValueError):
                self.check(document)

    def test_sensitive_and_chaotic_quantities_are_not_compared(self):
        document = copy.deepcopy(self.fixture)
        chaotic = self.case(document, "user-charge/default-start")
        chaotic["java"]["parameters"] = [value * 2 for value in chaotic["java"]["parameters"]]
        chaotic["java"]["method"] = "none"
        sensitive = next(case for case in document["cases"]
                         if case["java"].get("sensitivity", {}).get("uncertainties", 0) > corpus.FIT_SENSITIVITY_LIMIT
                         and case["java"]["uncertainties"])
        sensitive["java"]["uncertainties"] = [value * 3 for value in sensitive["java"]["uncertainties"]]
        # Within 100 times the measured last-bit sensitivity, a transcendental parameter may move.
        relaxed = self.case(document, "sinusoid/duplicate-x")
        moved = relaxed["java"]["sensitivity"]["parameters"]
        self.assertGreater(moved, 1e-10)
        relaxed["java"]["parameters"][1] *= 1 + 50 * moved
        self.check(document)
        # The data handling of a chaotic case still compares exactly.
        chaotic["java"]["points"] += 1
        with self.assertRaisesRegex(ValueError, "user-charge/default-start"):
            self.check(document)

    def test_another_jvm_may_classify_a_transcendental_case_as_chaotic(self):
        # Another JVM's Newton iteration diverges on a committed transcendental case and converges on a chaotic one.
        document = copy.deepcopy(self.fixture)
        diverged = self.case(document, "gaussian/noisy")
        diverged["agreement"] = "chaotic"
        del diverged["expect"]
        diverged["java"]["parameters"] = [value * 2 for value in diverged["java"]["parameters"]]
        diverged["java"]["method"] = "restored"
        converged = self.case(document, "dampedsine/outlier")
        converged["agreement"] = "transcendental"
        converged["expect"] = {"source": "gauss-newton", "parameters": [1, 2, 3, 4, 5], "uncertainties": [1, 1, 1, 1, 1]}
        self.check(document)
        # The data handling still compares, and no other agreement class may change.
        diverged["java"]["points"] += 1
        with self.assertRaisesRegex(ValueError, "gaussian/noisy"):
            self.check(document)
        document = copy.deepcopy(self.fixture)
        self.case(document, "line/noisy")["agreement"] = "chaotic"
        with self.assertRaisesRegex(ValueError, "line/noisy"):
            self.check(document)

    def test_a_tied_transcendental_fit_may_take_another_minimizer_path(self):
        # log/random-1 starts at its least-squares minimum, so Newton's result and the fallback tie.
        document = copy.deepcopy(self.fixture)
        tied = self.case(document, "log/random-1")
        self.assertEqual(tied["java"]["method"], "hessian")
        self.assertTrue(tied["java"]["tied"])
        tied["java"]["method"] = "levenberg-marquardt"
        self.check(document)
        # A fit that lowered its deviation keeps its path, and its start compares too.
        document = copy.deepcopy(self.fixture)
        moved = self.case(document, "gaussian/noisy")
        self.assertFalse(moved["java"]["tied"])
        moved["java"]["method"] = "levenberg-marquardt"
        with self.assertRaisesRegex(ValueError, "gaussian/noisy.java.method"):
            self.check(document)
        document = copy.deepcopy(self.fixture)
        self.case(document, "gaussian/noisy")["java"]["start"]["parameters"][0] *= 1 + 1e-5
        with self.assertRaisesRegex(ValueError, "gaussian/noisy.java.start"):
            self.check(document)
        document = copy.deepcopy(self.fixture)
        lorentz = self.case(document, "user-lorentz/levenberg-marquardt")
        self.assertEqual((lorentz["agreement"], lorentz["java"]["method"], lorentz["java"]["tied"]), ("exact", "levenberg-marquardt", False))
        lorentz["java"]["start"]["ssr"] = math.nextafter(lorentz["java"]["start"]["ssr"], 0)
        with self.assertRaisesRegex(ValueError, "user-lorentz/levenberg-marquardt.java.start.ssr"):
            self.check(document)
        document = copy.deepcopy(self.fixture)
        tied = self.case(document, "log/random-1")
        tied["java"]["method"] = "none"
        with self.assertRaisesRegex(ValueError, "log/random-1.java.method"):
            self.check(document)
        # The values must still agree, and arithmetic fits keep their exact path.
        tied["java"]["method"] = "restored"
        tied["java"]["parameters"][0] *= 1 + 1e-5
        with self.assertRaisesRegex(ValueError, "log/random-1"):
            self.check(document)
        document = copy.deepcopy(self.fixture)
        self.case(document, "user-line/noisy")["java"]["method"] = "restored"
        with self.assertRaisesRegex(ValueError, "user-line/noisy.java.method"):
            self.check(document)

    def test_record_writes_the_canonical_fixture_only_when_requested(self):
        destination = self.root / "science" / "fit-reference.json"
        with patch.object(corpus, "FIT_REFERENCE", destination):
            self.assertIn(f"recorded {len(self.fixture['cases'])}", self.check(self.fixture, record=True))
        self.assertEqual(destination.read_bytes(), corpus.fit_reference_text(self.fixture).encode("utf-8"))
        with patch.object(sys, "argv", ["corpus.py", "session", "--record"]), patch.object(corpus, "check_sessions") as sessions, \
                self.assertRaises(SystemExit), patch("sys.stderr", new_callable=io.StringIO):
            corpus.main()
        sessions.assert_not_called()
        with patch.object(sys, "argv", ["corpus.py", "fit-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_fit_oracle") as oracle:
            corpus.main()
        oracle.assert_called_once()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])

    def test_validation_rejects_malformed_or_incomplete_documents(self):
        mutations = {
            "version": lambda d: d.__setitem__("reference_version", "6.3.4"),
            "schema": lambda d: d.__setitem__("schema_version", 2),
            "fits": lambda d: d["fits"].pop(),
            "no-cases": lambda d: d.__setitem__("cases", []),
            "duplicate-case": lambda d: d["cases"].append(d["cases"][0]),
            "unknown-fit": lambda d: d["cases"][0].__setitem__("fit", "spline"),
            "agreement": lambda d: d["cases"][0].__setitem__("agreement", "approximate"),
            "defect": lambda d: d["cases"][0].__setitem__("defect", "unknown"),
            "method": lambda d: d["cases"][0]["java"].__setitem__("method", "simplex"),
            "result-keys": lambda d: d["cases"][0]["java"].pop("band"),
            "sensitivity": lambda d: self.case(d, "gaussian/noisy")["java"].pop("sensitivity"),
            "unpaired": lambda d: d["cases"][0]["y"].pop(),
            "data": lambda d: d["cases"][0].pop("x"),
            "intended": lambda d: self.case(d, "user-cos/fixed-text").pop("intended"),
            "uncovered-fit": lambda d: d.__setitem__("cases", [c for c in d["cases"] if c["fit"] != "log"]),
            "not-object": lambda d: d["cases"].__setitem__(0, "case"),
            "not-a-number": lambda d: d.__setitem__("minimizers", "none"),
        }
        for name, change in mutations.items():
            document = copy.deepcopy(self.fixture)
            change(document)
            with self.subTest(mutation=name), self.assertRaises(ValueError):
                corpus.validate_fit_reference(document, self.manifest)
        with self.assertRaises(ValueError):
            corpus.fit_number("1.5")
        self.assertTrue(math.isnan(corpus.fit_number("NaN")))
        self.assertEqual(corpus.fit_number("-Infinity"), -math.inf)


class FilterOracleTests(unittest.TestCase):
    """The video-filter kernel oracle (tools/reference/FilterOracle.java) without running Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()
        self.fixture = json.loads(corpus.FILTER_REFERENCE.read_text(encoding="utf-8"))

    def check(self, document, *, record=False, commands=None):
        """Run check_filter_oracle with a fake Java process that emits `document`."""
        def run(command):
            if commands is not None:
                commands.append(command)
            Path(command[-1]).write_text(json.dumps(document), encoding="utf-8")

        with patch.object(corpus, "verify_file") as verify, patch.object(corpus, "run", side_effect=run), \
                patch("sys.stdout", new_callable=io.StringIO) as output:
            corpus.check_filter_oracle(self.manifest, self.root, "java", record=record)
        verify.assert_called_once_with(self.root / self.manifest["reference"]["file"], self.manifest["reference"])
        return output.getvalue()

    def case(self, document, case_id):
        return next(case for case in document["cases"] if case["id"] == case_id)

    def test_committed_fixture_is_canonical_and_covers_every_registered_filter(self):
        corpus.validate_filter_reference(self.fixture, self.manifest)
        classes = {f["class"].rsplit(".", 1)[1] for case in self.fixture["cases"] for f in case["filters"]}
        self.assertEqual(classes, set(corpus.FILTER_CLASSES))
        self.assertEqual(len(corpus.FILTER_CLASSES), 14)
        self.assertNotIn("BarrelPincushionFilter", classes)
        self.assertEqual(corpus.FILTER_REFERENCE.read_bytes().decode("utf-8"), corpus.filter_reference_text(self.fixture))
        self.assertLess(corpus.FILTER_REFERENCE.stat().st_size, 256 * 1024)
        # Temporal filters are driven through multi-frame sequences with steps, seeks and resets.
        ops = {op["op"] for case in self.fixture["cases"] for op in case["ops"]}
        self.assertEqual(ops, corpus.FILTER_OPS)
        ghost = self.case(self.fixture, "ghost-reevaluate-seek")
        self.assertEqual([op["frame"] for op in ghost["ops"]], [0, 1, 2, 2, 2, 5, 1, 0, 3])
        # Tracker saves the enabled state only for the perspective filter, which then fails to reload headless.
        reloads = {entry["class"].rsplit(".", 1)[1]: entry["disabled_reloads_as"] for entry in self.fixture["persistence"]}
        self.assertEqual(reloads.pop("PerspectiveFilter"), "NullPointerException")
        self.assertEqual(set(reloads.values()), {"enabled"})
        self.assertEqual({entry["id"]: entry["exception"] for entry in self.fixture["failures"]},
                         {"resize-zero-width": "IllegalArgumentException", "perspective-no-key-0": "NullPointerException"})

    def test_check_runs_the_pinned_jar_privately_and_accepts_an_identical_regeneration(self):
        commands = []
        self.assertIn(f"{len(self.fixture['cases'])} Java filter cases match tests/science/video-filter-reference.json pixel for pixel",
                      self.check(self.fixture, commands=commands))
        [command] = commands
        self.assertEqual(command[0], "java")
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertIn("-Xmx512m", command)
        self.assertEqual(command[command.index("--class-path") + 1], str(self.root / self.manifest["reference"]["file"]))
        self.assertEqual(Path(command[-2]), corpus.ROOT / "tools/reference/FilterOracle.java")
        # A private, removed home: OSP preference files never land in the shared reference cache.
        home = next(arg for arg in command if arg.startswith("-Duser.home=")).split("=", 1)[1]
        self.assertFalse(Path(home).is_relative_to(self.root))
        self.assertFalse(Path(home).exists())

    def test_check_rejects_any_pixel_probe_or_reference_drift(self):
        for mutation in ["pixels", "size", "rgb", "probe", "trig", "lookup", "composite", "frame", "persistence", "failure",
                         "missing-case", "case-order"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "pixels":
                result = self.case(document, "ghost-0.05")["results"][3]
                result["sha256"] = "0" * 64
            elif mutation == "size":
                self.case(document, "rotate-CW")["results"][0]["width"] += 1
            elif mutation == "rgb":
                result = self.case(document, "perspective-small")["results"][2]
                result["rgb"] = "000000"
            elif mutation == "probe":
                probe = self.case(document, "radial-Rectilinear-Equisolid-0.75-170.0")["probes"][1]
                probe["value"] = math.nextafter(probe["value"], math.inf)
            elif mutation == "trig":
                document["math"]["brightness_trig"][50]["sin"] = "3fe6a09e667f3bcd"
            elif mutation == "lookup":
                document["math"]["log_lookup_sha256_prefix"]["-99"] = "0" * 16
            elif mutation == "composite":
                document["composites"][0]["sha256"] = "f" * 64
            elif mutation == "frame":
                document["frames"][0]["sha256"] = "e" * 64
            elif mutation == "persistence":
                document["persistence"][0]["disabled_reloads_as"] = "disabled"
            elif mutation == "failure":
                document["failures"][0]["exception"] = "none"
            elif mutation == "missing-case":
                document["cases"].pop()
            else:
                document["cases"][0], document["cases"][1] = document["cases"][1], document["cases"][0]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(document)

    def test_record_writes_the_canonical_fixture_only_when_requested(self):
        destination = self.root / "science" / "video-filter-reference.json"
        with patch.object(corpus, "FILTER_REFERENCE", destination):
            self.assertIn(f"recorded {len(self.fixture['cases'])}", self.check(self.fixture, record=True))
        self.assertEqual(destination.read_bytes(), corpus.filter_reference_text(self.fixture).encode("utf-8"))
        with patch.object(sys, "argv", ["corpus.py", "session", "--record"]), patch.object(corpus, "check_sessions") as sessions, \
                self.assertRaises(SystemExit), patch("sys.stderr", new_callable=io.StringIO):
            corpus.main()
        sessions.assert_not_called()
        with patch.object(sys, "argv", ["corpus.py", "filter-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_filter_oracle") as oracle:
            corpus.main()
        oracle.assert_called_once()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])

    def test_validation_rejects_malformed_or_incomplete_documents(self):
        mutations = ["version", "schema", "registry", "no-cases", "duplicate-case", "case-type", "unknown-class", "unknown-frame",
                     "frame-size", "duplicate-frame", "bad-op", "op-frame", "op-filter", "result-count", "result-hash", "rgb-on-large",
                     "rgb-missing", "rgb-length", "uncovered-class", "persistence", "lists", "repeat"]
        for mutation in mutations:
            document = copy.deepcopy(self.fixture)
            case = document["cases"][0]
            if mutation == "version": document["reference_version"] = "6.3.4"
            elif mutation == "schema": document["schema_version"] = 2
            elif mutation == "registry": document["registry"].reverse()
            elif mutation == "no-cases": document["cases"] = []
            elif mutation == "duplicate-case": document["cases"].append(case)
            elif mutation == "case-type": document["cases"][0] = "case"
            elif mutation == "unknown-class": case["filters"][0]["class"] = "org.opensourcephysics.media.core.BarrelPincushionFilter"
            elif mutation == "unknown-frame": case["frames"][0]["seed"] = 987654
            elif mutation == "frame-size": document["frames"][0]["width"] = corpus.MAX_FILTER_SIDE + 1
            elif mutation == "duplicate-frame": document["frames"].append(document["frames"][0])
            elif mutation == "bad-op": case["ops"].append({"op": "paste"})
            elif mutation == "op-frame": case["ops"][0]["frame"] = len(case["frames"])
            elif mutation == "op-filter": self.case(document, "ghost-clear-enable")["ops"][3]["filter"] = 1
            elif mutation == "result-count": case["results"].append(case["results"][0])
            elif mutation == "result-hash": case["results"][0]["sha256"] = "XYZ"
            elif mutation == "rgb-on-large": case["results"][0]["rgb"] = "00" * 3
            elif mutation == "rgb-missing": self.case(document, "negative-1x1")["results"][0].pop("rgb")
            elif mutation == "rgb-length": self.case(document, "negative-1x1")["results"][0]["rgb"] = "0000000"
            elif mutation == "uncovered-class":
                document["cases"] = [c for c in document["cases"] if not c["id"].startswith(("sum-", "stack-"))]
            elif mutation == "persistence": document["persistence"].pop()
            elif mutation == "repeat": self.case(document, "darkghost-black-then-white")["ops"][1]["repeat"] = 0
            else: document["composites"] = {}
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.validate_filter_reference(document, self.manifest)


class AnalysisOracleTests(unittest.TestCase):
    """The Data Tool analysis oracle (tools/reference/AnalysisOracle.java) without running Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()
        self.fixture = json.loads(corpus.ANALYSIS_REFERENCE.read_text(encoding="utf-8"))

    def check(self, document, *, record=False, commands=None):
        """Run check_analysis_oracle with a fake Java process that emits `document`."""
        def run(command):
            if commands is not None:
                commands.append(command)
            Path(command[-1]).write_text(json.dumps(document), encoding="utf-8")

        with patch.object(corpus, "verify_file") as verify, patch.object(corpus, "run", side_effect=run), \
                redirect_stdout(io.StringIO()) as output:
            corpus.check_analysis_oracle(self.manifest, self.root, "java", record=record)
        verify.assert_called_once_with(self.root / self.manifest["reference"]["file"], self.manifest["reference"])
        return output.getvalue()

    def case(self, document, case_id):
        return next(case for case in document["cases"] if case["id"] == case_id)

    def test_committed_fixture_covers_every_kind_in_its_canonical_layout(self):
        corpus.validate_analysis_reference(self.fixture, self.manifest)
        self.assertEqual({case["kind"] for case in self.fixture["cases"]}, set(corpus.ANALYSIS_KINDS))
        self.assertEqual(self.fixture["osp_commit"], "d56207b0ae737458a31365d368f1c28e4bd3202a")
        columns = {column["id"] for column in self.fixture["columns"]}
        self.assertTrue({"textbook", "constant-tenth", "offset-1e8", "most-negative", "overflowing-sum"} <= columns)
        series = {item["id"] for item in self.fixture["series"]}
        self.assertTrue({"parabola", "x-gaps", "infinite-y", "nyquist-16", "mod4-30", "noise-22", "same-x-8"} <= series)
        self.assertEqual(corpus.ANALYSIS_REFERENCE.read_bytes().decode("utf-8"), corpus.analysis_reference_text(self.fixture))
        self.assertLess(corpus.ANALYSIS_REFERENCE.stat().st_size, 256 * 1024)

    def test_fixture_records_the_reference_defects_it_documents(self):
        # The one-pass standard deviation, the folded Nyquist term, the FFTReal unpacking and the
        # Dataset.append tail are visible in the committed Java output.
        self.assertEqual(self.case(self.fixture, "statistics/offset-1e8")["sd"], 0.0)
        self.assertGreater(self.case(self.fixture, "statistics/constant-tenth")["sd"], 0.0)
        self.assertEqual(self.case(self.fixture, "statistics/most-negative")["max"], "NaN")
        nyquist = self.case(self.fixture, "fourier/nyquist-16")["spectrum"]
        self.assertEqual(nyquist["rows"], 8)
        self.assertAlmostEqual(nyquist["sine"][0], 0.5, places=12)
        mod4 = self.case(self.fixture, "fourier/mod4-30")["spectrum"]
        self.assertGreater(abs(mod4["power"][7] - 1.0), 0.5)
        working = self.case(self.fixture, "plot/x-gaps")["working"]["x"]
        self.assertEqual(working[-2:], working[-4:-2])
        area = self.case(self.fixture, "function-area/2/-3.0,5.0/40.0")
        self.assertLess(area["area"], 32.0 - 0.05)

    def test_check_runs_the_pinned_jar_privately_and_accepts_an_identical_regeneration(self):
        commands = []
        count = len(self.fixture["cases"])
        self.assertIn(f"{count} Java Data Tool cases match tests/science/analysis-reference.json",
                      self.check(self.fixture, commands=commands))
        [command] = commands
        self.assertEqual(command[0], "java")
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertIn("-Xmx256m", command)
        self.assertEqual(command[command.index("--class-path") + 1], str(self.root / self.manifest["reference"]["file"]))
        self.assertEqual(Path(command[-2]), corpus.ROOT / "tools/reference/AnalysisOracle.java")
        home = next(arg for arg in command if arg.startswith("-Duser.home=")).split("=", 1)[1]
        self.assertFalse(Path(home).is_relative_to(self.root))
        self.assertFalse(Path(home).exists())

    def test_transform_values_allow_last_bit_differences_and_nothing_else_does(self):
        document = copy.deepcopy(self.fixture)
        fft = self.case(document, "fft-complex/7")
        fft["output"][3] = math.nextafter(math.nextafter(fft["output"][3], math.inf), math.inf)
        spectrum = self.case(document, "fourier/sine-64")["spectrum"]
        spectrum["cosine"][4] = math.nextafter(spectrum["cosine"][4], -math.inf)
        spectrum["power"][8] = math.nextafter(spectrum["power"][8], math.inf)
        self.check(document)
        document = copy.deepcopy(self.fixture)
        mean = self.case(document, "statistics/noise")
        mean["mean"] = math.nextafter(mean["mean"], math.inf)
        with self.assertRaisesRegex(ValueError, "statistics/noise"):
            self.check(document)

    def test_check_rejects_numerical_count_and_missing_value_drift(self):
        def mutate(document, mutation):
            if mutation == "sd":
                case = self.case(document, "statistics/textbook")
                case["sd"] = math.nextafter(case["sd"], 0.0)
            elif mutation == "count":
                self.case(document, "statistics/gaps")["count"] += 1
            elif mutation == "area":
                case = self.case(document, "area/parabola/0.0,2.0")
                case["area"] = math.nextafter(case["area"], math.inf)
            elif mutation == "slope":
                case = self.case(document, "measure/line/3")
                case["slope"] = math.nextafter(case["slope"], math.inf)
            elif mutation == "nearest":
                self.case(document, "nearest/parabola/0.73/0.0,2.0")["index"] = 8
            elif mutation == "exception":
                self.case(document, "nearest/empty/0.0/0.0,1.0")["index"] = -1
            elif mutation == "frequency":
                spectrum = self.case(document, "fourier/shifted-60")["spectrum"]
                spectrum["frequency"][5] = math.nextafter(spectrum["frequency"][5], math.inf)
            elif mutation == "power":
                spectrum = self.case(document, "fourier/sine-64")["spectrum"]
                spectrum["power"][8] *= 1 + 1e-9
            elif mutation == "fft":
                self.case(document, "fft-real/30")["output"][14] += 1e-9
            elif mutation == "nan":
                self.case(document, "fourier/gap-8")["spectrum"]["cosine"][1] = 0.0
            elif mutation == "working":
                self.case(document, "plot/x-gaps")["working"]["x"].pop()
            elif mutation == "missing-case":
                document["cases"].pop()
            elif mutation == "series":
                document["series"][0]["y"][3] = math.nextafter(document["series"][0]["y"][3], math.inf)
            else:
                document["osp_commit"] = "swingJS"

        for mutation in ["sd", "count", "area", "slope", "nearest", "exception", "frequency", "power", "fft", "nan",
                         "working", "missing-case", "series", "commit"]:
            document = copy.deepcopy(self.fixture)
            mutate(document, mutation)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(document)

    def test_record_writes_the_canonical_fixture_only_when_requested(self):
        destination = self.root / "science" / "analysis-reference.json"
        with patch.object(corpus, "ANALYSIS_REFERENCE", destination):
            self.assertIn(f"recorded {len(self.fixture['cases'])}", self.check(self.fixture, record=True))
        self.assertEqual(destination.read_bytes(), corpus.analysis_reference_text(self.fixture).encode("utf-8"))
        with patch.object(sys, "argv", ["corpus.py", "session", "--record"]), \
                patch.object(corpus, "check_sessions") as sessions, self.assertRaises(SystemExit), \
                patch("sys.stderr", new_callable=io.StringIO):
            corpus.main()
        sessions.assert_not_called()
        with patch.object(sys, "argv", ["corpus.py", "analysis-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_analysis_oracle") as oracle:
            corpus.main()
        oracle.assert_called_once()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])

    def test_validation_rejects_malformed_or_incomplete_documents(self):
        for mutation in ["version", "schema", "commit", "no-cases", "duplicate-case", "unknown-kind", "unknown-series",
                         "unknown-column", "uncovered-kind", "duplicate-series", "no-columns", "not-object", "too-many"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "version":
                document["reference_version"] = "6.3.4"
            elif mutation == "schema":
                document["schema_version"] = 2
            elif mutation == "commit":
                del document["osp_commit"]
            elif mutation == "no-cases":
                document["cases"] = []
            elif mutation == "duplicate-case":
                document["cases"].append(document["cases"][0])
            elif mutation == "unknown-kind":
                document["cases"][0]["kind"] = "histogram"
            elif mutation == "unknown-series":
                self.case(document, "area/line/0.0,2.0")["series"] = "pendulum"
            elif mutation == "unknown-column":
                document["cases"][0]["column"] = "pressure"
            elif mutation == "uncovered-kind":
                document["cases"] = [case for case in document["cases"] if case["kind"] != "column"]
            elif mutation == "duplicate-series":
                document["series"].append(document["series"][0])
            elif mutation == "no-columns":
                document["columns"] = []
            elif mutation == "too-many":
                document["cases"] = [dict(document["cases"][0], id=f"statistics/{index}")
                                     for index in range(corpus.MAX_ANALYSIS_CASES + 1)]
            else:
                document["cases"][0] = "case"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.validate_analysis_reference(document, self.manifest)



class MotionOracleTests(unittest.TestCase):
    """The motion kernel oracle (tools/reference/MotionOracle.java) without running Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()
        self.fixture = json.loads(corpus.MOTION_REFERENCE.read_text(encoding="utf-8"))

    def check(self, document, *, record=False, commands=None):
        """Run check_motion_oracle with a fake Java process that emits `document`."""
        def run(command):
            if commands is not None:
                commands.append(command)
            Path(command[-1]).write_text(json.dumps(document), encoding="utf-8")

        with patch.object(corpus, "verify_file") as verify, patch.object(corpus, "run", side_effect=run), \
                patch("sys.stdout", new_callable=io.StringIO) as output:
            corpus.check_motion_oracle(self.manifest, self.root, "java", record=record)
        verify.assert_called_once_with(self.root / self.manifest["reference"]["file"], self.manifest["reference"])
        return output.getvalue()

    def result(self, document, case_id, algorithm):
        case = next(case for case in document["derivative_cases"] if case["id"] == case_id)
        return next(result for result in case["results"] if result["algorithm"] == algorithm)

    def test_committed_fixture_is_canonical_and_covers_every_kernel(self):
        corpus.validate_motion_reference(self.fixture, self.manifest)
        self.assertEqual(self.fixture["tracker"]["algorithm_codes"], {"FINITE_DIFF": 0, "BOUNCE_DETECT": 1, "FINITE_DIFF_VSPILL2": 2})
        self.assertEqual(self.fixture["tracker"]["spills"], {"vDerivSpill": 1, "aDerivSpill": 2, "bounceDerivsSpill": 3})
        self.assertEqual([check["name"] for check in self.fixture["self_test"]], list(corpus.MOTION_SELF_TESTS))
        failures = sorted((case["id"], result["error"]) for case in self.fixture["derivative_cases"] for result in case["results"]
                          if result["error"])
        self.assertEqual(failures, [("huge", "java.lang.StackOverflowError"), ("stride-2", "java.lang.NullPointerException"),
                                    ("stride-3-variable-time", "java.lang.NullPointerException")])
        self.assertEqual({case["type"] for case in self.fixture["filter_cases"]}, set(corpus.MOTION_FILTER_TYPES))
        methods = {case["method"] for case in self.fixture["numerical_cases"]}
        self.assertTrue({"first", "forward", "backward", "second", "partial", "romberg", "benchmark-sum"} <= methods)
        self.assertEqual(corpus.MOTION_REFERENCE.read_bytes().decode("utf-8"), corpus.motion_reference_text(self.fixture))
        self.assertLess(corpus.MOTION_REFERENCE.stat().st_size, 256 * 1024)

    def test_check_runs_the_pinned_jar_privately_and_accepts_an_identical_regeneration(self):
        commands = []
        self.assertIn("Java motion cases match tests/science/motion-reference.json", self.check(self.fixture, commands=commands))
        [command] = commands
        self.assertEqual(command[0], "java")
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertIn("-Xmx256m", command)
        self.assertEqual(command[command.index("--class-path") + 1], str(self.root / self.manifest["reference"]["file"]))
        self.assertEqual(Path(command[-2]), corpus.ROOT / "tools/reference/MotionOracle.java")
        # A private, removed home: OSP preference files never land in the shared reference cache.
        home = next(arg for arg in command if arg.startswith("-Duser.home=")).split("=", 1)[1]
        self.assertFalse(Path(home).is_relative_to(self.root))
        self.assertFalse(Path(home).exists())

    def test_check_rejects_numerical_missing_value_and_count_drift(self):
        import math

        for mutation in ["velocity", "missing", "moving-average", "savitzky-golay", "butterworth", "angle", "romberg-status",
                         "error", "tracker", "case-count", "self-test-metric"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "velocity":
                point = next(p for p in self.result(document, "free-fall", "finite_difference")["velocity"] if p)
                point[1] = math.nextafter(point[1], math.inf)
            elif mutation == "missing":
                result = self.result(document, "noisy-gaps", "bounce_detection")
                index = next(i for i, p in enumerate(result["first"]) if p)
                result["first"][index] = None
            elif mutation in {"moving-average", "savitzky-golay", "butterworth"}:
                prefix = {"moving-average": "ma-", "savitzky-golay": "sg-", "butterworth": "bw-default"}[mutation]
                case = next(c for c in document["filter_cases"] if c["id"].startswith(prefix) and any(c["output"]))
                index = next(i for i, value in enumerate(case["output"]) if value)
                # Butterworth allows a last-bit intrinsic change, not a 1e-9 one.
                factor = 1 + 1e-9 if mutation == "butterworth" else None
                case["output"][index] = case["output"][index] * factor if factor else math.nextafter(case["output"][index], math.inf)
            elif mutation == "angle":
                case = document["angular_cases"][0]
                index = next(i for i, value in enumerate(case["angle"]) if value)
                case["angle"][index] = math.nextafter(case["angle"][index], -math.inf)
            elif mutation == "romberg-status":
                next(c for c in document["numerical_cases"] if c.get("status") == "converged")["status"] = "not_converged"
            elif mutation == "error":
                self.result(document, "stride-2", "bounce_detection")["error"] = "java.lang.IllegalStateException"
            elif mutation == "tracker":
                document["tracker"]["spills"]["aDerivSpill"] = 1
            elif mutation == "case-count":
                document["filter_cases"].pop()
            else:
                document["self_test"][0]["rms_in"] *= 1 + 1e-6
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(document)

    def test_intrinsic_cases_allow_last_bit_differences_only(self):
        import math

        document = copy.deepcopy(self.fixture)
        case = next(c for c in document["filter_cases"] if c["type"] == "butterworth" and any(c["output"]))
        index = next(i for i, value in enumerate(case["output"]) if value)
        case["output"][index] = math.nextafter(case["output"][index], math.inf)
        numerical = next(c for c in document["numerical_cases"] if c["function"] == "x-log-sin-cos")
        numerical["value"] = math.nextafter(numerical["value"], -math.inf)
        self.check(document)

    def test_record_writes_the_canonical_fixture_only_when_requested(self):
        destination = self.root / "science" / "motion-reference.json"
        with patch.object(corpus, "MOTION_REFERENCE", destination):
            self.assertIn("recorded", self.check(self.fixture, record=True))
        self.assertEqual(destination.read_bytes(), corpus.motion_reference_text(self.fixture).encode("utf-8"))
        with patch.object(sys, "argv", ["corpus.py", "session", "--record"]), patch.object(corpus, "check_sessions") as sessions, \
                self.assertRaises(SystemExit), patch("sys.stderr", new_callable=io.StringIO):
            corpus.main()
        sessions.assert_not_called()
        with patch.object(sys, "argv", ["corpus.py", "motion-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_motion_oracle") as oracle:
            corpus.main()
        oracle.assert_called_once()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])

    def test_validation_rejects_malformed_or_incomplete_documents(self):
        for mutation in ["version", "schema", "not-object", "no-tracker", "self-test-order", "self-test-failed", "empty-section",
                         "duplicate-id", "missing-algorithm", "series-length", "positions-length", "filter-type",
                         "filter-signal", "filter-coverage", "no-failures", "case-not-object"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "version": document["reference_version"] = "6.3.4"
            elif mutation == "schema": document["schema_version"] = 2
            elif mutation == "not-object": document = []
            elif mutation == "no-tracker": del document["tracker"]
            elif mutation == "self-test-order": document["self_test"].reverse()
            elif mutation == "self-test-failed": document["self_test"][2]["passed"] = False
            elif mutation == "empty-section": document["angular_cases"] = []
            elif mutation == "duplicate-id": document["filter_cases"].append(document["filter_cases"][0])
            elif mutation == "missing-algorithm": document["derivative_cases"][0]["results"].pop()
            elif mutation == "series-length": document["derivative_cases"][0]["results"][0]["velocity"].pop()
            elif mutation == "positions-length": document["derivative_cases"][0]["positions"].pop()
            elif mutation == "filter-type": document["filter_cases"][0]["type"] = "kalman"
            elif mutation == "filter-signal": document["filter_cases"][0]["signal"] = "unknown"
            elif mutation == "filter-coverage":
                document["filter_cases"] = [c for c in document["filter_cases"] if c["type"] != "savitzky_golay"]
            elif mutation == "no-failures":
                for case in document["derivative_cases"]:
                    case["results"] = [dict(result, error=None, first=[None] * case["clip"]["step_count"],
                                            second=[None] * case["clip"]["step_count"],
                                            velocity=[None] * case["clip"]["step_count"],
                                            acceleration=[None] * case["clip"]["step_count"]) for result in case["results"]]
            else: document["difference_cases"][0] = "case"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.validate_motion_reference(document, self.manifest)


class SamplingOracleTests(unittest.TestCase):
    """The sampling kernel oracle (tools/reference/SamplingOracle.java) without running Java."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.manifest = corpus.load_manifest()
        self.fixture = json.loads(corpus.SAMPLING_REFERENCE.read_text(encoding="utf-8"))

    def check(self, document, *, record=False, commands=None):
        """Run check_sampling_oracle with a fake Java process that emits `document`."""
        def run(command):
            if commands is not None:
                commands.append(command)
            Path(command[-1]).write_text(json.dumps(document), encoding="utf-8")

        with patch.object(corpus, "verify_file") as verify, patch.object(corpus, "run", side_effect=run), \
                redirect_stdout(io.StringIO()) as output:
            corpus.check_sampling_oracle(self.manifest, self.root, "java", record=record)
        verify.assert_called_once_with(self.root / self.manifest["reference"]["file"], self.manifest["reference"])
        return output.getvalue()

    def case(self, document, case_id):
        return next(case for key in ("regions", "profiles") for case in document[key] if case["id"] == case_id)

    def test_committed_fixture_is_canonical_and_covers_both_kernels(self):
        corpus.validate_sampling_reference(self.fixture, self.manifest)
        self.assertEqual(corpus.SAMPLING_REFERENCE.read_bytes().decode("utf-8"), corpus.sampling_reference_text(self.fixture))
        self.assertLess(corpus.SAMPLING_REFERENCE.stat().st_size, 256 * 1024)
        self.assertEqual({frame["kind"] for frame in self.fixture["frames"]}, corpus.SAMPLING_FRAME_KINDS)
        self.assertEqual({case["shape"] for case in self.fixture["regions"]}, corpus.SAMPLING_SHAPES)
        self.assertEqual(self.fixture["tracker"]["line_profile_max_spread"], 100)
        profiles = self.fixture["profiles"]
        self.assertTrue(any(corpus.sampling_profile_is_tilted(case) for case in profiles))
        self.assertTrue(any(not corpus.sampling_profile_is_tilted(case) for case in profiles))
        # Recorded reference defects: int overflow, the all-black strip and tilted-probe exceptions.
        self.assertEqual(self.case(self.fixture, "overflow-white-200")["result"]["Rsd"], "NaN")
        self.assertEqual(self.case(self.fixture, "horizontal-all-black")["tracker_diagnostic"], "image_failed")
        self.assertEqual(self.case(self.fixture, "tilted-vertical-integer")["tracker_error"], "java.lang.NullPointerException")

    def test_check_runs_the_pinned_jar_privately_and_accepts_an_identical_regeneration(self):
        commands = []
        cases = len(self.fixture["regions"]) + len(self.fixture["profiles"])
        self.assertIn(f"{cases} Java sampling cases match tests/science/sampling-reference.json",
                      self.check(self.fixture, commands=commands))
        [command] = commands
        self.assertEqual(command[0], "java")
        self.assertIn("-Djava.awt.headless=true", command)
        self.assertIn("-Xmx256m", command)
        self.assertEqual(command[command.index("--class-path") + 1], str(self.root / self.manifest["reference"]["file"]))
        self.assertEqual(Path(command[-2]), corpus.ROOT / "tools/reference/SamplingOracle.java")
        # A private, removed home: OSP preference files never land in the shared reference cache.
        home = next(arg for arg in command if arg.startswith("-Duser.home=")).split("=", 1)[1]
        self.assertFalse(Path(home).is_relative_to(self.root))
        self.assertFalse(Path(home).exists())

    def test_check_rejects_drift_beyond_each_agreement_class(self):
        for mutation in ["region-mean", "region-nan", "region-null", "mask", "horizontal", "tilted", "tilted-n", "error",
                         "missing", "frame-hash", "constant"]:
            document = copy.deepcopy(self.fixture)
            if mutation == "region-mean":
                result = self.case(document, "ellipse-21-subpixel")["result"]
                result["R"] = math.nextafter(result["R"], math.inf)
            elif mutation == "region-nan":
                self.case(document, "overflow-white-200")["result"]["Rsd"] = 0.0
            elif mutation == "region-null":
                self.case(document, "ellipse-bounds-left-outside")["result"] = self.case(document, "ellipse-touching-left")["result"]
            elif mutation == "mask":
                self.case(document, "polygon-bowtie")["mask_fnv1a64"] = "0" * 16
            elif mutation == "horizontal":
                column = self.case(document, "horizontal-checker")["result"]["R"]
                column[0] = math.nextafter(column[0], math.inf)
            elif mutation == "tilted":
                self.case(document, "tilted-q2-s1")["result"]["G"][3] += 2e-9
            elif mutation == "tilted-n":
                self.case(document, "tilted-q2-s1")["result"]["n"][3] += 1
            elif mutation == "error":
                self.case(document, "tilted-integer-start")["tracker_error"] = None
            elif mutation == "missing":
                document["profiles"].pop()
            elif mutation == "frame-hash":
                document["frames"][0]["fnv1a64"] = "f" * 16
            else:
                document["tracker"]["rgb_default_edge_length"] = 21
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(document)

    def test_tilted_profiles_allow_last_bit_libm_differences_only(self):
        document = copy.deepcopy(self.fixture)
        self.case(document, "tilted-q2-s1")["result"]["G"][3] += 5e-10
        self.case(document, "tilted-q0-checker")["result"]["x"][0] += 5e-10
        self.check(document)
        # Horizontal profiles on the same frames must still match to the last bit.
        column = self.case(document, "horizontal-reversed")["result"]["G"]
        column[1] = math.nextafter(column[1], math.inf)
        with self.assertRaisesRegex(ValueError, "horizontal-reversed"):
            self.check(document)

    def test_record_writes_the_canonical_fixture_only_when_requested(self):
        destination = self.root / "science" / "sampling-reference.json"
        cases = len(self.fixture["regions"]) + len(self.fixture["profiles"])
        with patch.object(corpus, "SAMPLING_REFERENCE", destination):
            self.assertIn(f"recorded {cases}", self.check(self.fixture, record=True))
        self.assertEqual(destination.read_bytes(), corpus.sampling_reference_text(self.fixture).encode("utf-8"))
        with patch.object(sys, "argv", ["corpus.py", "session", "--record"]), patch.object(corpus, "check_sessions") as sessions, \
                self.assertRaises(SystemExit), patch("sys.stderr", new_callable=io.StringIO):
            corpus.main()
        sessions.assert_not_called()
        with patch.object(sys, "argv", ["corpus.py", "sampling-oracle", "--java", "jdk/java", "--record"]), \
                patch.object(corpus, "check_sampling_oracle") as oracle:
            corpus.main()
        oracle.assert_called_once()
        self.assertEqual(oracle.call_args.args[2], "jdk/java")
        self.assertTrue(oracle.call_args.kwargs["record"])

    def test_validation_rejects_malformed_or_incomplete_documents(self):
        for mutation in ["version", "schema", "no-regions", "duplicate", "unknown-frame", "unknown-kind", "frame-size", "shape",
                         "vertices", "size", "bounds", "region-columns", "ragged", "spread", "ends", "all-tilted", "not-object",
                         "tracker"]:
            document = copy.deepcopy(self.fixture)
            region, profile = document["regions"][0], document["profiles"][0]
            if mutation == "version": document["reference_version"] = "6.3.4"
            elif mutation == "schema": document["schema_version"] = 2
            elif mutation == "no-regions": document["regions"] = []
            elif mutation == "duplicate": document["profiles"].append(copy.deepcopy(region))
            elif mutation == "unknown-frame": profile["frame"] = "stripes"
            elif mutation == "unknown-kind": document["frames"][0]["kind"] = "stripes"
            elif mutation == "frame-size": document["frames"][0]["width"] = 0
            elif mutation == "shape": region["shape"] = "star"
            elif mutation == "vertices": region["vertices"] = [[0, 0], [1, 1], [2, 0]]
            elif mutation == "size": region["size"] = [20.5, 20]
            elif mutation == "bounds": region["bounds"] = [0, 0, 1]
            elif mutation == "region-columns": del region["result"]["Bsd"]
            elif mutation == "ragged": profile["result"]["luma"].pop()
            elif mutation == "spread": profile["spread"] = 101
            elif mutation == "ends": profile["ends"] = [[0, 0]]
            elif mutation == "all-tilted":
                for case in document["profiles"]:
                    case["always_horizontal"], case["axis_angle"] = False, 0.5
            elif mutation == "tracker": document["tracker"] = []
            else: document["regions"][0] = "case"
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                corpus.validate_sampling_reference(document, self.manifest)


if __name__ == "__main__":
    unittest.main()
