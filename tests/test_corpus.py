"""Integrity and failure-path tests for the compatibility corpus runner (no network)."""

import hashlib
import copy
from contextlib import nullcontext
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import warnings
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
        manifest = corpus.load_manifest()
        manifest["cases"].append(manifest["cases"][0])
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            corpus.load_manifest(path)
        with self.assertRaisesRegex(ValueError, "inside"):
            corpus.expected_path({"expected": "../../outside.json"})

    def test_session_validation_checks_loaded_content_and_java_resave(self):
        manifest = corpus.load_manifest()
        case = next(case for case in manifest["cases"] if case["id"] == "protractor")
        expected = json.loads(corpus.expected_path(case).read_text(encoding="utf-8"))["project"]
        tracks = []
        for identity in case["session_tracks"]:
            mass = next((mass for mass in expected["point_masses"] if mass["name"] == identity["name"]), None)
            tracks.append({**identity, "mass": mass["mass"] if mass else None,
                           "samples": [{"frame": sample["frame"], "points": [sample["image"]]}
                                       for sample in mass["samples"]] if mass else []})
        before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"]}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
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
        for key, value in [("reference_version", "6.3.4"), ("verification_level", "xml_and_coordinate_api")]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                corpus.validate_session(case, {**valid, key: value}, manifest)

    def test_session_checks_reject_non_linux_hosts(self):
        with patch.object(corpus.sys, "platform", "win32"), self.assertRaisesRegex(ValueError, "Linux"):
            corpus.check_sessions({}, self.root, "java")

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
        before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"]}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        for field, value in [("tail", [11.0, 20.0]), ("components", [3.0, 4.0]), ("frame", 1)]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state]["tracks"][1]["vector_samples"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

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
                           "samples": [{"frame": sample["frame"], "points": [sample["image"]]}
                                       for sample in mass["samples"]] if mass else [],
                           "key_frames": keys[index] if mass else None})
        before = {"tracks": tracks, "clip": expected["clip"], "coordinate_samples": expected["coordinate_samples"]}
        valid = {"reference_version": "6.3.5", "verification_level": "tracker_session_resave",
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

    def test_failed_session_cleans_its_private_home_and_outputs(self):
        manifest = corpus.load_manifest()
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
            with self.assertRaisesRegex(ValueError, "session failure"):
                corpus.check_sessions(manifest, self.root, "java")
        self.assertEqual(len(calls), 2)
        self.assertEqual(list(self.root.glob("session-*")), [])

    def test_reopen_runs_in_a_separate_process_and_private_home(self):
        manifest = corpus.load_manifest()
        manifest["cases"] = [next(case for case in manifest["cases"] if case["id"] == "protractor")]
        (self.root / "fixtures").mkdir()
        (self.root / "fixtures/protractor.trk").write_text("<object/>", encoding="utf-8")
        calls = []
        def session(command, *, display=None):
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
            report = {"reference_version": "6.3.5", "verification_level": "tracker_session_" + stage,
                      "loaded": {"stage": stage}, "after_save": {"stage": "after_save"}}
            (output / ("session-" + stage + ".json")).write_text(json.dumps(report), encoding="utf-8")
        with patch.object(corpus.sys, "platform", "linux"), \
                patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=session), \
                patch.object(corpus, "validate_session") as validate:
            corpus.check_sessions(manifest, self.root, "java")
        self.assertEqual([call[2] for call in calls], ["save", "reopen"])
        self.assertNotEqual(calls[0][1], calls[1][1])
        report = validate.call_args.args[1]
        self.assertEqual(report["before"], {"stage": "save"})
        self.assertEqual(report["after_save"], {"stage": "after_save"})
        self.assertEqual(report["after"], {"stage": "reopen"})
        self.assertEqual(list(self.root.glob("session-*")), [])


if __name__ == "__main__":
    unittest.main()
