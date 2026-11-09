"""Integrity and failure-path tests for the compatibility corpus runner (no network)."""

import hashlib
import copy
from contextlib import nullcontext
import io
import json
import os
from pathlib import Path
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

    def check_multitab_session_case(self, case_id):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == case_id)
        templates = [next(c for c in manifest["cases"] if c["id"] == tab["case"])
                     for tab in case["session_archive_tabs"]]
        snapshots = []
        for template in templates:
            expected = json.loads(corpus.expected_path(template).read_text(encoding="utf-8"))["project"]
            mass = expected["point_masses"][0]
            snapshots.append({"tracks": [template["session_tracks"][0], {
                **template["session_tracks"][1], "mass": mass["mass"],
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
            output = self.root / (case_id + "-" + (mutation or "valid"))
            output.mkdir()
            calls, homes = [], []
            observed = copy.deepcopy(snapshots)
            if mutation == "second-mass": observed[1]["tracks"][1]["mass"] = 8.0
            elif mutation == "second-pixels": observed[1]["media"]["frames"][0]["rgb_sha256"] = "0" * 64
            elif mutation == "second-origin": observed[1]["calibration"]["keys"][0]["origin"][0] += 1

            def session(command, *, display=None):
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
                    self.assertEqual(source.parent, output / "input")
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

            with self.subTest(mutation=mutation), patch.object(corpus, "run", side_effect=session):
                if mutation:
                    with self.assertRaisesRegex(ValueError, "checksum" if mutation.endswith("-bytes") else "."):
                        corpus.check_archive_tabs(case, manifest, self.root, "java", self.root / "classes",
                                                  self.root / "reference.jar", output, ":77")
                else:
                    corpus.check_archive_tabs(case, manifest, self.root, "java", self.root / "classes",
                                              self.root / "reference.jar", output, ":77")
                    self.assertEqual([stage for _, stage in calls], ["save-tabs", "reopen", "reopen"])
                    self.assertEqual(len(set(homes)), 3)
                    report = json.loads((self.root / "reports" / f"{case_id}-tracker_archive_tabs_resave.json").read_text())
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
            for mutation in [None, "xml", "path", "movie", "missing", "extra", "stored"]:
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
                    if mutation:
                        with self.assertRaises(ValueError):
                            corpus.verify_media_package_export(original, exported, selected, case, clip_mode, movie=True)
                    else:
                        corpus.verify_media_package_export(original, exported, selected, case, clip_mode, movie=True)
                        self.assertEqual(selected.read_text(encoding="utf-8"), xml)

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
                         ["archive-movie", "archive-movie-clip", "archive-movie-clip-single", "paired-archive-movie"])
        self.assertTrue(all(call.args[0]["id"] == "archived-movie" for call in check.call_args_list))

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

    def check_media_session_oracle(self, case_id, clip_mode=None):
        manifest = corpus.load_manifest()
        case = next(c for c in manifest["cases"] if c["id"] == case_id)
        expected = json.loads(corpus.expected_path(case).read_text())["project"]
        if clip_mode is not None:
            expected, case = corpus.linked_media_clip_expected(case, clip_mode == "single")
        keys = [[0, 1, 2]] if clip_mode is not None else None
        mass = expected["point_masses"][0]
        before = {"tracks": [case["session_tracks"][0], {
            **case["session_tracks"][1], "mass": mass["mass"], "key_frames": [0, 1, 2] if keys else [],
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
        corpus.validate_session(case, valid, manifest, expected, keys)
        for stages in [["after"], ["before", "after_save", "after"]]:
            for mutation in ["missing", "pixels", "order", "time", "dimensions", "count", "origin", "scale", "axes_grid", "engine",
                             "clip-range", "missing-mark", "display-time"]:
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
                    corpus.validate_session(case, invalid, manifest, expected, keys)
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


if __name__ == "__main__":
    unittest.main()
