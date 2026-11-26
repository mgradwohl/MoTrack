"""Tests for tools/translations.py and validation of the committed catalogs (no network, no Qt required).

lrelease and lupdate completeness run when the tools are on PATH; CTest's translation_catalogs test always passes
the Qt SDK's executables to `translations.py check`.
"""

import contextlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import translations as t

LOCALE = {locale.catalog: locale for locale in t.LOCALES}

SAMPLE = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="de">
  <context>
    <name>MainWindow</name>
    <message><source>&amp;Edit</source><translation type="unfinished"></translation></message>
    <message><source>Selected track</source><translation>Ausgewählte Spur</translation></message>
    <message><source>Line one:
%1</source><translation type="unfinished"></translation></message>
  </context>
<context>
    <name>motrack::WorkspaceWindow</name>
    <message>
        <source>Open &amp;video…</source>
        <translation type="unfinished" />
    </message>
    <message numerus="yes">
        <source>%n images</source>
        <translation type="unfinished">
            <numerusform /><numerusform /></translation>
    </message>
</context>

</TS>
"""


def candidate(english, translation, key="Key", rank=0, bundle="tracker_de.properties"):
    return t.Candidate(english, translation, f"{bundle} {key}", rank)


class PropertiesTests(unittest.TestCase):
    def test_parses_java_escapes_separators_comments_and_continuations(self):
        text = ("# comment\n! other comment\nplain=Value\nspaced : colon value\nwhite space value\n"
                "escaped\\=key=a\\tb\\nc\nunicode=\\u00e9\\u00df\\uD83D\\uDE00\ncontinued=first \\\n    second\n"
                "backslash=ends\\\\\nempty=\n")
        values = t.parse_properties(text)
        self.assertEqual(values["plain"], "Value")
        self.assertEqual(values["spaced"], "colon value")
        self.assertEqual(values["white"], "space value")
        self.assertEqual(values["escaped=key"], "a\tb\nc")
        self.assertEqual(values["unicode"], "éß😀")
        self.assertEqual(values["continued"], "first second")
        self.assertEqual(values["backslash"], "ends\\")
        self.assertEqual(values["empty"], "")
        self.assertNotIn("# comment", values)

    def test_accepts_cr_only_line_endings(self):
        self.assertEqual(t.parse_properties("a=1\rb=2\r"), {"a": "1", "b": "2"})

    def test_decodes_utf8_latin1_bom_and_mixed_lines(self):
        self.assertEqual(t.decode_properties("x=Ångra".encode("utf-8")), "x=Ångra")
        self.assertEqual(t.decode_properties(b"\xef\xbb\xbfx=1"), "x=1")
        self.assertEqual(t.decode_properties("x=Gör".encode("iso-8859-1")), "x=Gör")
        mixed = "a=Ångra".encode("utf-8") + b"\n" + "b=Gör om".encode("iso-8859-1")
        self.assertEqual(t.parse_properties(t.decode_properties(mixed)), {"a": "Ångra", "b": "Gör om"})

    def test_java_resource_bundle_chain(self):
        self.assertEqual(t.java_chain("el_GR"), ["el_GR", "el"])
        self.assertEqual(t.java_chain("ar"), ["ar"])


class NormalizationTests(unittest.TestCase):
    def test_mnemonics(self):
        self.assertEqual(t.mnemonic_letters("Step si&ze"), ["z"])
        self.assertEqual(t.mnemonic_letters("Save && close"), [])
        self.assertEqual(t.mnemonic_letters("ファイル(&F)"), ["F"])
        self.assertEqual(t.strip_mnemonic("&Save && close"), "Save & close")

    def test_normalize_ignores_presentation_but_keeps_case(self):
        self.assertEqual(t.normalize("Step si&ze", mnemonics=True), t.normalize("Step size:"))
        self.assertEqual(t.normalize("&Open project…", mnemonics=True), t.normalize("Open project..."))
        self.assertNotEqual(t.normalize("&Open project…", mnemonics=True), t.normalize("Open Project..."))
        self.assertEqual(t.loose("&Open project…", mnemonics=True), t.loose("Open Project..."))
        self.assertEqual(t.loose("Point-mass value."), t.loose("point mass value"))

    def test_placeholders(self):
        self.assertEqual(t.placeholders("%1 of %2, %n, %L1, %1"), {"%1": 2, "%2": 1, "%n": 1, "%L1": 1})


class MatchingTests(unittest.TestCase):
    def test_exact_match_is_finished_and_adapted_to_the_source(self):
        index = t.TrackerIndex([candidate("Open video...", "Video öffnen...")])
        match = index.match("Open &video…")
        self.assertEqual((match.tier, match.finished), ("exact", True))
        self.assertEqual(t.adapt(match.candidate.translation, "Open &video…", LOCALE["de"]), "&Video öffnen…")

    def test_normalized_match_is_always_unfinished(self):
        # Tracker's title case can be copied into the translation (pt "Nova Massa Pontual"), so review it.
        title_case = t.TrackerIndex([candidate("New Point Mass", "Nova Massa Pontual")]).match("New &point mass")
        self.assertEqual((title_case.tier, title_case.finished), ("normalized", False))
        lower = t.TrackerIndex([candidate("mass", "masse")]).match("Mass")
        self.assertEqual((lower.tier, lower.finished), ("normalized", False))
        self.assertEqual(t.adapt("masse", "Mass", LOCALE["fr"]), "Masse")
        sentence = t.TrackerIndex([candidate("Calibration updated", "Kalibrierung aktualisiert")]).match(
            "Calibration updated.")
        self.assertFalse(sentence.finished)
        self.assertEqual(t.adapt("校准已更新", "Calibration updated.", LOCALE["zh_CN"]), "校准已更新。")

    def test_ambiguous_candidates_choose_the_majority_and_stay_unfinished(self):
        index = t.TrackerIndex([candidate("View", "Fenster", "A"), candidate("View", "Ansicht", "B"),
                                candidate("View", "Ansicht", "C")])
        match = index.match("&View")
        self.assertEqual((match.candidate.translation, match.finished, match.alternatives), ("Ansicht", False, 2))

    def test_tracker_catalog_wins_over_osp_and_preferred_keys_win_over_votes(self):
        index = t.TrackerIndex([candidate("Clip Settings", "OSP", "ClipInspector.Title", 1),
                                candidate("Clip Settings", "Tracker", "TToolBar.Overflow.ClipSettings", 0)])
        match = index.match("Clip Settings")
        self.assertEqual((match.candidate.translation, match.finished), ("OSP", True))
        index = t.TrackerIndex([candidate("Tools", "OSP", "Menu.Tools", 1), candidate("Tools", "Werkzeuge", "X", 0)])
        self.assertEqual(index.match("T&ools").candidate.translation, "Werkzeuge")

    def test_strings_with_placeholders_are_never_matched(self):
        self.assertIsNone(t.TrackerIndex([candidate("Vector %1", "Vektor %1")]).match("Vector %1"))

    def test_rejects_untranslated_or_annotated_values(self):
        self.assertFalse(t.usable_translation("Tracks", "軌跡Tracks"))
        self.assertFalse(t.usable_translation("Experiments", "实验801"))
        self.assertFalse(t.usable_translation("Project", "Έργου (.trz)"))
        self.assertFalse(t.usable_translation("Video", "Video"))
        self.assertTrue(t.usable_translation("Frame %1", "Bild %1"))

    def test_rejects_values_with_lost_characters(self):
        # OSP's Simplified Chinese tools and video bundles store "?" where a character was lost (osp#59).
        self.assertFalse(t.usable_translation("Data Tool", "?据分析工具"))
        self.assertFalse(t.usable_translation("Bar", "??"))
        self.assertTrue(t.usable_translation("Data Tool", "数据分析工具"))
        self.assertTrue(t.usable_translation("Replace existing file?", "替換已有文檔?"))
        self.assertFalse(t.usable_translation("Replace existing file", "替換已有?"))

    def test_restored_values_have_no_lost_characters_and_name_known_bundles(self):
        bundles = {Path(stem).name for _, stem in t.BUNDLES}
        for catalog in (locale.catalog for locale in t.LOCALES):
            for filename, values in t.restored_values(catalog).items():
                self.assertIn(filename.split("_")[0], bundles, filename)
                self.assertTrue(values, filename)
                for key, value in values.items():
                    self.assertTrue(value.strip("? "), f"{filename} {key}")  # A question may end in "?".
                    self.assertFalse(value.startswith("?") or "??" in value, f"{filename} {key}")
        self.assertEqual(len(t.restored_values("zh_CN")), 2)
        self.assertEqual(t.restored_values("de"), {})

    def test_mnemonic_insertion(self):
        de, ja, ru = LOCALE["de"], LOCALE["ja"], LOCALE["ru"]
        self.assertEqual(t.insert_mnemonic("Video öffnen", "v", de), "&Video öffnen")
        self.assertEqual(t.insert_mnemonic("Video öffnen", "v", de, {"v"}), "Video &öffnen")
        self.assertEqual(t.insert_mnemonic("Schrittweite", "z", de), "&Schrittweite")
        self.assertEqual(t.insert_mnemonic("動画を開く", "v", ja), "動画を開く(&V)")
        self.assertEqual(t.insert_mnemonic("Открыть видео", "v", ru), "&Открыть видео")
        self.assertEqual(t.adapt("動画を開く...", "Open &video…", ja), "動画を開く(&V)…")
        self.assertEqual(t.adapt("Copy & Paste", "&Copy", de), "&Copy && Paste")
        self.assertEqual(t.adapt("Taille de l'incrément :", "Step si&ze", LOCALE["fr"]), "&Taille de l'incrément")


class BundleTests(unittest.TestCase):
    def test_loads_locale_chain_and_skips_stale_default_and_rejected_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stem = root / t.BUNDLES[0][1]
            stem.parent.mkdir(parents=True)
            stem.with_name("tracker.properties").write_text(
                "TMenuBar.Menu.File=File\nTActions.Action.SaveZip=Save Project\nPointMass.New.Name=mass\n"
                "Dialog.Button.Cancel=Cancel\nZoom=Zoom In\nGreek=Axes\n", encoding="utf-8")
            stem.with_name("tracker_el.properties").write_bytes(
                "Greek=\\u0386\\u03be\\u03bf\\u03bd\\u03b5\\u03c2\nTMenuBar.Menu.File=Generic\n".encode("ascii"))
            stem.with_name("tracker_el_GR.properties").write_text(
                "TMenuBar.Menu.File=Αρχείο\nTActions.Action.SaveZip=Εξαγωγή ZIP\nPointMass.New.Name=Σώμα\n"
                "Dialog.Button.Cancel=Cancel\n", encoding="utf-8")
            index = t.TrackerIndex.load({"tracker": root}, LOCALE["el"])
            self.assertEqual(index.match("&File").candidate.translation, "Αρχείο")
            self.assertEqual(index.match("&File").candidate.origin, "tracker_el_GR.properties TMenuBar.Menu.File")
            self.assertEqual(index.match("Axes").candidate.translation, "Άξονες")
            self.assertIsNone(index.match("&Save project"))  # Stale key.
            self.assertIsNone(index.match("Mass"))  # Default object name.
            self.assertIsNone(index.match("Cancel"))  # Untranslated.
            with patch.dict(t.LOCALE_REJECTED, {"el": {("File", "Αρχείο")}}):
                self.assertIsNone(t.TrackerIndex.load({"tracker": root}, LOCALE["el"]).match("&File"))

    def test_seeds_a_restored_value_only_in_place_of_one_with_lost_characters(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for repository, bundle in t.BUNDLES:  # Every OSP base bundle must exist; only "tools" has values here.
                if repository == "osp":
                    (root / bundle).parent.mkdir(parents=True, exist_ok=True)
                    (root / bundle).with_name(Path(bundle).name + ".properties").write_text("", encoding="utf-8")
            stem = root / next(stem for repository, stem in t.BUNDLES if stem.endswith("/tools"))
            stem.with_name("tools.properties").write_text(
                "Frame.Title=Data Tool\nChooser.Title=Select Data\nShape.Bar=Bar\nIntact.Key=Markers\n", encoding="utf-8")
            # As upstream stores them: "?" for each lost character, the others as Java escapes.
            stem.with_name("tools_zh_CN.properties").write_bytes(
                b"Frame.Title=?\\u636e\\u5206\\u6790\\u5de5\\u5177\nChooser.Title=???\\u636e\nShape.Bar=??\n"
                b"Intact.Key=\\u6807\\u8bb0\n")
            restored = {"tools_zh_CN.properties": {"Frame.Title": "数据分析工具", "Chooser.Title": "选择数据",
                                                   "Intact.Key": "never used"}}
            with patch.object(t, "restored_values", lambda catalog, path=None: restored if catalog == "zh_CN" else {}):
                index = t.TrackerIndex.load({"osp": root}, LOCALE["zh_CN"])
                other = t.TrackerIndex.load({"osp": root}, LOCALE["zh_TW"])
            match = index.match("Data Tool")
            self.assertEqual(match.candidate.translation, "数据分析工具")
            self.assertEqual(match.candidate.origin, f"tools_zh_CN.properties@{t.restored_revision()} Frame.Title")
            self.assertEqual(index.match("Select Data").candidate.translation, "选择数据")
            self.assertIsNone(index.match("Bar"))  # Lost, and no earlier value is recorded.
            self.assertEqual(index.match("Markers").candidate.translation, "标记")  # An intact value is kept.
            self.assertEqual(index.match("Markers").candidate.origin, "tools_zh_CN.properties Intact.Key")
            self.assertIsNone(other.match("Data Tool"))  # Another locale never takes zh_CN's values.

    def test_missing_base_bundle_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(t.ToolError):
                t.TrackerIndex.load({"tracker": Path(directory)}, LOCALE["de"])


class CatalogTests(unittest.TestCase):
    def catalog(self, directory, text=SAMPLE, name="motrack_de.ts"):
        path = Path(directory) / name
        path.write_text(text, encoding="utf-8", newline="\n")
        return t.Catalog.load(path)

    def test_fills_entries_in_place_and_keeps_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = self.catalog(directory)
            entries = {entry.source: entry for entry in catalog.entries()}
            catalog.set_translation(entries["&Edit"], ["&Bearbeiten"], True, "Tracker exact: tracker_de.properties X")
            catalog.set_translation(entries["Open &video…"], ["&Video öffnen…"], False, t.MACHINE_NOTE)
            catalog.set_translation(entries["%n images"], ["%n Bild", "%n Bilder"], False, t.MACHINE_NOTE)
            catalog.set_translation(entries["Line one:\n%1"], ["Zeile eins:\n%1"], False, t.MACHINE_NOTE)
            self.assertTrue(catalog.save())
            text = catalog.path.read_text(encoding="utf-8")
            self.assertIn('    <message><source>&amp;Edit</source><translatorcomment>Tracker exact: tracker_de.properties X'
                          '</translatorcomment><translation>&amp;Bearbeiten</translation></message>\n', text)
            self.assertIn("        <translatorcomment>Machine translation; review before marking finished."
                          "</translatorcomment>\n        <translation type=\"unfinished\">&amp;Video öffnen…</translation>\n", text)
            self.assertIn('<translation type="unfinished">\n            <numerusform>%n Bild</numerusform>\n'
                          '            <numerusform>%n Bilder</numerusform>\n        </translation>', text)
            self.assertIn("<source>Selected track</source><translation>Ausgewählte Spur</translation>", text)
            self.assertTrue(text.endswith("</context>\n\n</TS>\n"))
            reloaded = {entry.source: entry for entry in t.Catalog.load(catalog.path).entries()}
            self.assertEqual(reloaded["%n images"].forms, ["%n Bild", "%n Bilder"])
            self.assertEqual(reloaded["&Edit"].provenance, "tracker-exact")
            self.assertEqual(reloaded["Selected track"].provenance, "human")
            self.assertFalse(t.Catalog.load(catalog.path).save())  # Unchanged file is not rewritten.

    def test_replacing_an_entry_twice_writes_it_once(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = self.catalog(directory)
            entry = next(entry for entry in catalog.entries() if entry.source == "&Edit")
            catalog.set_translation(entry, ["&Erste"], False, t.MACHINE_NOTE)
            catalog.set_translation(entry, ["&Zweite"], False, t.MACHINE_NOTE)
            text = catalog.render()
            self.assertEqual(text.count("<source>&amp;Edit</source>"), 1)
            self.assertIn("&amp;Zweite", text)
            self.assertNotIn("&amp;Erste", text)

    def test_adds_missing_messages_in_matching_style_and_new_contexts(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = self.catalog(directory)
            catalog.add_messages([t.SourceMessage("MainWindow", "Ready", None, False),
                                  t.SourceMessage("motrack::WorkspaceWindow", "Workspace", None, False),
                                  t.SourceMessage("PositionPlot", "%n points", "plot", True)], 2)
            text = catalog.render()
            self.assertIn('    <message><source>Ready</source><translation type="unfinished"></translation></message>\n  </context>',
                          text)
            self.assertIn('    <message>\n        <source>Workspace</source>\n        <translation type="unfinished"></translation>\n'
                          '    </message>\n</context>', text)
            self.assertIn('  <context>\n    <name>PositionPlot</name>\n    <message numerus="yes"><source>%n points</source>'
                          '<comment>plot</comment><translation type="unfinished"><numerusform></numerusform>'
                          '<numerusform></numerusform></translation></message>\n  </context>\n</TS>', text)
            ET.fromstring(text.encode("utf-8"))

    def test_new_catalog_has_language_and_parses(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = t.Catalog.new(Path(directory) / "motrack_ar.ts", LOCALE["ar"])
            catalog.add_messages([t.SourceMessage("MainWindow", "%n images", None, True)], LOCALE["ar"].forms)
            text = catalog.render()
            self.assertIn('language="ar"', text)
            self.assertEqual(text.count("<numerusform>"), 6)

    def test_seed_fills_only_empty_and_tool_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = self.catalog(directory)
            index = t.TrackerIndex([candidate("Edit", "Bearbeiten"), candidate("Selected track", "Gewählte Spur"),
                                    candidate("Open Video", "Video öffnen")])
            counts = t.seed(catalog, index, LOCALE["de"])
            self.assertEqual(counts, {"exact": 1, "normalized": 1})
            entries = {entry.source: entry for entry in catalog.entries()}
            self.assertEqual(entries["&Edit"].forms, ["&Bearbeiten"])
            self.assertEqual(entries["Selected track"].forms, ["Ausgewählte Spur"])  # Human, untouched.
            self.assertEqual(entries["Open &video…"].forms, ["&Video öffnen…"])
            self.assertEqual(entries["Open &video…"].kind, "unfinished")

    def test_reseeding_updates_or_clears_finished_tracker_seeds_but_not_reviewed_ones(self):
        with tempfile.TemporaryDirectory() as directory:
            catalog = self.catalog(directory)
            first = t.TrackerIndex([candidate("Edit", "Bearbeiten"), candidate("Selected track", "Gewählte Spur")])
            t.seed(catalog, first, LOCALE["de"])
            catalog = t.Catalog(catalog.path, catalog.render())
            entry = next(entry for entry in catalog.entries() if entry.source == "&Edit")
            self.assertEqual((entry.kind, entry.provenance), (None, "tracker-exact"))
            self.assertTrue(entry.tool_owned)
            # The value is now rejected: the finished seed is emptied for a new translation, without a comment.
            self.assertEqual(t.seed(catalog, t.TrackerIndex([]), LOCALE["de"]), {"cleared": 1})
            catalog = t.Catalog(catalog.path, catalog.render())
            entry = next(entry for entry in catalog.entries() if entry.source == "&Edit")
            self.assertEqual((entry.kind, entry.empty, entry.note), ("unfinished", True, None))
            self.assertNotIn("translatorcomment", entry.text)
            # A reviewed entry (provenance comment removed) is never replaced.
            reviewed = SAMPLE.replace("<message><source>&amp;Edit</source><translation type=\"unfinished\"></translation>",
                                      "<message><source>&amp;Edit</source><translation>&amp;Bearbeiten</translation>")
            catalog = self.catalog(directory, reviewed)
            t.seed(catalog, t.TrackerIndex([candidate("Edit", "Editieren")]), LOCALE["de"])
            self.assertIn("&amp;Bearbeiten", catalog.render())


class ValidationTests(unittest.TestCase):
    def problems(self, source, forms, catalog="de", numerus=False):
        return t.validate_translation(source, forms, numerus, LOCALE[catalog])

    def test_placeholders_mnemonics_and_layout(self):
        self.assertEqual(self.problems("Opening %1…", ["%1 wird geöffnet…"]), [])
        self.assertTrue(self.problems("Opening %1…", ["Öffnen…"]))
        self.assertTrue(self.problems("%1/%2", ["%1/%1"]))
        self.assertTrue(self.problems("Open &video…", ["Video öffnen…"]))
        self.assertTrue(self.problems("Open &video…", ["&Video &öffnen…"]))
        self.assertTrue(self.problems("Ready", ["&Bereit"]))
        self.assertTrue(self.problems("Line:\n%1", ["Zeile: %1"]))
        self.assertTrue(self.problems("Ready", [" Bereit"]))
        self.assertTrue(self.problems("%1[*] — MoTrack", ["%1 — MoTrack"]))
        self.assertTrue(self.problems("Tracker projects (*.trk)", ["Tracker-Projekte (*.trz)"]))
        self.assertEqual(self.problems("Save && close", ["Speichern && schließen"]), [])

    def test_plural_forms(self):
        self.assertEqual(self.problems("%n images", ["%n Bild", "%n Bilder"], numerus=True), [])
        self.assertTrue(self.problems("%n images", ["%n Bilder"], numerus=True))
        self.assertTrue(self.problems("%n images", ["%n Bild", ""], numerus=True))
        self.assertTrue(self.problems("%n images", ["ein Bild", "Bilder"], numerus=True))
        self.assertTrue(self.problems("%n images", ["%n Bild %n", "%n Bilder"], numerus=True))
        arabic = ["لا توجد صور", "صورة واحدة", "صورتان", "%n صور", "%n صورة", "%n صورة"]
        self.assertEqual(self.problems("%n images", arabic, "ar", True), [])
        self.assertEqual(self.problems("%n images", ["%n 枚の画像"], "ja", True), [])

    def test_check_catalog_reports_structure_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motrack_fr.ts"
            text = SAMPLE.replace('language="de"', 'language="fr"').replace(
                "<message><source>Selected track</source><translation>Ausgewählte Spur</translation></message>",
                "<message><source>Selected track</source><translation></translation></message>"
                "<message><source>&amp;Edit</source><translation>É&amp;dition</translation></message>")
            path.write_text(text, encoding="utf-8")
            sources = [t.SourceMessage("MainWindow", "&Edit", None, False),
                       t.SourceMessage("MainWindow", "Missing", None, False)]
            errors, warnings = t.check_catalog(t.Catalog.load(path), LOCALE["de"], sources)
            joined = "\n".join(errors)
            self.assertIn("language 'fr', expected 'de'", joined)
            self.assertIn("duplicate message", joined)
            self.assertIn("finished message without translation", joined)
            self.assertIn("missing MainWindow: 'Missing'", joined)
            self.assertTrue(any("no longer in the sources" in warning for warning in warnings))

    def test_mnemonic_collisions_beyond_english_are_warnings(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motrack_de.ts"
            rows = "".join(f"<message><source>{t.escape(source)}</source><translation>{t.escape(text)}</translation></message>"
                           for source, text in (("&Undo", "&Rückgängig"), ("&Redo", "&Wiederholen")))
            path.write_text(SAMPLE.replace("<message><source>&amp;Edit</source>", rows + "<message><source>&amp;Edit</source>"),
                            encoding="utf-8")
            self.assertFalse(t.check_catalog(t.Catalog.load(path), LOCALE["de"], None)[1])
            path.write_text(path.read_text(encoding="utf-8").replace("&amp;Wiederholen", "W&amp;iederholen").replace(
                "&amp;Rückgängig", "Rückgängig &amp;i"), encoding="utf-8")
            self.assertIn("Edit menu repeats mnemonic i", "\n".join(t.check_catalog(t.Catalog.load(path), LOCALE["de"], None)[1]))


class CommandTests(unittest.TestCase):
    def run_command(self, *argv):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = t.main(list(argv))
        return code, output.getvalue(), errors.getvalue()

    def test_import_validates_before_writing_and_pending_lists_remaining_strings(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(t, "TRANSLATIONS", Path(directory)):
            (Path(directory) / "motrack_de.ts").write_text(SAMPLE, encoding="utf-8")
            reference = SAMPLE.replace('language="de"', 'language="fr"').replace("Ausgewählte Spur", "Piste choisie")
            (Path(directory) / "motrack_fr.ts").write_text(reference, encoding="utf-8")
            pending = Path(directory) / "pending.json"
            self.assertEqual(self.run_command("pending", "--locale", "de", "--out", str(pending), "--reference", "fr")[0], 0)
            data = json.loads(pending.read_text(encoding="utf-8"))
            self.assertEqual([item["source"] for item in data["pending"]], ["&Edit", "Line one:\n%1", "Open &video…", "%n images"])
            self.assertEqual(data["pending"][3]["plural_forms"], 2)
            self.assertEqual(data["pending"][0]["mnemonic_scope"], "menu bar")
            self.assertEqual(data["existing"], {"Selected track": "Ausgewählte Spur"})

            supplied = Path(directory) / "mt.json"
            supplied.write_text(json.dumps({"&Edit": "Bearbeiten", "%n images": ["%n Bild", "%n Bilder"]}), encoding="utf-8")
            code, _, errors = self.run_command("import", "--locale", "de", "--input", str(supplied))
            self.assertEqual(code, 1)
            self.assertIn("mnemonics", errors)
            self.assertNotIn("%n Bild", (Path(directory) / "motrack_de.ts").read_text(encoding="utf-8"))

            supplied.write_text(json.dumps({"&Edit": "&Bearbeiten", "%n images": ["%n Bild", "%n Bilder"],
                                            "Selected track": "Überschrieben"}), encoding="utf-8")
            code, output, _ = self.run_command("import", "--locale", "de", "--input", str(supplied))
            self.assertEqual((code, "2 machine translations imported" in output), (0, True))
            entries = {entry.source: entry for entry in t.Catalog.load(Path(directory) / "motrack_de.ts").entries()}
            self.assertEqual(entries["&Edit"].provenance, "machine")
            self.assertEqual(entries["Selected track"].forms, ["Ausgewählte Spur"])

            code, output, _ = self.run_command("stats", "--locale", "de")
            self.assertEqual(code, 0)
            self.assertRegex(output, r"de\s+5\s+1\s+2\s+2\s+0\s+0\s+2\s+1")

    def test_unknown_locale_is_reported(self):
        code, _, errors = self.run_command("stats", "--locale", "xx")
        self.assertEqual(code, 1)
        self.assertIn("Unknown locale", errors)


class RepositoryCatalogTests(unittest.TestCase):
    """The committed catalogs: every Tracker locale, valid structure and translations, CMake registration."""

    # Tracker.getLocales() on the SwingJS branch, without English, mapped to Qt catalog names.
    TRACKER_LOCALES = {"ar": "ar", "ca": "ca", "cs": "cs", "da": "da", "de": "de", "el_GR": "el", "es": "es",
                       "fi": "fi", "fr": "fr", "hu_HU": "hu", "in": "id", "it": "it", "iw_IL": "he", "ja": "ja",
                       "ko": "ko", "lv": "lv", "ms_MY": "ms", "nl_NL": "nl", "pl": "pl", "pt_BR": "pt_BR",
                       "pt_PT": "pt", "ru": "ru", "sk": "sk", "sl": "sl", "sv": "sv", "th_TH": "th", "tr": "tr",
                       "uk": "uk", "vi_VN": "vi", "zh_CN": "zh_CN", "zh_TW": "zh_TW"}

    def test_locale_table_covers_tracker(self):
        self.assertEqual({locale.java: locale.catalog for locale in t.LOCALES}, self.TRACKER_LOCALES)
        for catalog, rule in t.PLURAL_RULES.items():
            self.assertEqual(len(rule), LOCALE[catalog].forms, catalog)
        self.assertEqual({locale.catalog for locale in t.LOCALES if locale.rtl}, {"ar", "he"})

    def test_cmake_lists_every_catalog(self):
        self.assertEqual(t.check_cmake(), [])

    def test_english_catalog_holds_only_plural_forms(self):
        english = t.Catalog.load(t.catalog_path(t.ENGLISH))
        sources = t.read_messages(t.TRANSLATIONS / "motrack_de.ts")
        self.assertEqual(t.check_catalog(english, t.ENGLISH, sources)[0], [])
        self.assertEqual({entry.source for entry in english.entries()}, {m.source for m in sources if m.numerus})
        self.assertEqual(t.selected_locales(["en"], english=True), [t.ENGLISH])
        with self.assertRaises(t.ToolError):
            t.selected_locales(["en"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "motrack_en.ts"
            path.write_text(SAMPLE.replace('language="de"', 'language="en"'), encoding="utf-8")
            errors = "\n".join(t.check_catalog(t.Catalog.load(path), t.ENGLISH, None)[0])
            self.assertIn("holds only plural messages", errors)
            self.assertIn("write the English plural forms", errors)

    def test_every_catalog_is_valid(self):
        sources = t.read_messages(t.TRANSLATIONS / "motrack_de.ts")
        for locale in t.LOCALES:
            with self.subTest(locale=locale.catalog):
                catalog = t.Catalog.load(t.catalog_path(locale))
                errors, _ = t.check_catalog(catalog, locale, None)
                self.assertEqual(errors, [])
                present = {entry.key for entry in catalog.entries()}
                self.assertEqual([m.source for m in sources if m.key not in present and m.source != "Save project &as…"], [])

    def test_mnemonic_groups_name_current_source_strings(self):
        sources = {entry.source for entry in t.Catalog.load(t.TRANSLATIONS / "motrack_ar.ts").entries()}
        for group, members in t.MNEMONIC_GROUPS.items():
            self.assertEqual([member for member in members if member not in sources], [], group)

    @unittest.skipUnless(shutil.which("lrelease"), "lrelease is not on PATH (CTest runs it via translation_catalogs)")
    def test_lrelease_compiles_every_catalog(self):
        for locale in t.LOCALES:
            with self.subTest(locale=locale.catalog):
                self.assertIsNone(t.run_lrelease(shutil.which("lrelease"), t.catalog_path(locale)))


if __name__ == "__main__":
    unittest.main()
