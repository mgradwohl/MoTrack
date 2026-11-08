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
                     "view_preparation": "visit_custom_tables_restore_selection", "before": before,
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
                 "view_preparation": "visit_custom_tables_restore_selection", "before": before,
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
                     "view_preparation": "visit_custom_tables_restore_selection", "before": before,
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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
        for case_id in ["protractor", "dynamic-sine", "plot-guests"]:
            case = next(case for case in manifest["cases"] if case["id"] == case_id)
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
                     "view_preparation": "visit_custom_tables_restore_selection",
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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
                     "view_preparation": "visit_custom_tables_restore_selection",
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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
                  "view_preparation": "visit_custom_tables_restore_selection",
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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
                 "view_preparation": "visit_custom_tables_restore_selection",
                 "before": before, "after_save": copy.deepcopy(before), "after": copy.deepcopy(before)}
        corpus.validate_session(case, valid, manifest)
        for field, value in [("tail", [11.0, 20.0]), ("components", [3.0, 4.0]), ("frame", 1)]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state]["tracks"][1]["vector_samples"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

        for field in ["length_unit", "mass_unit"]:
            invalid = copy.deepcopy(valid)
            for state in ["before", "after_save", "after"]:
                invalid[state][field] = "wrong-unit"
            with self.subTest(field=field), self.assertRaises(ValueError):
                corpus.validate_session(case, invalid, manifest)

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
                 "view_preparation": "visit_custom_tables_restore_selection",
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
                        raise failure
                    output = Path(output)
                    (output / "tracker-resaved.trk").write_text("<object/>", encoding="utf-8")
                    report = {"reference_version": "6.3.5", "verification_level": "tracker_session_save",
                              "view_preparation": "visit_custom_tables_restore_selection", "loaded": {}, "after_save": {}}
                    (output / "session-save.json").write_text(json.dumps(report), encoding="utf-8")
                with self.subTest(stage=failing_stage, error=type(failure).__name__), \
                        patch.object(corpus.sys, "platform", "linux"), \
                        patch.object(corpus, "private_display", return_value=nullcontext(":77")), \
                        patch.object(corpus, "artifacts", return_value=[]), patch.object(corpus, "run", side_effect=session):
                    with self.assertRaisesRegex(ValueError, "protractor/session/" + failing_stage) as raised:
                        corpus.check_sessions(manifest, self.root, "java")
                    self.assertIs(raised.exception.__cause__, failure)
                    self.assertIn("protractor.trk" if failing_stage == "save" else "tracker-resaved.trk", str(raised.exception))
                    self.assertEqual(calls, ["save"] if failing_stage == "save" else ["save", "reopen"])
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
                      "view_preparation": "visit_custom_tables_restore_selection",
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
