"""Maintain MoTrack's Qt translation catalogs (Python standard library only).

MoTrack offers every UI language listed by Tracker's ``Tracker.getLocales()`` (SwingJS branch). Empty catalog
entries are seeded from Tracker and OSP resource bundles when Tracker's English text matches a MoTrack source
string; the remaining strings receive machine translations marked ``unfinished`` for later human review. Existing
human translations are never modified. See CONTRIBUTING.md, "Translations".

Commands:
  update   Add missing source strings to every catalog (creating absent catalogs) and seed empty entries from
           local Tracker/OSP checkouts. Without --tracker/--osp it only synchronizes source strings.
  pending  Write the strings a catalog still lacks, its plural rule and Tracker terminology as JSON for translators.
  import   Fill empty entries of one catalog from a JSON object that maps source text to a translation (a list of
           plural forms for numerus messages). Entries are validated first and marked unfinished.
  check    Validate catalogs: XML, language, completeness against lupdate, placeholders, mnemonics, plural forms,
           the CMake catalog list and lrelease.
  stats    Report finished/unfinished/empty counts and provenance for each catalog.

Catalogs keep their hand-maintained layout: the tool edits only the messages it fills or adds.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
TRANSLATIONS = ROOT / "translations"
RESTORED = ROOT / "tools" / "translation-restored.json"


@dataclass(frozen=True)
class Locale:
    """One MoTrack UI locale and the Tracker locale it is seeded from."""

    catalog: str  # motrack_<catalog>.ts; QTranslator matches it against QLocale::uiLanguages().
    language: str  # Qt TS language attribute; lrelease derives the plural rule from it.
    java: str  # Tracker/OSP bundle suffix (Java keeps legacy codes such as "iw" and "in").
    forms: int  # Qt numerus forms for this language (as written by lupdate -target-language).
    rtl: bool = False


# Tracker.getLocales() on the SwingJS branch, minus English (MoTrack's source language). Unlisted Tracker catalogs
# (fil_PH, sn, sr and the duplicate ca_UTF encoding) are not offered by Tracker's language menu and are not shipped.
LOCALES = (
    Locale("ar", "ar", "ar", 6, rtl=True),
    Locale("ca", "ca", "ca", 2),
    Locale("cs", "cs", "cs", 3),
    Locale("da", "da", "da", 2),
    Locale("de", "de", "de", 2),
    Locale("el", "el", "el_GR", 2),
    Locale("es", "es", "es", 2),
    Locale("fi", "fi", "fi", 2),
    Locale("fr", "fr", "fr", 2),
    Locale("he", "he", "iw_IL", 2, rtl=True),
    Locale("hu", "hu", "hu_HU", 1),
    Locale("id", "id", "in", 1),
    Locale("it", "it", "it", 2),
    Locale("ja", "ja", "ja", 1),
    Locale("ko", "ko", "ko", 1),
    Locale("lv", "lv", "lv", 3),
    Locale("ms", "ms", "ms_MY", 1),
    Locale("nl", "nl", "nl_NL", 2),
    Locale("pl", "pl", "pl", 3),
    Locale("pt", "pt_PT", "pt_PT", 2),
    Locale("pt_BR", "pt_BR", "pt_BR", 2),
    Locale("ru", "ru", "ru", 3),
    Locale("sk", "sk", "sk", 3),
    Locale("sl", "sl", "sl", 4),
    Locale("sv", "sv", "sv", 2),
    Locale("th", "th", "th_TH", 1),
    Locale("tr", "tr", "tr", 1),
    Locale("uk", "uk", "uk", 3),
    Locale("vi", "vi", "vi_VN", 1),
    Locale("zh_CN", "zh_CN", "zh_CN", 1),
    Locale("zh_TW", "zh_TW", "zh_TW", 1),
)
LOCALE_BY_CATALOG = {locale.catalog: locale for locale in LOCALES}

# English is the source language. motrack_en.ts (qt_add_translations PLURALS_TS_FILE) holds only English plural forms.
# Shipping it also makes QTranslator stop at English when English precedes another language in the system's list.
ENGLISH = Locale("en", "en", "en", 2)

# Qt Linguist plural rules (numerus.cpp), in form order, for translators. Two-form languages not listed use n == 1.
PLURAL_RULES = {
    "ar": ("n == 0", "n == 1", "n == 2", "n % 100 in 3..10", "n % 100 in 11..99", "other (100, 101, 102, ...)"),
    "cs": ("n == 1", "n in 2..4", "other"),
    "sk": ("n == 1", "n in 2..4", "other"),
    "lv": ("n % 10 == 1 and n % 100 != 11", "other nonzero", "n == 0"),
    "pl": ("n == 1", "n % 10 in 2..4 and n % 100 not in 10..19", "other"),
    "ru": ("n % 10 == 1 and n % 100 != 11", "n % 10 in 2..4 and n % 100 not in 10..19", "other"),
    "uk": ("n % 10 == 1 and n % 100 != 11", "n % 10 in 2..4 and n % 100 not in 10..19", "other"),
    "sl": ("n % 100 == 1", "n % 100 == 2", "n % 100 in 3..4", "other"),
    "fr": ("n <= 1", "other"),
    "pt_BR": ("n <= 1", "other"),
}

# Tracker resource bundles, highest priority first: Tracker's own catalog, then the OSP bundles MoTrack shares.
BUNDLES = (
    ("tracker", "src/org/opensourcephysics/cabrillo/tracker/resources/tracker"),
    ("osp", "src/org/opensourcephysics/resources/media/video"),
    ("osp", "src/org/opensourcephysics/resources/tools/tools"),
    ("osp", "src/org/opensourcephysics/resources/display/display_res"),
    ("osp", "src/org/opensourcephysics/resources/display/dialogs_res"),
    ("osp", "src/org/opensourcephysics/resources/controls/controls_res"),
)

# Menus and dialogs whose mnemonics share one keyboard scope. English already repeats some letters; check reports
# a catalog only when it adds collisions beyond the English ones.
MNEMONIC_GROUPS = {
    "menu bar": ("&File", "&Edit", "&View", "&Tracks", "T&ools", "&Experiments"),
    "File menu": ("&Open project…", "Open &video…", "Open &images…", "&Save project", "Save MoTrack copy &as…",
                  "Save &Tracker project…", "Export portable &archive…", "&Export measurements…", "Export &plot data…",
                  "Save &all projects", "&Revert experiment…", "&Hide experiment view", "Close &project…", "&Quit"),
    "Edit menu": ("&Undo", "&Redo"),
    "View menu": ("Show coordinate &axes", "Zoom &in", "Zoom &out", "&Fit video to view"),
    "Tracks menu": ("New &point mass", "New &vector", "&Rename track…", "Set &mass…", "&Delete track",
                    "Delete mark on this &frame", "&AutoTracker"),
    "Tools menu": ("Measure &distance", "Measure &angle"),
    "Clip Settings": ("&Start frame", "&End frame", "Step si&ze"),
}
GROUP_OF = {source: group for group, sources in MNEMONIC_GROUPS.items() for source in sources}

# English terms whose Tracker translations translators should reuse.
GLOSSARY_TERMS = (
    "Point Mass", "Vector", "Calibration Stick", "Calibration Tape", "Calibration Points", "Tape Measure",
    "Protractor", "Axes", "Coordinate System", "Clip Settings", "Start frame", "End frame", "Step size", "Frame",
    "Step", "Track", "Tracks", "Mass", "Position", "Velocity", "Speed", "Acceleration", "Momentum",
    "Kinetic Energy", "Time", "Distance", "Angle", "Autotracker", "Template", "Target", "Undo", "Redo", "File",
    "Edit", "View", "Tools", "Export", "Import", "Open", "Save", "Save As", "Close", "Cancel", "OK", "Zoom In",
    "Zoom Out", "Video", "Images", "Image", "Project", "Tab", "Plot", "Table", "Data", "Origin", "Units", "Unit",
    "Calibration", "Clip", "Delete", "Rename", "Name", "Linear", "Slope", "Intercept", "Fit", "Exit", "Length",
    "Scale", "Help", "Mark", "Point", "Tip", "Tail", "Experiment", "Archive", "Library",
)

# Tracker keys whose English text was later changed while most translations kept the old meaning (checked across
# all locales on 2026-11-21): "Save Project" was "Export ZIP", "View" was "Window", "Track" was "Track Control".
STALE_KEYS = frozenset({"TActions.Action.SaveZip", "ZipResourceDialog.Title", "TMenuBar.Menu.Window",
                        "TrackControl.Name"})

# Keys whose English text is a homograph of a MoTrack string with another meaning: Edit > Copy Image > "Frame" copies
# the whole Tracker window (TMenuBar.java), not a video frame.
HOMOGRAPH_KEYS = frozenset({"TMenuBar.MenuItem.CopyFrame"})

# Tracker keys used in the same role as a MoTrack string, chosen over other keys with the same English text.
PREFERRED_KEYS = {
    "&File": ("TMenuBar.Menu.File",),
    "&Edit": ("TMenuBar.Menu.Edit",),
    "T&ools": ("TMenuBar.Menu.Tools",),
    "&Undo": ("TMenuBar.MenuItem.Undo",),
    "&Redo": ("TMenuBar.MenuItem.Redo",),
    "Zoom &in": ("MainTView.Popup.MenuItem.ZoomIn",),
    "Zoom &out": ("MainTView.Popup.MenuItem.ZoomOut",),
    "Clip Settings": ("ClipInspector.Title",),
    "&Start frame": ("ClipInspector.Label.StartFrame",),
    "&End frame": ("ClipInspector.Label.EndFrame",),
    "Step si&ze": ("ClipInspector.Label.StepSize",),
    "Frame": ("VideoPlayer.Readout.MenuItem.Frame",),  # Frame-number column and plot axis.
    "Play": ("VideoPlayer.Button.Play.ToolTip",),
    "Pause": ("VideoPlayer.Button.Pause.ToolTip",),
    "Cancel": ("Dialog.Button.Cancel",),
}


# (English, translation) pairs in Tracker catalogs that do not mean the English text, reviewed across every matched
# MoTrack string on 2026-11-21. Other keys with the same English text may still match.
LOCALE_REJECTED = {  # Upstream reports: MoTrack #274.
    "ar": {("kinetic energy", "الطاقةالحركية")},  # Missing space.
    "ca": {("Edit", "Edia")},  # Typo.
    "da": {("Redo", "Fortryd fortryd"), ("Track", "Markeringer")},  # "Undo undo"; "markings" (MoTrack's marks).
    "fi": {("New Point Mass", "Uusi paikka")},  # "New place".
    "he": {("Pause", "דום")},  # "Freeze!" (command).
    "hu": {("Length Unit", "Hosszúság")},  # "Length".
    "ko": {("mass", "질점"), ("View", "창"), ("View", "개체")},  # "Point mass"; "window"; "object".
    "pl": {("Length Unit", "DŁugość jednostek")},  # Typo, "length of units".
    "sv": {("View", "Vy")},  # Swedish View menus say "Visa".
    "uk": {("Zoom In", "Зменшити"), ("Zoom Out", "Збільшити"), ("Track", "Спостереження")},  # Swapped; "observation".
    "vi": {("Undo", "Hủy bỏ"), ("View", "Tầm nhìn"), ("View", "Nhìn"), ("View", "Lượt xem"),  # "Cancel"; "eyesight";
           ("Step size", "Khích thước bước")},  # "look"; "view count"; typo.
    "zh_CN": {("Zoom In", "缩小"), ("Zoom Out", "放大"), ("Clip Settings", "视频剪辑固定")},  # Swapped; "clip fixed".
    "zh_TW": {("Start frame", "開啟視窗"), ("End frame", "末頁框"), ("Frame", "視窗")},  # "Open window"; "window".
}

def lost_characters(english: str, translation: str) -> bool:
    """A translation that stores a literal "?" where a character was lost in a file conversion, as OSP's Simplified
    Chinese tools and video bundles do ("?据分析工具" for "Data Tool"; OpenSourcePhysics/osp#59): more "?" than the
    English text has. A real question mark the English text lacks is rejected too; such a string is then translated
    anew rather than risk copying a damaged one."""
    return translation.count("?") > english.count("?")


def restored_values(catalog: str, path: Path | None = None) -> dict[str, dict[str, str]]:
    """Values an upstream bundle lost and an earlier revision still has, by bundle file name and key, for one MoTrack
    catalog (tools/translation-restored.json). Seeding uses one only in place of a value with lost characters."""
    path = RESTORED if path is None else path
    if not path.is_file():
        return {}
    document = json.loads(path.read_text(encoding="utf-8"))
    return document.get("locales", {}).get(catalog, {})


def restored_revision(path: Path | None = None) -> str:
    path = RESTORED if path is None else path
    return json.loads(path.read_text(encoding="utf-8")).get("revision", "restored") if path.is_file() else "restored"


# Catalogs not used for seeding. Tracker's Turkish catalog is written without Turkish letters (ç, ğ, ı, ö, ş, ü),
# so its terms are given to translators as a glossary instead of being copied.
UNSEEDED_LOCALES = {"tr"}


def usable_translation(english: str, translation: str) -> bool:
    """Reject untranslated, partly English, numbered or annotated Tracker values that are not plain translations."""
    english, translation = english.strip(), translation.strip()
    if not translation or translation == english:
        return False
    folded = normalize(english).casefold()
    if len(folded) >= 4 and folded in translation.casefold():
        return False  # English left inside the translation, as in "軌跡Tracks".
    if sorted(re.findall(r"\d", translation)) != sorted(re.findall(r"\d", english)):
        return False
    if lost_characters(english, translation):
        return False
    return all(translation.count(mark) <= english.count(mark) for mark in "()[]{}<>")


MACHINE_NOTE = "Machine translation; review before marking finished."
EXACT_NOTE = "Tracker exact"
NORMALIZED_NOTE = "Tracker normalized"
TOOL_NOTES = (MACHINE_NOTE.split(";")[0], EXACT_NOTE, NORMALIZED_NOTE)

PLACEHOLDER = re.compile(r"%L?(?:\d{1,2}|n)")
ELLIPSIS = re.compile(r"\.\.\.|…")
BRACKET_SCRIPT = {"ja", "ko", "zh_CN", "zh_TW", "th"}  # Menu mnemonics use a Latin "(&F)" suffix.
FULLWIDTH_PUNCTUATION = {"ja", "zh_CN", "zh_TW"}


class ToolError(RuntimeError):
    pass


# ---------------------------------------------------------------------------------------------------------------
# Java properties


def decode_properties(data: bytes) -> str:
    """Decode a properties file as UTF-8, else ISO-8859-1 (Java 9+ PropertyResourceBundle), line by line.

    Java decides for the whole file; a few Tracker catalogs are ISO-8859-1 with some UTF-8 lines (for example
    "Ångra" in tracker_sv.properties), which Java shows as mojibake. Decoding per line keeps both forms readable.
    """
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    lines = []
    for line in data.split(b"\n"):
        try:
            lines.append(line.decode("utf-8"))
        except UnicodeDecodeError:
            lines.append(line.decode("iso-8859-1"))
    return "\n".join(lines)


def _unescape(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        char = text[i]
        if char != "\\" or i + 1 == len(text):
            out.append(char)
            i += 1
            continue
        escaped = text[i + 1]
        if escaped == "u" and re.fullmatch(r"[0-9a-fA-F]{4}", text[i + 2:i + 6]):
            out.append(chr(int(text[i + 2:i + 6], 16)))
            i += 6
            continue
        out.append({"t": "\t", "n": "\n", "r": "\r", "f": "\f"}.get(escaped, escaped))
        i += 2
    # Combine UTF-16 surrogate pairs written as two \u escapes.
    return "".join(out).encode("utf-16", "surrogatepass").decode("utf-16", "surrogatepass")


def parse_properties(text: str) -> dict[str, str]:
    """Parse java.util.Properties syntax, including CR-only line endings and continuation lines."""
    values: dict[str, str] = {}
    lines = re.split(r"\r\n|\r|\n", text)
    i = 0
    while i < len(lines):
        line = lines[i].lstrip(" \t\f")
        i += 1
        if not line or line[0] in "#!":
            continue
        while re.search(r"(?<!\\)(?:\\\\)*\\$", line) and i <= len(lines):
            line = line[:-1] + (lines[i].lstrip(" \t\f") if i < len(lines) else "")
            i += 1
        key_end = 0
        while key_end < len(line):
            char = line[key_end]
            if char == "\\":
                key_end += 2
                continue
            if char in "=: \t\f":
                break
            key_end += 1
        key = line[:key_end]
        rest = line[key_end:].lstrip(" \t\f")
        if rest[:1] in ("=", ":"):
            rest = rest[1:].lstrip(" \t\f")
        values[_unescape(key)] = _unescape(rest)
    return values


def read_properties(path: Path) -> dict[str, str]:
    return parse_properties(decode_properties(path.read_bytes()))


def java_chain(java_locale: str) -> list[str]:
    """ResourceBundle candidate suffixes, most specific first (base bundle excluded)."""
    parts = java_locale.split("_")
    return ["_".join(parts[:size]) for size in range(len(parts), 0, -1)]


# ---------------------------------------------------------------------------------------------------------------
# Normalization and matching


def mnemonic_letters(text: str) -> list[str]:
    """Characters marked by Qt '&' mnemonics ('&&' is a literal ampersand)."""
    letters = []
    i = 0
    while i < len(text) - 1:
        if text[i] == "&":
            following = text[i + 1]
            if following == "&":
                i += 2
                continue
            if not following.isspace():
                letters.append(following)
        i += 1
    return letters


def strip_mnemonic(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        if text[i] == "&" and i + 1 < len(text):
            following = text[i + 1]
            if following == "&":
                out.append("&")
                i += 2
                continue
            if not following.isspace():
                i += 1
                continue
        out.append(text[i])
        i += 1
    return "".join(out)


def _trim_suffixes(text: str) -> str:
    previous = None
    while previous != text:
        previous = text
        text = re.sub(r"[\s ]+$", "", text)
        text = re.sub(r"(?:\.\.\.|…|[:：])$", "", text)
    return text


def normalize(text: str, mnemonics: bool = False) -> str:
    """Presentation-neutral form: no mnemonic, one ellipsis form, collapsed spaces, no trailing ellipsis/colon."""
    if mnemonics:
        text = strip_mnemonic(text)
    text = ELLIPSIS.sub("…", text)
    text = re.sub(r"[\s ]+", " ", text).strip()
    return _trim_suffixes(text)


def loose(text: str, mnemonics: bool = False) -> str:
    """normalize() plus case folding, hyphen/space equivalence and no terminal sentence punctuation."""
    text = normalize(text, mnemonics).casefold().replace("-", " ")
    text = re.sub(r"[.!?]+$", "", text)
    return re.sub(r" +", " ", text).strip()


def placeholders(text: str) -> Counter:
    return Counter(PLACEHOLDER.findall(text))


@dataclass(frozen=True)
class Candidate:
    english: str
    translation: str
    origin: str  # "<bundle file> <key>"
    rank: int  # 0 for Tracker's catalog, 1 for OSP bundles.


@dataclass(frozen=True)
class Match:
    tier: str  # "exact" or "normalized"
    candidate: Candidate
    finished: bool
    alternatives: int  # Distinct translations among the best-ranked candidates.


class TrackerIndex:
    """English-to-translation candidates from Tracker/OSP bundles for one locale."""

    def __init__(self, candidates: list[Candidate]):
        self.exact: dict[str, list[Candidate]] = {}
        self.loose: dict[str, list[Candidate]] = {}
        for candidate in candidates:
            self.exact.setdefault(normalize(candidate.english), []).append(candidate)
            self.loose.setdefault(loose(candidate.english), []).append(candidate)

    @classmethod
    def load(cls, roots: dict[str, Path], locale: Locale) -> TrackerIndex:
        candidates = []
        for rank, (repository, stem) in enumerate(BUNDLES):
            root = roots.get(repository)
            if root is None:
                continue
            base_path = root / f"{stem}.properties"
            if not base_path.is_file():
                raise ToolError(f"Missing Tracker/OSP bundle: {base_path}")
            english = read_properties(base_path)
            localized: dict[str, tuple[str, str]] = {}
            for suffix in reversed(java_chain(locale.java)):  # More specific files override their parents.
                path = root / f"{stem}_{suffix}.properties"
                if path.is_file():
                    for key, value in read_properties(path).items():
                        localized[key] = (value, path.name)
            rejected = {(normalize(term), normalize(text)) for term, text in LOCALE_REJECTED.get(locale.catalog, ())}
            restored = restored_values(locale.catalog)
            for key, source in english.items():
                if key not in localized or key in STALE_KEYS | HOMOGRAPH_KEYS or key.endswith(".New.Name"):
                    continue  # Default object names are chosen freely by translators, not translated.
                value, filename = localized[key]
                earlier = restored.get(filename, {}).get(key)
                if earlier is not None and lost_characters(source, value):
                    value, filename = earlier, f"{filename}@{restored_revision()}"  # The translator's intact text.
                if usable_translation(source, value) and (normalize(source), normalize(value)) not in rejected:
                    candidates.append(Candidate(source, value, f"{filename} {key}", min(rank, 1)))
        return cls(candidates)

    def match(self, source: str) -> Match | None:
        """Tracker's translation of a MoTrack source string, or None when Tracker has no equivalent text.

        "exact" matches differ only in mnemonics, ellipsis, spacing or a trailing colon and are finished when
        Tracker translates them consistently (or the key is in PREFERRED_KEYS). "normalized" matches also ignore
        case, hyphens and terminal punctuation; they always stay unfinished.
        """
        if PLACEHOLDER.search(source):
            return None  # Tracker composes such strings in code; its fragments are not equivalent.
        tier, found = "exact", self.exact.get(normalize(source, mnemonics=True))
        if not found:
            tier, found = "normalized", self.loose.get(loose(source, mnemonics=True))
        if not found:
            return None
        preferred = [candidate for key in PREFERRED_KEYS.get(source, ()) for candidate in found
                     if candidate.origin.split(" ", 1)[1] == key]
        if not preferred:
            best = min(candidate.rank for candidate in found)
            found = [candidate for candidate in found if candidate.rank == best]
        votes = Counter(normalize(candidate.translation) for candidate in found)
        first: dict[str, int] = {}
        for position, candidate in enumerate(found):
            first.setdefault(normalize(candidate.translation), position)
        chosen_text = max(votes, key=lambda text: (votes[text], -first[text]))
        chosen = preferred[0] if preferred else found[first[chosen_text]]
        # Only case-identical matches are finished: a case difference (such as Tracker's title case) may also be
        # copied into the translation, so normalized matches always stay unfinished for review.
        finished = tier == "exact" and (len(votes) == 1 or bool(preferred))
        return Match(tier, chosen, finished, len(votes))


def _initial_case(text: str) -> str | None:
    for char in text:
        if char.isalpha():
            if char.isupper():
                return "upper"
            if char.islower():
                return "lower"
            return None
    return None


def adapt(translation: str, source: str, locale: Locale, used: set[str] | None = None) -> str:
    """Apply the source string's presentation (mnemonic, ellipsis, colon, period, initial case) to a translation.

    Literal ampersands in Tracker text are escaped for Qt. ``used`` lists lower-case mnemonic letters already taken
    in the same menu so a different letter is preferred.
    """
    text = re.sub(r"[\s\u00a0]+", " ", translation.replace("&", "&&")).strip()
    text = _trim_suffixes(text)
    plain = strip_mnemonic(source).rstrip()
    ends_sentence = re.search(r"[.!?]$", plain) is not None
    if ends_sentence and not re.search(r"[.!?。！？]$", text) and locale.catalog != "th":
        text += "。" if locale.catalog in FULLWIDTH_PUNCTUATION else "."
    elif not ends_sentence and re.search(r"(?<!\.)[.。]$", text):
        text = text[:-1]
    if _initial_case(plain) == "upper" and text[:1].islower():
        text = text[:1].upper() + text[1:]
    letters = mnemonic_letters(source)
    if letters:
        text = insert_mnemonic(text, letters[0], locale, used)
    if plain.endswith(":"):
        text += {"fr": "\u00a0:"}.get(locale.catalog, "：" if locale.catalog in FULLWIDTH_PUNCTUATION else ":")
    elif plain.endswith("…"):
        text += "…"
    elif plain.endswith("..."):
        text += "..."
    return text


def insert_mnemonic(text: str, letter: str, locale: Locale, used: set[str] | None = None) -> str:
    """Mark one character as the mnemonic: a word start with the source letter, any word start, the source letter
    elsewhere, then any letter, skipping letters in ``used`` when possible.

    Chinese, Japanese, Korean and Thai menus conventionally append the Latin source letter, as in "ファイル(&F)".
    """
    used = used or set()
    if locale.catalog in BRACKET_SCRIPT or not any(char.isalpha() for char in text):
        return f"{text}(&{letter.upper()})"
    positions = [i for i, char in enumerate(text) if char.isalnum() and (i == 0 or text[i - 1] != "&")]
    word_start = [i for i in positions if i == 0 or not text[i - 1].isalnum()]
    same = [i for i in positions if text[i].lower() == letter.lower()]
    ordered = [i for i in word_start if i in same] + word_start + same + positions
    chosen = next((i for i in ordered if text[i].lower() not in used), ordered[0])
    return text[:chosen] + "&" + text[chosen:]


# ---------------------------------------------------------------------------------------------------------------
# Source strings and catalogs


@dataclass(frozen=True)
class SourceMessage:
    context: str
    source: str
    comment: str | None
    numerus: bool

    @property
    def key(self) -> tuple[str, str, str | None]:
        return self.context, self.source, self.comment


def cmake_translation_block(root: Path = ROOT) -> dict[str, list[str]]:
    """TS_FILES (expanding set(motrack_ts_files ...)), PLURALS_TS_FILE and SOURCES of qt_add_translations()."""
    text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    block = re.search(r"qt_add_translations\((.*?)\)", text, re.S)
    if not block:
        raise ToolError("CMakeLists.txt has no qt_add_translations() call.")
    lists: dict[str, list[str]] = {}
    current = None
    for word in block.group(1).split():
        if re.fullmatch(r"[A-Z_]+", word):
            current = lists.setdefault(word, [])
        elif current is not None:
            current.append(word.strip('"'))
    ts_files = lists.get("TS_FILES", [])
    if ts_files == ["${motrack_ts_files}"]:
        variable = re.search(r"set\(motrack_ts_files\s+(.*?)\)", text, re.S)
        ts_files = variable.group(1).split() if variable else []
    return {"TS_FILES": ts_files, "PLURALS_TS_FILE": lists.get("PLURALS_TS_FILE", []), "SOURCES": lists.get("SOURCES", [])}


def read_messages(path: Path) -> list[SourceMessage]:
    root = ET.parse(path).getroot()
    messages = []
    for context in root.iter("context"):
        name = context.findtext("name") or ""
        for message in context.iter("message"):
            obsolete = message.find("translation")
            if obsolete is not None and obsolete.get("type") in ("obsolete", "vanished"):
                continue
            messages.append(SourceMessage(name, message.findtext("source") or "", message.findtext("comment"),
                                          message.get("numerus") == "yes"))
    return messages


def extract_sources(lupdate: str, root: Path = ROOT) -> list[SourceMessage]:
    """Run lupdate on the CMake translation sources into a temporary catalog."""
    sources = cmake_translation_block(root)["SOURCES"]
    if not sources:
        raise ToolError("qt_add_translations() lists no SOURCES.")
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "sources.ts"
        result = subprocess.run([lupdate, "-silent", "-locations", "none", "-no-obsolete",
                                 *[str(root / source) for source in sources], "-ts", str(output)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
        if result.returncode != 0 or not output.is_file():
            raise ToolError(f"lupdate failed: {result.stdout}{result.stderr}")
        return read_messages(output)


def escape(text: str) -> str:
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


@dataclass
class Entry:
    """One message of a catalog and its location in the file text."""

    context: str
    source: str
    comment: str | None
    numerus: bool
    kind: str | None  # translation type attribute: None (finished), "unfinished", "obsolete", "vanished".
    forms: list[str]
    note: str | None  # translatorcomment
    span: tuple[int, int]
    text: str

    @property
    def key(self) -> tuple[str, str, str | None]:
        return self.context, self.source, self.comment

    @property
    def empty(self) -> bool:
        return not any(form for form in self.forms)

    @property
    def tool_owned(self) -> bool:
        """Empty or tool-written entries the tool may replace: unfinished ones, and finished Tracker seeds that still
        carry their provenance comment (a reviewer removes the comment when accepting an entry)."""
        if self.kind == "unfinished" and (self.empty or bool(self.note and self.note.startswith(TOOL_NOTES))):
            return True
        return self.kind is None and bool(self.note and self.note.startswith((EXACT_NOTE, NORMALIZED_NOTE)))

    @property
    def provenance(self) -> str:
        if self.empty:
            return "empty"
        if self.note and self.note.startswith(EXACT_NOTE):
            return "tracker-exact"
        if self.note and self.note.startswith(NORMALIZED_NOTE):
            return "tracker-normalized"
        if self.note and self.note.startswith(TOOL_NOTES[0]):
            return "machine"
        return "human"


@dataclass
class Context:
    name: str
    span: tuple[int, int]  # From "<context>" to the end of "</context>".
    entries: list[Entry] = field(default_factory=list)


class Catalog:
    """A Qt .ts file edited in place so untouched messages keep their existing layout."""

    def __init__(self, path: Path, text: str):
        self.path = path
        self.text = text
        self.root = ET.fromstring(text.encode("utf-8"))
        if self.root.tag != "TS":
            raise ToolError(f"{path}: not a Qt TS file")
        self.language = self.root.get("language")
        self.contexts: list[Context] = []
        for match in re.finditer(r"<context>.*?</context>", text, re.S):
            body = match.group(0)
            name = ET.fromstring(re.search(r"<name>.*?</name>", body, re.S).group(0).encode("utf-8")).text or ""
            context = Context(name, match.span())
            for message in re.finditer(r"<message\b[^>]*>.*?</message>", body, re.S):
                start, end = match.start() + message.start(), match.start() + message.end()
                context.entries.append(_parse_entry(name, message.group(0), (start, end)))
            self.contexts.append(context)
        self._replacements: dict[tuple[int, int], str] = {}
        self._insertions: list[tuple[int, str]] = []

    @classmethod
    def load(cls, path: Path) -> Catalog:
        return cls(path, path.read_text(encoding="utf-8"))

    @classmethod
    def new(cls, path: Path, locale: Locale) -> Catalog:
        text = (f'<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n<TS version="2.1" language="{locale.language}">\n'
                "</TS>\n")
        return cls(path, text)

    def entries(self) -> list[Entry]:
        return [entry for context in self.contexts for entry in context.entries]

    def set_translation(self, entry: Entry, forms: list[str], finished: bool, note: str | None) -> None:
        """Replace one tool-owned entry's translation element and translator comment, keeping its layout.

        Without ``note`` the translator comment is removed (used to empty a seed whose Tracker match is gone).
        """
        body = entry.text
        body = re.sub(r"<translatorcomment>.*?</translatorcomment>\s*", "", body, flags=re.S)
        translation = re.search(r"<translation\b[^>]*?(?:/>|>.*?</translation>)", body, re.S)
        if not translation:
            raise ToolError(f"{self.path}: message without translation: {entry.source!r}")
        line_start = body.rfind("\n", 0, translation.start()) + 1
        indent = body[line_start:translation.start()]
        expanded = "\n" in body and not indent.strip()
        rendered = _render_translation(forms, entry.numerus, finished, indent if expanded else None)
        comment = "" if note is None else (
            f"<translatorcomment>{escape(note)}</translatorcomment>" + ("\n" + indent if expanded else ""))
        body = body[:translation.start()] + comment + rendered + body[translation.end():]
        self._replacements[entry.span] = body
        entry.forms, entry.kind, entry.note, entry.text = forms, None if finished else "unfinished", note, body

    def add_messages(self, messages: list[SourceMessage], forms: int) -> None:
        """Append missing messages, empty and unfinished, to their contexts (creating contexts when needed)."""
        by_context: dict[str, list[SourceMessage]] = {}
        for message in messages:
            by_context.setdefault(message.context, []).append(message)
        for name, items in by_context.items():
            context = next((c for c in self.contexts if c.name == name), None)
            if context is None:
                position = self.text.rindex("</TS>")
                lines = "".join(_render_new(m, forms, None) for m in items)
                self._insertions.append((position, f"  <context>\n    <name>{escape(name)}</name>\n{lines}  </context>\n"))
                continue
            close = self.text.rindex("</context>", context.span[0], context.span[1])
            line_start = self.text.rfind("\n", 0, close) + 1
            if self.text[line_start:close].strip():
                line_start = close
            last = context.entries[-1] if context.entries else None
            # Match the last message's layout: one element per line, or the compact one-line form.
            expanded = last is not None and re.match(r"<message\b[^>]*>[ \t]*\n", last.text) is not None
            self._insertions.append((line_start, "".join(_render_new(m, forms, expanded) for m in items)))

    def render(self) -> str:
        edits = [(start, end, text) for (start, end), text in self._replacements.items()]
        edits += [(position, position, text) for position, text in self._insertions]
        text = self.text
        for start, end, replacement in sorted(edits, key=lambda edit: (edit[0], edit[1]), reverse=True):
            text = text[:start] + replacement + text[end:]
        return text

    def save(self) -> bool:
        text = self.render()
        ET.fromstring(text.encode("utf-8"))
        if self.path.is_file() and self.path.read_text(encoding="utf-8") == text:
            return False
        with self.path.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        return True


def _parse_entry(context: str, text: str, span: tuple[int, int]) -> Entry:
    element = ET.fromstring(text.encode("utf-8"))
    numerus = element.get("numerus") == "yes"
    translation = element.find("translation")
    kind = translation.get("type") if translation is not None else "unfinished"
    if translation is None:
        forms = [""]
    elif numerus:
        forms = [form.text or "" for form in translation.findall("numerusform")] or [translation.text or ""]
    else:
        forms = [translation.text or ""]
    return Entry(context, element.findtext("source") or "", element.findtext("comment"), numerus, kind, forms,
                 element.findtext("translatorcomment"), span, text)


def _render_translation(forms: list[str], numerus: bool, finished: bool, indent: str | None) -> str:
    attribute = "" if finished else ' type="unfinished"'
    if not numerus:
        return f"<translation{attribute}>{escape(forms[0])}</translation>"
    if indent is None:
        inner = "".join(f"<numerusform>{escape(form)}</numerusform>" for form in forms)
        return f"<translation{attribute}>{inner}</translation>"
    inner = "".join(f"\n{indent}    <numerusform>{escape(form)}</numerusform>" for form in forms)
    return f"<translation{attribute}>{inner}\n{indent}</translation>"


def _render_new(message: SourceMessage, forms: int, expanded: bool | None) -> str:
    numerus = ' numerus="yes"' if message.numerus else ""
    comment = f"<comment>{escape(message.comment)}</comment>" if message.comment is not None else ""
    empty = [""] * (forms if message.numerus else 1)
    if expanded:
        parts = [f"<source>{escape(message.source)}</source>"] + ([comment] if comment else [])
        parts.append(_render_translation(empty, message.numerus, False, " " * 8))
        return f"    <message{numerus}>\n" + "".join(f"        {part}\n" for part in parts) + "    </message>\n"
    return (f"    <message{numerus}><source>{escape(message.source)}</source>{comment}"
            f"{_render_translation(empty, message.numerus, False, None)}</message>\n")


# ---------------------------------------------------------------------------------------------------------------
# Validation


def validate_translation(source: str, forms: list[str], numerus: bool, locale: Locale) -> list[str]:
    """Problems with one non-empty translation: placeholders, mnemonics, newlines and plural form count."""
    problems = []
    if numerus and len(forms) != locale.forms:
        problems.append(f"has {len(forms)} plural forms; {locale.language} needs {locale.forms}")
    if any(not form.strip() for form in forms):
        problems.append("has an empty form")
    expected = placeholders(source)
    for index, form in enumerate(forms):
        label = f"form {index}: " if numerus else ""
        found = placeholders(form)
        if numerus and ("%n" in expected or "%Ln" in expected):
            found_numbers = Counter({k: v for k, v in found.items() if k not in ("%n", "%Ln")})
            wanted = Counter({k: v for k, v in expected.items() if k not in ("%n", "%Ln")})
            extra_n = sum(v for k, v in found.items() if k in ("%n", "%Ln")) > 1
            if found_numbers != wanted or extra_n:
                problems.append(f"{label}placeholders {dict(found)} differ from {dict(expected)}")
        elif found != expected:
            problems.append(f"{label}placeholders {dict(found)} differ from {dict(expected)}")
        wanted_mnemonics = len(mnemonic_letters(source))
        found_mnemonics = len(mnemonic_letters(form))
        if found_mnemonics != wanted_mnemonics:
            problems.append(f"{label}has {found_mnemonics} mnemonics; source has {wanted_mnemonics}")
        if form.count("\n") != source.count("\n"):
            problems.append(f"{label}has {form.count(chr(10))} line breaks; source has {source.count(chr(10))}")
        if form != form.strip() and source == source.strip():
            problems.append(f"{label}has leading or trailing whitespace")
        for token, pattern in (("window-modified marker [*]", r"\[\*\]"), ("file pattern", r"\*\.\w+")):
            if sorted(re.findall(pattern, form)) != sorted(re.findall(pattern, source)):
                problems.append(f"{label}changes the {token}")
    if numerus and ("%n" in expected or "%Ln" in expected) and not any(PLACEHOLDER.search(form) for form in forms):
        problems.append("no plural form shows the count")
    return problems


def check_catalog(catalog: Catalog, locale: Locale, sources: list[SourceMessage] | None) -> tuple[list[str], list[str]]:
    errors, warnings = [], []
    name = catalog.path.name
    if catalog.language != locale.language:
        errors.append(f"{name}: language {catalog.language!r}, expected {locale.language!r}")
    if locale is ENGLISH:
        sources = None if sources is None else [message for message in sources if message.numerus]
        for entry in catalog.entries():
            if not entry.numerus:
                errors.append(f"{name}: the English catalog holds only plural messages, not {entry.source!r}")
            elif entry.empty:
                errors.append(f"{name}: write the English plural forms of {entry.source!r}")
    seen: dict[tuple, Entry] = {}
    for entry in catalog.entries():
        if entry.key in seen:
            errors.append(f"{name}: duplicate message {entry.context}: {entry.source!r}")
        seen[entry.key] = entry
        if entry.kind in ("obsolete", "vanished"):
            continue
        if entry.empty:
            if entry.kind != "unfinished":
                errors.append(f"{name}: finished message without translation: {entry.source!r}")
            elif entry.numerus and len(entry.forms) not in (1, locale.forms):
                errors.append(f"{name}: {entry.source!r} has {len(entry.forms)} plural forms; needs {locale.forms}")
            continue
        for problem in validate_translation(entry.source, entry.forms, entry.numerus, locale):
            errors.append(f"{name}: {entry.source!r} {problem}")
    if sources is not None:
        current = {message.key: message for message in sources}
        for key, message in current.items():
            if key not in seen:
                errors.append(f"{name}: missing {message.context}: {message.source!r} (run tools/translations.py update)")
            elif seen[key].numerus != message.numerus:
                errors.append(f"{name}: {message.source!r} numerus attribute differs from the source")
        for key, entry in seen.items():
            if key not in current and entry.kind not in ("obsolete", "vanished"):
                warnings.append(f"{name}: {entry.context}: {entry.source!r} is no longer in the sources")
    translations = {entry.source: entry.forms[0] for entry in catalog.entries() if not entry.empty}
    for group, members in MNEMONIC_GROUPS.items():
        english = [mnemonic_letters(source)[0].lower() for source in members if mnemonic_letters(source)]
        local = [mnemonic_letters(translations[source])[0].lower() for source in members
                 if source in translations and mnemonic_letters(translations[source])]
        if len(local) - len(set(local)) > len(english) - len(set(english)):
            duplicates = sorted(letter for letter, count in Counter(local).items() if count > 1)
            warnings.append(f"{name}: {group} repeats mnemonic {', '.join(duplicates)}")
    return errors, warnings


def check_cmake(root: Path = ROOT) -> list[str]:
    lists = cmake_translation_block(root)
    ts_files = lists["TS_FILES"]
    expected = [f"translations/motrack_{locale.catalog}.ts" for locale in LOCALES]
    plurals = f"translations/motrack_{ENGLISH.catalog}.ts"
    errors = [f"CMakeLists.txt: TS_FILES lacks {path}" for path in expected if path not in ts_files]
    errors += [f"CMakeLists.txt: TS_FILES has unsupported {path}" for path in ts_files if path not in expected]
    if lists["PLURALS_TS_FILE"] != [plurals]:
        errors.append(f"CMakeLists.txt: qt_add_translations needs PLURALS_TS_FILE {plurals}")
    errors += [f"translations: {path.name} is not in LOCALES" for path in sorted((root / "translations").glob("*.ts"))
               if f"translations/{path.name}" not in expected + [plurals]]
    # Maintainer decision (2026-11-22): unfinished machine translations ship visible, so lrelease keeps them.
    if re.search(r"-nounfinished|LRELEASE_OPTIONS", (root / "CMakeLists.txt").read_text(encoding="utf-8")):
        errors.append("CMakeLists.txt: lrelease must keep unfinished translations (no -nounfinished/LRELEASE_OPTIONS)")
    return errors


def run_lrelease(lrelease: str, catalog: Path, unfinished: int | None = None) -> str | None:
    """Compile with the options CMake uses; with ``unfinished``, require that many unfinished translations in the
    .qm (machine translations ship visible, so lrelease must not drop them)."""
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / (catalog.stem + ".qm")
        result = subprocess.run([lrelease, str(catalog), "-qm", str(output)], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", check=False)
        text = result.stdout + result.stderr
        if result.returncode != 0 or not output.is_file() or re.search(r"\berror\b", text, re.I):
            return f"{catalog.name}: lrelease failed: {text.strip()}"
        generated = re.search(r"\((\d+) finished and (\d+) unfinished\)", text)
        if unfinished is not None and (not generated or int(generated.group(2)) != unfinished):
            return f"{catalog.name}: lrelease kept {generated.group(2) if generated else 'no'} of {unfinished} unfinished translations"
    return None


# ---------------------------------------------------------------------------------------------------------------
# Commands


def catalog_path(locale: Locale, directory: Path | None = None) -> Path:
    return (directory or TRANSLATIONS) / f"motrack_{locale.catalog}.ts"


def selected_locales(names: list[str] | None, english: bool = False) -> list[Locale]:
    """Translated locales by catalog name (all by default); with ``english``, "en" names the plural-only catalog."""
    available = {**LOCALE_BY_CATALOG, **({ENGLISH.catalog: ENGLISH} if english else {})}
    if not names:
        return list(LOCALES) + ([ENGLISH] if english else [])
    unknown = [name for name in names if name not in available]
    if unknown:
        raise ToolError(f"Unknown locale: {', '.join(unknown)}")
    return [available[name] for name in names]


def load_sources(args) -> list[SourceMessage] | None:
    if getattr(args, "template", None):
        return read_messages(Path(args.template))
    lupdate = getattr(args, "lupdate", None) or shutil.which("lupdate")
    return extract_sources(lupdate) if lupdate else None


def seed(catalog: Catalog, index: TrackerIndex, locale: Locale) -> Counter:
    """Fill empty and tool-generated unfinished entries with Tracker translations of identical English text."""
    counts = Counter()
    entries = catalog.entries()
    by_source: dict[str, list[Entry]] = {}
    for entry in entries:
        by_source.setdefault(entry.source, []).append(entry)
    for entry in entries:
        if entry.numerus or not entry.tool_owned:
            continue  # Tracker has no plural forms; human and finished entries are never replaced.
        match = index.match(entry.source)
        if match is None:
            if entry.provenance in ("tracker-exact", "tracker-normalized"):
                catalog.set_translation(entry, [""] * len(entry.forms), False, None)
                counts["cleared"] += 1
            continue
        used = set()
        for member in MNEMONIC_GROUPS.get(GROUP_OF.get(entry.source, ""), ()):
            for other in by_source.get(member, []) if member != entry.source else []:
                if not other.empty and mnemonic_letters(other.forms[0]):
                    used.add(mnemonic_letters(other.forms[0])[0].lower())
        text = adapt(match.candidate.translation, entry.source, locale, used)
        if entry.provenance in ("tracker-exact", "tracker-normalized") and strip_mnemonic(entry.forms[0]) == strip_mnemonic(text):
            text = entry.forms[0]  # Same Tracker text: keep the mnemonic already chosen, so reseeding is stable.
        if validate_translation(entry.source, [text], False, locale):
            continue
        note = f"{EXACT_NOTE if match.tier == 'exact' else NORMALIZED_NOTE}: {match.candidate.origin}"
        if match.alternatives > 1:
            note += f" ({match.alternatives} differing Tracker translations)"
        counts[match.tier] += 1
        if entry.forms != [text] or entry.note != note or (entry.kind is None) != match.finished:
            catalog.set_translation(entry, [text], match.finished, note)
    return counts


def command_update(args) -> int:
    sources = load_sources(args)
    if sources is None:
        raise ToolError("lupdate was not found; pass --lupdate or --template.")
    roots = {}
    if args.tracker:
        roots["tracker"] = Path(args.tracker)
    if args.osp:
        roots["osp"] = Path(args.osp)
    for locale in selected_locales(args.locale, english=True):
        path = catalog_path(locale)
        catalog = Catalog.load(path) if path.is_file() else Catalog.new(path, locale)
        present = {entry.key for entry in catalog.entries()}
        wanted = [message for message in sources if message.numerus] if locale is ENGLISH else sources
        missing = [message for message in wanted if message.key not in present]
        if missing:
            catalog.add_messages(missing, locale.forms)
            catalog = Catalog(path, catalog.render())
        seeded = roots and locale is not ENGLISH and locale.catalog not in UNSEEDED_LOCALES
        counts = seed(catalog, TrackerIndex.load(roots, locale), locale) if seeded else Counter()
        changed = catalog.save()
        print(f"{path.name}: {len(missing)} added, {counts['exact']} Tracker exact, "
              f"{counts['normalized']} Tracker normalized, {counts['cleared']} cleared{' (written)' if changed else ''}")
    return 0


def command_pending(args) -> int:
    locale = selected_locales([args.locale])[0]
    catalog = Catalog.load(catalog_path(locale))
    roots = {name: Path(value) for name, value in (("tracker", args.tracker), ("osp", args.osp)) if value}
    index = TrackerIndex.load(roots, locale) if roots else None
    glossary = {}
    if index:
        for term in GLOSSARY_TERMS:
            found = index.loose.get(loose(term), [])
            votes = Counter(normalize(candidate.translation) for candidate in found)
            if votes:
                glossary[term] = [text for text, _ in votes.most_common(3)]
    references = {}
    for name in args.reference or []:
        for entry in Catalog.load(catalog_path(selected_locales([name])[0])).entries():
            if not entry.empty and entry.kind is None:
                references.setdefault(entry.source, {})[name] = entry.forms if entry.numerus else entry.forms[0]
    pending, seen, examples = [], set(), {}
    for entry in catalog.entries():
        if entry.kind in ("obsolete", "vanished"):
            continue
        if not entry.empty:
            examples[entry.source] = entry.forms if entry.numerus else entry.forms[0]
            continue
        if entry.source in seen:
            continue
        seen.add(entry.source)
        item = {"source": entry.source}
        if entry.numerus:
            item["plural_forms"] = locale.forms
        if mnemonic_letters(entry.source):
            item["mnemonic_scope"] = GROUP_OF.get(entry.source, "its own dialog")
        if entry.source in references:
            item["reviewed_translations"] = references[entry.source]
        pending.append(item)
    data = {"locale": locale.catalog, "language": locale.language, "right_to_left": locale.rtl,
            "plural_rule": PLURAL_RULES.get(locale.catalog, ("n == 1", "other") if locale.forms == 2 else ("all",)),
            "mnemonic_groups": MNEMONIC_GROUPS, "glossary": glossary, "existing": examples, "pending": pending}
    Path(args.out).write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"{catalog.path.name}: {len(pending)} pending strings written to {args.out}")
    return 0


def command_import(args) -> int:
    locale = selected_locales([args.locale])[0]
    path = catalog_path(locale)
    catalog = Catalog.load(path)
    supplied = json.loads(Path(args.input).read_text(encoding="utf-8"))
    if not isinstance(supplied, dict):
        raise ToolError("The input must be a JSON object mapping source text to translations.")
    known = {entry.source for entry in catalog.entries()}
    errors = [f"unknown source {source!r}" for source in supplied if source not in known]
    applied = 0
    for entry in catalog.entries():
        if entry.source not in supplied or entry.kind in ("obsolete", "vanished"):
            continue
        if not (entry.empty and entry.kind == "unfinished" or args.replace_machine and entry.provenance == "machine"):
            continue
        value = supplied[entry.source]
        forms = value if isinstance(value, list) else [value]
        if not all(isinstance(form, str) for form in forms):
            errors.append(f"{entry.source!r}: translations must be strings")
            continue
        problems = validate_translation(entry.source, forms, entry.numerus, locale)
        if problems:
            errors.append(f"{entry.source!r}: {'; '.join(problems)}")
            continue
        catalog.set_translation(entry, forms, False, MACHINE_NOTE)
        applied += 1
    for error in errors:
        print(f"{path.name}: {error}", file=sys.stderr)
    if errors and not args.partial:
        print(f"{path.name}: not written; fix the input or pass --partial", file=sys.stderr)
        return 1
    catalog.save()
    print(f"{path.name}: {applied} machine translations imported")
    return 1 if errors else 0


def command_check(args) -> int:
    sources = load_sources(args)
    errors: list[str] = []
    warnings: list[str] = []
    if args.cmake:
        errors += check_cmake()
    for locale in selected_locales(args.locale, english=True):
        path = catalog_path(locale)
        if not path.is_file():
            errors.append(f"{path.name}: missing catalog")
            continue
        try:
            catalog = Catalog.load(path)
        except (ET.ParseError, ToolError) as error:
            errors.append(f"{path.name}: {error}")
            continue
        found_errors, found_warnings = check_catalog(catalog, locale, sources)
        errors += found_errors
        warnings += found_warnings
        if args.lrelease and not found_errors:
            unfinished = sum(1 for entry in catalog.entries() if entry.kind == "unfinished" and not entry.empty)
            failure = run_lrelease(args.lrelease, path, unfinished)
            if failure:
                errors.append(failure)
    for warning in warnings:
        print(f"warning: {warning}")
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    scope = "against lupdate sources" if sources is not None else "without lupdate completeness"
    print(f"Checked {len(selected_locales(args.locale, english=True))} catalogs {scope}"
          f"{' and lrelease' if args.lrelease else ''}: {len(errors)} errors, {len(warnings)} warnings")
    return 1 if errors else 0


def command_stats(args) -> int:
    print(f"{'catalog':<8} {'total':>5} {'finished':>8} {'unfinished':>10} {'empty':>5}  "
          f"{'exact':>5} {'normal':>6} {'machine':>7} {'human':>5}")
    for locale in selected_locales(args.locale):
        catalog = Catalog.load(catalog_path(locale))
        entries = [entry for entry in catalog.entries() if entry.kind not in ("obsolete", "vanished")]
        origin = Counter(entry.provenance for entry in entries)
        finished = sum(1 for entry in entries if entry.kind is None)
        unfinished = sum(1 for entry in entries if entry.kind == "unfinished" and not entry.empty)
        print(f"{locale.catalog:<8} {len(entries):>5} {finished:>8} {unfinished:>10} {origin['empty']:>5}  "
              f"{origin['tracker-exact']:>5} {origin['tracker-normalized']:>6} {origin['machine']:>7} {origin['human']:>5}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    def sources_options(command):
        command.add_argument("--lupdate", help="lupdate executable (default: from PATH)")
        command.add_argument("--template", help="read source strings from this .ts instead of running lupdate")

    update = commands.add_parser("update", help="synchronize and seed catalogs")
    sources_options(update)
    update.add_argument("--tracker", help="checkout of OpenSourcePhysics/tracker (SwingJS)")
    update.add_argument("--osp", help="checkout of OpenSourcePhysics/osp (swingJS)")
    update.add_argument("--locale", nargs="*", help="catalog names (default: all)")
    update.set_defaults(func=command_update)

    pending = commands.add_parser("pending", help="export untranslated strings for one catalog")
    pending.add_argument("--locale", required=True)
    pending.add_argument("--out", required=True)
    pending.add_argument("--tracker")
    pending.add_argument("--osp")
    pending.add_argument("--reference", nargs="*", help="catalogs whose finished translations show the intended meaning")
    pending.set_defaults(func=command_pending)

    importer = commands.add_parser("import", help="import machine translations for one catalog")
    importer.add_argument("--locale", required=True)
    importer.add_argument("--input", required=True)
    importer.add_argument("--replace-machine", action="store_true", help="also replace earlier machine translations")
    importer.add_argument("--partial", action="store_true", help="write valid entries even if others are rejected")
    importer.set_defaults(func=command_import)

    check = commands.add_parser("check", help="validate catalogs")
    sources_options(check)
    check.add_argument("--lrelease", help="also compile each catalog with this lrelease")
    check.add_argument("--no-cmake", dest="cmake", action="store_false", help="skip the CMake TS_FILES comparison")
    check.add_argument("--locale", nargs="*")
    check.set_defaults(func=command_check)

    stats = commands.add_parser("stats", help="report catalog provenance")
    stats.add_argument("--locale", nargs="*")
    stats.set_defaults(func=command_stats)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ToolError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
