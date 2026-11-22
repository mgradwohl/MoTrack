"""Fetch pinned Tracker examples and compare native imports with Java reference snapshots."""

import argparse
import copy
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import urllib.parse
import xml.dom.minidom as minidom
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "tests/corpus"
MAX_XML_BYTES = 32 * 1024 * 1024

# Commands that check the reference corpus; kernel oracles register themselves below with @kernel_oracle.
CORPUS_COMMANDS = ["fetch", "verify", "reference", "check", "session", "roundtrip", "xml-oracle", "library-oracle",
                   "state-oracle", "state-check", "state-diff"]
# Kernel oracle commands -> the name of their check function. Each oracle is defined as
# `check_<x>_oracle(manifest, cache, java, *, record=False)` and registered where it is defined, so a new
# kernel adds no lines to main().
KERNEL_ORACLES = {}


def kernel_oracle(command):
    def register(function):
        if command in KERNEL_ORACLES or command in CORPUS_COMMANDS:
            raise ValueError(f"Duplicate corpus command {command}")
        KERNEL_ORACLES[command] = function.__name__
        return function
    return register


def load_manifest(path=CORPUS / "manifest.json"):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest["schema_version"] != 1:
        raise ValueError("Unsupported corpus manifest version")
    dependencies = manifest["reference"].get("runtime_dependencies", [])
    if not isinstance(dependencies, list) or len(dependencies) > 8:
        raise ValueError("Invalid reference runtime dependency list")
    dependency_names = set()
    for dependency in dependencies:
        if (not isinstance(dependency, dict) or "local_file" in dependency
                or not {"file", "url", "sha256", "bytes"}.issubset(dependency)
                or not all(isinstance(dependency[key], str) for key in ["file", "url", "sha256"])
                or type(dependency["bytes"]) is not int or not dependency["file"].endswith(".jar")
                or dependency["file"] in dependency_names or dependency["file"] == manifest["reference"]["file"]):
            raise ValueError("Invalid or duplicate reference runtime dependency")
        dependency_names.add(dependency["file"])
    seen = set()
    for case in manifest["cases"]:
        required = case.get("reference_dependencies", [])
        if (not isinstance(required, list) or len(required) > 8
                or any(not isinstance(name, str) or name not in dependency_names for name in required)
                or len(set(required)) != len(required)):
            raise ValueError("Invalid or unknown case reference dependency")
        if not re.fullmatch(r"[a-z0-9-]+", case["id"]) or case["id"] in seen:
            raise ValueError("Invalid or duplicate corpus case ID")
        seen.add(case["id"])
        if not isinstance(case.get("tracker_version"), str) or not case["tracker_version"]:
            raise ValueError("Corpus case requires its original Tracker version")
        if "session_exclusion" in case and ("session_tracks" in case or any(
                key.startswith("native_") and key.endswith("_roundtrip") and value for key, value in case.items())):
            raise ValueError(f"{case['id']}: session_exclusion conflicts with session_tracks or an enabled native round-trip flag")
        expected_path(case)
        if "native_reference_index" in case:
            expected_path({"expected": case["native_reference_index"]})
        if case.get("session_archive") and (not case.get("project_member") or "session_tracks" not in case):
            raise ValueError("Archive sessions require an explicit project member and independent track expectations")
    by_id = {case["id"]: case for case in manifest["cases"]}
    for case in manifest["cases"]:
        if case.get("native_archive_group_roundtrip") and (case["id"] not in {"multitab-images", "archived-movie"}
                                                        or len(case.get("session_archive_tabs", [])) != 2):
            raise ValueError("Grouped archive acceptance requires its two pinned experiments")
        if "session_archive_tabs" not in case:
            continue
        tabs = case["session_archive_tabs"]
        if not case.get("session_archive") or not isinstance(tabs, list) or not 2 <= len(tabs) <= 8:
            raise ValueError("Multi-tab archive sessions require 2–8 explicit member expectations")
        members = set()
        for tab in tabs:
            if not isinstance(tab, dict) or set(tab) != {"member", "case"}:
                raise ValueError("Invalid multi-tab archive expectation")
            member = tab["member"]
            reference = by_id.get(tab["case"]) if isinstance(tab["case"], str) else None
            if (not isinstance(member, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+\.trk", member)
                    or member in members or reference is None or "session_tracks" not in reference
                    or reference.get("session_archive") or reference.get("session_known_issue")):
                raise ValueError("Invalid, duplicate or unaudited archive member expectation")
            members.add(member)
        if tabs[0]["member"] != case["project_member"] or by_id[tabs[0]["case"]]["expected"] != case["expected"]:
            raise ValueError("Selected archive member must match its independent expectation")
    assets = []
    for case in manifest["cases"]:
        values = case.get("assets", [])
        if not isinstance(values, list) or len(values) > 10000:
            raise ValueError("Invalid corpus asset list")
        for asset in values:
            if not isinstance(asset, dict) or "local_file" not in asset:
                raise ValueError("Corpus media assets must be pinned local fixtures")
        assets.extend(values)
    for artifact in [manifest["reference"], *dependencies, *manifest["cases"], *assets]:
        local = "local_file" in artifact
        if (not re.fullmatch(r"[A-Za-z0-9_.-]+", artifact["file"])
                or artifact["file"] in {".", ".."}
                or (local and ("url" in artifact or artifact is manifest["reference"]))
                or (not local and not artifact.get("url", "").startswith("https://"))
                or not re.fullmatch(r"[a-f0-9]{64}", artifact["sha256"])
                or not 0 < artifact["bytes"] <= 128 * 1024 * 1024):
            raise ValueError("Invalid corpus artifact")
        if local:
            local_fixture_path(artifact)
    for value in manifest["tolerance"].values():
        if not math.isfinite(value) or value < 0:
            raise ValueError("Invalid numerical tolerance")
    state = manifest.get("state_tolerance")
    if (not isinstance(state, dict) or set(state) != {"default", "dimensions"} or not isinstance(state["dimensions"], dict)
            or any(not isinstance(item, dict) or set(item) != {"absolute", "relative"}
                   or any(type(value) is not float or not math.isfinite(value) or value < 0 for value in item.values())
                   for item in [state["default"], *state["dimensions"].values()])):
        raise ValueError("Invalid state tolerance table")
    return manifest


def expected_path(case):
    path = (CORPUS / case["expected"]).resolve()
    if not path.is_relative_to(CORPUS.resolve()):
        raise ValueError("Expected snapshot must stay inside the corpus")
    return path


def local_fixture_path(case):
    path = (CORPUS / case["local_file"]).resolve()
    if not path.is_relative_to((CORPUS / "fixtures").resolve()):
        raise ValueError("Local fixture must stay inside the corpus fixtures directory")
    return path


def verify_file(path, artifact):
    if not path.is_file() or path.stat().st_size != artifact["bytes"]:
        raise ValueError(f"Missing or wrong-sized artifact: {path}; run corpus.py fetch")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(65536):
            digest.update(block)
    if digest.hexdigest() != artifact["sha256"]:
        raise ValueError(f"Checksum mismatch: {path}; existing file was left untouched")


def fetch_file(path, artifact):
    if path.exists():
        verify_file(path, artifact)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", suffix=".part", delete=False) as output:
            pending = Path(output.name)
            request = urllib.request.Request(artifact["url"], headers={"User-Agent": "MoTrack-Compatibility-Corpus/1"})
            with urllib.request.urlopen(request, timeout=30) as response:
                if not response.geturl().startswith("https://"):
                    raise ValueError("Corpus download redirected away from HTTPS")
                remaining = artifact["bytes"]
                while block := response.read(min(65536, remaining + 1)):
                    remaining -= len(block)
                    if remaining < 0:
                        raise ValueError("Corpus download exceeds its pinned size")
                    output.write(block)
        verify_file(pending, artifact)
        pending.replace(path)
    finally:
        if pending is not None:
            pending.unlink(missing_ok=True)


def artifacts(manifest, cache):
    yield cache / manifest["reference"]["file"], manifest["reference"]
    for dependency in manifest["reference"].get("runtime_dependencies", []):
        yield cache / "runtime" / dependency["file"], dependency
    for case in manifest["cases"]:
        yield local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"], case
        for asset in case.get("assets", []):
            yield local_fixture_path(asset), asset


def java_classpath(manifest, cache, *, classes=None, dependencies=()):
    """Use only the pinned runtime jars explicitly selected for this reference case."""
    available = {item["file"] for item in manifest["reference"].get("runtime_dependencies", [])}
    if any(name not in available for name in dependencies):
        raise ValueError("Unknown reference runtime dependency")
    paths = ([classes] if classes is not None else []) + [cache / manifest["reference"]["file"]]
    paths.extend(cache / "runtime" / name for name in dependencies)
    return os.pathsep.join(map(str, paths))


def java_reference_command(java, home, classpath, program, *arguments):
    """Headless pinned-reference JVM with a private home and bounded heap (corpus and library checks)."""
    return [str(java), "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}", "-Xmx256m",
            "--class-path", classpath, str(program), *map(str, arguments)]


def project_path(case, cache):
    source = local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"]
    if "project_member" not in case:
        if source.stat().st_size > MAX_XML_BYTES:
            raise ValueError("Corpus project exceeds the XML size limit")
        return source
    with zipfile.ZipFile(source) as archive:
        matches = [entry for entry in archive.infolist() if entry.filename == case["project_member"]]
        if len(matches) != 1 or matches[0].is_dir() or matches[0].file_size > MAX_XML_BYTES:
            raise ValueError("Missing, ambiguous or oversized corpus project member")
        # Read only the named project; never extract archive paths onto the filesystem.
        with archive.open(matches[0]) as stream:
            xml = stream.read(MAX_XML_BYTES + 1)
        if len(xml) > MAX_XML_BYTES:
            raise ValueError("Expanded corpus XML exceeds the size limit")
    destination = cache / "projects" / (case["id"] + ".trk")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(xml)
    return destination


def compare(expected, actual, tolerance, location="project"):
    if isinstance(expected, dict):
        if not isinstance(actual, dict) or expected.keys() != actual.keys():
            raise ValueError(f"{location}: object keys differ")
        for key in expected:
            compare(expected[key], actual[key], tolerance, f"{location}.{key}")
    elif isinstance(expected, list):
        if not isinstance(actual, list):
            raise ValueError(f"{location}: expected a list, got {json.dumps(actual)[:80]}")
        if len(expected) != len(actual):
            raise ValueError(f"{location}: list length differs")
        for index, value in enumerate(expected):
            compare(value, actual[index], tolerance, f"{location}[{index}]")
    elif type(expected) is float:
        if (type(actual) not in {int, float} or not math.isfinite(actual)
                or not math.isclose(expected, actual, rel_tol=tolerance["relative"], abs_tol=tolerance["absolute"])):
            raise ValueError(f"{location}: expected {expected!r}, got {actual!r}")
    elif type(expected) is int:
        if type(actual) not in {int, float} or actual != expected:
            raise ValueError(f"{location}: exact integer mismatch")
    elif type(actual) is not type(expected) or actual != expected:
        raise ValueError(f"{location}: expected {expected!r}, got {actual!r}")


def run(command, *, display=None, timeout=45):
    environment = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    if display is not None:
        environment["DISPLAY"] = display
        environment.pop("WAYLAND_DISPLAY", None)
        environment.pop("XAUTHORITY", None)
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                            timeout=timeout, env=environment)
    if result.returncode:
        raise ValueError(f"Probe failed ({result.returncode}):\n{(result.stdout + result.stderr)[-5000:]}")
    # The bounded output lets callers read diagnostics a successful helper printed.
    return "".join(part for part in (result.stdout, result.stderr) if isinstance(part, str))


MATCH_REFERENCE = ROOT / "tests/science/match-reference.json"
MATCH_LISTS = ("composite", "masks", "search_points", "matches", "sequences")
MATCH_EXACT = {"relative": 0.0, "absolute": 0.0}
# One-dimensional cases build the line with Math.tan and convert the subpixel offset with Math.cos and
# Math.sin, which Java specifies only to within 1 ulp; positions derived from them get this slack.
MATCH_TRIG_TOLERANCE = {"relative": 0.0, "absolute": 1e-9}
MAX_MATCH_ENTRIES = 1000
MATCH_HASH = re.compile(r"[0-9a-f]{16}")


def validate_match_reference(document, manifest):
    """Check the template-matching oracle schema before comparing or recording it."""
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or document.get("reference_version") != manifest["reference"]["version"]):
        raise ValueError("Unexpected template-matching oracle schema or reference version")
    if set(document) != {"schema_version", "reference_version", "evolution", "trig", *MATCH_LISTS}:
        raise ValueError("Unexpected template-matching oracle sections")
    trig = document["trig"]
    if (not isinstance(trig, dict) or not isinstance(trig.get("hashes"), dict)
            or set(trig["hashes"]) != {"sin", "cos", "tan"}
            or not all(isinstance(value, str) and MATCH_HASH.fullmatch(value) for value in trig["hashes"].values())
            or not isinstance(trig.get("not_correctly_rounded"), list)
            or any(not isinstance(entry, dict) or entry.get("function") not in {"sin", "cos", "tan"}
                   or type(entry.get("k")) is not int for entry in trig["not_correctly_rounded"])):
        raise ValueError("Invalid Math.sin/cos/tan oracle section")
    for section in MATCH_LISTS:
        entries = document[section]
        if (not isinstance(entries, list) or not entries or len(entries) > MAX_MATCH_ENTRIES
                or not all(isinstance(entry, dict) for entry in entries)):
            raise ValueError(f"Invalid template-matching oracle section {section}")
    composite = document["composite"]
    if ([entry.get("alpha") for entry in composite] != list(range(1, 256))
            or not all(isinstance(entry.get(key), str) and MATCH_HASH.fullmatch(entry[key])
                       for entry in composite for key in ("opaque", "translucent"))):
        raise ValueError("The compositing oracle must hash every alpha from 1 to 255")
    for section in ("masks", "matches", "sequences"):
        ids = [entry.get("id") for entry in document[section]]
        if not all(isinstance(value, str) for value in ids) or len(set(ids)) != len(ids):
            raise ValueError(f"Missing or duplicate template-matching oracle id in {section}")
    if any(not isinstance(case.get("result"), dict) for case in document["matches"]):
        raise ValueError("Every template-matching oracle case needs a result")
    evolution = document["evolution"]
    if not isinstance(evolution, dict) or not isinstance(evolution.get("steps"), list) or not evolution["steps"]:
        raise ValueError("Invalid template-evolution oracle case")
    for sequence in document["sequences"]:
        steps = sequence.get("steps")
        if (type(sequence.get("frames")) is not int or not isinstance(steps, list)
                or [step.get("k") if isinstance(step, dict) else None for step in steps] != list(range(sequence["frames"]))):
            raise ValueError(f"Template-matching sequence {sequence['id']!r} must record every frame")


def match_reference_text(document):
    """Render the fixture with one case per line so that review diffs stay readable."""
    compact = {"separators": (",", ":"), "ensure_ascii": False}
    lines = ["{"]
    keys = list(document)
    for index, key in enumerate(keys):
        separator = "," if index + 1 < len(keys) else ""
        value = document[key]
        if key in MATCH_LISTS:
            lines.append(f"  {json.dumps(key)}: [")
            lines.extend(f"    {json.dumps(entry, **compact)}{',' if position + 1 < len(value) else ''}"
                         for position, entry in enumerate(value))
            lines.append(f"  ]{separator}")
        else:
            lines.append(f"  {json.dumps(key)}: {json.dumps(value, **compact)}{separator}")
    lines.append("}")
    return "\n".join(lines) + "\n"


def match_tolerance(section, entry):
    one_dimensional = (section == "matches" and entry.get("line") is not None) or (
        section == "sequences" and entry.get("angle") is not None)
    return MATCH_TRIG_TOLERANCE if one_dimensional else MATCH_EXACT


@kernel_oracle("match-oracle")
def check_match_oracle(manifest, cache, java, *, record=False):
    """Run the pinned TemplateMatcher on the synthetic template-matching scenes and diff the fixture."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # A private home keeps OSP preference files out of the shared reference cache.
    with tempfile.TemporaryDirectory(prefix="match-oracle-") as temporary:
        home = Path(temporary) / "home"
        home.mkdir()
        output = Path(temporary) / "match-java.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}",
             "-Xmx256m", "--class-path", java_classpath(manifest, cache),
             str(ROOT / "tools/reference/MatchOracle.java"), str(output)])
        actual = json.loads(output.read_text(encoding="utf-8"))
    validate_match_reference(actual, manifest)
    location = MATCH_REFERENCE.relative_to(ROOT).as_posix() if MATCH_REFERENCE.is_relative_to(ROOT) else str(MATCH_REFERENCE)
    count = sum(len(actual[section]) for section in MATCH_LISTS) + 1
    if record:
        MATCH_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        MATCH_REFERENCE.write_text(match_reference_text(actual), encoding="utf-8", newline="\n")
        print(f"match-oracle: recorded {count} Java template-matching cases in {location}")
        return
    expected = json.loads(MATCH_REFERENCE.read_text(encoding="utf-8"))
    validate_match_reference(expected, manifest)
    compare(expected["evolution"], actual["evolution"], MATCH_EXACT, "match-reference.evolution")
    compare(expected["trig"], actual["trig"], MATCH_EXACT, "match-reference.trig")
    for section in MATCH_LISTS:
        if len(expected[section]) != len(actual[section]):
            raise ValueError(f"match-reference.{section}: the Java case list differs from the fixture")
        for index, (want, got) in enumerate(zip(expected[section], actual[section])):
            compare(want, got, match_tolerance(section, want), f"match-reference.{section}[{index}]")
    print(f"match-oracle: {count} Java template-matching cases match {location}")


def session_file_diagnostic(path):
    """Observe a failed reference input without changing it or retrying the loader."""
    try:
        limit = 1024 * 1024
        with path.open("rb") as source:
            data = source.read(limit + 1)
        if len(data) > limit:
            return {"inspection": "skipped: input exceeds 1 MiB diagnostic limit"}
        report = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        report["prefix"] = data[:256].decode("utf-8", errors="replace")
        report["suffix"] = data[-512:].decode("utf-8", errors="replace")
        try:
            root = ET.fromstring(data)
            report.update(xml="well-formed", root=root.tag[:128], root_class=root.get("class", "")[:256])
        except ET.ParseError as error:
            report.update(xml="malformed", parse_error=str(error)[:256])
            line, column = error.position
            lines = data.splitlines()
            if 0 < line <= len(lines):
                # The parser column counts characters, so decode before centering the excerpt.
                text = lines[line - 1].decode("utf-8", errors="replace")
                report["error_line_excerpt"] = text[max(0, column - 128):column + 384]
        return report
    except (OSError, ValueError, LookupError) as error:
        return {"inspection_error": str(error)[:256]}


def format_session_file_diagnostic(path):
    report = session_file_diagnostic(path)
    # Bound each escaped string before serializing. Cutting the finished JSON can drop
    # parser metadata entirely when corrupted bytes expand to six-character escapes.
    # At most six string fields (including the 64-character hash) keep the report below 3000.
    for key, value in report.items():
        if isinstance(value, str):
            while len(json.dumps(value, ensure_ascii=True)) > 512:
                size = len(value) // 2
                value = value[-size:] if key == "suffix" else value[:size]
            report[key] = value
    return json.dumps(report, ensure_ascii=True)


def check_cases(manifest, cache, *, java=None, probe=None, record=False):
    for path, artifact in artifacts(manifest, cache):
        verify_file(path, artifact)
    reports = cache / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    reference_home = cache / "java-home"
    if java:
        reference_home.mkdir(parents=True, exist_ok=True)
    for case in manifest["cases"]:
        project = project_path(case, cache)
        output = reports / (case["id"] + ("-java.json" if java else "-native.json"))
        output.unlink(missing_ok=True)
        if java:
            run(java_reference_command(java, reference_home,
                                       java_classpath(manifest, cache, dependencies=case.get("reference_dependencies", [])),
                                       ROOT / "tools/reference/TrackerReference.java", project, output))
        else:
            run([str(Path(probe).resolve()), str(project), str(output)])
        actual = json.loads(output.read_text(encoding="utf-8"))
        if java:
            if (actual["reference_version"] != manifest["reference"]["version"]
                    or actual["verification_level"] != manifest["reference"]["verification_level"]):
                raise ValueError("Unexpected reference version or verification level")
            if actual["project"]["source_version"] != case["tracker_version"]:
                raise ValueError("Project version differs from manifest")
        expected_file = expected_path(case)
        if record:
            expected_file.parent.mkdir(parents=True, exist_ok=True)
            expected_file.write_text(json.dumps(actual, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        else:
            expected = json.loads(expected_file.read_text(encoding="utf-8"))
            compare(expected if java else expected["project"], actual, manifest["tolerance"], case["id"])
        print(f"{case['id']}: {'recorded reference' if record else 'passed'}"
              f" ({'Java XML/coordinate API' if java else 'MoTrack import/coordinates'}; full round trip untested)")
        if not java and "native_reference_index" in case:
            reference_output = reports / (case["id"] + "-native-references.json")
            reference_output.unlink(missing_ok=True)
            run([str(Path(probe).resolve()), "--references", str(project), str(reference_output)])
            reference_expected = expected_path({"expected": case["native_reference_index"]})
            compare(json.loads(reference_expected.read_text(encoding="utf-8")),
                    json.loads(reference_output.read_text(encoding="utf-8")), manifest["tolerance"], case["id"] + ".reference_index")
            print(f"{case['id']}: passed (partial native source-reference index; rename export untested)")


def session_units(case, expected):
    # Java materializes the pinned application's default when the original XML omits a label.
    # The corpus records that default independently; do not infer expectations from resaved output.
    return {"length_unit": expected["length_unit"] or case["session_default_length_unit"],
            "mass_unit": expected["mass_unit"], "time_unit": expected["time_unit"]}


def validate_session(case, report, manifest, expected=None, key_frames=None, calibration=None):
    if (report["reference_version"] != manifest["reference"]["version"]
            or report["verification_level"] != "tracker_session_resave"
            or report.get("view_preparation") != "expand_visit_custom_tables_restore_selection"):
        raise ValueError("Unexpected session reference version or verification level")
    before = report["before"]
    tolerance = manifest["tolerance"]
    compare(before, report["after_save"], tolerance, case["id"] + ".save")
    compare(before, report["after"], tolerance, case["id"] + ".resave")
    identities = [{"class": track["class"], "name": track["name"]} for track in before["tracks"]]
    compare(case["session_tracks"], identities, tolerance, case["id"] + ".tracks")
    if "session_names" in case:
        compare(case["session_names"], before.get("name_references"), tolerance, case["id"] + ".name_references")
    if "session_selected_track" in case:
        names = before.get("name_references")
        compare(case["session_selected_track"], names.get("selected_track") if isinstance(names, dict) else None,
                tolerance, case["id"] + ".selected_track")
    if "session_views" in case:
        compare(case["session_views"], before.get("saved_views"), tolerance, case["id"] + ".saved_views")
    if "session_centers" in case:
        compare(case["session_centers"], before.get("centers_of_mass"), tolerance, case["id"] + ".centers_of_mass")
    if "session_sums" in case:
        compare(case["session_sums"], before.get("vector_sums"), tolerance, case["id"] + ".vector_sums")
    if "session_attachments" in case:
        compare(case["session_attachments"], before.get("attachments"), tolerance, case["id"] + ".attachments")
    if "session_models" in case:
        compare(case["session_models"], before.get("models"), tolerance, case["id"] + ".models")
    if expected is None:
        expected = json.loads(expected_path(case).read_text(encoding="utf-8"))["project"]
    compare(session_units(case, expected), {key: before[key] for key in ["length_unit", "mass_unit", "time_unit"]},
            tolerance, case["id"] + ".units")
    if "session_time_samples" in case:
        compare(case["session_time_samples"], before.get("time_samples"), tolerance, case["id"] + ".time_samples")
    if "session_frame_duration_ms" in case:
        compare(case["session_frame_duration_ms"], before.get("frame_duration_ms"), tolerance, case["id"] + ".frame_duration_ms")
    if "session_media" in case:
        compare(case["session_media"], before.get("media"), tolerance, case["id"] + ".media")
    elif before.get("media") is not None:
        raise ValueError("Media sessions require independent pixel and timing expectations")
    for name, appearance in case.get("session_track_appearances", {}).items():
        matches = [track for track in before["tracks"] if track["name"] == name]
        if len(matches) != 1:
            raise ValueError("Created movie track appearance target missing or ambiguous")
        compare(appearance, matches[0].get("appearance"), tolerance, case["id"] + ".appearance." + name)
    if "session_created_mass_appearance" in case:
        compare(case["session_created_mass_appearance"], before["tracks"][-1].get("appearance"),
                tolerance, case["id"] + ".created_mass_appearance")
    if "session_created_vector_appearance" in case:
        compare(case["session_created_vector_appearance"], before["tracks"][-1].get("appearance"),
                tolerance, case["id"] + ".created_vector_appearance")
    for key in ["clip", "coordinate_samples"]:
        compare(expected[key], before[key], tolerance, case["id"] + "." + key)
    masses = []
    for track in before["tracks"]:
        if track["class"] == "org.opensourcephysics.cabrillo.tracker.PointMass":
            masses.append({"name": track["name"], "mass": track["mass"], "samples": [
                {"frame": sample["frame"], "image": sample["points"][0], "world": sample["world"]}
                for sample in track["samples"]]})
    compare(expected["point_masses"], masses, tolerance, case["id"] + ".point_masses")
    if "session_axes" in case:
        compare(case["session_axes"], before.get("axes"), tolerance, case["id"] + ".axes")
    if calibration is None:
        calibration = case.get("session_calibration")
    if calibration is not None:
        compare(calibration, before["calibration"], tolerance, case["id"] + ".calibration")
    vectors = [{"name": track["name"], "samples": track["vector_samples"]} for track in before["tracks"]
               if track["class"] == "org.opensourcephysics.cabrillo.tracker.Vector"]
    compare(expected["vectors"], vectors, tolerance, case["id"] + ".vectors")
    if "session_vector_data" in case:
        calibrated = [{"name": track["name"], "samples": track.get("calibrated_vector_samples")}
                      for track in before["tracks"] if track["class"] == "org.opensourcephysics.cabrillo.tracker.Vector"]
        compare(case["session_vector_data"], calibrated, tolerance, case["id"] + ".calibrated_vectors")
    if key_frames is None:
        key_frames = case.get("session_key_frames")
    if key_frames is not None:
        actual = [track["key_frames"] for track in before["tracks"]
                  if track["class"] == "org.opensourcephysics.cabrillo.tracker.PointMass"]
        compare(key_frames, actual, tolerance, case["id"] + ".key_frames")


def validate_known_session_issue(case, report, manifest, saved):
    """Reproduce one exact upstream defect; never count it as round-trip acceptance."""
    if case.get("session_known_issue") == "tracker-6.3.5-vector-negative-time":
        if (case["id"] not in {"vector-movie-negative-time", "vector-images-negative-time"}
                or manifest["reference"]["version"] != "6.3.5"):
            raise ValueError("Unknown Tracker session defect")
        # Independent correct expectations stay in the manifest. The ordinary validator must
        # reject this report; only this exact, non-accepted diagnostic permits omitted rows.
        observed = [{"name": "vector A", "samples": [
                        {"frame": 2, "components": [-2.0, 0.5], "magnitude": math.sqrt(4.25)}]},
                    {"name": "vector B", "samples": [
                        {"frame": 3, "components": [0.0, 0.0], "magnitude": 0.0}]},
                    {"name": "empty vector", "samples": []}]
        validate_session({**case, "session_vector_data": observed}, report, manifest)
        return {"verification_level": "tracker_session_known_issue", "compatibility_accepted": False,
                "known_issue": case["session_known_issue"],
                "expected_vector_data": copy.deepcopy(case["session_vector_data"]), "tracker_session": report}
    issue = "tracker-6.3.5-vector-sum-members"
    if (case["id"] != "vector-sum" or case.get("session_known_issue") != issue
            or manifest["reference"]["version"] != "6.3.5"):
        raise ValueError("Unknown Tracker session defect")
    observed = [{"name": "resultant", "members": [], "samples": []}]
    # All unaffected tracks, units, calibration and three-stage comparisons still apply.
    # The ordinary validator retains the independent, correct sums and rejects this report.
    validate_session({**case, "session_sums": observed}, report, manifest)
    for stage in ["before", "after_save", "after"]:
        sums = [track for track in report[stage]["tracks"]
                if track["class"] == "org.opensourcephysics.cabrillo.tracker.VectorSum"]
        if len(sums) != 1 or sums[0]["samples"] != [] or sums[0]["vector_samples"] != []:
            raise ValueError("Known vector-sum defect changed: unexpected step data")
    if saved.stat().st_size > MAX_XML_BYTES:
        raise ValueError("Known-defect resave exceeds XML limit")
    sums = ET.parse(saved).getroot().findall(
        "property[@name='tracks']/property/object[@class='org.opensourcephysics.cabrillo.tracker.VectorSum']")
    members = sums[0].findall("property[@name='vectors']") if len(sums) == 1 else []
    if (len(members) != 1 or members[0].get("type") != "collection"
            or members[0].get("class") != "java.util.ArrayList"
            or len(members[0]) != 0 or (members[0].text or "").strip()):
        raise ValueError("Known vector-sum defect changed: expected lost saved member names")
    return {"verification_level": "tracker_session_known_issue", "compatibility_accepted": False,
            "known_issue": issue, "expected_vector_sums": copy.deepcopy(case["session_sums"]),
            "tracker_session": report}


XVFB_READY_SECONDS = 30
XVFB_LOG_BYTES = 2000


def file_tail(file, limit):
    """Last `limit` bytes of a file another process may still append to, without moving the shared offset."""
    fd = file.fileno()
    start = max(0, os.fstat(fd).st_size - limit)
    if hasattr(os, "pread"):
        return os.pread(fd, limit, start)
    file.seek(start)  # Windows has no pread; Xvfb itself only runs on Linux.
    return file.read(limit)


@contextmanager
def private_display():
    if sys.platform != "linux":
        raise ValueError("Session checks currently require Linux with Xvfb")
    executable = shutil.which("Xvfb")
    if not executable:
        raise ValueError("Install Xvfb to run the isolated session checks")
    # Xvfb writes its diagnostics for the whole session here; the fatal (EE) line comes last, so
    # startup errors quote the bounded tail (#208). Opened first so no descriptor leaks on any path.
    with tempfile.TemporaryFile() as log:

        def failure(message):
            detail = file_tail(log, XVFB_LOG_BYTES).decode("utf-8", "replace").strip()
            return ValueError(f"{message}: {detail}" if detail else message)

        reader, writer = os.pipe()
        server = None
        try:
            try:
                server = subprocess.Popen([executable, "-displayfd", str(writer), "-screen", "0", "1280x800x24",
                                           "-nolisten", "tcp"], pass_fds=(writer,),
                                          stdout=subprocess.DEVNULL, stderr=log)
            finally:
                os.close(writer)
            # A cold start on a loaded runner can exceed a few seconds; a healthy server answers at once.
            if not select.select([reader], [], [], XVFB_READY_SECONDS)[0]:
                raise failure(f"Private Xvfb display did not become ready within {XVFB_READY_SECONDS} s")
            number = os.read(reader, 32).decode("ascii")
            if not re.fullmatch(r"[0-9]+\n", number) or server.poll() is not None:
                raise failure("Private Xvfb display failed to start")
            yield ":" + number.strip()
        finally:
            os.close(reader)
            if server is not None:
                if server.poll() is None:
                    server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)


MOVIE_TRACK_SCENARIOS = ("selected-name", "unselected-name", "create", "delete-selected", "delete-unselected", "delete-all", "replace")


def movie_tracks_roundtrip_expected(case, scenario):
    if case["id"] not in {"movie-tracks", "archived-movie-tracks"} or scenario not in MOVIE_TRACK_SCENARIOS:
        raise ValueError("Movie authoring requires the pinned two-mass fixture and a known edit")
    expected = linked_media_roundtrip_expected(case)
    case = copy.deepcopy(case)
    masses = expected["point_masses"]
    keys = [[0, 1, 2], [1, 3]]
    if scenario in {"selected-name", "unselected-name"}:
        masses[0 if scenario == "selected-name" else 1]["name"] = RENAME_LABEL
        if scenario == "selected-name":
            case["session_selected_track"] = RENAME_LABEL
    elif scenario == "create":
        # Fixed origin (2,20), scales (2,4), y up. No values are read from the native export.
        masses.extend([{"name": CREATED_MASS_NAME, "mass": 4.0, "samples": [
            {"frame": 0, "image": [8.0, 4.0], "world": [3.0, 4.0]},
            {"frame": 3, "image": [16.0, 8.0], "world": [7.0, 3.0]}]},
            {"name": "empty movie track", "mass": 1.0, "samples": []}])
        keys.extend([[0, 3], []])
        case["session_track_appearances"] = {name: copy.deepcopy(CREATED_MASS_APPEARANCE)
                                             for name in [CREATED_MASS_NAME, "empty movie track"]}
    else:
        removed = {0, 1} if scenario == "delete-all" else {1} if scenario == "delete-unselected" else {0}
        expected["point_masses"] = [mass for i, mass in enumerate(masses) if i not in removed]
        keys = [value for i, value in enumerate(keys) if i not in removed]
        if 0 in removed:
            case["session_selected_track"] = None
        if scenario == "replace":
            expected["point_masses"].append({"name": "movie mass", "mass": 7.0, "samples": [
                {"frame": 3, "image": [14.0, 0.0], "world": [6.0, 5.0]}]})
            keys.append([3])
            case["session_track_appearances"] = {"movie mass": copy.deepcopy(CREATED_MASS_APPEARANCE)}
    case["session_tracks"] = [case["session_tracks"][0]] + [
        {"class": "org.opensourcephysics.cabrillo.tracker.PointMass", "name": mass["name"]}
        for mass in expected["point_masses"]]
    case["session_key_frames"] = keys
    return expected, case


def patch_expected_movie_tracks(document, scenario):
    """Independent XML changes to the frozen two-mass fixture, after the common mass/mark edit."""
    if scenario not in MOVIE_TRACK_SCENARIOS:
        raise ValueError("Unknown movie authoring scenario")
    def children(node, tag):
        return [child for child in node.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == tag]
    def prop(node, name):
        values = [child for child in children(node, "property") if child.getAttribute("name") == name]
        if len(values) != 1:
            raise ValueError("Expected unique pinned movie property: " + name)
        return values[0]
    def scalar(node, text):
        for child in list(node.childNodes):
            node.removeChild(child)
        node.appendChild(document.createTextNode(text))
    root = document.documentElement
    tracks = prop(root, "tracks")
    items = [item for item in children(tracks, "property") if children(item, "object")[0].getAttribute("class")
             == "org.opensourcephysics.cabrillo.tracker.PointMass"]
    if len(items) != 2:
        raise ValueError("Expected two original movie masses")
    if scenario in {"selected-name", "unselected-name"}:
        scalar(prop(children(items[0 if scenario == "selected-name" else 1], "object")[0], "name"), RENAME_LABEL)
        if scenario == "selected-name":
            scalar(prop(root, "selectedtrack"), RENAME_LABEL)
    elif scenario != "create":
        for index in ({0, 1} if scenario == "delete-all" else {1} if scenario == "delete-unselected" else {0}):
            tracks.removeChild(items[index])
        if scenario != "delete-unselected":
            root.removeChild(prop(root, "selectedtrack"))
    additions = ([(CREATED_MASS_NAME, 4, [(0, 8, 4), (3, 16, 8)]), ("empty movie track", 1, [])] if scenario == "create"
                 else [("movie mass", 7, [(3, 14, 0)])] if scenario == "replace" else [])
    for name, mass, samples in additions:
        framedata = ''
        if samples:
            framedata = ('<property name="framedata" type="array" class="[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;">'
                + ''.join(f'<property name="[{frame}]" type="object"><object class="org.opensourcephysics.cabrillo.tracker.PointMass$FrameData">'
                          f'<property name="x" type="double">{java_double(x)}</property><property name="y" type="double">{java_double(y)}</property></object></property>'
                          for frame, x, y in samples) + '</property><property name="keyFrames" type="array" class="[I">'
                '<property name="array" type="string">{' + ','.join(str(row[0]) for row in samples) + '}</property></property>')
        # PointMass.Loader saves mass, then TTrack's name, color, footprint, visible and trail (locked only when true).
        with minidom.parseString('<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.PointMass">'
                f'<property name="mass" type="double">{java_double(mass)}</property><property name="name" type="string"><![CDATA[{name}]]></property>'
                '<property name="color" type="object"><object class="java.awt.Color"><property name="red" type="int">255</property>'
                '<property name="green" type="int">0</property><property name="blue" type="int">0</property>'
                '<property name="alpha" type="int">255</property></object></property>'
                '<property name="footprint" type="string">Footprint.Diamond</property>'
                '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
                + framedata + '</object></property>') as added:
            tracks.appendChild(document.importNode(added.documentElement, True))


def linked_media_roundtrip_expected(case):
    if case["id"] not in {"linked-images", "archived-images", "multitab-images", "linked-movie", "linked-mov", "archived-movie",
                          "movie-tracks", "archived-movie-tracks"}:
        raise ValueError("Linked-media edits require a pinned experiment")
    expected = json.loads(expected_path(case).read_text(encoding="utf-8"))["project"]
    expected["point_masses"][0]["mass"] = 5.0
    expected["point_masses"][0]["samples"].insert(1, {"frame": 1, "image": [8.0, 12.0], "world": [3.0, 2.0]})
    return expected


def secondary_archive_roundtrip_expected(case):
    if case["id"] != "secondary-images":
        raise ValueError("Grouped archive edits require the pinned independent secondary experiment")
    expected = json.loads(expected_path(case).read_text(encoding="utf-8"))["project"]
    expected["point_masses"][0]["mass"] = 7.0
    # Independent fixed coordinates: X=(9-2)/2, Y=(20-11)/4.
    expected["point_masses"][0]["samples"].insert(1, {"frame": 1, "image": [9.0, 11.0], "world": [3.5, 2.25]})
    return expected


def linked_media_clip_expected(case, single=False):
    if case["id"] not in {"linked-images", "archived-images", "linked-movie", "linked-mov", "archived-movie"}:
        raise ValueError("Media clip edits require a pinned image/movie case")
    expected = linked_media_roundtrip_expected(case)
    expected["clip"].update(first_frame=2 if single else 1, step_size=1 if single else 2, step_count=1 if single else 2)
    for sample, frame in zip(expected["coordinate_samples"], [2, 2] if single else [1, 3]):
        sample["frame"] = frame  # Independently pinned fixed calibration.
    case = copy.deepcopy(case)
    case["session_time_samples"] = ([{"frame": 2, "time_ms": -250.0}, {"frame": 2, "time_ms": -250.0}] if single
                                    else [{"frame": 1, "time_ms": -250.0}, {"frame": 3, "time_ms": 0.0}])
    # The raw video retains all four frames, while seeks respect its active bounds (not stride).
    # Full source inventories are independently checksum-verified at export and after Tracker resaves.
    case["session_media"]["frames"] = [case["session_media"]["frames"][2]] if single else case["session_media"]["frames"][1:]
    return expected, case


def linked_movie_calibration_expected(case):
    if case["id"] not in {"linked-movie", "linked-mov", "archived-movie"}:
        raise ValueError("Movie calibration requires a pinned movie experiment")
    expected = linked_media_roundtrip_expected(case)
    expected["length_unit"] = "cm"
    # Independent 90-degree inverse transform: X=(18-y)/8, Y=(10-x)/4.
    for sample, world in zip(expected["point_masses"][0]["samples"],
                             [[0.25, 1.5], [0.75, 0.5], [1.75, -0.5]], strict=True):
        sample["world"] = world
    for sample in expected["coordinate_samples"]:
        sample["world"] = [0.09375, -0.875]
    case = copy.deepcopy(case)
    case["session_calibration"] = {"fixed_origin": True, "fixed_angle": True, "fixed_scale": True,
                                   "keys": [{"frame": 0, "origin": [10.0, 18.0], "angle": 90.0, "scale": [4.0, 8.0]}]}
    return expected, case


def keyed_movie_calibration(case, edited=False, *, normalized=True):
    flags = case.get("movie_calibration_flags")
    if (type(flags) is not int or flags not in range(7)
            or case["id"] not in {f"keyed-movie-{flags}", f"archived-keyed-movie-{flags}"}):
        raise ValueError("Expected a pinned keyed movie calibration fixture")
    return keyed_media_calibration(flags, edited, normalized=normalized)


def keyed_vector_calibration(case, edited=False, *, normalized=True):
    flags = case.get("vector_calibration_flags")
    kind = case.get("media_vector_kind")
    if (type(flags) is not int or flags not in range(7) or kind not in {"images", "movie"}
            or case["id"] not in {f"keyed-vector-{kind}-{flags}", f"archived-keyed-vector-{kind}-{flags}"}):
        raise ValueError("Expected a pinned keyed vector calibration fixture")
    return keyed_media_calibration(flags, edited, normalized=normalized)


def keyed_media_calibration(flags, edited=False, *, normalized=True):
    # Outcomes of distance at frame 2, axes at frame 2, then axes at frame 1. These rows
    # are specified independently of the native writer; fixed origin/angle normalize on Java load.
    edited_rows = {
        0: [[0, 2, 20, 0, 2, 4], [1, 6, 14, -90, 2, 4], [2, 12, 16, 0, 8, 16]],
        1: [[0, 10, 18, 0, 2, 4], [1, 6, 14, -90, 2, 4], [2, 6, 14, 0, 8, 16]],
        2: [[0, 2, 20, 90, 2, 4], [1, 6, 14, -90, 2, 4], [2, 12, 16, -90, 8, 16]],
        3: [[0, 10, 18, 90, 2, 4], [2, 6, 14, -90, 8, 16]],
        4: [[0, 2, 20, 0, 8, 16], [1, 6, 14, -90, 8, 16], [2, 12, 16, 0, 8, 16]],
        5: [[0, 10, 18, 0, 8, 16], [1, 6, 14, -90, 8, 16], [2, 6, 14, 0, 8, 16]],
        6: [[0, 2, 20, 90, 8, 16], [1, 6, 14, -90, 8, 16], [2, 12, 16, -90, 8, 16]]}
    rows = edited_rows[flags] if edited else [[0, 2, 20, 0, 2, 4], [2, 10, 18, 90, 4, 8]]
    keys = [{"frame": row[0], "origin": [float(v) for v in row[1:3]], "angle": float(row[3]),
             "scale": [float(v) for v in row[4:6]]} for row in rows]
    if normalized or not edited:
        origin, angle = list(keys[-1]["origin"]), keys[-1]["angle"]
        scale = list(keys[0]["scale"]) if edited else [4.0, 8.0]
        for key in keys:
            if flags & 1:
                key["origin"] = list(origin)
            if flags & 2:
                key["angle"] = angle
            if flags & 4:
                key["scale"] = list(scale)
    return {"fixed_origin": bool(flags & 1), "fixed_angle": bool(flags & 2), "fixed_scale": bool(flags & 4), "keys": keys}


def keyed_movie_roundtrip_expected(case):
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    expected["length_unit"] = "cm"
    expected["point_masses"][0]["mass"] = 5.0
    expected["point_masses"][0]["samples"][1]["image"] = [8.0, 12.0]
    calibration = keyed_movie_calibration(case, True)
    for sample in expected["point_masses"][0]["samples"] + expected["coordinate_samples"]:
        key = next(key for key in reversed(calibration["keys"]) if key["frame"] <= sample["frame"])
        u, v = [(point - origin) / scale for point, origin, scale in zip(sample["image"], key["origin"], key["scale"])]
        sample["world"] = {0.0: [u, -v], 90.0: [-v, -u], -90.0: [v, u]}[key["angle"]]
    case = copy.deepcopy(case)
    case["session_calibration"] = calibration
    return expected, case


def patch_expected_movie_calibration(document, case):
    frames = {int(node.parentNode.getAttribute("name")[1:-1]): node for node in document.getElementsByTagName("object")
              if node.getAttribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem$FrameData"}
    if sorted(frames) != [0, 2]:
        raise ValueError("Expected pinned movie calibration keys 0/2")
    calibration = keyed_vector_calibration if "vector_calibration_flags" in case else keyed_movie_calibration
    for key in calibration(case, True, normalized=False)["keys"]:
        number = key["frame"]
        added_key = number not in frames
        if added_key:
            added = frames[0].parentNode.cloneNode(True)
            added.setAttribute("name", f"[{number}]")
            frames[2].parentNode.parentNode.insertBefore(added, frames[2].parentNode)
            frames[number] = added.getElementsByTagName("object")[0]
        values = key["origin"] + [key["angle"]] + key["scale"]
        nodes = [next(node for node in frames[number].childNodes if node.nodeType == node.ELEMENT_NODE
                      and node.getAttribute("name") == name).firstChild
                 for name in ["xorigin", "yorigin", "angle", "xscale", "yscale"]]
        if added_key or [float(node.data) for node in nodes] != values:
            for node, value in zip(nodes, values):
                node.data = java_double(value)


MEDIA_VECTOR_SCENARIOS = ("samples", "geometry", "empty", "authoring", "replace", "delete-all")


def media_vector_scenarios(case):
    return ("keyed-calibration",) if "vector_calibration_flags" in case else MEDIA_VECTOR_SCENARIOS


def media_vector_roundtrip_expected(case, scenario):
    if case.get("media_vector_kind") not in {"images", "movie"} or scenario not in media_vector_scenarios(case):
        raise ValueError("Expected a pinned media vector scenario")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    case = copy.deepcopy(case)
    vectors = expected["vectors"]
    data = case["session_vector_data"]
    created = []
    new_sample = {"frame": 2, "tail": [32.0, 0.0], "components": [-2.0, -4.0]}
    new_data = {"frame": 2, "components": [-1.0, 1.0], "magnitude": math.sqrt(2.0)}
    if scenario == "keyed-calibration":
        expected["length_unit"] = "cm"
        calibration = keyed_vector_calibration(case, True)
        case["session_calibration"] = calibration
        vectors[0]["samples"] = [{"frame": 0, "tail": [0.0, 0.0], "components": [4.0, 4.0]},
                                  {"frame": 3, "tail": [32.0, 0.0], "components": [-2.0, -4.0]}]
        for sample in expected["coordinate_samples"]:
            key = next(key for key in reversed(calibration["keys"]) if key["frame"] <= sample["frame"])
            u, v = [(value - origin) / scale for value, origin, scale in zip(sample["image"], key["origin"], key["scale"])]
            sample["world"] = {0.0: [u, -v], 90.0: [-v, -u], -90.0: [v, u]}[key["angle"]]
        for vector, measured in zip(vectors, data, strict=True):
            measured["samples"] = []
            for sample in vector["samples"]:
                key = next(key for key in reversed(calibration["keys"]) if key["frame"] <= sample["frame"])
                u, v = [value / scale for value, scale in zip(sample["components"], key["scale"])]
                components = {0.0: [u, -v], 90.0: [-v, -u], -90.0: [v, u]}[key["angle"]]
                measured["samples"].append({"frame": sample["frame"], "components": components, "magnitude": math.hypot(*components)})
    elif scenario == "samples":
        vectors[0]["samples"] = [{"frame": 0, "tail": [0.0, 0.0], "components": [4.0, 4.0]},
                                  {"frame": 3, "tail": [0.0, 12.0], "components": [4.0, 4.0]}]
        data[0]["samples"] = [{"frame": frame, "components": [2.0, -1.0], "magnitude": math.sqrt(5.0)} for frame in [0, 3]]
        vectors[2]["samples"] = [new_sample]
        data[2]["samples"] = [new_data]
    elif scenario == "geometry":
        expected["length_unit"] = "cm"
        expected["clip"].update(first_frame=1, step_size=2, step_count=2)
        expected["coordinate_samples"] = [{"frame": frame, "image": [13.5, 17.25], "world": [0.09375, -0.875]} for frame in [1, 3]]
        case["session_calibration"] = {"fixed_origin": True, "fixed_angle": True, "fixed_scale": True,
                                       "keys": [{"frame": 0, "origin": [10.0, 18.0], "angle": 90.0, "scale": [4.0, 8.0]}]}
        case["session_time_samples"] = [{"frame": 1, "time_ms": 0.0}, {"frame": 3, "time_ms": 250.0}]
        case["session_media"]["frames"] = case["session_media"]["frames"][1:]
        # Tracker's live analysis includes only clip steps 1 and 3. Raw A samples at 0/2 remain saved.
        data[0]["samples"] = []
        data[1]["samples"] = [{"frame": 1, "components": [0.0, 0.75], "magnitude": 0.75},
                               {"frame": 3, "components": [0.0, 0.0], "magnitude": 0.0}]
    elif scenario == "empty":
        for track in vectors + data:
            track["samples"] = []
    elif scenario == "authoring":
        vectors[0]["name"] = data[0]["name"] = case["session_selected_track"] = RENAME_LABEL
        del vectors[1]
        del data[1]
        new_sample["frame"] = new_data["frame"] = 3
        created = ["nouvelle β & <C>", "new empty vector"]
        vectors.extend([{"name": created[0], "samples": [new_sample]}, {"name": created[1], "samples": []}])
        data.extend([{"name": created[0], "samples": [new_data]}, {"name": created[1], "samples": []}])
    elif scenario == "replace":
        vectors[1]["name"] = data[1]["name"] = RENAME_LABEL
        del vectors[0]
        del data[0]
        created = ["vector A"]
        vectors.append({"name": "vector A", "samples": [new_sample]})
        data.append({"name": "vector A", "samples": [new_data]})
        case["session_selected_track"] = None
    else:
        vectors.clear()
        data.clear()
        case["session_selected_track"] = None
    case["session_tracks"] = [{"class": "org.opensourcephysics.cabrillo.tracker.CoordAxes", "name": "axes"}] + [
        {"class": "org.opensourcephysics.cabrillo.tracker.Vector", "name": vector["name"]} for vector in vectors]
    case["session_track_appearances"] = {name: copy.deepcopy(CREATED_VECTOR_APPEARANCE) for name in created}
    return expected, case


def patch_expected_media_vectors(document, scenario, case=None):
    """Independent explicit XML edit oracle; preserve source null padding and untouched fields."""
    def elements(node, tag):
        return [child for child in node.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == tag]
    def prop(node, name):
        found = [child for child in elements(node, "property") if child.getAttribute("name") == name]
        if len(found) != 1:
            raise ValueError("Unexpected pinned media vector property: " + name)
        return found[0]
    def scalar(node, value):
        for child in list(node.childNodes):
            node.removeChild(child)
        node.appendChild(document.createTextNode(java_double(value) if isinstance(value, (int, float)) else str(value)))
    def frame_xml(frame, values):
        fields = "".join(f'<property name="{name}" type="double">{java_double(value)}</property>' for name, value in zip(
            ["xtail", "ytail", "xcomponent", "ycomponent"], values, strict=True))
        return (f'<property name="[{frame}]" type="object"><object class="org.opensourcephysics.cabrillo.tracker.Vector$FrameData">'
                + fields + '</object></property>')
    def append_xml(parent, xml, before=None):
        with minidom.parseString(xml) as fragment:
            parent.insertBefore(document.importNode(fragment.documentElement, True), before)
    root = document.documentElement
    tracks = prop(root, "tracks")
    vectors = [obj for item in elements(tracks, "property") for obj in elements(item, "object")
               if obj.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.Vector"]
    if len(vectors) != 3 or scenario not in (*MEDIA_VECTOR_SCENARIOS, "keyed-calibration"):
        raise ValueError("Expected three pinned media vectors")
    def add(name, frame=None):
        data = ('' if frame is None else '<property name="framedata" type="array" class="[Lorg.opensourcephysics.cabrillo.tracker.Vector$FrameData;">'
                + frame_xml(frame, [32, 0, -2, -4]) + '</property>')
        # Vector.Loader saves TTrack's name, color, footprint, visible and trail (locked only when true).
        append_xml(tracks, '<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.Vector">'
                   '<property name="name" type="string"><![CDATA[' + name + ']]></property>'
                   '<property name="color" type="object"><object class="java.awt.Color">'
                   '<property name="red" type="int">255</property><property name="green" type="int">0</property>'
                   '<property name="blue" type="int">255</property><property name="alpha" type="int">255</property></object></property>'
                   '<property name="footprint" type="string">Footprint.BoldArrow</property>'
                   '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
                   + data + '</object></property>')
    if scenario == "keyed-calibration":
        if case is None:
            raise ValueError("Missing keyed vector calibration case")
        keyed_vector_calibration(case)  # Validate provenance before applying the shared key-row oracle.
        patch_expected_movie_calibration(document, case)
        scalar(prop(root, "length_unit"), "cm")
        first = prop(vectors[0], "framedata")
        frame = elements(prop(first, "[0]"), "object")[0]
        for name, value in zip(["xtail", "ytail", "xcomponent", "ycomponent"], [0, 0, 4, 4], strict=True):
            scalar(prop(frame, name), value)
        first.removeChild(prop(first, "[2]"))
        append_xml(first, frame_xml(3, [32, 0, -2, -4]), prop(first, "[9]"))
    elif scenario == "samples":
        first, empty = prop(vectors[0], "framedata"), prop(vectors[2], "framedata")
        frame = elements(prop(first, "[0]"), "object")[0]
        for name, value in zip(["xtail", "ytail", "xcomponent", "ycomponent"], [0, 0, 4, 4], strict=True):
            scalar(prop(frame, name), value)
        first.removeChild(prop(first, "[2]"))
        append_xml(first, frame_xml(3, [0, 12, 4, 4]), prop(first, "[9]"))
        append_xml(empty, frame_xml(2, [32, 0, -2, -4]), prop(empty, "[9]"))
    elif scenario == "geometry":
        pass  # Coordinates and clip fields are handled by the independent media oracle.
    elif scenario == "empty":
        for vector in vectors:
            data = prop(vector, "framedata")
            for entry in elements(data, "property"):
                if elements(entry, "object"):
                    data.removeChild(entry)
    elif scenario == "authoring":
        scalar(prop(vectors[0], "name"), RENAME_LABEL)
        scalar(prop(root, "selectedtrack"), RENAME_LABEL)
        tracks.removeChild(vectors[1].parentNode)
        add("nouvelle β & <C>", 3)
        add("new empty vector")
    elif scenario == "replace":
        scalar(prop(vectors[1], "name"), RENAME_LABEL)
        tracks.removeChild(vectors[0].parentNode)
        root.removeChild(prop(root, "selectedtrack"))
        add("vector A", 2)
    else:
        for vector in vectors:
            tracks.removeChild(vector.parentNode)
        root.removeChild(prop(root, "selectedtrack"))


def verify_linked_vector_export_xml(original, exported, case, scenario):
    with minidom.parse(str(original)) as expected, minidom.parse(str(exported)) as actual:
        patch_expected_linked_media(expected, movie=case["media_vector_kind"] == "movie", vector_scenario=scenario, vector_case=case)
        if canonical_tracker_xml(expected) != canonical_tracker_xml(actual):
            raise ValueError("Linked vector export changed unrequested XML content")
    for asset in case["assets"]:
        verify_file(exported.parent / "media é & vectors" / asset["file"], asset)


def patch_expected_linked_media(document, relocate=True, packaged=False, movie=False, clip_mode=None, calibrated=False, keyed_case=None, authoring=None, vector_scenario=None, vector_case=None):
    if vector_scenario == "geometry":
        clip_mode, calibrated = "stride", True
    if clip_mode not in {None, "stride", "single"}:
        raise ValueError("Unknown linked-media clip mode")
    if (calibrated or keyed_case is not None) and not movie and vector_scenario != "geometry":
        raise ValueError("This calibration oracle requires movie media")
    if calibrated and keyed_case is not None:
        raise ValueError("Choose one movie calibration oracle")
    def elements(node, tag):
        return [child for child in node.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == tag]
    def prop(node, name):
        return next(child for child in elements(node, "property") if child.getAttribute("name") == name)
    def scalar(node, value):
        for child in list(node.childNodes):
            node.removeChild(child)
        node.appendChild(document.createTextNode(value))
    root = document.documentElement
    if keyed_case is not None:
        patch_expected_movie_calibration(document, keyed_case)
        scalar(prop(root, "length_unit"), "cm")
    if calibrated:
        coords = elements(prop(root, "coords"), "object")[0]
        frames = prop(coords, "framedata")
        frame = elements(prop(frames, "[0]"), "object")[0]
        for name, value in [("xorigin", 10), ("yorigin", 18), ("angle", 90), ("xscale", 4), ("yscale", 8)]:
            scalar(prop(frame, name), java_double(value))
        scalar(prop(root, "length_unit"), "cm")
    clip = elements(prop(root, "videoclip"), "object")[0]
    if clip_mode is not None:
        values = [("startframe", "2"), ("stepsize", "1"), ("stepcount", "1")] if clip_mode == "single" else [
            ("startframe", "1"), ("stepsize", "2"), ("stepcount", "2")]
        for name, value in values:
            scalar(prop(clip, name), value)
    video = elements(prop(clip, "video"), "object")[0]
    if movie:
        if packaged:
            scalar(prop(video, "path"), "media/movie.mov")
        elif relocate and vector_scenario:
            scalar(prop(video, "path"), "media é & vectors/constant-rate.mov")
        elif relocate:
            scalar(prop(video, "path"), "média & movie/constant-rate." + ("mov" if keyed_case is not None or authoring else "avi"))
    elif relocate or packaged:
        paths = prop(video, "paths")
        directory = "media é & vectors" if vector_scenario else "media é & frames"
        references = [f"images/frame-{index:06d}.png" if packaged else f"{directory}/frame{index}.png" for index in range(4)]
        for index in range(4):
            scalar(prop(paths, f"[{index}]"), references[index])
        scalar(prop(video, "path"), references[0])
    if vector_scenario:
        patch_expected_media_vectors(document, vector_scenario, vector_case)
        return
    mass = next(obj for item in elements(prop(root, "tracks"), "property") for obj in elements(item, "object")
                if obj.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.PointMass")
    scalar(prop(mass, "mass"), java_double(5))
    data = prop(mass, "framedata")
    entry = document.createElement("property")
    entry.setAttribute("name", "[1]")
    entry.setAttribute("type", "object")
    obj = document.createElement("object")
    obj.setAttribute("class", "org.opensourcephysics.cabrillo.tracker.PointMass$FrameData")
    entry.appendChild(obj)
    for axis, value in [("x", java_double(8)), ("y", java_double(12))]:
        coordinate = document.createElement("property")
        coordinate.setAttribute("name", axis)
        coordinate.setAttribute("type", "double")
        coordinate.appendChild(document.createTextNode(value))
        obj.appendChild(coordinate)
    if keyed_case is not None:
        data.replaceChild(entry, prop(data, "[1]"))
    else:
        data.insertBefore(entry, prop(data, "[2]"))
    keys = prop(mass, "keyFrames")
    if elements(keys, "property"):
        raise ValueError("Pinned linked-image source must have initially automatic mark keys")
    if keyed_case is not None:
        # Every original frame is already marked. Editing frame 1's position preserves
        # the source's empty manual-key array; only adding/removing frames rewrites it.
        return
    values = document.createElement("property")
    values.setAttribute("name", "array")
    values.setAttribute("type", "string")
    values.appendChild(document.createTextNode("{0,1,2}"))
    keys.appendChild(values)
    if authoring:
        patch_expected_movie_tracks(document, authoring)


def verify_linked_image_export_xml(original, exported, case, clip_mode=None):
    with minidom.parse(str(original)) as expected, minidom.parse(str(exported)) as actual:
        patch_expected_linked_media(expected, clip_mode=clip_mode)
        if canonical_tracker_xml(expected) != canonical_tracker_xml(actual):
            raise ValueError("Linked-image export changed content beyond the requested paths, mass, mark and clip range")
    for asset in case["assets"]:
        verify_file(exported.parent / "media é & frames" / asset["file"], asset)


def verify_saved_image_inventory(saved, assets):
    """Check every serialized image reference, including frames outside the active clip."""
    if saved.stat().st_size > MAX_XML_BYTES:
        raise ValueError("Saved image experiment exceeds XML limit")
    videos = ET.parse(saved).getroot().findall("property[@name='videoclip']/object/property[@name='video']/object")
    if len(videos) != 1 or videos[0].get("class") != "org.opensourcephysics.media.core.ImageVideo":
        raise ValueError("Saved image inventory requires exactly one ImageVideo")
    arrays = videos[0].findall("property[@name='paths']")
    if len(arrays) != 1 or arrays[0].get("type") != "array" or arrays[0].get("class") != "[Ljava.lang.String;":
        raise ValueError("Saved image inventory lost its complete path array")
    entries = list(arrays[0])
    if len(entries) != len(assets):
        raise ValueError("Saved image inventory changed its frame count")
    inventory = []
    for index, (entry, asset) in enumerate(zip(entries, assets, strict=True)):
        if (entry.tag != "property" or entry.get("name") != f"[{index}]" or entry.get("type") != "string"
                or len(entry) or not entry.text or len(entry.text) > 4096):
            raise ValueError("Saved image inventory contains a missing, reordered or invalid path")
        reference = entry.text.replace("\\", "/")
        if reference.startswith("jar:"):
            reference = reference[4:]
        container, separator, member = reference.partition("!/")
        url = urllib.parse.urlsplit(container)
        if url.scheme == "file":
            if url.netloc not in {"", "localhost"} or url.query or url.fragment:
                raise ValueError("Saved image reference is not a local file")
            container = urllib.request.url2pathname(url.path)
        elif url.scheme and not re.match(r"^[A-Za-z]:/", container):
            raise ValueError("Saved image reference is not a local file")
        if container.replace("\\", "/").startswith("//"):
            raise ValueError("Saved image reference is not a local file")
        path = Path(container)
        if not path.is_absolute():
            path = saved.parent / path
        if separator:
            if (not member or PurePosixPath(member).is_absolute() or ".." in PurePosixPath(member).parts
                    or "!/" in member or path.stat().st_size > 1024 * 1024):
                raise ValueError("Saved image archive reference exceeds fixture bounds")
            with zipfile.ZipFile(path) as archive:
                matches = [info for info in archive.infolist() if info.filename == member]
                if len(matches) != 1 or matches[0].file_size != asset["bytes"] or matches[0].is_dir():
                    raise ValueError("Saved image archive member is missing, ambiguous or changed")
                with archive.open(matches[0]) as source:
                    payload = source.read(asset["bytes"] + 1)
        else:
            with path.open("rb") as source:
                payload = source.read(asset["bytes"] + 1)
        digest = hashlib.sha256(payload).hexdigest()
        if len(payload) != asset["bytes"] or digest != asset["sha256"]:
            raise ValueError(f"Saved image checksum changed at original frame {index}")
        inventory.append({"frame": index, "reference": entry.text, "bytes": len(payload), "sha256": digest})
    return inventory


def verify_saved_movie_asset(saved, asset, *, allow_archive=False):
    """Verify the complete pinned movie after Tracker saves, including excluded frames."""
    if saved.stat().st_size > MAX_XML_BYTES:
        raise ValueError("Saved movie experiment exceeds XML limit")
    videos = ET.parse(saved).getroot().findall("property[@name='videoclip']/object/property[@name='video']/object")
    if len(videos) != 1 or videos[0].get("class") != "org.opensourcephysics.media.xuggle.XuggleVideo":
        raise ValueError("Saved movie requires exactly one XuggleVideo")
    paths = videos[0].findall("property[@name='path']")
    if (len(paths) != 1 or paths[0].get("type") != "string" or len(paths[0])
            or not paths[0].text or len(paths[0].text) > 4096):
        raise ValueError("Saved movie lost its local path")
    reference = paths[0].text.replace("\\", "/")
    if allow_archive and reference.startswith("jar:"):
        reference = reference[4:]
    container, separator, member = reference.partition("!/")
    if separator and not allow_archive:
        raise ValueError("Saved movie reference is not a standalone local file")
    url = urllib.parse.urlsplit(container)
    if url.scheme == "file":
        if url.netloc not in {"", "localhost"} or url.query or url.fragment:
            raise ValueError("Saved movie reference is not a local file")
        container = urllib.request.url2pathname(url.path)
    elif url.scheme and not re.match(r"^[A-Za-z]:/", container):
        raise ValueError("Saved movie reference is not a local file")
    if container.replace("\\", "/").startswith("//"):
        raise ValueError("Saved movie reference is not a local file")
    path = Path(container)
    if not path.is_absolute():
        path = saved.parent / path
    if separator:
        if (not member or PurePosixPath(member).is_absolute() or ".." in PurePosixPath(member).parts
                or "!/" in member or path.stat().st_size > 1024 * 1024):
            raise ValueError("Saved movie archive reference exceeds fixture bounds")
        with zipfile.ZipFile(path) as archive:
            matches = [info for info in archive.infolist() if info.filename == member]
            if len(matches) != 1 or matches[0].file_size != asset["bytes"] or matches[0].is_dir():
                raise ValueError("Saved movie archive member is missing, ambiguous or changed")
            with archive.open(matches[0]) as source:
                payload = source.read(asset["bytes"] + 1)
    else:
        with path.open("rb") as source:
            payload = source.read(asset["bytes"] + 1)
    digest = hashlib.sha256(payload).hexdigest()
    if len(payload) != asset["bytes"] or digest != asset["sha256"]:
        raise ValueError("Saved movie checksum changed")
    return {"reference": paths[0].text, "bytes": len(payload), "sha256": digest}


def verify_linked_movie_export_xml(original, exported, case, clip_mode=None, *, calibrated=False, keyed=False, authoring=None):
    with minidom.parse(str(original)) as expected, minidom.parse(str(exported)) as actual:
        patch_expected_linked_media(expected, movie=True, clip_mode=clip_mode, calibrated=calibrated, keyed_case=case if keyed else None, authoring=authoring)
        if canonical_tracker_xml(expected) != canonical_tracker_xml(actual):
            raise ValueError("Linked-movie export changed content beyond the requested path, mass, mark, clip range and calibration")
    verify_file(exported.parent / "média & movie" / case["assets"][0]["file"], case["assets"][0])


def verify_media_package_export(original, exported, selected_xml, case, clip_mode=None, *, movie=False, calibrated=False, keyed=False, authoring=None, vector_scenario=None):
    """Independent ZIP/XML/hash oracle for a new package; no external media paths may survive."""
    if exported.stat().st_size > 1024 * 1024:
        raise ValueError("Media package reference exceeds the small-fixture limit")
    media = ["media/movie.mov"] if movie else [f"images/frame-{index:06d}.png" for index in range(4)]
    try:
        with zipfile.ZipFile(exported) as package:
            if (package.namelist() != ["experiment.trk", *media] or package.comment
                    or any(entry.compress_type != zipfile.ZIP_DEFLATED or entry.extract_version > 20 for entry in package.infolist())):
                raise ValueError("Unexpected media package inventory or encoding")
            # Reading every bounded member checks its CRC without an unbounded testzip() pass.
            for member, asset in zip(media, case["assets"], strict=True):
                if package.getinfo(member).file_size != asset["bytes"]:
                    raise ValueError("Packaged media size changed")
                payload = package.read(member)
                if len(payload) != asset["bytes"] or hashlib.sha256(payload).hexdigest() != asset["sha256"]:
                    raise ValueError("Packaged media bytes changed")
            if package.getinfo("experiment.trk").file_size > MAX_XML_BYTES:
                raise ValueError("Packaged XML exceeds fixture bounds")
            xml = package.read("experiment.trk")
            with minidom.parse(str(original)) as expected, minidom.parseString(xml) as actual:
                patch_expected_linked_media(expected, packaged=True, clip_mode=clip_mode, movie=movie, calibrated=calibrated,
                                            keyed_case=case if keyed else None, authoring=authoring, vector_scenario=vector_scenario, vector_case=case)
                if canonical_tracker_xml(expected) != canonical_tracker_xml(actual):
                    raise ValueError("Media package changed XML beyond the requested paths, mass, mark, clip and calibration")
            selected_xml.write_bytes(xml)
    except zipfile.BadZipFile as error:
        raise ValueError("Media package ZIP integrity check failed") from error


def patch_expected_secondary_archive_member(document):
    """The second experiment has different mass/position edits and retains all other XML."""
    patch_expected_linked_media(document, relocate=False)
    mass = next(obj for obj in document.getElementsByTagName("object")
                if obj.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.PointMass")
    def property(parent, name):
        return next(node for node in parent.childNodes if node.nodeType == node.ELEMENT_NODE
                    and node.tagName == "property" and node.getAttribute("name") == name)
    def scalar(parent, name, value):
        node = property(parent, name)
        for child in list(node.childNodes):
            node.removeChild(child)
        node.appendChild(document.createTextNode(value))
    scalar(mass, "mass", java_double(7))
    frame = property(property(mass, "framedata"), "[1]").getElementsByTagName("object")[0]
    scalar(frame, "x", java_double(9))
    scalar(frame, "y", java_double(11))


def verify_archive_media_export(original, exported, selected_xml, member_count=7, clip_mode=None, *, movie=False, calibrated=False, keyed_case=None, authoring=None, vector_scenario=None, vector_case=None, grouped=False):
    """Independent ZIP reader: only requested XML and its encoding metadata may change."""
    if max(original.stat().st_size, exported.stat().st_size) > 1024 * 1024:
        raise ValueError("Archive reference exceeds the small-fixture limit")
    original_bytes, output_bytes = original.read_bytes(), exported.read_bytes()
    with zipfile.ZipFile(original) as before, zipfile.ZipFile(exported) as after:
        left, right = before.infolist(), after.infolist()
        if ([entry.filename for entry in left] != [entry.filename for entry in right]
                or len(left) != member_count or before.comment != after.comment or after.testzip() is not None):
            raise ValueError("Archive inventory, comment or member integrity changed")
        ordered_left = sorted(left, key=lambda entry: entry.header_offset)
        ordered_right = sorted(right, key=lambda entry: entry.header_offset)
        if [entry.filename for entry in ordered_left] != [entry.filename for entry in ordered_right]:
            raise ValueError("Archive physical record order changed")
        changed = {"experiment.trk", "second.trk"} if grouped else {"experiment.trk"}
        if not changed.issubset({entry.filename for entry in left}):
            raise ValueError("Archive lacks a requested edited experiment")
        for a, b in zip(left, right):
            for field in ["filename", "date_time", "comment", "extra", "create_system", "create_version", "extract_version",
                          "internal_attr", "external_attr", "volume"]:
                if getattr(a, field) != getattr(b, field):
                    raise ValueError(f"Archive member metadata changed: {a.filename}/{field}")
            if a.filename in changed:
                if b.compress_type != zipfile.ZIP_STORED or b.flag_bits != a.flag_bits & 0x0800:
                    raise ValueError("Unexpected edited-member encoding")
                with minidom.parseString(before.read(a)) as expected, minidom.parseString(after.read(b)) as actual:
                    if a.filename == "second.trk":
                        patch_expected_secondary_archive_member(expected)
                    else:
                        patch_expected_linked_media(expected, relocate=False, movie=movie, clip_mode=clip_mode, calibrated=calibrated,
                                                    keyed_case=keyed_case, authoring=authoring, vector_scenario=vector_scenario, vector_case=vector_case)
                    if canonical_tracker_xml(expected) != canonical_tracker_xml(actual):
                        raise ValueError("Archive export changed XML beyond the requested mass, mark, clip and calibration")
                if a.filename == "experiment.trk":
                    selected_xml.write_bytes(after.read(b))
            else:
                if before.read(a) != after.read(b):
                    raise ValueError("Untouched archive payload changed")
                next_a = next((entry.header_offset for entry in ordered_left if entry.header_offset > a.header_offset), before.start_dir)
                next_b = next((entry.header_offset for entry in ordered_right if entry.header_offset > b.header_offset), after.start_dir)
                if original_bytes[a.header_offset:next_a] != output_bytes[b.header_offset:next_b]:
                    raise ValueError("Untouched compressed archive record changed")
        # Central records retain all bytes except the relocated local-header offsets and the
        # selected member's method/flags/CRC/sizes. This also audits metadata not exposed by ZipInfo.
        old_pos, new_pos = before.start_dir, after.start_dir
        for a in left:
            old_len = 46 + sum(struct.unpack_from("<HHH", original_bytes, old_pos + 28))
            new_len = 46 + sum(struct.unpack_from("<HHH", output_bytes, new_pos + 28))
            old_record = bytearray(original_bytes[old_pos:old_pos + old_len])
            new_record = bytearray(output_bytes[new_pos:new_pos + new_len])
            old_record[42:46] = new_record[42:46]
            if a.filename in changed:
                old_record[8:12] = new_record[8:12]
                old_record[16:28] = new_record[16:28]
            if old_record != new_record:
                raise ValueError("Untouched central-directory metadata changed")
            old_pos += old_len
            new_pos += new_len


def mass_roundtrip_expected(case):
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    if not expected["point_masses"]:
        raise ValueError("Mass round trip needs a point-mass baseline")
    expected["point_masses"][0]["mass"] *= 2.5
    return expected


RENAME_LABEL = "élève β & <B>"


def rename_roundtrip_expected(case, mode):
    vector = mode == "vector-name"
    plot = mode in {"plot-owner-name", "plot-guest-name"}
    center = mode == "center-member-name"
    attachment = mode in {"attachment-first-name", "attachment-last-name"}
    model = mode == "model-booster-name"
    if not ((vector and case["id"] == "independent-vectors") or
            (plot and case["id"] == "plot-guests") or
            (center and case["id"] == "center-of-mass") or
            (attachment and case["id"] == "attachments") or
            (model and case["id"] == "model-dependencies") or
            (case["id"] == "protractor" and mode in {"name", "unselected-name"})):
        raise ValueError("Rename acceptance requires a pinned name case")
    expected = json.loads(expected_path(case).read_text(encoding="utf-8"))["project"]
    index = 0 if mode in {"unselected-name", "plot-owner-name", "center-member-name", "attachment-first-name", "model-booster-name"} else 1
    kind, key, prefix = ("Vector", "vectors", "vector") if vector else ("PointMass", "point_masses", "mass")
    original_names = ["mass A"] if model else [prefix + " A", prefix + " B"] + (["mass C"] if plot else [])
    if [track["name"] for track in expected[key]] != original_names:
        raise ValueError("Unexpected rename source identities")
    expected[key][index]["name"] = RENAME_LABEL
    updated = copy.deepcopy(case)
    old = prefix + (" B" if index == 1 else " A")
    if vector:
        updated["session_names"] = {"selected_track": "vector B",
                                    "view_tracks": [["vector B", None], [None, "vector B"], [None, None], [None, None]],
                                    "packed_view_tracks": "vector B,null;null,vector B;null,null;null,null"}
    for track in updated["session_tracks"]:
        if track["class"] == "org.opensourcephysics.cabrillo.tracker." + kind and track["name"] == old:
            track["name"] = RENAME_LABEL
    if "session_names" in updated:
        names = updated["session_names"]
        if names["selected_track"] == old:
            names["selected_track"] = RENAME_LABEL
        names["view_tracks"] = [[RENAME_LABEL if name == old else name for name in pair] for pair in names["view_tracks"]]
        names["packed_view_tracks"] = ";".join(",".join(name if name is not None else "null" for name in pair)
                                                for pair in names["view_tracks"])
    if plot:
        updated["session_selected_track"] = RENAME_LABEL if mode == "plot-owner-name" else "mass A"
        for view in updated["session_views"]["plots"]:
            if view["selected_track"] == old:
                view["selected_track"] = RENAME_LABEL
            for owner in view["track_views"]:
                if owner["track"] == old:
                    owner["track"] = RENAME_LABEL
                for panel in owner["plots"]:
                    panel["guests"] = [RENAME_LABEL if guest == old else guest for guest in panel["guests"]]
    if center:
        updated["session_selected_track"] = "center"
        for dependent in updated["session_centers"]:
            dependent["members"] = [RENAME_LABEL if member == old else member for member in dependent["members"]]
    if attachment:
        updated["session_selected_track"] = "both ends"
        for tape in updated["session_attachments"]:
            tape["targets"] = [RENAME_LABEL if target == old else target for target in tape["targets"]]
    if model:
        updated["session_selected_track"] = "system"
        for particle in updated["session_models"]:
            if particle["booster"] == old:
                particle["booster"] = RENAME_LABEL
    for table in updated.get("session_views", {}).get("tables", []):
        if table["selected_track"] == old:
            table["selected_track"] = RENAME_LABEL
        for key in ["track_columns", "column_formats"]:
            for row in table[key]:
                if row[0] == old:
                    row[0] = RENAME_LABEL
    return expected, updated


def mass_unit_roundtrip_expected(case):
    if case["id"] != "protractor":
        raise ValueError("The mass-unit oracle requires the pinned protractor case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    expected["mass_unit"] = "g"
    return expected


def verify_mass_unit_export_xml(original, exported):
    expected = minidom.parse(str(original))
    units = [node for node in expected.documentElement.childNodes if node.nodeType == node.ELEMENT_NODE
             and node.tagName == "property" and node.getAttribute("name") == "mass_unit"]
    if len(units) != 1 or units[0].getAttribute("type") != "string":
        raise ValueError("Expected pinned kilogram label")
    unit = units[0]
    if len(unit.childNodes) != 1 or unit.firstChild.nodeType not in {unit.TEXT_NODE, unit.CDATA_SECTION_NODE}:
        raise ValueError("Unexpected pinned unit contents")
    if unit.firstChild.data != "kg":
        raise ValueError("Expected pinned kilogram label")
    unit.replaceChild(expected.createCDATASection("g"), unit.firstChild)
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested mass unit")


TIME_UNIT_LABEL = "τ & <tick>"


def time_unit_roundtrip_expected(case):
    if case["id"] != "protractor":
        raise ValueError("The time-unit oracle requires the pinned protractor case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    expected["time_unit"] = TIME_UNIT_LABEL
    return expected


def verify_time_unit_export_xml(original, exported):
    expected = minidom.parse(str(original))
    units = [node for node in expected.documentElement.childNodes if node.nodeType == node.ELEMENT_NODE
             and node.tagName == "property" and node.getAttribute("name") == "time_unit"]
    if units:
        raise ValueError("Expected the pinned source without an explicit time unit")
    unit = expected.createElement("property")
    unit.setAttribute("name", "time_unit")
    unit.setAttribute("type", "string")
    unit.appendChild(expected.createCDATASection(TIME_UNIT_LABEL))
    expected.documentElement.appendChild(unit)
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested time unit")


CREATED_MASS_NAME = "nouvelle β & <C>"
CREATED_MASS_APPEARANCE = {"visible": True, "trail": True, "locked": False, "footprint": "Footprint.Diamond", "argb": -65536}


def create_mass_roundtrip_expected(case, empty=False):
    if case["id"] != "protractor":
        raise ValueError("The created-mass oracle requires the pinned protractor case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    # Pinned origin (320, 240), unit scale, y up. Expectations are independent of exported XML.
    expected["point_masses"].append({"name": CREATED_MASS_NAME, "mass": 3.0, "samples": [] if empty else [
        {"frame": 1, "image": [330.0, 220.0], "world": [10.0, 20.0]},
        {"frame": 4, "image": [340.0, 210.0], "world": [20.0, 30.0]},
        {"frame": 8, "image": [360.0, 230.0], "world": [40.0, 10.0]}]})
    case = copy.deepcopy(case)
    case["session_tracks"].append({"class": "org.opensourcephysics.cabrillo.tracker.PointMass", "name": CREATED_MASS_NAME})
    case["session_created_mass_appearance"] = copy.deepcopy(CREATED_MASS_APPEARANCE)
    return expected, case


def verify_create_mass_export_xml(original, exported, empty=False):
    expected = minidom.parse(str(original))
    tracks = [node for node in expected.documentElement.childNodes if node.nodeType == node.ELEMENT_NODE
              and node.tagName == "property" and node.getAttribute("name") == "tracks"]
    if len(tracks) != 1:
        raise ValueError("Expected one original track collection")
    framedata = ""
    if not empty:
        framedata = ('<property name="framedata" type="array" class="[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;">'
                     + "".join(f'<property name="[{frame}]" type="object"><object class="org.opensourcephysics.cabrillo.tracker.PointMass$FrameData">'
                               f'<property name="x" type="double">{java_double(x)}</property><property name="y" type="double">{java_double(y)}</property>'
                               '</object></property>' for frame, x, y in [(1, 330, 220), (4, 340, 210), (8, 360, 230)])
                     + '</property><property name="keyFrames" type="array" class="[I">'
                       '<property name="array" type="string">{1,4,8}</property></property>')
    # PointMass.Loader saves mass, then TTrack's name, color, footprint, visible and trail (locked only when true).
    added = minidom.parseString(
        '<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.PointMass">'
        '<property name="mass" type="double">3.0</property><property name="name" type="string"><![CDATA[nouvelle β & <C>]]></property>'
        '<property name="color" type="object"><object class="java.awt.Color">'
        '<property name="red" type="int">255</property><property name="green" type="int">0</property>'
        '<property name="blue" type="int">0</property><property name="alpha" type="int">255</property></object></property>'
        '<property name="footprint" type="string">Footprint.Diamond</property>'
        '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
        + framedata + '</object></property>')
    tracks[0].appendChild(expected.importNode(added.documentElement, True))
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    for document in [expected, actual, added]:
        document.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested new point mass")


CREATED_VECTOR_NAME = "vecteur β & <C>"
# Track class, name and the properties Tracker's loaders model for a created track (oracle O7).
CREATED_TRACK_MODES = {
    "create-mass": ("org.opensourcephysics.cabrillo.tracker.PointMass", CREATED_MASS_NAME,
                    ["mass", "name", "color", "footprint", "visible", "trail"]),
    "create-vector": ("org.opensourcephysics.cabrillo.tracker.Vector", CREATED_VECTOR_NAME,
                      ["name", "color", "footprint", "visible", "trail"])}
CREATED_TRACK_MODES["create-empty-mass"] = CREATED_TRACK_MODES["create-mass"]
CREATED_TRACK_MODES["create-empty-vector"] = CREATED_TRACK_MODES["create-vector"]


def created_track_properties(path, track_class, name, fields):
    """The created track's modeled properties in file order, as (name, type, value) snapshots."""
    def value(node):
        attributes = tuple(sorted(node.attributes.items()))
        if node.tagName == "object" or node.getAttribute("type") in {"object", "array", "collection"}:
            return (node.tagName, attributes, tuple(value(child) for child in node.childNodes if child.nodeType == child.ELEMENT_NODE))
        return (node.tagName, attributes, "".join(child.data for child in node.childNodes
                                                  if child.nodeType in {child.TEXT_NODE, child.CDATA_SECTION_NODE}))
    with minidom.parse(str(path)) as document:
        for track in document.getElementsByTagName("object"):
            if track.getAttribute("class") != track_class:
                continue
            properties = [child for child in track.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == "property"]
            names = [value(child)[2] for child in properties if child.getAttribute("name") == "name"]
            if names == [name]:
                return [value(child) for child in properties if child.getAttribute("name") in fields]
    raise ValueError(f"{path.name}: created track {name!r} is missing")


def verify_created_track_fixpoint(exported, resaved, mode):
    """O7: for a track MoTrack created, the properties Tracker models are written exactly as Tracker writes them."""
    track_class, name, fields = mode
    ours = created_track_properties(exported, track_class, name, fields)
    tracker = created_track_properties(resaved, track_class, name, fields)
    if ours != tracker or [item[1][0][1] for item in ours] != fields:
        raise ValueError(f"O7: the created track's properties differ from Tracker's resave:\n{ours}\n{tracker}")
CREATED_VECTOR_APPEARANCE = {"visible": True, "trail": True, "locked": False, "footprint": "Footprint.BoldArrow", "argb": -65281}


PLOT_DELETION_NAMES = {"delete-plot-owner": "mass A", "delete-plot-guest": "mass B", "delete-first-plot-guest": "mass C"}
TABLE_DELETION_NAMES = {"delete-first-table-owner": {"mass A"}, "delete-selected-table-owner": {"mass B"},
                        "delete-last-table-owner": {"mass C"}, "delete-all-table-owners": {"mass A", "mass B", "mass C"}}


def deletion_roundtrip_expected(case, mode):
    mass_modes = {"delete-mass", "delete-selected-mass", "delete-all-masses"}
    if (mode in mass_modes and case["id"] != "protractor"
            or mode in {"delete-vector", "delete-all-vectors"} and case["id"] != "independent-vectors"
            or mode in PLOT_DELETION_NAMES and case["id"] != "plot-guests"
            or mode in TABLE_DELETION_NAMES and case["id"] != "table-default-format-owners"
            or mode not in mass_modes | {"delete-vector", "delete-all-vectors"} | PLOT_DELETION_NAMES.keys() | TABLE_DELETION_NAMES.keys()):
        raise ValueError("The deletion oracle requires its pinned mass/vector case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    case = copy.deepcopy(case)
    if mode in TABLE_DELETION_NAMES:
        removed = TABLE_DELETION_NAMES[mode]
        expected["point_masses"] = [track for track in expected["point_masses"] if track["name"] not in removed]
        all_owners = mode == "delete-all-table-owners"
        selected = None if all_owners else "mass A" if "mass B" in removed else "mass B"
        first = None if all_owners else "mass B" if "mass A" in removed else "mass A"
        case["session_names"] = {"selected_track": None if "mass B" in removed else "mass B",
                                 "view_tracks": [[first, selected], [None, first], [None, None], [None, None]],
                                 "packed_view_tracks": f"{first or 'null'},{selected or 'null'};null,{first or 'null'};null,null;null,null"}
        if all_owners:
            case["session_views"]["tables"] = []
        else:
            table = case["session_views"]["tables"][0]
            table["selected_track"] = selected
            for field in ["track_columns", "column_formats"]:
                table[field] = [row for row in table[field] if row[0] not in removed]
    elif mode in PLOT_DELETION_NAMES:
        removed = {PLOT_DELETION_NAMES[mode]}
        expected["point_masses"] = [track for track in expected["point_masses"] if track["name"] not in removed]
        owner = mode == "delete-plot-owner"
        selected = "mass B" if owner else "mass A"
        case["session_names"] = {"selected_track": None if owner else "mass A",
                                 "view_tracks": [[selected, None], [None, selected], [None, None], [None, None]],
                                 "packed_view_tracks": f"{selected},null;null,{selected};null,null;null,null"}
        if owner:
            case["session_views"]["plots"] = []
        else:
            for plot in case["session_views"]["plots"][0]["track_views"][0]["plots"]:
                plot["guests"] = [guest for guest in plot["guests"] if guest not in removed]
    elif mode == "delete-mass":
        expected["point_masses"] = expected["point_masses"][1:]
        removed = {"mass A"}
    elif mode in {"delete-selected-mass", "delete-all-masses"}:
        expected["point_masses"] = [] if mode == "delete-all-masses" else expected["point_masses"][:1]
        removed = {"mass A", "mass B"} if mode == "delete-all-masses" else {"mass B"}
        fallback = "protractor A" if mode == "delete-all-masses" else "mass A"
        case["session_names"] = {"selected_track": None,
                                 "view_tracks": [[fallback, None], [None, fallback], [None, None], [None, None]],
                                 "packed_view_tracks": f"{fallback},null;null,{fallback};null,null;null,null"}
        case["session_views"]["tables"][0]["selected_track"] = fallback
    else:
        expected["vectors"] = [] if mode == "delete-all-vectors" else expected["vectors"][1:]
        removed = {"vector A", "vector B"} if mode == "delete-all-vectors" else {"vector A"}
        case["session_vector_data"] = [] if mode == "delete-all-vectors" else [
            {"name": "vector B", "samples": [{"frame": 2, "components": [0.0, 0.0], "magnitude": 0.0}]}]
    case["session_tracks"] = [track for track in case["session_tracks"] if track["name"] not in removed]
    return expected, case


def verify_deletion_export_xml(original, exported, mode):
    names = {"delete-mass": {"mass A"}, "delete-selected-mass": {"mass B"}, "delete-all-masses": {"mass A", "mass B"},
             "delete-vector": {"vector A"}, "delete-all-vectors": {"vector A", "vector B"},
             **{key: {value} for key, value in PLOT_DELETION_NAMES.items()}, **TABLE_DELETION_NAMES}
    if mode not in names:
        raise ValueError("Unknown deletion mode")
    expected = minidom.parse(str(original))
    root = expected.documentElement
    if mode in PLOT_DELETION_NAMES:
        patch_expected_plot_deletion(expected, mode)
    elif mode in TABLE_DELETION_NAMES:
        patch_expected_table_deletion(expected, mode)
    if mode in {"delete-selected-mass", "delete-all-masses"}:
        fallback = "protractor A" if mode == "delete-all-masses" else "mass A"

        def direct_property(parent, name, value):
            nodes = [node for node in parent.childNodes if node.nodeType == node.ELEMENT_NODE
                     and node.tagName == "property" and node.getAttribute("name") == name]
            if len(nodes) != 1 or nodes[0].getAttribute("type") != "string":
                raise ValueError("Expected one original selection property")
            node = nodes[0]
            if any(c.nodeType not in {c.TEXT_NODE, c.CDATA_SECTION_NODE} for c in node.childNodes):
                raise ValueError("Unexpected selection content")
            if "".join(c.data for c in node.childNodes) != value:
                raise ValueError("Unexpected original selection")
            return node

        def replace_text(node, text):
            for child in list(node.childNodes):
                node.removeChild(child)
            node.appendChild(expected.createTextNode(text))

        root.removeChild(direct_property(root, "selectedtrack", "mass B"))
        packed = direct_property(root, "selected_track_views", "mass B,null;null,mass B;null,null;null,null")
        replace_text(packed, f"{fallback},null;null,{fallback};null,null;null,null")
        tables = [node for node in root.getElementsByTagName("object")
                  if node.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.TableTView"]
        if len(tables) != 1:
            raise ValueError("Expected the original custom table")
        replace_text(direct_property(tables[0], "selected_track", "mass B"), fallback)
    collections = [node for node in root.childNodes if node.nodeType == node.ELEMENT_NODE
                   and node.tagName == "property" and node.getAttribute("name") == "tracks"]
    if len(collections) != 1:
        raise ValueError("Expected one original track collection")
    deleted = set()
    for item in list(collections[0].childNodes):
        if item.nodeType != item.ELEMENT_NODE:
            continue
        objects = [node for node in item.childNodes if node.nodeType == node.ELEMENT_NODE and node.tagName == "object"]
        if len(objects) != 1:
            raise ValueError("Expected one original track object")
        labels = [node for node in objects[0].childNodes if node.nodeType == node.ELEMENT_NODE
                  and node.tagName == "property" and node.getAttribute("name") == "name"]
        if len(labels) != 1:
            raise ValueError("Expected one original track name")
        name = "".join(node.data for node in labels[0].childNodes if node.nodeType in {node.TEXT_NODE, node.CDATA_SECTION_NODE})
        if name in names[mode]:
            expected_class = "org.opensourcephysics.cabrillo.tracker." + ("Vector" if "vector" in mode else "PointMass")
            if objects[0].getAttribute("class") != expected_class or name in deleted:
                raise ValueError("Unexpected deletion target")
            deleted.add(name)
            collections[0].removeChild(item)
    if deleted != names[mode]:
        raise ValueError("Missing deletion target")
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested track deletion")


def patch_expected_table_deletion(document, mode):
    """Remove only the explicitly requested owners from the pinned three-table source."""
    def prop(parent, name):
        values = [node for node in parent.childNodes if node.nodeType == node.ELEMENT_NODE
                  and node.tagName == "property" and node.getAttribute("name") == name]
        if len(values) != 1:
            raise ValueError("Expected one original table property")
        return values[0]

    def text(node):
        if node.getAttribute("type") != "string" or any(c.nodeType not in {c.TEXT_NODE, c.CDATA_SECTION_NODE} for c in node.childNodes):
            raise ValueError("Expected an original table string")
        return "".join(c.data for c in node.childNodes)

    def cells(node):
        items = [c for c in node.childNodes if c.nodeType == c.ELEMENT_NODE]
        if [c.getAttribute("name") for c in items] != [f"[{i}]" for i in range(len(items))]:
            raise ValueError("Expected contiguous original table indices")
        return items

    tables = [node for node in document.getElementsByTagName("object")
              if node.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.TableTView"]
    if len(tables) != 1:
        raise ValueError("Expected the original custom table")
    table = tables[0]
    root = document.documentElement
    if text(prop(table, "selected_track")) != "mass B" or text(prop(root, "selectedtrack")) != "mass B":
        raise ValueError("Expected the original table selections")
    removed = TABLE_DELETION_NAMES[mode]
    columns = prop(table, "track_columns")
    rows = cells(columns)
    if [[text(value) for value in cells(row)] for row in rows] != [["mass A", "t", "x", "y", "frame"], ["mass B", "t", "x", "frame"], ["mass C", "t", "y", "frame"]]:
        raise ValueError("Expected the pinned original table columns")
    formats = prop(table, "column_formats")
    groups = cells(formats)
    if [[[text(value) for value in cells(row)] for row in cells(group)] for group in groups] != [
            [["mass A", "frame", "0"], ["mass A", "step", "0"]],
            [["mass B", "frame", "0"], ["mass B", "t", "0.000"]], [["mass C", "step", "0"]]]:
        raise ValueError("Expected the pinned original table formats")
    for array, entries in [(columns, rows), (formats, groups)]:
        for name, entry in zip(["mass A", "mass B", "mass C"], entries):
            if name in removed:
                array.removeChild(entry)
        survivors = [node for node in array.childNodes if node.nodeType == node.ELEMENT_NODE]
        if survivors:
            for i, node in enumerate(survivors):
                node.setAttribute("name", f"[{i}]")
        else:
            table.removeChild(array)
    if "mass B" in removed:
        root.removeChild(prop(root, "selectedtrack"))
        selected = prop(table, "selected_track")
        if mode == "delete-all-table-owners":
            table.removeChild(selected)
            # The pinned saver omits a table with no custom state. An empty TableTView
            # would pass a null selection to TrackerPanel.getTrackByName during load.
            slot = table.parentNode
            chooser = slot.parentNode
            views = chooser.parentNode
            chooser.removeChild(slot)
            if not any(child.nodeType == child.ELEMENT_NODE for child in chooser.childNodes):
                views.removeChild(chooser)
            if not any(child.nodeType == child.ELEMENT_NODE for child in views.childNodes):
                root.removeChild(views)
        else:
            for child in list(selected.childNodes):
                selected.removeChild(child)
            selected.appendChild(document.createTextNode("mass A"))
    packed = prop(root, "selected_track_views")
    if text(packed) != "mass A,mass B;null,mass A;null,null;null,null":
        raise ValueError("Expected the pinned original packed table selections")
    first = next((name for name in ["mass A", "mass B", "mass C"] if name not in removed), "null")
    a = "mass A" if "mass A" not in removed else first
    b = "mass B" if "mass B" not in removed else first
    for child in list(packed.childNodes):
        packed.removeChild(child)
    packed.appendChild(document.createTextNode(f"{a},{b};null,{a};null,null;null,null"))


def patch_expected_plot_deletion(document, mode):
    """Independent, fixture-specific XML edits; never derive expected links from MoTrack output."""
    def prop(parent, name):
        values = [node for node in parent.childNodes if node.nodeType == node.ELEMENT_NODE
                  and node.tagName == "property" and node.getAttribute("name") == name]
        if len(values) != 1:
            raise ValueError("Expected one original plot property")
        return values[0]

    def text(node):
        if node.getAttribute("type") != "string" or any(c.nodeType not in {c.TEXT_NODE, c.CDATA_SECTION_NODE} for c in node.childNodes):
            raise ValueError("Expected an original plot string")
        return "".join(c.data for c in node.childNodes)

    views = [node for node in document.getElementsByTagName("object")
             if node.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.PlotTView"]
    owners = [node for node in document.getElementsByTagName("object")
              if node.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.PlotTrackView"]
    if len(views) != 1 or len(owners) != 1 or text(prop(owners[0], "track")) != "mass A":
        raise ValueError("Expected the pinned original plot owner")
    view, owner = views[0], owners[0]
    if text(prop(view, "selected_track")) != "mass A" or text(prop(document.documentElement, "selectedtrack")) != "mass A":
        raise ValueError("Expected the pinned original plot selections")
    for index, expected_guests in [(0, ["mass C", "mass B"]), (1, ["mass B"])]:
        plot = next(node for node in prop(owner, f"plot{index}").childNodes if node.nodeType == node.ELEMENT_NODE)
        guests = prop(plot, "guests")
        cells = [node for node in guests.childNodes if node.nodeType == node.ELEMENT_NODE]
        if [text(cell) for cell in cells] != expected_guests or [cell.getAttribute("name") for cell in cells] != [f"[{i}]" for i in range(len(cells))]:
            raise ValueError("Expected the pinned original guest order")
        if mode != "delete-plot-owner":
            for cell in cells:
                if text(cell) == PLOT_DELETION_NAMES[mode]:
                    guests.removeChild(cell)
            survivors = [node for node in guests.childNodes if node.nodeType == node.ELEMENT_NODE]
            if not survivors:
                plot.removeChild(guests)
            else:
                for i, cell in enumerate(survivors):
                    cell.setAttribute("name", f"[{i}]")
    if mode == "delete-plot-owner":
        item = owner.parentNode
        item.parentNode.removeChild(item)
        root = document.documentElement
        root.removeChild(prop(root, "selectedtrack"))
        selected = prop(view, "selected_track")
        for child in list(selected.childNodes):
            selected.removeChild(child)
        selected.appendChild(document.createTextNode("mass B"))


def create_vector_roundtrip_expected(case, empty=False):
    if case["id"] != "independent-vectors":
        raise ValueError("The created-vector oracle requires the pinned independent-vector case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    expected["vectors"].append({"name": CREATED_VECTOR_NAME, "samples": [] if empty else [
        {"frame": 1, "tail": [100.0, 120.0], "components": [4.0, -8.0]},
        {"frame": 6, "tail": [200.0, 180.0], "components": [0.0, 0.0]},
        {"frame": 8, "tail": [160.0, 100.0], "components": [-6.0, 12.0]}]})
    case = copy.deepcopy(case)
    case["session_tracks"].append({"class": "org.opensourcephysics.cabrillo.tracker.Vector", "name": CREATED_VECTOR_NAME})
    case["session_created_vector_appearance"] = copy.deepcopy(CREATED_VECTOR_APPEARANCE)
    # Independent arithmetic for the pinned angle=0, scaleX=2, scaleY=4, image-y-down calibration.
    # Include old tracks too: identical incorrect values at load/save/reopen must still fail.
    case["session_vector_data"] = [
        {"name": "vector A", "samples": [
            {"frame": 0, "components": [1.5, 1.0], "magnitude": math.sqrt(3.25)},
            {"frame": 5, "components": [-2.5, -3.0], "magnitude": math.sqrt(15.25)}]},
        {"name": "vector B", "samples": [{"frame": 2, "components": [0.0, 0.0], "magnitude": 0.0}]},
        {"name": CREATED_VECTOR_NAME, "samples": [] if empty else [
            {"frame": 1, "components": [2.0, 2.0], "magnitude": math.sqrt(8.0)},
            {"frame": 6, "components": [0.0, 0.0], "magnitude": 0.0},
            {"frame": 8, "components": [-3.0, -3.0], "magnitude": math.sqrt(18.0)}]}]
    return expected, case


def verify_create_vector_export_xml(original, exported, empty=False):
    expected = minidom.parse(str(original))
    tracks = [node for node in expected.documentElement.childNodes if node.nodeType == node.ELEMENT_NODE
              and node.tagName == "property" and node.getAttribute("name") == "tracks"]
    if len(tracks) != 1:
        raise ValueError("Expected one original vector collection")
    framedata = ""
    if not empty:
        framedata = ('<property name="framedata" type="array" class="[Lorg.opensourcephysics.cabrillo.tracker.Vector$FrameData;">'
                     + "".join(f'<property name="[{frame}]" type="object"><object class="org.opensourcephysics.cabrillo.tracker.Vector$FrameData">'
                               f'<property name="xtail" type="double">{java_double(x)}</property><property name="ytail" type="double">{java_double(y)}</property>'
                               f'<property name="xcomponent" type="double">{java_double(cx)}</property><property name="ycomponent" type="double">{java_double(cy)}</property>'
                               '</object></property>' for frame, x, y, cx, cy in [(1, 100, 120, 4, -8), (6, 200, 180, 0, 0), (8, 160, 100, -6, 12)])
                     + '</property>')
    # Vector.Loader saves TTrack's name, color, footprint, visible and trail (locked only when true).
    added = minidom.parseString(
        '<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.Vector">'
        '<property name="name" type="string"><![CDATA[vecteur β & <C>]]></property>'
        '<property name="color" type="object"><object class="java.awt.Color">'
        '<property name="red" type="int">255</property><property name="green" type="int">0</property>'
        '<property name="blue" type="int">255</property><property name="alpha" type="int">255</property></object></property>'
        '<property name="footprint" type="string">Footprint.BoldArrow</property>'
        '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
        + framedata + '</object></property>')
    tracks[0].appendChild(expected.importNode(added.documentElement, True))
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    for document in [expected, actual, added]:
        document.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested new vector")


def mark_roundtrip_expected(case, mode="marks"):
    if case["id"] != "protractor":
        raise ValueError("The mark oracle is defined for the pinned protractor case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    if mode in {"empty-marks", "first-mark"}:
        expected["point_masses"][0]["samples"] = [] if mode == "empty-marks" else [
            {"frame": 4, "image": [11.25, 22.5], "world": [-308.75, 217.5]}]
        if mode == "first-mark":
            expected["source_version"] = "6.3.5" # Input is Tracker's resave of the empty-mark case.
        return expected
    mark = expected["point_masses"][0]["samples"][0]
    mark["image"][0] += 12.5
    mark["image"][1] -= 3.25
    mark["world"][0] += 12.5
    mark["world"][1] += 3.25
    # This pinned experiment has scale 1 and origin (320, 240), independently recorded by Java.
    expected["point_masses"][0]["samples"].append({"frame": 3, "image": [100.125, 200.5], "world": [-219.875, 39.5]})
    expected["point_masses"][1]["samples"] = [{"frame": 7, "image": [50.0, 60.0], "world": [-270.0, 180.0]}]
    return expected


def vector_roundtrip_expected(case, mode="vectors"):
    if case["id"] != "independent-vectors":
        raise ValueError("The vector oracle is defined for the Java-authored two-vector case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    if mode == "empty-vectors":
        expected["vectors"][0]["samples"] = []
    elif mode == "first-vector":
        expected.update(session_units(case, expected))
        expected["vectors"][0]["samples"] = [{"frame": 4, "tail": [11.25, 22.5], "components": [0.0, 0.0]}]
    else:
        expected["vectors"][0]["samples"] = [
            {"frame": 0, "tail": [22.5, 16.75], "components": [-6.5, 8.25]},
            {"frame": 3, "tail": [100.125, 200.5], "components": [0.0, -2.5]}]
        expected["vectors"][1]["samples"] = [{"frame": 7, "tail": [50.0, 60.0], "components": [-10.0, 20.0]}]
    return expected


def length_unit_roundtrip_expected(case):
    if case["id"] != "independent-vectors":
        raise ValueError("The length-unit oracle requires the fixed-coordinate vector fixture")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    expected["length_unit"] = "\u00b5m"
    return expected


def verify_length_unit_export_xml(original, exported):
    expected = minidom.parse(str(original))
    root = expected.documentElement
    properties = [node for node in root.childNodes if node.nodeType == node.ELEMENT_NODE
                  and node.tagName == "property" and node.getAttribute("name") == "length_unit"]
    if properties:
        raise ValueError("Pinned fixture must have no explicit length unit")
    unit = expected.createElement("property")
    unit.setAttribute("name", "length_unit")
    unit.setAttribute("type", "string")
    unit.appendChild(expected.createCDATASection("\u00b5m"))
    root.appendChild(unit)
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested length unit")


def calibration_roundtrip_expected(case):
    if case["id"] != "independent-vectors":
        raise ValueError("The calibration oracle requires the fixed-coordinate vector fixture")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    # Independent 90-degree inverse transform: X = (120-y)/8, Y = (100-x)/4.
    for sample in expected["coordinate_samples"]:
        sample["world"] = [12.84375, 21.625]
    return expected


def verify_calibration_export_xml(original, exported):
    expected = minidom.parse(str(original))
    frames = [node for node in expected.getElementsByTagName("object")
              if node.getAttribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem$FrameData"]
    if len(frames) != 1:
        raise ValueError("Expected one fixed calibration frame")
    for name, value in [(name, java_double(number)) for name, number in
                        [("xorigin", 100), ("yorigin", 120), ("angle", 90), ("xscale", 4), ("yscale", 8)]]:
        properties = [node for node in frames[0].childNodes if node.nodeType == node.ELEMENT_NODE
                      and node.tagName == "property" and node.getAttribute("name") == name]
        if len(properties) != 1 or properties[0].getAttribute("type") != "double":
            raise ValueError("Unexpected pinned calibration property: " + name)
        nodes = [node for node in properties[0].childNodes if node.nodeType in {node.TEXT_NODE, node.CDATA_SECTION_NODE}]
        if len(nodes) != 1:
            raise ValueError("Unexpected pinned calibration scalar: " + name)
        nodes[0].data = value
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested calibration")


def keyed_calibration_expected(edited=False):
    keys = [{"frame": 0, "origin": [320.0, 240.0], "angle": 0.0, "scale": [2.0, 4.0]},
            {"frame": 4, "origin": [100.0, 120.0], "angle": 90.0, "scale": [4.0, 8.0]},
            {"frame": 9, "origin": [110.0, 130.0], "angle": -90.0, "scale": [2.0, 8.0]}]
    if edited:
        keys[1] = {"frame": 4, "origin": [120.0, 140.0], "angle": 0.0, "scale": [8.0, 16.0]}
        keys.insert(1, {"frame": 3, "origin": [80.0, 90.0], "angle": -90.0, "scale": [2.0, 4.0]})
    return {"fixed_origin": False, "fixed_angle": False, "fixed_scale": False, "keys": keys}


def keyed_calibration_roundtrip_expected(case):
    if case["id"] != "keyed-calibration":
        raise ValueError("The keyed-calibration oracle requires its Java-authored fixture")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    # New key 3: origin (80,90), -90 degrees, scales (2,4).
    # Edited key 4: origin (120,140), 0 degrees, scales (8,16). Keys 0/9 remain unchanged.
    world = {0: [-155.0, 55.0], 3: [-17.5, -35.0], 4: [-13.75, 7.5], 8: [-13.75, 7.5], 9: [-13.75, -50.0]}
    for sample in expected["point_masses"][0]["samples"]:
        sample["world"] = world[sample["frame"]]
    return expected


def verify_keyed_calibration_export_xml(original, exported):
    expected = minidom.parse(str(original))
    frames = [node for node in expected.getElementsByTagName("object")
              if node.getAttribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem$FrameData"]
    if [node.parentNode.getAttribute("name") for node in frames] != ["[0]", "[4]", "[9]"]:
        raise ValueError("Expected pinned calibration keys 0/4/9")
    added = frames[0].parentNode.cloneNode(True)
    added.setAttribute("name", "[3]")
    frames[1].parentNode.parentNode.insertBefore(added, frames[1].parentNode)
    new_frame = added.getElementsByTagName("object")[0]
    for frame, values in [(new_frame, [80, 90, -90, 2, 4]), (frames[1], [120, 140, 0, 8, 16])]:
        for name, value in zip(["xorigin", "yorigin", "angle", "xscale", "yscale"], values):
            properties = [node for node in frame.childNodes if node.nodeType == node.ELEMENT_NODE
                          and node.tagName == "property" and node.getAttribute("name") == name]
            if len(properties) != 1 or properties[0].getAttribute("type") != "double":
                raise ValueError("Unexpected pinned keyed scalar: " + name)
            nodes = [node for node in properties[0].childNodes if node.nodeType in {node.TEXT_NODE, node.CDATA_SECTION_NODE}]
            if len(nodes) != 1:
                raise ValueError("Unexpected pinned keyed scalar contents: " + name)
            nodes[0].data = java_double(value)
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested calibration keys")


def mixed_calibration_expected(case, edited=False, *, normalized=True):
    flags = case.get("mixed_calibration_flags")
    if type(flags) is not int or flags not in range(1, 7) or case["id"] != f"mixed-calibration-{flags}":
        raise ValueError("Expected an independently authored partly fixed calibration case")
    # Explicit outcomes of the three declared native commands. Rows are frame, origin x/y,
    # angle degrees and scale x/y. Fixed origin/angle records can differ before Java normalizes them.
    edited_rows = {
        1: [[0, 110, 130, 0, 2, 4], [3, 80, 90, -90, 2, 4], [4, 120, 140, 0, 8, 16], [9, 80, 90, -90, 2, 8]],
        2: [[0, 320, 240, -90, 2, 4], [3, 80, 90, -90, 2, 4], [4, 120, 140, 0, 8, 16], [9, 110, 130, -90, 2, 8]],
        3: [[0, 110, 130, -90, 2, 4], [4, 110, 130, -90, 8, 16], [9, 80, 90, -90, 2, 8]],
        4: [[0, 320, 240, 0, 8, 32], [3, 80, 90, -90, 8, 32], [4, 120, 140, 0, 8, 32], [9, 110, 130, -90, 8, 32]],
        5: [[0, 110, 130, 0, 8, 32], [3, 80, 90, -90, 8, 32], [4, 120, 140, 0, 8, 32], [9, 80, 90, -90, 8, 32]],
        6: [[0, 320, 240, -90, 8, 32], [3, 80, 90, -90, 8, 32], [4, 120, 140, 0, 8, 32], [9, 110, 130, -90, 8, 32]]}
    rows = edited_rows[flags] if edited else [[0, 320, 240, 0, 2, 4], [4, 100, 120, 90, 4, 8], [9, 110, 130, -90, 2, 8]]
    keys = [{"frame": row[0], "origin": [float(v) for v in row[1:3]], "angle": float(row[3]),
             "scale": [float(v) for v in row[4:6]]} for row in rows]
    if normalized or not edited:
        origin, angle = keys[-1]["origin"], keys[-1]["angle"]
        # Public authoring setters apply the last requested scale everywhere; loading fixed scale
        # subsequently uses the first serialized key. Authored keys already agree on that value.
        scale = keys[0]["scale"] if edited else [2.0, 8.0]
        for key in keys:
            if flags & 1:
                key["origin"] = list(origin)
            if flags & 2:
                key["angle"] = angle
            if flags & 4:
                key["scale"] = list(scale)
    return {"fixed_origin": bool(flags & 1), "fixed_angle": bool(flags & 2), "fixed_scale": bool(flags & 4), "keys": keys}


def mixed_calibration_roundtrip_expected(case):
    calibration = mixed_calibration_expected(case, True)
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    for sample in expected["point_masses"][0]["samples"] + expected["coordinate_samples"]:
        key = next(key for key in reversed(calibration["keys"]) if key["frame"] <= sample["frame"])
        u, v = [(point - origin) / scale for point, origin, scale in zip(sample["image"], key["origin"], key["scale"])]
        # Exact quadrant arithmetic, independent of the native trigonometric transform.
        sample["world"] = {0.0: [u, -v], 90.0: [-v, -u], -90.0: [v, u]}[key["angle"]]
    return expected


def verify_mixed_calibration_export_xml(case, original, exported):
    expected = minidom.parse(str(original))
    frames = {int(node.parentNode.getAttribute("name")[1:-1]): node for node in expected.getElementsByTagName("object")
              if node.getAttribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem$FrameData"}
    if sorted(frames) != [0, 4, 9]:
        raise ValueError("Expected pinned partly fixed keys 0/4/9")
    for key in mixed_calibration_expected(case, True, normalized=False)["keys"]:
        number = key["frame"]
        added_key = number not in frames
        if added_key:
            added = frames[0].parentNode.cloneNode(True)
            added.setAttribute("name", f"[{number}]")
            frames[4].parentNode.parentNode.insertBefore(added, frames[4].parentNode)
            frames[number] = added.getElementsByTagName("object")[0]
        values = key["origin"] + [key["angle"]] + key["scale"]
        scalar_nodes = []
        for name, value in zip(["xorigin", "yorigin", "angle", "xscale", "yscale"], values):
            properties = [node for node in frames[number].childNodes if node.nodeType == node.ELEMENT_NODE
                          and node.tagName == "property" and node.getAttribute("name") == name]
            if len(properties) != 1 or properties[0].getAttribute("type") != "double":
                raise ValueError("Unexpected partly fixed scalar: " + name)
            nodes = [node for node in properties[0].childNodes if node.nodeType in {node.TEXT_NODE, node.CDATA_SECTION_NODE}]
            if len(nodes) != 1:
                raise ValueError("Unexpected partly fixed scalar contents: " + name)
            scalar_nodes.append(nodes[0])
        if added_key or [float(node.data) for node in scalar_nodes] != values:
            for node, value in zip(scalar_nodes, values):
                node.data = java_double(value)
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond partly fixed calibration edits")


def clip_timing_expected(ranged=False):
    return {"frame_duration_ms": 250.0,
            "time_samples": [{"frame": 2 if ranged else 0, "time_ms": -500.0},
                             {"frame": 8 if ranged else 9, "time_ms": 1000.0 if ranged else 1750.0}]}


def clip_timing_roundtrip_expected(case, ranged=False):
    if case["id"] != "protractor":
        raise ValueError("The timing oracle requires the pinned protractor case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    expected["clip"]["start_ms"] = -500.0
    if ranged:
        expected["clip"].update(first_frame=2, step_size=2, step_count=4)
        for sample, frame in zip(expected["coordinate_samples"], [2, 8]):
            sample["frame"] = frame  # This fixture's coordinate system is fixed.
    return expected


def verify_clip_timing_export_xml(original, exported, ranged=False):
    expected = minidom.parse(str(original))
    changes = [("VideoClip", "starttime", "double", java_double(-500)), ("StepperClipControl", "delta_t", "double", java_double(250))]
    if ranged:
        changes += [("VideoClip", "startframe", "int", "2"), ("VideoClip", "stepsize", "int", "2"),
                    ("VideoClip", "stepcount", "int", "4")]
    for kind, name, scalar_type, value in changes:
        objects = [node for node in expected.getElementsByTagName("object")
                   if node.getAttribute("class") == "org.opensourcephysics.media.core." + kind]
        if len(objects) != 1:
            raise ValueError("Expected one timing object: " + kind)
        properties = [node for node in objects[0].childNodes if node.nodeType == node.ELEMENT_NODE
                      and node.tagName == "property" and node.getAttribute("name") == name]
        if len(properties) != 1 or properties[0].getAttribute("type") != scalar_type:
            raise ValueError("Unexpected timing scalar: " + name)
        cells = [node for node in properties[0].childNodes if node.nodeType in {node.TEXT_NODE, node.CDATA_SECTION_NODE}]
        if len(cells) != 1:
            raise ValueError("Unexpected timing text: " + name)
        cells[0].data = value
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested clip timing")


def check_native_clip_timing(probe, project, output, tolerance, ranged):
    run([str(Path(probe).resolve()), "--timing", str(project), str(output)])
    compare(clip_timing_expected(ranged), json.loads(output.read_text(encoding="utf-8")), tolerance, "native_clip_timing")


def clip_roundtrip_expected(case, boundary=False):
    if case["id"] != "independent-vectors":
        raise ValueError("The clip oracle requires the fixed-coordinate vector fixture")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    expected["clip"].update(first_frame=500000 if boundary else 2, step_size=1 if boundary else 2, step_count=1 if boundary else 4)
    if boundary:
        expected["vectors"] = []  # Independently authored axes-only boundary fixture.
    # Fixed calibration: changing the sampled frame leaves both independent coordinate values intact.
    for sample, frame in zip(expected["coordinate_samples"], [500000, 500000] if boundary else [2, 8]):
        sample["frame"] = frame
    return expected


def verify_clip_export_xml(original, exported, boundary=False):
    expected = minidom.parse(str(original))
    clips = [node for node in expected.getElementsByTagName("object")
             if node.getAttribute("class") == "org.opensourcephysics.media.core.VideoClip"]
    if len(clips) != 1:
        raise ValueError("Expected one pinned clip")
    values = [("startframe", "500000"), ("stepsize", "1"), ("stepcount", "1")] if boundary else [("startframe", "2"), ("stepsize", "2"), ("stepcount", "4")]
    for name, value in values:
        properties = [node for node in clips[0].childNodes if node.nodeType == node.ELEMENT_NODE
                      and node.tagName == "property" and node.getAttribute("name") == name]
        if len(properties) != 1 or properties[0].getAttribute("type") != "int":
            raise ValueError("Unexpected pinned clip property: " + name)
        nodes = [node for node in properties[0].childNodes if node.nodeType in {node.TEXT_NODE, node.CDATA_SECTION_NODE}]
        if len(nodes) != 1:
            raise ValueError("Unexpected pinned clip scalar: " + name)
        nodes[0].data = value
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested clip range")


def snapping_roundtrip_expected(case, mode):
    if case["id"] != "independent-vectors" or mode not in {"snap-origin", "snap-vector"}:
        raise ValueError("Unknown rejected snapping case")
    expected = copy.deepcopy(json.loads(expected_path(case).read_text(encoding="utf-8"))["project"])
    if mode == "snap-origin":
        expected["vectors"][0]["samples"][0]["tail"] = [320.0, 240.0]
    else:
        expected["vectors"][1]["samples"][0]["tail"] = [13.0, 16.0]
    return expected


def java_double(value):
    """Double.toString as OSP's writer formats it (JDK 19+: the shortest decimal that rounds back, plain for
    1e-3 <= |x| < 1e7 and computerized scientific notation otherwise). MoTrack's standard writer uses it for every
    double it writes, so the expected XML of an edit uses it too."""
    import decimal
    value = float(value)
    if math.isnan(value):
        return "NaN"
    if math.isinf(value):
        return "Infinity" if value > 0 else "-Infinity"
    if value == 0:
        return "-0.0" if math.copysign(1.0, value) < 0 else "0.0"
    sign = "-" if value < 0 else ""
    digits, exponent = decimal.Decimal(repr(abs(value))).normalize().as_tuple()[1:]
    text = "".join(str(digit) for digit in digits)
    position = len(text) + exponent  # Digits before the decimal point.
    if 1e-3 <= abs(value) < 1e7:
        if position <= 0:
            return sign + "0." + "0" * -position + text
        if position >= len(text):
            return sign + text + "0" * (position - len(text)) + ".0"
        return sign + text[:position] + "." + text[position:]
    return sign + text[0] + "." + (text[1:] or "0") + "E" + str(position - 1)


def canonical_tracker_xml(document):
    def normalize(node, preserve=False):
        if node.nodeType == node.ELEMENT_NODE:
            space = node.getAttribute("xml:space")
            preserve = True if space == "preserve" else False if space == "default" else preserve
            structural = node.tagName == "object" or (node.tagName == "property" and
                         node.getAttribute("type") in {"object", "array", "collection"})
            children = list(node.childNodes)
            if (structural and not preserve
                    and all(child.nodeType != child.ELEMENT_NODE or child.tagName in {"object", "property"} for child in children)
                    and not any(child.nodeType == child.CDATA_SECTION_NODE or
                                (child.nodeType == child.TEXT_NODE and child.data.strip(" \t\r\n")) for child in children)):
                for child in children:
                    if child.nodeType == child.TEXT_NODE:
                        node.removeChild(child)
        for child in list(node.childNodes):
            normalize(child, preserve)
    normalize(document)
    # Compare decoded DOM values directly: serializing and reparsing would normalize a decoded
    # character-reference CR to LF and hide a change to an untouched scalar or attribute.
    def snapshot(node):
        attributes = tuple(sorted(node.attributes.items())) if node.attributes else ()
        children = []
        for child in node.childNodes:
            if child.nodeType in {child.TEXT_NODE, child.CDATA_SECTION_NODE}:
                if children and children[-1][0] == "text":
                    children[-1] = ("text", children[-1][1] + child.data)
                else:
                    children.append(("text", child.data))
            else:
                children.append(snapshot(child))
        return (node.nodeType, node.nodeName, node.nodeValue, attributes, tuple(children))
    return snapshot(document)


def verify_mass_export_xml(original, exported):
    # Compare the complete source tree, including unknown objects/comments/PIs, allowing only one mass edit.
    def elements(node, tag):
        return [child for child in node.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == tag]
    expected = minidom.parse(str(original))
    tracks = [prop for prop in elements(expected.documentElement, "property") if prop.getAttribute("name") == "tracks"]
    masses = [obj for track in tracks for prop in elements(track, "property") for obj in elements(prop, "object")
              if obj.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.PointMass"]
    if not masses:
        raise ValueError("Mass round trip needs a source point mass")
    mass = [prop for prop in elements(masses[0], "property") if prop.getAttribute("name") == "mass"]
    if len(mass) != 1:
        raise ValueError("Mass reference requires an explicit original mass")
    values = [node for node in mass[0].childNodes if node.nodeType in {node.TEXT_NODE, node.CDATA_SECTION_NODE}]
    if not values:
        raise ValueError("Mass reference has no numeric value")
    value = java_double(float("".join(node.data for node in values)) * 2.5)
    values[0].data = value
    for node in values[1:]:
        mass[0].removeChild(node)
    canonical = canonical_tracker_xml(expected)
    expected.unlink()
    actual = minidom.parse(str(exported))
    actual_canonical = canonical_tracker_xml(actual)
    actual.unlink()
    if canonical != actual_canonical:
        raise ValueError("Standard export changed content beyond the requested mass edit")


def verify_rename_export_xml(original, exported, mode):
    plot = mode in {"plot-owner-name", "plot-guest-name"}
    center = mode == "center-member-name"
    attachment = mode in {"attachment-first-name", "attachment-last-name"}
    model = mode == "model-booster-name"
    if mode not in {"name", "unselected-name", "vector-name"} and not plot and not center and not attachment and not model:
        raise ValueError("Unexpected rename mode")
    expected = minidom.parse(str(original))
    def props(node):
        return [child for child in node.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == "property"]
    def prop(node, name):
        found = [child for child in props(node) if child.getAttribute("name") == name]
        if len(found) != 1:
            raise ValueError("Unexpected rename field: " + name)
        return found[0]
    def value(node):
        if node.getAttribute("type") != "string" or any(child.nodeType == child.ELEMENT_NODE for child in node.childNodes):
            raise ValueError("Unexpected rename string")
        return "".join(child.data for child in node.childNodes if child.nodeType in {child.TEXT_NODE, child.CDATA_SECTION_NODE})
    def set_value(node, text):
        cells = [child for child in node.childNodes if child.nodeType in {child.TEXT_NODE, child.CDATA_SECTION_NODE}]
        if not cells:
            raise ValueError("Missing rename string")
        cells[0].data = text
        for cell in cells[1:]:
            node.removeChild(cell)
    root = expected.documentElement
    kind, prefix = ("Vector", "vector") if mode == "vector-name" else ("PointMass", "mass")
    tracks = [obj for obj in prop(root, "tracks").getElementsByTagName("object")
              if obj.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker." + kind]
    original_names = ["mass A"] if model else [prefix + " A", prefix + " B"] + (["mass C"] if plot else [])
    if [value(prop(track, "name")) for track in tracks] != original_names:
        raise ValueError("Unexpected rename source tracks")
    index = 0 if mode in {"unselected-name", "plot-owner-name", "center-member-name", "attachment-first-name", "model-booster-name"} else 1
    old = prefix + (" B" if index == 1 else " A")
    set_value(prop(tracks[index], "name"), RENAME_LABEL)
    selected = prop(root, "selectedtrack")
    if value(selected) == old:
        set_value(selected, RENAME_LABEL)
    if not (plot or center or attachment or model) or any(child.getAttribute("name") == "selected_track_views" for child in props(root)):
        packed = prop(root, "selected_track_views")
        pairs = [pair.split(",") for pair in value(packed).split(";")]
        if len(pairs) != 4 or any(len(pair) != 2 for pair in pairs):
            raise ValueError("Unexpected packed rename selections")
        set_value(packed, ";".join(",".join(RENAME_LABEL if name == old else name for name in pair) for pair in pairs))
    if plot:
        for obj in expected.getElementsByTagName("object"):
            kind = obj.getAttribute("class")
            if kind in {"org.opensourcephysics.cabrillo.tracker.PlotTView", "org.opensourcephysics.cabrillo.tracker.PlotTrackView"}:
                target = prop(obj, "selected_track" if kind.endswith(".PlotTView") else "track")
                if value(target) == old:
                    set_value(target, RENAME_LABEL)
            elif kind == "org.opensourcephysics.cabrillo.tracker.TrackPlottingPanel":
                for group in props(obj):
                    if group.getAttribute("name") == "guests":
                        for guest in props(group):
                            if value(guest) == old:
                                set_value(guest, RENAME_LABEL)
    if center:
        for dependent in expected.getElementsByTagName("object"):
            if dependent.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.CenterOfMass":
                for member in props(prop(dependent, "masses")):
                    if value(member) == old:
                        set_value(member, RENAME_LABEL)
    for table in expected.getElementsByTagName("object"):
        if model and table.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.DynamicParticle":
            for field in props(table):
                if field.getAttribute("name") == "booster" and value(field) == old:
                    set_value(field, RENAME_LABEL)
        if attachment and table.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.TapeMeasure":
            for slot in props(prop(table, "attachments")):
                if slot.getAttribute("type") == "string" and value(slot) == old:
                    set_value(slot, RENAME_LABEL)
        if table.getAttribute("class") != "org.opensourcephysics.cabrillo.tracker.TableTView":
            continue
        selected = prop(table, "selected_track")
        if value(selected) == old:
            set_value(selected, RENAME_LABEL)
        for group in props(table):
            if group.getAttribute("name") not in {"track_columns", "column_formats"}:
                continue
            for row in group.getElementsByTagName("property"):
                if row.getAttribute("type") == "array" and row.getAttribute("class") == "[Ljava.lang.String;":
                    owner = prop(row, "[0]")
                    if value(owner) == old:
                        set_value(owner, RENAME_LABEL)
    actual = minidom.parse(str(exported))
    same = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not same:
        raise ValueError("Standard export changed content beyond the requested rename and references")


def verify_mark_export_xml(original, exported, expected_project, mode="marks"):
    expected = minidom.parse(str(original))
    def elements(node, tag):
        return [child for child in node.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == tag]
    def prop(node, name):
        found = [child for child in elements(node, "property") if child.getAttribute("name") == name]
        if len(found) != 1:
            raise ValueError("Unexpected pinned mark fixture structure: " + name)
        return found[0]
    def text(node, value):
        values = [child for child in node.childNodes if child.nodeType in {child.TEXT_NODE, child.CDATA_SECTION_NODE}]
        if len(values) != 1:
            raise ValueError("Unexpected pinned mark scalar")
        values[0].data = value
    def add_frame(data, number, x, y):
        entry = expected.createElement("property")
        entry.setAttribute("name", f"[{number}]")
        entry.setAttribute("type", "object")
        obj = expected.createElement("object")
        obj.setAttribute("class", "org.opensourcephysics.cabrillo.tracker.PointMass$FrameData")
        entry.appendChild(obj)
        for name, value in [("x", x), ("y", y)]:
            coordinate = expected.createElement("property")
            coordinate.setAttribute("name", name)
            coordinate.setAttribute("type", "double")
            coordinate.appendChild(expected.createTextNode(java_double(value)))
            obj.appendChild(coordinate)
        following = next((child for child in elements(data, "property")
                          if int(child.getAttribute("name")[1:-1]) > number), None)
        data.insertBefore(entry, following)
    masses = [obj for item in elements(prop(expected.documentElement, "tracks"), "property")
              for obj in elements(item, "object") if obj.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.PointMass"]
    if len(masses) != 2:
        raise ValueError("Expected the pinned two-mass experiment")
    first, second = [prop(mass, "framedata") for mass in masses]
    if mode == "empty-marks":
        first.removeChild(prop(first, "[0]"))
        keys = prop(masses[0], "keyFrames")
        keys.removeChild(prop(keys, "array"))
    elif mode == "first-mark":
        add_frame(first, 4, 11.25, 22.5)
        keys = prop(masses[0], "keyFrames")
        value = expected.createElement("property")
        value.setAttribute("name", "array")
        value.setAttribute("type", "string")
        value.appendChild(expected.createTextNode("{4}"))
        keys.appendChild(value)
    else:
        original_mark = elements(prop(first, "[0]"), "object")[0]
        image = expected_project["point_masses"][0]["samples"][0]["image"]
        text(prop(original_mark, "x"), java_double(image[0]))
        text(prop(original_mark, "y"), java_double(image[1]))
        add_frame(first, 3, 100.125, 200.5)
        second.removeChild(prop(second, "[0]"))
        add_frame(second, 7, 50.0, 60.0)
        text(prop(prop(masses[0], "keyFrames"), "array"), "{0,3}")
        text(prop(prop(masses[1], "keyFrames"), "array"), "{7}")
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested mark edits")


def verify_vector_export_xml(original, exported, expected_project):
    expected = minidom.parse(str(original))
    def elements(node, tag):
        return [child for child in node.childNodes if child.nodeType == child.ELEMENT_NODE and child.tagName == tag]
    def prop(node, name):
        matches = [child for child in elements(node, "property") if child.getAttribute("name") == name]
        if len(matches) != 1:
            raise ValueError("Unexpected pinned vector fixture structure: " + name)
        return matches[0]
    vectors = [obj for item in elements(prop(expected.documentElement, "tracks"), "property")
               for obj in elements(item, "object") if obj.getAttribute("class") == "org.opensourcephysics.cabrillo.tracker.Vector"]
    if len(vectors) != 2 or len(expected_project["vectors"]) != 2:
        raise ValueError("Expected the pinned two-vector experiment")
    for vector, desired in zip(vectors, expected_project["vectors"]):
        data = prop(vector, "framedata")
        entries = {int(item.getAttribute("name")[1:-1]): item for item in elements(data, "property")}
        samples = {sample["frame"]: sample for sample in desired["samples"]}
        for frame, item in list(entries.items()):
            if frame not in samples and elements(item, "object"):
                data.removeChild(item)
                del entries[frame]
        for frame, sample in samples.items():
            item = entries.get(frame)
            if item is None:
                item = expected.createElement("property")
                item.setAttribute("name", f"[{frame}]")
                item.setAttribute("type", "object")
                following = next((entry for number, entry in sorted(entries.items()) if number > frame), None)
                data.insertBefore(item, following)
                entries[frame] = item
            objects = elements(item, "object")
            if objects:
                obj = objects[0]
                old_values = []
                for name in ["xtail", "ytail", "xcomponent", "ycomponent"]:
                    old_values.append(float("".join(child.data for child in prop(obj, name).childNodes
                                                    if child.nodeType in {child.TEXT_NODE, child.CDATA_SECTION_NODE})))
                if old_values == sample["tail"] + sample["components"]:
                    continue
            else:
                for child in list(item.childNodes):
                    item.removeChild(child)
                obj = expected.createElement("object")
                obj.setAttribute("class", "org.opensourcephysics.cabrillo.tracker.Vector$FrameData")
                item.appendChild(obj)
            for name, value in zip(["xtail", "ytail", "xcomponent", "ycomponent"], sample["tail"] + sample["components"]):
                matches = [child for child in elements(obj, "property") if child.getAttribute("name") == name]
                if matches:
                    coordinate = matches[0]
                    values = [child for child in coordinate.childNodes if child.nodeType in {child.TEXT_NODE, child.CDATA_SECTION_NODE}]
                    if len(values) != 1:
                        raise ValueError("Unexpected pinned vector scalar")
                    values[0].data = java_double(value)
                else:
                    coordinate = expected.createElement("property")
                    coordinate.setAttribute("name", name)
                    coordinate.setAttribute("type", "double")
                    coordinate.appendChild(expected.createTextNode(java_double(value)))
                    obj.appendChild(coordinate)
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested vector edits")


def check_archive_tabs(case, manifest, cache, java, classes, jar, output, display, probe=None, mode="archive-images"):
    """Open the actual archive once, save every loaded tab, then reopen each in a fresh JVM."""
    by_id = {item["id"]: item for item in manifest["cases"]}
    tabs = case["session_archive_tabs"]
    members = [tab["member"] for tab in tabs]
    templates = [copy.deepcopy(by_id[tab["case"]]) for tab in tabs]
    movie = templates[0].get("session_media", {}).get("class") == "org.opensourcephysics.media.xuggle.XuggleVideo"
    dependencies = sorted({name for template in [case, *templates] for name in template.get("reference_dependencies", [])})
    classpath = java_classpath(manifest, cache, classes=classes, dependencies=dependencies)
    source = local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"]
    with zipfile.ZipFile(source) as archive:
        projects = [entry.filename for entry in archive.infolist() if entry.filename.lower().endswith(".trk")]
        if sorted(projects) != sorted(members):
            raise ValueError("Archive experiments do not match the independent tab inventory")
        for member, template in zip(members, templates):
            if archive.getinfo(member).file_size > MAX_XML_BYTES or archive.read(member) != project_path(template, cache).read_bytes():
                raise ValueError("Archive tab differs from its pinned independent source")
    expected = [json.loads(expected_path(template).read_text(encoding="utf-8"))["project"] for template in templates]
    grouped = mode in {"archive-group-forward", "archive-group-reverse"}
    if grouped and (case["id"] not in {"multitab-images", "archived-movie"} or len(templates) != 2
                    or templates[1]["id"] != "secondary-images"):
        raise ValueError("Grouped archive edits require both independent pinned experiments")
    if probe:
        exported = output / "motrack-export.trz"
        vector_scenario = mode.split("-", 4)[4] if mode.startswith("media-vectors-") else None
        export_mode = mode if movie or vector_scenario or grouped else "multitab-archive-images"
        calibrated = movie and mode.endswith("-movie-calibration")
        keyed = movie and mode.endswith("-movie-keyed-calibration")
        authoring = mode.split("-movie-tracks-", 1)[1] if "-movie-tracks-" in mode else None
        clip_mode = "single" if mode == "archive-movie-clip-single" else "stride" if mode == "archive-movie-clip" else None
        run([str(Path(probe).resolve()), "--export-" + export_mode, str(source), str(exported)])
        verify_archive_media_export(source, exported, output / "exported-member.trk", member_count=9 if movie else 8,
                                    clip_mode=clip_mode, movie=movie, calibrated=calibrated, keyed_case=templates[0] if keyed else None,
                                    authoring=authoring, vector_scenario=vector_scenario, vector_case=templates[0], grouped=grouped)
        if grouped:
            expected[1] = secondary_archive_roundtrip_expected(templates[1])
        if vector_scenario:
            expected[0], templates[0] = media_vector_roundtrip_expected(templates[0], vector_scenario)
        elif authoring:
            expected[0], templates[0] = movie_tracks_roundtrip_expected(templates[0], authoring)
        elif keyed:
            expected[0], templates[0] = keyed_movie_roundtrip_expected(templates[0])
        else:
            expected[0] = linked_media_roundtrip_expected(case)
        if clip_mode:
            expected[0], templates[0] = linked_media_clip_expected(templates[0], single=clip_mode == "single")
        if calibrated:
            expected[0], templates[0] = linked_movie_calibration_expected(templates[0])
        source = exported
    else:
        # A fixture's neighboring movie must not stand in for extraction from the archive.
        # Tracker may extract supported movies beside their container; keep that write isolated.
        isolated = output / "input" / source.name
        isolated.parent.mkdir()
        shutil.copyfile(source, isolated)
        verify_file(isolated, case)
        source = isolated
    metadata = output / "metadata"
    metadata.mkdir()
    with zipfile.ZipFile(source) as archive:
        for member in members:
            if archive.getinfo(member).file_size > MAX_XML_BYTES:
                raise ValueError("Archive member exceeds the metadata limit")
            (metadata / member).write_bytes(archive.read(member))

    def execute(input_file, destination, stage, metadata_args=()):
        home = destination / "private-home"
        home.mkdir(parents=True)
        try:
            run([str(java), f"-Duser.home={home}", f"-Djava.io.tmpdir={home}", "-Duser.language=en", "-Duser.country=US", "-Xmx256m",
                 "-cp", classpath, "TrackerSessionReference",
                 str(input_file), str(destination), stage, *map(str, metadata_args)], display=display)
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            raise ValueError(f"{case['id']}/{stage}: multi-tab reference failed: {error}") from error
        report = json.loads((destination / f"session-{stage}.json").read_text(encoding="utf-8"))
        if (report.get("reference_version") != manifest["reference"]["version"]
                or report.get("verification_level") != "tracker_session_" + stage
                or report.get("view_preparation") != "expand_visit_custom_tables_restore_selection"
                or report.get("cache_policy") != "private_persistent_osp_cache"
                or type(report.get("loader_callbacks")) is not int or report["loader_callbacks"] < 1):
            raise ValueError("Unexpected multi-tab Tracker session stage")
        return report

    saved = execute(source, output, "save-tabs", [metadata / member for member in members])
    if (not isinstance(saved.get("tabs"), list) or any(not isinstance(tab, dict) for tab in saved["tabs"])
            or [tab.get("member") for tab in saved["tabs"]] != members):
        raise ValueError("Tracker did not report each archive experiment exactly once")
    reports = []
    for i, (member, template, desired, before) in enumerate(zip(members, templates, expected, saved["tabs"])):
        file = output / f"tracker-resaved-{i}.trk"
        def inventory():
            if template.get("session_media", {}).get("class") == "org.opensourcephysics.media.xuggle.XuggleVideo":
                return verify_saved_movie_asset(file, template["assets"][0], allow_archive=True)
            return verify_saved_image_inventory(file, template["assets"])
        saved_inventory = inventory()
        reopened = execute(file, output / f"reopen-{i}", "reopen")
        report = {"reference_version": saved["reference_version"], "verification_level": "tracker_session_resave",
                  "view_preparation": saved["view_preparation"], "before": before["loaded"],
                  "after_save": before["after_save"], "after": reopened["loaded"],
                  "cache_policy": {"save": saved["cache_policy"], "reopen": reopened["cache_policy"]},
                  "saved_media_inventories": {"save": saved_inventory, "reopen": inventory()}}
        try:
            keys = template.get("session_key_frames", [[0, 1, 2]])
            validate_session(template, report, manifest, desired, keys if probe and (i == 0 or grouped) else None)
        except ValueError as error:
            raise ValueError(f"{case['id']}/{member}: {error}\nTracker tab snapshots (bounded): {json.dumps(report)[:24000]}") from error
        if probe:
            native = output / f"native-reimport-{i}.json"
            run([str(Path(probe).resolve()), str(file), str(native)])
            desired["source_version"] = manifest["reference"]["version"]
            desired.update(session_units(template, desired))
            compare(desired, json.loads(native.read_text(encoding="utf-8")), manifest["tolerance"], member + ".reimport")
        reports.append({"member": member, "tracker_session": report, "expected": desired})
    level = "motrack_archive_tabs_roundtrip" if probe else "tracker_archive_tabs_resave"
    label = case["id"] + ("-" + mode if probe and (movie or grouped or mode.startswith("media-vectors-")) else "")
    (cache / "reports" / f"{label}-{level}.json").write_text(
        json.dumps({"verification_level": level, "tabs": reports}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{label}: passed ({level}; all loaded tabs saved and individually reopened)")


def check_sessions(manifest, cache, java, probe=None):
    if sys.platform != "linux":
        raise ValueError("Session checks currently require Linux with Xvfb")
    modes = [("linked-movie", "native_linked_movie_roundtrip"),
             ("archive-group-forward", "native_archive_group_roundtrip"),
             ("archive-group-reverse", "native_archive_group_roundtrip"),
             ("linked-movie-keyed-calibration", "native_keyed_movie_roundtrip"),
             ("linked-movie-calibration", "native_linked_movie_calibration_roundtrip"),
             ("linked-movie-clip", "native_linked_movie_clip_roundtrip"),
             ("linked-movie-clip-single", "native_linked_movie_clip_roundtrip"),
             ("archive-movie", "native_archive_movie_roundtrip"),
             ("archive-movie-clip", "native_archive_movie_roundtrip"),
             ("archive-movie-clip-single", "native_archive_movie_roundtrip"),
             ("paired-archive-movie", "native_archive_movie_roundtrip"),
             ("archive-movie-calibration", "native_archive_movie_calibration_roundtrip"),
             ("paired-archive-movie-calibration", "native_archive_movie_calibration_roundtrip"),
             ("archive-movie-keyed-calibration", "native_keyed_movie_archive_roundtrip"),
             ("paired-archive-movie-keyed-calibration", "native_keyed_movie_archive_roundtrip"),
             ("archive-images", "native_archive_image_roundtrip"), ("linked-images", "native_linked_image_roundtrip"),
             ("linked-image-clip", "native_linked_image_clip_roundtrip"), ("linked-image-clip-single", "native_linked_image_clip_roundtrip"),
             ("archive-image-clip", "native_archive_image_clip_roundtrip"), ("packaged-image-clip", "native_linked_image_clip_roundtrip"),
             ("packaged-images", "native_image_package_roundtrip"),
             ("packaged-movie", "native_movie_package_roundtrip"),
             ("packaged-movie-calibration", "native_movie_package_calibration_roundtrip"),
             ("packaged-movie-keyed-calibration", "native_keyed_movie_package_roundtrip"),
             ("packaged-movie-clip", "native_movie_package_roundtrip"),
             ("packaged-movie-clip-single", "native_movie_package_roundtrip"),
             ("paired-archive-images", "native_paired_archive_roundtrip"), ("paired-linked-images", "native_paired_linked_roundtrip"),
             ("mass", "native_mass_roundtrip"), ("marks", "native_mark_roundtrip"),
             ("empty-marks", "native_mark_roundtrip"), ("first-mark", "native_mark_roundtrip"),
             ("vectors", "native_vector_roundtrip"), ("empty-vectors", "native_vector_roundtrip"),
             ("first-vector", "native_vector_roundtrip"), ("clip", "native_clip_roundtrip"), ("clip-boundary", "native_clip_roundtrip"),
             ("snap-origin", "native_vector_roundtrip"), ("snap-vector", "native_vector_roundtrip"),
             ("calibration", "native_calibration_roundtrip"), ("length-unit", "native_length_unit_roundtrip"),
             ("keyed-calibration", "native_keyed_calibration_roundtrip"),
             ("mixed-calibration", "native_mixed_calibration_roundtrip"),
             ("mass-unit", "native_mass_unit_roundtrip"), ("time-unit", "native_time_unit_roundtrip"),
             ("clip-timing", "native_clip_timing_roundtrip"), ("clip-timing-range", "native_clip_timing_roundtrip"),
             ("create-mass", "native_create_mass_roundtrip"), ("create-empty-mass", "native_create_mass_roundtrip"),
             ("create-vector", "native_create_vector_roundtrip"), ("create-empty-vector", "native_create_vector_roundtrip"),
             ("delete-mass", "native_delete_mass_roundtrip"), ("delete-vector", "native_delete_vector_roundtrip"),
             ("delete-all-vectors", "native_delete_vector_roundtrip"),
             ("delete-selected-mass", "native_delete_selected_mass_roundtrip"),
             ("delete-all-masses", "native_delete_selected_mass_roundtrip"),
             ("delete-plot-owner", "native_delete_plot_roundtrip"), ("delete-plot-guest", "native_delete_plot_roundtrip"),
             ("delete-first-plot-guest", "native_delete_plot_roundtrip"),
             ("delete-first-table-owner", "native_delete_table_roundtrip"), ("delete-selected-table-owner", "native_delete_table_roundtrip"),
             ("delete-last-table-owner", "native_delete_table_roundtrip"), ("delete-all-table-owners", "native_delete_table_roundtrip"),
             ("name", "native_name_roundtrip"), ("unselected-name", "native_name_roundtrip"),
             ("vector-name", "native_vector_name_roundtrip"),
             ("plot-owner-name", "native_plot_name_roundtrip"), ("plot-guest-name", "native_plot_name_roundtrip"),
             ("center-member-name", "native_center_name_roundtrip"),
             ("attachment-first-name", "native_attachment_name_roundtrip"),
             ("attachment-last-name", "native_attachment_name_roundtrip"),
             ("model-booster-name", "native_model_name_roundtrip")] if probe else [("session", "session_tracks")]
    if probe:
        modes.extend((prefix + "-movie-tracks-" + scenario, flag)
                     for prefix, flag in [("linked", "native_movie_tracks_roundtrip"),
                                          ("packaged", "native_movie_tracks_package_roundtrip"),
                                          ("archive", "native_movie_tracks_archive_roundtrip"),
                                          ("paired-archive", "native_movie_tracks_archive_roundtrip")]
                     for scenario in MOVIE_TRACK_SCENARIOS)
    cases = [(case, mode) for case in manifest["cases"] if "session_tracks" in case
             for mode, flag in modes if case.get(flag)]
    if probe:
        cases.extend((case, f"media-vectors-{case['media_vector_kind']}-{route}-{scenario}")
                     for case in manifest["cases"] if case.get("native_media_vector_roundtrip") or case.get("native_media_vector_archive_roundtrip")
                     for route in (["archive", "paired"] if case.get("native_media_vector_archive_roundtrip") else ["linked", "packaged"])
                     for scenario in media_vector_scenarios(case))
    if not cases:
        raise ValueError("No applicable Tracker session cases")
    for path, artifact in artifacts(manifest, cache):
        verify_file(path, artifact)
    java_path = Path(shutil.which(java) or java).resolve()
    javac = java_path.with_name("javac")
    reports = cache / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="session-", dir=cache) as temporary, private_display() as display:
        root = Path(temporary)
        classes = root / "classes"
        classes.mkdir()
        jar = cache / manifest["reference"]["file"]
        run([str(javac), "-cp", java_classpath(manifest, cache, dependencies=[
                 item["file"] for item in manifest["reference"].get("runtime_dependencies", [])]), "-d", str(classes),
             str(ROOT / "tools/reference/TrackerReference.java"),
             str(ROOT / "tools/reference/TrackerSessionReference.java"), str(ROOT / "tools/reference/VectorFixture.java"),
             str(ROOT / "tools/reference/TrackerSessionDiagnosticsTest.java")])
        diagnostic_home = root / "diagnostic-home"
        diagnostic_home.mkdir()
        run([str(java_path), "-Djava.awt.headless=true", f"-Duser.home={diagnostic_home}", "-Xmx256m",
             "-cp", java_classpath(manifest, cache, classes=classes), "TrackerSessionDiagnosticsTest"])
        for case, mode in cases:
            case_classpath = java_classpath(manifest, cache, classes=classes, dependencies=case.get("reference_dependencies", []))
            if probe and case.get("session_known_issue"):
                raise ValueError("Known Tracker defects cannot be used as native export acceptance")
            output = root / (case["id"] + ("-" + mode if probe else ""))
            java_home = output / "java-home"
            java_home.mkdir(parents=True)
            if case.get("session_archive_tabs"):
                check_archive_tabs(case, manifest, cache, java_path, classes, jar, output, display, probe, mode)
                continue
            project = project_path(case, cache)
            archive_input = (local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"]) if case.get("session_archive") else None
            if mode in {"name", "unselected-name", "vector-name", "plot-owner-name", "plot-guest-name", "center-member-name",
                        "attachment-first-name", "attachment-last-name", "model-booster-name"}:
                # Preserve the pinned original. Add a non-reference note to the temporary source,
                # keeping OSP's line-oriented XML layout, so blind text substitution cannot pass.
                text = project.read_text(encoding="utf-8")
                before, after = text.rsplit("</object>", 1)
                project = output / "rename-source.trk"
                prefix = "vector" if mode == "vector-name" else "mass"
                selection = ('<property name="selectedtrack" type="string">vector B</property>\n'
                             '<property name="selected_track_views" type="string">'
                             'vector B,null;null,vector B;null,null;null,null</property>\n') if mode == "vector-name" else ""
                project.write_text(before + selection + '<property name="description" type="string"><![CDATA['
                                   + prefix + ' A and ' + prefix + ' B are words in this note.]]></property>\n</object>' + after,
                                   encoding="utf-8")
            if mode == "first-mark":
                project = root / (case["id"] + "-empty-marks") / "tracker-resaved.trk"
            elif mode == "first-vector":
                project = root / (case["id"] + "-empty-vectors") / "tracker-resaved.trk"
            elif mode == "clip-boundary":
                project = output / "java-boundary-source.trk"
                run([str(java_path), "-Djava.awt.headless=true", f"-Duser.home={java_home}", "-Xmx256m",
                     "-cp", case_classpath, "VectorFixture", str(project), mode])
                case = {**case, "session_tracks": [track for track in case["session_tracks"]
                                                   if track["class"] == "org.opensourcephysics.cabrillo.tracker.CoordAxes"]}
            expected = None
            authoring = mode.split("-movie-tracks-", 1)[1] if "-movie-tracks-" in mode else None
            vector_scenario = mode.split("-", 4)[4] if mode.startswith("media-vectors-") else None
            vector_package = bool(vector_scenario and mode.split("-")[3] == "packaged")
            if vector_scenario:
                exported = output / ("motrack-export.trz" if vector_package else "motrack-export.trk")
                run([str(Path(probe).resolve()), "--export-" + mode, str(project), str(exported)])
                if vector_package:
                    selected = output / "exported-member.trk"
                    verify_media_package_export(project, exported, selected, case,
                                                movie=case["media_vector_kind"] == "movie", vector_scenario=vector_scenario)
                    if (output / "media é & vectors").exists():
                        raise ValueError("Vector package retained original external media")
                    archive_input, project = exported, selected
                else:
                    verify_linked_vector_export_xml(project, exported, case, vector_scenario)
                    project = exported
                expected, case = media_vector_roundtrip_expected(case, vector_scenario)
                native_report = output / "native-export.json"
                run([str(Path(probe).resolve()), str(project), str(native_report)])
                compare(expected, json.loads(native_report.read_text(encoding="utf-8")), manifest["tolerance"], case["id"] + ".export")
            elif mode in {"packaged-images", "packaged-image-clip", "packaged-movie", "packaged-movie-clip", "packaged-movie-clip-single",
                        "packaged-movie-calibration", "packaged-movie-keyed-calibration"} or mode.startswith("packaged-movie-tracks-"):
                packaged_movie = mode.startswith("packaged-movie")
                clip_mode = ("single" if mode in {"packaged-image-clip", "packaged-movie-clip-single"}
                             else "stride" if mode == "packaged-movie-clip" else None)
                exported = output / "motrack-export.trz"
                run([str(Path(probe).resolve()), "--export-" + mode, str(project), str(exported)])
                selected = output / "exported-member.trk"
                verify_media_package_export(project, exported, selected, case, clip_mode, movie=packaged_movie,
                                            calibrated=mode == "packaged-movie-calibration", keyed=mode == "packaged-movie-keyed-calibration", authoring=authoring)
                if (output / ("média & movie" if packaged_movie else "media é & frames")).exists():
                    raise ValueError("Media package probe retained original external media")
                project = selected
                if authoring:
                    expected, case = movie_tracks_roundtrip_expected(case, authoring)
                elif mode == "packaged-movie-keyed-calibration":
                    expected, case = keyed_movie_roundtrip_expected(case)
                else:
                    expected = linked_media_roundtrip_expected(case)
                if clip_mode:
                    expected, case = linked_media_clip_expected(case, single=clip_mode == "single")
                if mode == "packaged-movie-calibration":
                    expected, case = linked_movie_calibration_expected(case)
                native_report = output / "native-export.json"
                run([str(Path(probe).resolve()), str(project), str(native_report)])
                compare(expected, json.loads(native_report.read_text(encoding="utf-8")), manifest["tolerance"], case["id"] + ".export")
                archive_input = exported
            elif mode in {"archive-images", "paired-archive-images", "archive-image-clip"}:
                exported = output / "motrack-export.trz"
                run([str(Path(probe).resolve()), "--export-" + mode, str(archive_input), str(exported)])
                project = output / "exported-member.trk"
                verify_archive_media_export(archive_input, exported, project, clip_mode="stride" if mode == "archive-image-clip" else None)
                expected = linked_media_roundtrip_expected(case)
                if mode == "archive-image-clip":
                    expected, case = linked_media_clip_expected(case)
                native_report = output / "native-export.json"
                run([str(Path(probe).resolve()), str(project), str(native_report)])
                compare(expected, json.loads(native_report.read_text(encoding="utf-8")), manifest["tolerance"], case["id"] + ".export")
                archive_input = exported
            elif probe and mode.startswith("snap-"):
                # Rejected exports preserve their existing destination. Java independently authors
                # the unstable geometry so the actual application proves why rejection is required.
                destination = output / "rejected-destination.trk"
                run([str(Path(probe).resolve()), "--export-" + mode, str(project), str(destination)])
                if destination.read_bytes() != project.read_bytes():
                    raise ValueError("Rejected snapping export changed the destination")
                project = output / "java-snapping-source.trk"
                run([str(java_path), "-Djava.awt.headless=true", f"-Duser.home={java_home}", "-Xmx256m",
                     "-cp", case_classpath, "VectorFixture", str(project), mode])
                expected = snapping_roundtrip_expected(case, mode)
            elif probe:
                exported = output / "motrack-export.trk"
                run([str(Path(probe).resolve()), "--export-" + mode, str(project), str(exported)])
                if mode.startswith("linked-movie-tracks-"):
                    verify_linked_movie_export_xml(project, exported, case, authoring=authoring)
                    expected, case = movie_tracks_roundtrip_expected(case, authoring)
                elif mode == "linked-movie":
                    verify_linked_movie_export_xml(project, exported, case)
                    expected = linked_media_roundtrip_expected(case)
                elif mode == "linked-movie-calibration":
                    verify_linked_movie_export_xml(project, exported, case, calibrated=True)
                    expected, case = linked_movie_calibration_expected(case)
                elif mode == "linked-movie-keyed-calibration":
                    verify_linked_movie_export_xml(project, exported, case, keyed=True)
                    expected, case = keyed_movie_roundtrip_expected(case)
                elif mode in {"linked-movie-clip", "linked-movie-clip-single"}:
                    single = mode == "linked-movie-clip-single"
                    verify_linked_movie_export_xml(project, exported, case, "single" if single else "stride")
                    expected, case = linked_media_clip_expected(case, single)
                elif mode in {"linked-images", "paired-linked-images"}:
                    verify_linked_image_export_xml(project, exported, case)
                    expected = linked_media_roundtrip_expected(case)
                elif mode in {"linked-image-clip", "linked-image-clip-single"}:
                    single = mode == "linked-image-clip-single"
                    verify_linked_image_export_xml(project, exported, case, "single" if single else "stride")
                    expected, case = linked_media_clip_expected(case, single)
                elif mode == "mass":
                    verify_mass_export_xml(project, exported)
                    expected = mass_roundtrip_expected(case)
                elif mode in {"create-mass", "create-empty-mass"}:
                    empty = mode == "create-empty-mass"
                    verify_create_mass_export_xml(project, exported, empty)
                    expected, case = create_mass_roundtrip_expected(case, empty)
                elif mode in {"create-vector", "create-empty-vector"}:
                    empty = mode == "create-empty-vector"
                    verify_create_vector_export_xml(project, exported, empty)
                    expected, case = create_vector_roundtrip_expected(case, empty)
                elif mode in {"delete-mass", "delete-vector", "delete-all-vectors", "delete-selected-mass", "delete-all-masses"} | PLOT_DELETION_NAMES.keys() | TABLE_DELETION_NAMES.keys():
                    verify_deletion_export_xml(project, exported, mode)
                    expected, case = deletion_roundtrip_expected(case, mode)
                elif mode in {"name", "unselected-name", "vector-name", "plot-owner-name", "plot-guest-name", "center-member-name",
                              "attachment-first-name", "attachment-last-name", "model-booster-name"}:
                    verify_rename_export_xml(project, exported, mode)
                    expected, case = rename_roundtrip_expected(case, mode)
                elif mode in {"clip-timing", "clip-timing-range"}:
                    ranged = mode == "clip-timing-range"
                    expected = clip_timing_roundtrip_expected(case, ranged)
                    verify_clip_timing_export_xml(project, exported, ranged)
                    timing = clip_timing_expected(ranged)
                    case = {**case, "session_time_samples": timing["time_samples"],
                            "session_frame_duration_ms": timing["frame_duration_ms"]}
                    check_native_clip_timing(probe, exported, output / "native-timing.json", manifest["tolerance"], ranged)
                elif mode == "time-unit":
                    expected = time_unit_roundtrip_expected(case)
                    verify_time_unit_export_xml(project, exported)
                    case = {**case, "session_time_samples": [{"frame": 0, "time_ms": 0.0}, {"frame": 9, "time_ms": 900.0}]}
                elif mode == "mass-unit":
                    expected = mass_unit_roundtrip_expected(case)
                    verify_mass_unit_export_xml(project, exported)
                elif mode == "mixed-calibration":
                    expected = mixed_calibration_roundtrip_expected(case)
                    verify_mixed_calibration_export_xml(case, project, exported)
                elif mode == "keyed-calibration":
                    expected = keyed_calibration_roundtrip_expected(case)
                    verify_keyed_calibration_export_xml(project, exported)
                elif mode == "length-unit":
                    expected = length_unit_roundtrip_expected(case)
                    verify_length_unit_export_xml(project, exported)
                elif mode == "calibration":
                    expected = calibration_roundtrip_expected(case)
                    verify_calibration_export_xml(project, exported)
                elif mode in {"clip", "clip-boundary"}:
                    expected = clip_roundtrip_expected(case, mode == "clip-boundary")
                    verify_clip_export_xml(project, exported, mode == "clip-boundary")
                elif "vector" in mode:
                    expected = vector_roundtrip_expected(case, mode)
                    verify_vector_export_xml(project, exported, expected)
                else:
                    expected = mark_roundtrip_expected(case, mode)
                    verify_mark_export_xml(project, exported, expected, mode)
                native_report = output / "native-export.json"
                run([str(Path(probe).resolve()), str(exported), str(native_report)])
                compare(expected, json.loads(native_report.read_text(encoding="utf-8")), manifest["tolerance"], case["id"] + ".export")
                project = exported
            stages = {}
            image_inventories = {}
            movie_inventories = {}
            # Tracker has static per-panel drawing state but allocates IDs separately in each TFrame.
            # A fresh JVM prevents two windows' vector snapping registries from sharing the same IDs.
            for stage in ["save", "reopen"]:
                home = java_home if stage == "save" else output / "java-reopen-home"
                home.mkdir(exist_ok=True)
                source = (archive_input or project) if stage == "save" else output / "tracker-resaved.trk"
                metadata_args = [str(project)] if stage == "save" and archive_input else []
                try:
                    run([str(java_path), f"-Duser.home={home}", f"-Djava.io.tmpdir={home}", "-Duser.language=en", "-Duser.country=US",
                         "-Xmx256m", "-cp", case_classpath, "TrackerSessionReference",
                         str(source), str(output), stage, *metadata_args], display=display)
                except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                    diagnostic = format_session_file_diagnostic(source)
                    resave = ("\nTracker resave diagnostic (bounded): " + format_session_file_diagnostic(output / "tracker-resaved.trk")
                              if stage == "save" else "")
                    raise ValueError(f"{case['id']}/{mode}/{stage}: Tracker reference failed for {source.name}: {error}"
                                     f"\nTracker input diagnostic (bounded): {diagnostic}{resave}") from error
                stages[stage] = json.loads((output / ("session-" + stage + ".json")).read_text(encoding="utf-8"))
                if (stages[stage]["reference_version"] != manifest["reference"]["version"]
                        or stages[stage]["verification_level"] != "tracker_session_" + stage
                        or stages[stage].get("view_preparation") != "expand_visit_custom_tables_restore_selection"
                        or ((mode.startswith("packaged-movie") or vector_package) and stages[stage].get("cache_policy") != "private_persistent_osp_cache")):
                    raise ValueError("Unexpected Tracker session stage")
                callbacks = stages[stage].get("loader_callbacks")
                if type(callbacks) is not int or callbacks < 1:
                    raise ValueError("Missing Tracker loader callback evidence")
                if callbacks > 1:
                    print(f"{case['id']}/{mode}/{stage}: observed {callbacks} loader callbacks; reference action ran once")
                if case.get("media_vector_kind") == "images":
                    image_inventories[stage] = verify_saved_image_inventory(output / "tracker-resaved.trk", case["assets"])
                elif case["id"] in {"linked-images", "archived-images"}:
                    # Both cases use the independently pinned four-image fixture, including excluded frames.
                    assets = next(item["assets"] for item in manifest["cases"] if item["id"] == "linked-images")
                    image_inventories[stage] = verify_saved_image_inventory(output / "tracker-resaved.trk", assets)
                elif case["id"] in {"linked-movie", "linked-mov", "movie-tracks"} or "movie_calibration_flags" in case or case.get("media_vector_kind") == "movie":
                    movie_inventories[stage] = verify_saved_movie_asset(output / "tracker-resaved.trk", case["assets"][0],
                                                                       allow_archive=mode.startswith("packaged-movie") or vector_package)
            if probe and mode in CREATED_TRACK_MODES:
                verify_created_track_fixpoint(project, output / "tracker-resaved.trk", CREATED_TRACK_MODES[mode])
            report = {"reference_version": stages["save"]["reference_version"], "verification_level": "tracker_session_resave",
                      "view_preparation": stages["save"]["view_preparation"],
                      "loader_callbacks": {stage: stages[stage]["loader_callbacks"] for stage in stages},
                      "cache_policy": {stage: stages[stage].get("cache_policy") for stage in stages},
                      "before": stages["save"]["loaded"], "after_save": stages["save"]["after_save"],
                      "after": stages["reopen"]["loaded"], "saved_image_inventories": image_inventories, "saved_movie_assets": movie_inventories}
            keys = {"marks": [[0, 3], [7]], "empty-marks": [[], [0]], "first-mark": [[4], [0]]}.get(mode)
            if mode in {"linked-movie", "linked-movie-clip", "linked-movie-clip-single", "linked-images", "linked-image-clip", "linked-image-clip-single", "archive-images",
                        "paired-linked-images", "paired-archive-images", "packaged-images", "archive-image-clip", "packaged-image-clip",
                        "packaged-movie", "packaged-movie-clip", "packaged-movie-clip-single", "linked-movie-calibration", "packaged-movie-calibration"}:
                keys = [[0, 1, 2]]
            if mode in {"create-mass", "create-empty-mass"}:
                keys = [[0], [0], [] if mode == "create-empty-mass" else [1, 4, 8]]
            elif mode in {"linked-movie-keyed-calibration", "packaged-movie-keyed-calibration"}:
                keys = case["session_key_frames"]
            elif mode in {"delete-mass", "delete-selected-mass"}:
                keys = [[0]]
            elif mode == "delete-all-masses":
                keys = []
            elif mode in PLOT_DELETION_NAMES:
                keys = [[0, 2], [1, 3]] if mode != "delete-first-plot-guest" else [[0, 2], [0, 2]]
            elif mode in TABLE_DELETION_NAMES:
                keys = [] if mode == "delete-all-table-owners" else [[0, 2], [0, 2]] if mode == "delete-last-table-owner" else [[0, 2], [1, 3]]
            calibration = keyed_calibration_expected(mode == "keyed-calibration") if case["id"] == "keyed-calibration" else None
            if "mixed_calibration_flags" in case:
                calibration = mixed_calibration_expected(case, mode == "mixed-calibration")
            known_report = None
            try:
                if case.get("session_known_issue"):
                    known_report = validate_known_session_issue(case, report, manifest, output / "tracker-resaved.trk")
                else:
                    validate_session(case, report, manifest, expected, keys, calibration)
            except ValueError as error:
                raise ValueError(f"{error}\nTracker session snapshots (bounded): {json.dumps(report)[:24000]}") from error
            if probe:
                reimport = output / "native-reimport.json"
                run([str(Path(probe).resolve()), str(output / "tracker-resaved.trk"), str(reimport)])
                expected["source_version"] = manifest["reference"]["version"]
                expected.update(session_units(case, expected))
                compare(expected, json.loads(reimport.read_text(encoding="utf-8")), manifest["tolerance"], case["id"] + ".reimport")
                if mode in {"clip-timing", "clip-timing-range"}:
                    check_native_clip_timing(probe, output / "tracker-resaved.trk", output / "resaved-timing.json",
                                            manifest["tolerance"], mode == "clip-timing-range")
                saved_report = {"verification_level": "motrack_" + mode + "_roundtrip", "tracker_session": report, "expected_reimport": expected}
                suffix = "-" + mode + "-roundtrip.json"
                if mode.startswith("snap-"):
                    saved_report["verification_level"] = "motrack_rejected_" + mode
                    print(f"{case['id']}/{mode}: passed (MoTrack rejected unsafe export; actual Tracker snapping confirmed; resave/reimport)")
                else:
                    standard = "TRZ" if mode in {"archive-images", "paired-archive-images", "packaged-images",
                                                "archive-image-clip", "packaged-image-clip",
                                                "packaged-movie", "packaged-movie-clip", "packaged-movie-clip-single",
                                                "packaged-movie-calibration", "packaged-movie-keyed-calibration"} or mode.startswith("packaged-movie-tracks-") or vector_package else "TRK"
                    pair = "interrupted paired save/recovery; " if mode.startswith("paired-") else ""
                    print(f"{case['id']}/{mode}: passed (MoTrack edit/native reopen/{standard} export; {pair}Tracker load/save/reopen; native reimport)")
            elif known_report is not None:
                saved_report, suffix = known_report, "-known-issue.json"
                print(f"{case['id']}: KNOWN TRACKER DEFECT reproduced ({known_report['known_issue']}; round trip NOT accepted)")
            else:
                saved_report, suffix = report, "-session.json"
                print(f"{case['id']}: passed (Tracker application load/save/reopen; MoTrack export untested)")
            (reports / (case["id"] + suffix)).write_text(
                json.dumps(saved_report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


# Loaded-state oracle (O3/O4) and the known-defect registry. The schema, comparison rules, gap baseline
# and registry semantics are defined in docs/oracle-dumps.md.
STATE_DIR = CORPUS / "state"


KNOWN_DEFECTS = CORPUS / "known-defects.json"


STATE_GAPS = CORPUS / "state-gaps.json"


STATE_SCHEMA_VERSION = 1


STATE_PREPARATION = "expand_visit_custom_tables_bounded_restore_selection"


STATE_ABSENT = "@absent"


STATE_LOAD_FAILURE = "@load_failure"


STATE_NONFINITE = ("NaN", "Infinity", "-Infinity")


STATE_REPORT_LIMIT = 40


MAX_STATE_BYTES = 8 * 1024 * 1024


MAX_STATE_ITEMS = 10000


STATE_PANEL_KEYS = ("units", "selected_track", "properties", "clip", "video", "coords", "tracks", "views")


STATE_TRACK_KEYS = ("class", "name", "properties", "steps", "data")


STATE_JAVA_SOURCES = ("TrackerReference.java", "TrackerSessionReference.java", "TrackerStateDump.java")


# Customized tables are visited for this many 25 ms polls; a case whose visit ends on the bound is dumped
# again with the long bound (10 s), which must give the same state.
STATE_TABLE_POLLS = 40


STATE_LONG_TABLE_POLLS = 400


STATE_TABLE_VISIT = re.compile(r"^TrackerStateDump table visit owner=(.*) result=(populated|bounded|skipped) polls=([0-9]+)$", re.M)


MOTRACK_ISSUE = re.compile(r"https://github\.com/mgradwohl/MoTrack/issues/[1-9][0-9]*")


TRACKER_PACKAGE = "org.opensourcephysics.cabrillo.tracker."


# Layout of committed dumps: these containers are expanded one item per line; all else stays compact.
STATE_EXPANDED = {(), ("panel",), ("panel", "tracks"), ("panel", "tracks", "*"), ("panel", "tracks", "*", "steps"),
                  ("panel", "tracks", "*", "data"), ("panel", "tracks", "*", "data", "rows"), ("panel", "coords"),
                  ("panel", "coords", "frames"), ("panel", "clip"), ("panel", "clip", "steps")}


# Point-mass columns that exist only with a time base; without one they are timing gaps, not unmodeled.
STATE_TIMING_COLUMNS = ("t", "v_{x}", "v_{y}", "v", "a_{x}", "a_{y}", "p_{x}", "p_{y}", "K")


STATE_GAP_REASONS = {
    "unmodeled-class": "MoTrack preserves this track class but does not model it yet.",
    "unmodeled-field": "MoTrack's model does not hold this loaded-state field yet.",
    "unmodeled-column": "MoTrack does not compute this data column yet.",
    "probe-media-timing": "The XML-only probe does not decode media; movie timing comes from MoTrack's decoder.",
    "default-timing": "Tracker applies its 100 ms default frame duration; MoTrack does not yet (#214).",
    "default-length-unit": "Tracker applies its default length unit; MoTrack does not yet (#214).",
    "uncalibrated": "Without a saved calibration MoTrack reports no world quantities; Tracker uses its default coordinates.",
    "coords-range": "MoTrack reports coordinates through the clip's last frame; Tracker's system spans the whole video.",
    "known-defect-unmodeled": "A registered known defect lies at a path or key MoTrack does not model yet.",
}


# Pinned 6.3.5 TTrack.getDefaultFormatPatterns(int) (source candidate dc064f9, TTrack.java:4038-4057):
# point-mass tables format t with NumberField.DECIMAL_3_PATTERN and step/frame with INTEGER_PATTERN.
TABLE_DEFAULT_PATTERNS = {"frame": "0", "step": "0", "t": "0.000"}


def state_path(case_id):
    return STATE_DIR / (case_id + ".json")


def shown_path(path):
    return path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path)


def is_state_number(value):
    return (type(value) in (int, float) and math.isfinite(value)) or value in STATE_NONFINITE


def load_state_json(text, source):
    """Parse dump JSON, rejecting the bare NaN/Infinity tokens Python's parser would otherwise accept."""
    def reject(token):
        raise ValueError(f"bare {token} is not JSON; specials must be strings")
    try:
        return json.loads(text, parse_constant=reject)
    except ValueError as error:
        raise ValueError(f"{source}: {error}") from error


def validate_state_dump(document, manifest, kind="tracker", source="state dump"):
    """Check a loaded-state dump's schema before it is compared, recorded or trusted."""
    def require(condition, message):
        if not condition:
            raise ValueError(f"{source}: invalid {kind} state dump: {message}")

    require(isinstance(document, dict) and set(document) == {"schema_version", "producer", "panel"}
            and document["schema_version"] == STATE_SCHEMA_VERSION, "schema")
    producer, panel = document["producer"], document["panel"]
    require(isinstance(producer, dict) and producer.get("kind") == kind, "producer")
    if kind == "tracker":
        require(producer == {"kind": "tracker", "version": manifest["reference"]["version"], "loading": "tracker_session",
                             "view_preparation": STATE_PREPARATION}, "reference version or loading mode")
        require(isinstance(panel, dict) and tuple(panel) == STATE_PANEL_KEYS, "panel keys")
    else:
        classes = producer.get("supported_classes")
        require(set(producer) == {"kind", "supported_classes"} and isinstance(classes, list) and classes
                and all(isinstance(name, str) for name in classes) and len(set(classes)) == len(classes), "supported classes")
        require(isinstance(panel, dict) and "tracks" in panel and set(panel) <= set(STATE_PANEL_KEYS), "panel keys")
    tracks = panel["tracks"]
    require(isinstance(tracks, list) and len(tracks) <= MAX_STATE_ITEMS, "track list")
    for track in tracks:
        require(isinstance(track, dict) and isinstance(track.get("class"), str) and isinstance(track.get("name"), str),
                "track identity")
        if kind == "tracker":
            require(tuple(track) == STATE_TRACK_KEYS, f"keys of track {track['name']!r}")
        else:
            require(set(track) <= set(STATE_TRACK_KEYS) and track["class"] in producer["supported_classes"],
                    f"keys or class of track {track['name']!r}")
        steps = track.get("steps", [])
        require(isinstance(steps, list) and len(steps) <= MAX_STATE_ITEMS
                and all(isinstance(step, dict) and type(step.get("frame")) is int for step in steps), "steps")
        frames = [step["frame"] for step in steps]
        require(len(set(frames)) == len(frames), f"duplicate step frames in {track['name']!r}")
        data = track.get("data")
        if data is None:
            continue
        require(isinstance(data, dict) and set(data) == {"columns", "constants", "rows"}, "data keys")
        columns, rows = data["columns"], data["rows"]
        require(isinstance(columns, list) and all(isinstance(column, dict) and set(column) == {"name", "dimensions"}
                                                  and isinstance(column["name"], str)
                                                  and (column["dimensions"] is None or isinstance(column["dimensions"], str))
                                                  for column in columns), "data columns")
        names = [column["name"] for column in columns]
        require(len(set(names)) == len(names), f"duplicate data columns in {track['name']!r}")
        require(isinstance(data["constants"], dict) and all(is_state_number(value) for value in data["constants"].values()),
                "data constants")
        require(isinstance(rows, list) and len(rows) <= MAX_STATE_ITEMS
                and all(isinstance(row, list) and len(row) == len(columns) and all(is_state_number(value) for value in row)
                        for row in rows), f"data rows of {track['name']!r}")
    clip = panel.get("clip")
    if clip is not None:
        # A partial producer writes null for an unknown step time (an untimed clip); Tracker always has one.
        require(isinstance(clip, dict) and isinstance(clip.get("steps", []), list)
                and all(isinstance(row, list) and len(row) == 3 and type(row[0]) is int and type(row[1]) is int
                        and (is_state_number(row[2]) or (kind == "motrack" and row[2] is None))
                        for row in clip.get("steps", [])), "clip steps")
    coords = panel.get("coords")
    if coords is not None:
        require(isinstance(coords, dict) and isinstance(coords.get("frames"), list)
                and all(isinstance(row, list) and row and type(row[0]) is int for row in coords["frames"]), "coordinate frames")


def state_text(document):
    """Render a dump with a stable, reviewable layout: one track, step, data row or frame per line."""
    def render(value, path, indent):
        pattern = tuple("*" if isinstance(part, int) else part for part in path)
        if pattern not in STATE_EXPANDED or not isinstance(value, (dict, list)) or not value:
            return json.dumps(value, separators=(",", ":"), ensure_ascii=False)
        pad = " " * (indent + 2)
        if isinstance(value, dict):
            items = [f"{pad}{json.dumps(key, ensure_ascii=False)}: {render(item, path + (key,), indent + 2)}"
                     for key, item in value.items()]
            return "{\n" + ",\n".join(items) + "\n" + " " * indent + "}"
        items = [pad + render(item, path + (index,), indent + 2) for index, item in enumerate(value)]
        return "[\n" + ",\n".join(items) + "\n" + " " * indent + "]"
    return render(document, (), 0) + "\n"


def read_state(path, manifest, kind="tracker"):
    if path.stat().st_size > MAX_STATE_BYTES:
        raise ValueError(f"{shown_path(path)}: state dump exceeds its size limit")
    document = load_state_json(path.read_text(encoding="utf-8"), shown_path(path))
    validate_state_dump(document, manifest, kind, shown_path(path))
    return document


class StateComparison:
    """Collects bounded failures and coverage gaps while comparing two loaded-state dumps.

    Identities, frame numbers, names, classes and dimensions compare exactly; numbers use the manifest's
    state tolerance table, chosen by a data column's dimensions. In subset mode the actual dump is a
    partial producer (MoTrack): only what it reports is compared, and what it omits is recorded as a gap.
    Gaps are checked against a committed per-case baseline, so a value that disappears fails.
    """

    def __init__(self, manifest, case_id, *, subset=False):
        self.case, self.subset = case_id, subset
        self.default = manifest["state_tolerance"]["default"]
        self.by_dimensions = manifest["state_tolerance"]["dimensions"]
        self.failures, self.gaps = [], []

    def fail(self, path, message):
        self.failures.append((path, f"{path}: {message}"))

    def gap(self, description):
        if description not in self.gaps:
            self.gaps.append(description)

    def tolerance(self, dimensions=None):
        return self.by_dimensions.get(dimensions, self.default)

    def scalar(self, expected, actual, path, tolerance=None):
        tolerance = tolerance or self.default
        if type(expected) in (int, float) and type(actual) in (int, float):
            if type(expected) is int and type(actual) is int:
                if expected != actual:
                    self.fail(path, f"expected {expected}, got {actual}")
            elif not math.isclose(expected, actual, rel_tol=tolerance["relative"], abs_tol=tolerance["absolute"]):
                self.fail(path, f"expected {expected!r}, got {actual!r} (absolute {tolerance['absolute']:g}, "
                                f"relative {tolerance['relative']:g})")
        elif type(expected) is not type(actual) or expected != actual:
            self.fail(path, f"expected {expected!r}, got {actual!r}")

    def tree(self, expected, actual, path, tolerance=None):
        if isinstance(expected, dict):
            if not isinstance(actual, dict):
                self.fail(path, f"expected an object, got {type(actual).__name__}")
                return
            if expected.keys() != actual.keys():
                self.fail(path, f"keys differ: missing {sorted(expected.keys() - actual.keys())}, "
                                f"unexpected {sorted(actual.keys() - expected.keys())}")
            for key in expected:
                if key in actual:
                    self.tree(expected[key], actual[key], f"{path}.{key}", tolerance)
        elif isinstance(expected, list):
            if not isinstance(actual, list) or len(expected) != len(actual):
                self.fail(path, f"expected {len(expected)} items, got "
                                f"{len(actual) if isinstance(actual, list) else type(actual).__name__}")
                return
            for index, (want, got) in enumerate(zip(expected, actual)):
                self.tree(want, got, f"{path}[{index}]", tolerance)
        else:
            self.scalar(expected, actual, path, tolerance)

    def section(self, expected, actual, key, path, compare):
        """Compare one optional section; a partial producer that omits it records a gap."""
        if key not in actual:
            if not self.subset:
                self.fail(f"{path}.{key}", "missing")
            else:
                self.gap(key)
            return
        compare(expected.get(key), actual[key], f"{path}.{key}")

    def mapping(self, expected, actual, path, label=None):
        """Compare an object; in subset mode only the keys the actual producer reports.

        `label` names unreported keys in the gap list (for example "PointMass.properties").
        """
        if not self.subset or not isinstance(expected, dict) or not isinstance(actual, dict):
            self.tree(expected, actual, path)
            return
        for key in sorted(actual.keys() - expected.keys()):
            self.fail(f"{path}.{key}", "not reported by Tracker")
        for key in sorted(expected.keys() - actual.keys()):
            self.gap(f"{label or path.split('.', 1)[-1]}.{key}")
        for key in expected.keys() & actual.keys():
            self.tree(expected[key], actual[key], f"{path}.{key}")

    def clip(self, expected, actual, path):
        steps_expected, steps_actual = expected.get("steps", []), actual.get("steps")
        self.mapping({k: v for k, v in expected.items() if k != "steps"},
                     {k: v for k, v in actual.items() if k != "steps"}, path, "clip")
        if steps_actual is None:
            self.gap("clip.steps")
            return
        # Steps are compared one by one, so each missing, extra or remapped step is reported (and can be
        # registered) individually.
        want_steps = {row[0]: row for row in steps_expected}
        got_steps = {row[0]: row for row in steps_actual}
        for step in sorted(want_steps.keys() - got_steps.keys()):
            self.fail(f"{path}.steps[{step}]", "missing step")
        for step in sorted(got_steps.keys() - want_steps.keys()):
            self.fail(f"{path}.steps[{step}]", "unexpected step")
        for step in sorted(want_steps.keys() & got_steps.keys()):
            want, got = want_steps[step], got_steps[step]
            if want[1] != got[1]:
                self.fail(f"{path}.steps[{step}]", f"frame {got[1]} differs from {want[1]}")
            elif got[2] is None:
                self.gap("clip.steps.time")
            else:
                self.scalar(want[2], got[2], f"{path}.steps[{step}].time_ms")

    def coords(self, expected, actual, path):
        if expected is None or actual is None:
            self.tree(expected, actual, path)
            return
        self.mapping({k: v for k, v in expected.items() if k != "frames"},
                     {k: v for k, v in actual.items() if k != "frames"}, path, "coords")
        frames = {row[0]: row for row in expected["frames"]}
        actual_frames = actual["frames"]
        reported = [row[0] for row in actual_frames]
        if not self.subset:
            if reported != list(frames):
                self.fail(f"{path}.frames", "frame list differs")
                return
        elif reported != list(range(len(reported))) or not reported:
            # A partial producer must report a contiguous range from frame 0; fewer frames are a gap.
            self.fail(f"{path}.frames", f"expected contiguous frames from 0, got {reported[:5]}...")
            return
        elif len(reported) < len(frames):
            self.gap(f"coords.frames after {reported[-1]}")
        for row in actual_frames:
            want = frames.get(row[0])
            if want is None:
                self.fail(f"{path}.frames[{row[0]}]", "frame not in Tracker's coordinate system")
                continue
            if len(row) < len(want):
                if not self.subset:
                    self.fail(f"{path}.frames[{row[0]}]", "truncated frame")
                self.gap("coords.frames.transform")
            self.tree(want[:len(row)], row, f"{path}.frames[{row[0]}]")

    def tracks(self, expected, actual, supported, path):
        candidates = [track for track in expected if supported is None or track["class"] in supported]
        if supported is not None:
            for track in expected:
                if track["class"] not in supported:
                    self.gap(f"track class {track['class'].rsplit('.', 1)[-1]}")
        if [(t["class"], t["name"]) for t in candidates] != [(t["class"], t["name"]) for t in actual]:
            self.fail(path, f"track identities differ: expected {[t['name'] for t in candidates]}, "
                            f"got {[t['name'] for t in actual]}")
            return
        names = [track["name"] for track in actual]
        for index, (want, got) in enumerate(zip(candidates, actual)):
            selector = f"name={got['name']}" if names.count(got["name"]) == 1 else str(index)
            where = f"{path}[{selector}]"
            kind = got["class"].rsplit(".", 1)[-1]
            for key in ("properties", "steps", "data"):
                if key not in got:
                    if not self.subset:
                        self.fail(f"{where}.{key}", "missing")
                    else:
                        self.gap(f"{kind}.{key}")
                    continue
                if key == "properties":
                    self.mapping(want[key], got[key], f"{where}.properties", f"{kind}.properties")
                elif key == "steps":
                    self.steps(want[key], got[key], f"{where}.steps", kind)
                else:
                    self.data(want[key], got[key], f"{where}.data", kind)

    def steps(self, expected, actual, path, kind):
        frames = {step["frame"]: step for step in expected}
        actual_frames = {step["frame"]: step for step in actual}
        for frame in sorted(frames.keys() - actual_frames.keys()):
            self.fail(f"{path}[frame={frame}]", "missing step")
        for frame in sorted(actual_frames.keys() - frames.keys()):
            self.fail(f"{path}[frame={frame}]", "unexpected step")
        for frame in sorted(frames.keys() & actual_frames.keys()):
            self.mapping(frames[frame], actual_frames[frame], f"{path}[frame={frame}]", f"{kind}.steps")

    def data(self, expected, actual, path, kind):
        if expected is None or actual is None:
            self.tree(expected, actual, path)
            return
        want_columns = {column["name"]: column for column in expected["columns"]}
        got_columns = [column["name"] for column in actual["columns"]]
        if not self.subset and got_columns != list(want_columns):
            self.fail(f"{path}.columns", f"expected {list(want_columns)}, got {got_columns}")
            return
        for column in actual["columns"]:
            want = want_columns.get(column["name"])
            if want is None:
                self.fail(f"{path}.columns.{column['name']}", "not a Tracker column")
            elif want["dimensions"] != column["dimensions"]:
                self.fail(f"{path}.columns.{column['name']}", f"dimensions {want['dimensions']!r} != {column['dimensions']!r}")
        for name in sorted(want_columns.keys() - set(got_columns)):
            self.gap(f"{kind}.data.{name}")
        self.mapping(expected["constants"], actual["constants"], f"{path}.constants", f"{kind}.data.constants")
        want_rows, got_rows = state_rows(expected), state_rows(actual)
        keyed = "frame" in want_columns and "frame" in got_columns

        def key(index, row):
            if not keyed:
                return index
            frame = row["frame"]
            # Tracker stores frame numbers in double datasets; rows are addressed by the integer frame.
            return int(frame) if type(frame) is float and frame.is_integer() else frame
        want_map = {key(i, row): row for i, row in enumerate(want_rows)}
        got_map = {key(i, row): row for i, row in enumerate(got_rows)}
        label = "frame" if keyed else "index"
        for item in sorted(want_map.keys() - got_map.keys(), key=str):
            self.fail(f"{path}.rows[{label}={item}]", "missing row")
        for item in sorted(got_map.keys() - want_map.keys(), key=str):
            self.fail(f"{path}.rows[{label}={item}]", "unexpected row")
        dimensions = {column["name"]: column["dimensions"] for column in expected["columns"]}
        for item in sorted(want_map.keys() & got_map.keys(), key=str):
            for name, value in got_map[item].items():
                if name in want_map[item]:
                    self.scalar(want_map[item][name], value, f"{path}.rows[{label}={item}].{name}",
                                self.tolerance(dimensions.get(name)))

    def panel(self, expected, actual, supported=None):
        path = "panel"
        for key in STATE_PANEL_KEYS:
            if key == "tracks":
                self.tracks(expected["tracks"], actual["tracks"], supported, f"{path}.tracks")
            elif key == "clip":
                self.section(expected, actual, key, path, self.clip)
            elif key == "coords":
                self.section(expected, actual, key, path, self.coords)
            elif key == "units":
                self.section(expected, actual, key, path, self.mapping)
            else:
                self.section(expected, actual, key, path, self.tree)


def state_rows(data):
    names = [column["name"] for column in data["columns"]]
    return [dict(zip(names, row)) for row in data["rows"]]


def compare_state(expected, actual, manifest, case_id, *, subset=False):
    """Compare two validated dumps; returns the StateComparison holding failures and gaps."""
    comparison = StateComparison(manifest, case_id, subset=subset)
    supported = set(actual["producer"]["supported_classes"]) if subset else None
    comparison.panel(expected["panel"], actual["panel"], supported)
    return comparison


STATE_SEGMENT = re.compile(r"([^.\[\]]+)((?:\[[^\[\]]+\])*)")


def resolve_state_path(panel, target, path):
    """Resolve a registry object/path in a dump panel; STATE_ABSENT when it does not exist."""
    value = panel
    if target is not None:
        matches = [track for track in panel.get("tracks", []) if track["name"] == target["track"]]
        if len(matches) > 1:
            raise ValueError(f"Ambiguous registry track {target['track']!r}")
        if not matches:
            return STATE_ABSENT
        value = matches[0]
    for segment in path.split(".") if path else []:
        match = STATE_SEGMENT.fullmatch(segment)
        if not match:
            raise ValueError(f"Invalid registry path segment {segment!r}")
        key, selectors = match.groups()
        if not isinstance(value, dict) or key not in value:
            return STATE_ABSENT
        value = state_rows(value) if key == "rows" and "columns" in value else value[key]
        for selector in re.findall(r"\[([^\[\]]+)\]", selectors):
            if not isinstance(value, list):
                return STATE_ABSENT
            if re.fullmatch(r"[0-9]+", selector):
                index = int(selector)
                if index >= len(value):
                    return STATE_ABSENT
                value = value[index]
                continue
            field, _, wanted = selector.partition("=")
            wanted = json.loads(wanted) if re.fullmatch(r"-?[0-9.eE+-]+|true|false|null", wanted) else wanted
            found = [item for item in value if isinstance(item, dict) and item.get(field) == wanted]
            if len(found) > 1:
                raise ValueError(f"Ambiguous registry selector [{selector}]")
            if not found:
                return STATE_ABSENT
            value = found[0]
    return value


def state_matches(expected, actual, tolerance):
    """Tracker-side registry match: listed object keys must match, lists exactly.

    Unlisted keys are unconstrained here because state-oracle separately compares every Tracker dump with
    its committed copy exactly; the registry only names the deviating part.
    """
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(key in actual and state_matches(value, actual[key], tolerance)
                                                for key, value in expected.items())
    if isinstance(expected, list):
        return (isinstance(actual, list) and len(expected) == len(actual)
                and all(state_matches(want, got, tolerance) for want, got in zip(expected, actual)))
    if type(expected) in (int, float) and type(actual) in (int, float):
        return math.isclose(expected, actual, rel_tol=tolerance["relative"], abs_tol=tolerance["absolute"])
    return type(expected) is type(actual) and expected == actual


def registered_check(expected, actual, tolerance, path=""):
    """MoTrack-side registry match. Returns (mismatches, missing): every key MoTrack reports must be listed
    and equal; listed keys MoTrack does not report, and values it reports as null (unknown), are missing
    (gaps)."""
    if actual is None and expected is not None:
        return [], [path or ".value"]
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return [f"{path or 'value'}: expected an object, got {json.dumps(actual)[:80]}"], []
        mismatches, missing = [], []
        for key in actual:
            if key not in expected:
                mismatches.append(f"{path}.{key}: not part of the independent value")
            else:
                more, absent = registered_check(expected[key], actual[key], tolerance, f"{path}.{key}")
                mismatches += more
                missing += absent
        missing += [f"{path}.{key}" for key in expected if key not in actual]
        return mismatches, missing
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(expected) != len(actual):
            return [f"{path or 'value'}: expected {len(expected)} items"], []
        mismatches, missing = [], []
        for index, (want, got) in enumerate(zip(expected, actual)):
            more, absent = registered_check(want, got, tolerance, f"{path}[{index}]")
            mismatches += more
            missing += absent
        return mismatches, missing
    if type(expected) in (int, float) and type(actual) in (int, float):
        ok = math.isclose(expected, actual, rel_tol=tolerance["relative"], abs_tol=tolerance["absolute"])
    else:
        ok = type(expected) is type(actual) and expected == actual
    return ([] if ok else [f"{path or 'value'}: expected {expected!r}, got {actual!r}"]), []


def is_whole_file(entry):
    return entry["object"] is None and entry["path"] is None


def load_known_defects(manifest, path=None):
    """Validate the per-case registry of reference defects that MoTrack must not inherit."""
    source = path or KNOWN_DEFECTS
    registry = load_state_json(source.read_text(encoding="utf-8"), shown_path(source))
    if (not isinstance(registry, dict) or set(registry) != {"schema_version", "reference_version", "entries"}
            or registry["schema_version"] != 1 or registry["reference_version"] != manifest["reference"]["version"]
            or not isinstance(registry["entries"], list) or len(registry["entries"]) > 1000):
        raise ValueError("Unexpected known-defect registry schema or reference version")
    cases = {case["id"] for case in manifest["cases"]}
    ids, keys, whole = set(), set(), set()
    for entry in registry["entries"]:
        required = {"id", "case", "object", "path", "issue", "upstream", "summary", "expected", "tracker"}
        if not isinstance(entry, dict) or set(entry) != required:
            raise ValueError(f"Invalid known-defect entry keys: {entry.get('id') if isinstance(entry, dict) else entry!r}")
        name = entry["id"]
        # One entry names exactly one case and one object: nothing is waived across files.
        if (not isinstance(name, str) or not re.fullmatch(r"[a-z0-9-]+", name) or name in ids
                or not isinstance(entry["case"], str) or entry["case"] not in cases
                or not isinstance(entry["issue"], str) or not MOTRACK_ISSUE.fullmatch(entry["issue"])
                or not (entry["upstream"] is None or (isinstance(entry["upstream"], str) and entry["upstream"].startswith("https://")))
                or not isinstance(entry["summary"], str) or not entry["summary"]):
            raise ValueError(f"Invalid or duplicate known-defect entry {name!r}")
        ids.add(name)
        if is_whole_file(entry):
            expected, tracker = entry["expected"], entry["tracker"]
            if (not isinstance(tracker, dict) or set(tracker) != {"load_failure"} or not isinstance(tracker["load_failure"], list)
                    or not tracker["load_failure"] or not all(isinstance(text, str) and text for text in tracker["load_failure"])
                    or not isinstance(expected, dict) or set(expected) != {"state_of", "project_sha256"}
                    or expected["state_of"] not in cases or expected["state_of"] == entry["case"]
                    or not re.fullmatch(r"[a-f0-9]{64}", str(expected["project_sha256"])) or entry["case"] in whole):
                raise ValueError(f"Invalid whole-file known defect {name!r}")
            whole.add(entry["case"])
            continue
        target, where = entry["object"], entry["path"]
        if (not (target is None or (isinstance(target, dict) and set(target) == {"track"} and isinstance(target["track"], str)))
                or not isinstance(where, str) or not where or "*" in where
                or not all(STATE_SEGMENT.fullmatch(segment) for segment in where.split("."))):
            raise ValueError(f"Invalid known-defect object or path {name!r}")
        key = (entry["case"], None if target is None else target["track"], where)
        if (key in keys or (isinstance(entry["tracker"], dict) and "load_failure" in entry["tracker"])
                or state_matches(entry["expected"], entry["tracker"], manifest["state_tolerance"]["default"])):
            raise ValueError(f"Duplicate or non-deviating known-defect entry {name!r}")
        keys.add(key)
    if whole & {case for case, _, _ in keys}:
        raise ValueError("A whole-file known defect cannot also have object entries")
    return registry


def check_known_defects(registry, manifest, dumps, failures, errors=None):
    """Require each registered Tracker deviation to persist; return the reproduced entries.

    `dumps` maps case IDs to Tracker dumps or STATE_LOAD_FAILURE, and `errors` maps failed cases to their
    failure text. A deviation Tracker no longer shows fails as STALE, a different one as CHANGED, so
    re-pinning the reference surfaces every entry for review.
    """
    tolerance = manifest["state_tolerance"]["default"]
    errors = errors or {}
    reproduced = []
    for entry in registry["entries"]:
        observed = dumps.get(entry["case"])
        if observed is None:
            failures.append(f"{entry['case']}: known defect {entry['id']}: case was not dumped")
            continue
        if is_whole_file(entry):
            if observed != STATE_LOAD_FAILURE:
                failures.append(f"{entry['case']}: STALE known defect {entry['id']} ({entry['issue']}): Tracker now loads this file")
                continue
            text = errors.get(entry["case"], "")
            missing = [signature for signature in entry["tracker"]["load_failure"] if signature not in text]
            if missing:
                failures.append(f"{entry['case']}: CHANGED known defect {entry['id']} ({entry['issue']}): the load failure "
                                f"no longer shows {missing}: {text[-1500:]}")
                continue
        else:
            if observed == STATE_LOAD_FAILURE:
                failures.append(f"{entry['case']}: known defect {entry['id']}: Tracker failed to load the file")
                continue
            actual = resolve_state_path(observed["panel"], entry["object"], entry["path"])
            if not state_matches(entry["tracker"], actual, tolerance):
                state = "STALE" if state_matches(entry["expected"], actual, tolerance) else "CHANGED"
                failures.append(f"{entry['case']}: {state} known defect {entry['id']} ({entry['issue']}) at "
                                f"{entry['path']}: Tracker now reports {json.dumps(actual, ensure_ascii=False)[:400]}")
                continue
        reproduced.append(entry)
    return reproduced


def apply_known_defects(comparison, registry, case_id, actual, expected):
    """In a MoTrack comparison, registered paths must equal the independent value, not Tracker's.

    Only what an entry lists is overridden. Where Tracker has an object at the path, unlisted keys stay
    compared with Tracker; where Tracker has no value (or a non-object), every key MoTrack reports must be
    listed. Listed keys MoTrack does not report are gaps.
    """
    tolerance = comparison.default
    for entry in registry["entries"]:
        if entry["case"] != case_id or is_whole_file(entry):
            continue
        prefix = "panel" + ("" if entry["object"] is None else f".tracks[name={entry['object']['track']}]") + "." + entry["path"]
        tracker = resolve_state_path(expected["panel"], entry["object"], entry["path"])
        partial = isinstance(tracker, dict) and isinstance(entry["expected"], dict)
        covered = [prefix + "." + key for key in entry["expected"]] if partial else [prefix]
        comparison.failures = [(path, message) for path, message in comparison.failures
                               if not any(path == item or path.startswith(item + ".") or path.startswith(item + "[")
                                          for item in covered)]
        top = entry["path"].split(".", 1)[0].split("[", 1)[0]
        owner = actual["panel"] if entry["object"] is None else next(
            (track for track in actual["panel"]["tracks"] if track["name"] == entry["object"]["track"]), None)
        if owner is None or top not in owner:
            comparison.gap(f"known defect {entry['id']}")
            continue
        value = resolve_state_path(actual["panel"], entry["object"], entry["path"])
        if partial and isinstance(value, dict):
            value = {key: item for key, item in value.items() if key in entry["expected"]}
        mismatches, missing = registered_check(entry["expected"], value, tolerance)
        for item in missing:
            comparison.gap(f"known defect {entry['id']}{item}")
        if mismatches:
            comparison.fail(prefix, f"must equal the independent value of known defect {entry['id']} ({entry['issue']}): "
                                    + "; ".join(mismatches)[:400])


def state_report(comparison):
    lines = [message for _, message in comparison.failures[:STATE_REPORT_LIMIT]]
    if len(comparison.failures) > STATE_REPORT_LIMIT:
        lines.append(f"... {len(comparison.failures) - STATE_REPORT_LIMIT} further differences")
    return "\n".join(f"  {comparison.case}: {line}" for line in lines)


# Independent expectations. Each non-whole-file registry value is recomputed here from the saved project
# XML, the media's container tables and documented pinned defaults, never from Tracker's output. Where
# Tracker is not defective, the same derivation must also reproduce Tracker's dump (a self-check).
def osp_property(element, name):
    for item in element.findall("property"):
        if item.get("name") == name:
            return item
    return None


def osp_value(item):
    """Decode an OSP XML property: scalars, arrays (including compact {a,b}), collections, objects."""
    if item is None:
        return None
    kind = item.get("type")
    if kind == "int":
        return int(item.text)
    if kind == "double":
        return float(item.text)
    if kind == "boolean":
        return item.text == "true"
    if kind == "string":
        return item.text or ""
    if kind == "object":
        return item.find("object")
    if kind == "collection":
        return [osp_value(child) for child in item.findall("property")]
    if kind == "array":
        compact = osp_property(item, "array")
        if compact is not None and compact.get("type") == "string":
            return [float(part) if "." in part else int(part) for part in compact.text.strip("{}").split(",") if part]
        values = {int(child.get("name")[1:-1]): osp_value(child) for child in item.findall("property")}
        return [values.get(index) for index in range(max(values) + 1)] if values else []
    raise ValueError(f"Unsupported OSP property type {kind!r}")


def osp_object(element, name):
    value = osp_value(osp_property(element, name))
    return {child.get("name"): osp_value(child) for child in value.findall("property")} if value is not None else None


def project_root(case, cache):
    return ET.fromstring(project_path(case, cache).read_bytes())


def project_tracks(root):
    return {osp_value(osp_property(track, "name")): track for track in osp_value(osp_property(root, "tracks"))}


def fixed_world(root):
    """OSP ImageCoordSystem image-to-world maps for a fully fixed system (angle saved in degrees)."""
    coords = osp_value(osp_property(root, "coords"))
    flags = [osp_value(osp_property(coords, name)) for name in ("fixedorigin", "fixedangle", "fixedscale")]
    data = osp_value(osp_property(coords, "framedata"))
    if flags != [True, True, True] or not data:
        raise ValueError("Independent derivations support fully fixed coordinates only")
    key = {child.get("name"): osp_value(child) for child in data[0].findall("property")}
    angle = math.radians(key["angle"])

    def components(dx, dy):
        x, y = dx / key["xscale"], dy / key["yscale"]
        return math.cos(angle) * x - math.sin(angle) * y, -math.sin(angle) * x - math.cos(angle) * y

    def point(px, py):
        return components(px - key["xorigin"], py - key["yorigin"])
    return point, components


def iso_video_frames(data):
    """Frame start times (ms) of the first video track of an MP4/QuickTime file, from its sample tables.

    Times are decode times accumulated from `stts` deltas. Composition offsets (`ctts`) and edit lists (`elst`)
    are not applied, so this is exact only for constant-rate files whose presentation starts at zero without
    reordering offsets, as the corpus movies used here are; callers must not rely on it for other media.
    """
    def boxes(start, end):
        position = start
        while position + 8 <= end:
            size, kind = struct.unpack(">I4s", data[position:position + 8])
            header = 8
            if size == 1:
                size, header = struct.unpack(">Q", data[position + 8:position + 16])[0], 16
            elif size == 0:
                size = end - position
            if size < header:
                raise ValueError("Malformed media box")
            yield kind, position + header, position + size
            position += size

    def find(start, end, path):
        for kind, body, stop in boxes(start, end):
            if kind == path[0]:
                yield from ([(body, stop)] if len(path) == 1 else find(body, stop, path[1:]))

    for moov, moov_end in find(0, len(data), [b"moov"]):
        for trak, trak_end in find(moov, moov_end, [b"trak"]):
            if next(data[b + 8:b + 12] for b, _ in find(trak, trak_end, [b"mdia", b"hdlr"])) != b"vide":
                continue
            mdhd = next(b for b, _ in find(trak, trak_end, [b"mdia", b"mdhd"]))
            timescale = struct.unpack(">I", data[mdhd + (12 if data[mdhd] == 0 else 20):][:4])[0]
            stts = next(b for b, _ in find(trak, trak_end, [b"mdia", b"minf", b"stbl", b"stts"]))
            times, elapsed = [], 0
            for index in range(struct.unpack(">I", data[stts + 4:stts + 8])[0]):
                count, delta = struct.unpack(">II", data[stts + 8 + 8 * index:stts + 16 + 8 * index])
                for _ in range(count):
                    times.append(elapsed * 1000 / timescale)
                    elapsed += delta
            return times
    raise ValueError("No video track in media")


def derive_known_defect_values(manifest, cache):
    """Independent values for the registry, keyed by entry ID, plus (case, track, path, value) self-checks
    that the same derivations reproduce Tracker where it is not defective."""
    by_id = {case["id"]: case for case in manifest["cases"]}
    values, checks = {}, []

    # #64: saved table views, per-track number formats and the pinned default patterns.
    for case_id in ["table-owners-subset", "table-owners-defaults", "table-owners-legacy-formats", "table-owners"]:
        if case_id not in by_id:
            continue
        root = project_root(by_id[case_id], cache)
        views = osp_value(osp_property(root, "views"))
        table = next((chooser_index, slot) for chooser_index, chooser in enumerate(views or []) for slot in (chooser or [])
                     if slot is not None and slot.get("class") == TRACKER_PACKAGE + "TableTView")
        chooser, element = table
        columns = sorted(osp_value(osp_property(element, "track_columns")), key=lambda row: row[0])
        patterns = {(owner, column): pattern for owner in [row[0] for row in columns]
                    for column, pattern in TABLE_DEFAULT_PATTERNS.items()}
        for group in osp_value(osp_property(element, "column_formats")) or []:
            for owner, column, pattern in group:
                patterns[(owner, column)] = pattern
        for name, track in project_tracks(root).items():
            pairs = osp_value(osp_property(track, "number_formats")) or []
            for index in range(0, len(pairs), 2):
                patterns[(name, pairs[index])] = pairs[index + 1]
        table = {"chooser": chooser, "selected_track": osp_value(osp_property(element, "selected_track")),
                 "track_columns": columns,
                 "column_formats": [[owner, column, patterns[(owner, column)]] for owner, column in sorted(patterns)],
                 "dataset_indices": []}
        if case_id == "table-owners-defaults":
            values[case_id + "-table"] = [table]
        for key in ["track_columns", "selected_track", "column_formats"]:
            values[f"{case_id}-{key.replace('_', '-')}"] = table[key]

    def vector_rows(root, name, times, point, components, frames=None):
        rows = {}
        for frame, sample in enumerate(osp_value(osp_property(project_tracks(root)[name], "framedata"))):
            if sample is None or (frames is not None and frame not in frames):
                continue
            item = {child.get("name"): osp_value(child) for child in sample.findall("property")}
            x, y = components(item["xcomponent"], item["ycomponent"])
            tail = point(item["xtail"], item["ytail"])
            rows[frame] = {"t": times(frame), "x": x, "y": y, "mag": math.hypot(x, y), "θ": math.atan2(y, x),
                           "x_{tail}": tail[0], "y_{tail}": tail[1], "step": frame, "frame": frame}
        return rows

    # #146: negative-time vector rows from the saved samples, calibration and the clip's time base.
    for case_id, prefix in [("vector-images-negative-time", "vector-images-negative-time"),
                            ("vector-movie-negative-time", "vector-movie-negative-time")]:
        if case_id not in by_id:
            continue
        case = by_id[case_id]
        root = project_root(case, cache)
        clip = osp_value(osp_property(root, "videoclip"))
        start = osp_value(osp_property(clip, "starttime"))
        if case_id.startswith("vector-movie"):
            media = local_fixture_path(case).parent / osp_value(osp_property(osp_value(osp_property(clip, "video")), "path"))
            frame_times = iso_video_frames(media.read_bytes())
        else:
            delta = osp_value(osp_property(osp_value(osp_property(clip, "video")), "delta_t"))
            frame_times = [frame * delta for frame in range(64)]
        point, components = fixed_world(root)
        for name, suffix, frame in [("vector A", "a", 0), ("vector B", "b", 1)]:
            rows = vector_rows(root, name, lambda f: (start + frame_times[f]) / 1000, point, components)
            row = rows.pop(frame)
            if suffix == "b":
                # (-1.5, 0) lies on atan2's branch cut: its angle (+/-pi) depends only on the sign of zero.
                del row["θ"]
            values[f"{prefix}-{suffix}"] = row
            checks.extend((case_id, name, f"data.rows[frame={other}]", value) for other, value in rows.items()
                          if (start + frame_times[other]) >= 0)

    # #63: the sum of the named members where every member is marked, at the sum's saved tails.
    if "vector-sum" in by_id:
        derive_vector_sum(project_root(by_id["vector-sum"], cache), values)
    if "ball-toss" in by_id:
        checks.extend(derive_ball_toss(by_id["ball-toss"], cache, values))
    return values, checks


def derive_vector_sum(root, values):
    """#63: the sum of the named members where every member is marked, at the sum's saved tails."""
    tracks = project_tracks(root)
    point, components = fixed_world(root)
    members = osp_value(osp_property(tracks["resultant"], "vectors"))
    saved = osp_value(osp_property(tracks["resultant"], "framedata"))
    samples = {name: {frame: {c.get("name"): osp_value(c) for c in item.findall("property")}
                      for frame, item in enumerate(osp_value(osp_property(tracks[name], "framedata"))) if item is not None}
               for name in members}
    steps, rows = [], []
    for frame, item in enumerate(saved):
        if item is None or not all(frame in samples[name] for name in members):
            continue
        tail_item = {c.get("name"): osp_value(c) for c in item.findall("property")}
        dx = sum(samples[name][frame]["xcomponent"] for name in members)
        dy = sum(samples[name][frame]["ycomponent"] for name in members)
        tail = [tail_item["xtail"], tail_item["ytail"]]
        steps.append({"frame": frame, "tail": tail, "tip": [tail[0] + dx, tail[1] + dy], "components": [dx, dy]})
        x, y = components(dx, dy)
        world_tail = point(*tail)
        # StepperClipControl's default 100 ms frame duration applies: the fixture saves no clip control.
        row = {"t": frame * 0.1, "x": x, "y": y, "mag": math.hypot(x, y), "x_{tail}": world_tail[0],
               "y_{tail}": world_tail[1], "step": frame, "frame": frame}
        if dx or dy:
            row["θ"] = math.atan2(y, x)  # A zero sum has no direction.
        rows.append(row)
    values["vector-sum-members"] = {"items": members}
    values["vector-sum-steps"] = steps
    values["vector-sum-data"] = rows


def derive_ball_toss(case, cache, values):
    """#209: BallToss's MP4 sample table, the saved clip and marks, and Tracker's derivative definitions
    (spill-1 central velocity; five-point acceleration, see #207). Returns its Tracker self-checks."""
    checks = []
    root = project_root(case, cache)
    source = local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"]
    clip = osp_value(osp_property(root, "videoclip"))
    with zipfile.ZipFile(source) as archive:
        frame_times = iso_video_frames(archive.read(osp_value(osp_property(osp_value(osp_property(clip, "video")), "path"))))
    count, first, stride = (osp_value(osp_property(clip, name)) for name in ("stepcount", "startframe", "stepsize"))
    point, _ = fixed_world(root)
    marks = {frame: {c.get("name"): osp_value(c) for c in item.findall("property")}
             for frame, item in enumerate(osp_value(osp_property(project_tracks(root)["ball"], "framedata"))) if item is not None}
    world = {frame: point(mark["x"], mark["y"]) for frame, mark in marks.items()}
    dt = frame_times[1] / 1000
    values["ball-toss-video-frames"] = len(frame_times)
    values["ball-toss-clip-steps"] = count
    values["ball-toss-clip-end"] = first + (count - 1) * stride
    for frame in (11, 12):
        values[f"ball-toss-step-{frame}"] = [frame, frame, frame_times[frame]]
        values[f"ball-toss-row-{frame}"] = {"t": frame_times[frame] / 1000, "step": frame, "frame": frame, "x": world[frame][0],
                                            "y": world[frame][1], "pixel_{x}": marks[frame]["x"], "pixel_{y}": marks[frame]["y"]}

    def velocity(frame):
        return [(world[frame + 1][axis] - world[frame - 1][axis]) / (2 * dt) for axis in (0, 1)]

    def acceleration(frame):
        return [(2 * world[frame - 2][axis] - world[frame - 1][axis] - 2 * world[frame][axis] - world[frame + 1][axis]
                 + 2 * world[frame + 2][axis]) / (7 * dt * dt) for axis in (0, 1)]
    for frame, keys in [(9, "a"), (10, "va")]:
        row = {}
        if "v" in keys:
            vx, vy = velocity(frame)
            row.update({"v_{x}": vx, "v_{y}": vy, "v": math.hypot(vx, vy)})
        ax, ay = acceleration(frame)
        row.update({"a_{x}": ax, "a_{y}": ay, "a": math.hypot(ax, ay)})
        values[f"ball-toss-derivatives-{frame}"] = row
    checks.extend(("ball-toss", "ball", f"data.rows[frame={frame}]",
                   {"x": world[frame][0], "y": world[frame][1], "pixel_{x}": marks[frame]["x"], "pixel_{y}": marks[frame]["y"]})
                  for frame in range(2, 11))
    checks.extend(("ball-toss", "ball", f"data.rows[frame={frame}]", dict(zip(("v_{x}", "v_{y}"), velocity(frame))))
                  for frame in range(3, 10))
    checks.extend(("ball-toss", "ball", f"data.rows[frame={frame}]", dict(zip(("a_{x}", "a_{y}"), acceleration(frame))))
                  for frame in range(4, 9))
    return checks


def check_known_defect_derivations(registry, manifest, cache, dumps, failures):
    """Every registry value must equal its independent derivation, and each derivation must reproduce
    Tracker's dump where Tracker is not defective. Whole-file expectations must hash-match."""
    values, checks = derive_known_defect_values(manifest, cache)
    tolerance = {"absolute": 1e-9, "relative": 1e-9}
    for entry in registry["entries"]:
        if is_whole_file(entry):
            by_id = {case["id"]: case for case in manifest["cases"]}
            digests = {hashlib.sha256(project_path(by_id[case_id], cache).read_bytes()).hexdigest()
                       for case_id in (entry["case"], entry["expected"]["state_of"])}
            if digests != {entry["expected"]["project_sha256"]}:
                failures.append(f"{entry['case']}: whole-file known defect {entry['id']}: the project does not hash-match "
                                f"{entry['expected']['state_of']}")
            continue
        if entry["id"] not in values:
            failures.append(f"{entry['case']}: known defect {entry['id']} has no independent derivation")
            continue
        mismatches, missing = registered_check(values[entry["id"]], entry["expected"], tolerance)
        if mismatches or missing:
            failures.append(f"{entry['case']}: known defect {entry['id']}: registered value differs from its derivation: "
                            + "; ".join(mismatches + [f"missing {item}" for item in missing])[:400])
    for case_id, track, path, value in checks:
        dump = dumps.get(case_id)
        if dump is None or dump == STATE_LOAD_FAILURE:
            continue
        actual = resolve_state_path(dump["panel"], {"track": track}, path)
        if not state_matches(value, actual, {"absolute": 1e-9, "relative": 1e-9}):
            failures.append(f"{case_id}: independent derivation does not reproduce Tracker at {track} {path}: "
                            f"{json.dumps(value)[:200]} vs {json.dumps(actual)[:200]}")


def classify_gap(gap, tracker, motrack):
    """Name why MoTrack does not report a value: the committed baseline records one reason per gap."""
    if gap.startswith("track class "):
        return "unmodeled-class"
    timed = "frame_duration_ms" in motrack["panel"].get("clip", {})
    if gap.startswith("known defect "):
        key = gap.rsplit(".", 1)[-1] if "." in gap else None
        # A registered clip step row holds its time at index 2.
        if (key in STATE_TIMING_COLUMNS or re.search(r"-step-[0-9]+\[2\]$", gap)) and not timed:
            return "default-timing" if tracker["panel"]["video"] is None else "probe-media-timing"
        return "known-defect-unmodeled"
    if gap.startswith("coords.frames after "):
        return "coords-range"
    if gap == "units.length":
        return "default-length-unit"
    column = gap.split(".data.", 1)[1] if ".data." in gap else None
    if gap in {"clip.frame_duration_ms", "clip.steps.time"} or (column in STATE_TIMING_COLUMNS and not timed):
        return "default-timing" if tracker["panel"]["video"] is None else "probe-media-timing"
    if column in {"x", "y", *STATE_TIMING_COLUMNS} and "coords" not in motrack["panel"]:
        return "uncalibrated"
    if column is not None:
        return "unmodeled-column"
    return "unmodeled-field"


def gap_baseline_text(baseline):
    """One case per line, gaps grouped by reason; gaps shared by every case are listed once."""
    dump = lambda value: json.dumps(value, ensure_ascii=False, separators=(",", ":"))  # noqa: E731
    lines = ["{", '  "schema_version": 1,', f'  "reasons": {dump(STATE_GAP_REASONS)},',
             f'  "all_cases": {dump(baseline["all_cases"])},', '  "cases": {']
    cases = baseline["cases"]
    lines += [f"    {json.dumps(case_id)}: {dump(cases[case_id])}{',' if i + 1 < len(cases) else ''}"
              for i, case_id in enumerate(cases)]
    lines += ["  }", "}"]
    return "\n".join(lines) + "\n"


def gap_baseline(gaps):
    """Group per-case {gap: reason} maps into the committed layout."""
    common = set.intersection(*(set(items.items()) for items in gaps.values())) if gaps else set()
    group = lambda pairs: {reason: sorted(gap for gap, why in pairs if why == reason)  # noqa: E731
                           for reason in sorted({why for _, why in pairs})}
    return {"all_cases": group(common),
            "cases": {case_id: group(set(items.items()) - common) for case_id, items in gaps.items()}}


def load_gap_baseline(manifest, path=None):
    source = path or STATE_GAPS
    document = load_state_json(source.read_text(encoding="utf-8"), shown_path(source))
    if (not isinstance(document, dict) or set(document) != {"schema_version", "reasons", "all_cases", "cases"}
            or document["schema_version"] != 1 or document["reasons"] != STATE_GAP_REASONS
            or set(document["cases"]) != {case["id"] for case in manifest["cases"]}):
        raise ValueError(f"{shown_path(source)}: unexpected gap baseline schema, reasons or cases")
    result = {}
    for case_id, groups in document["cases"].items():
        items = {}
        for reason, names in [*document["all_cases"].items(), *groups.items()]:
            if reason not in STATE_GAP_REASONS or not isinstance(names, list):
                raise ValueError(f"{shown_path(source)}: unknown gap reason {reason!r}")
            items.update({name: reason for name in names})
        result[case_id] = items
    return result


def tracker_state_input(case, cache, directory):
    """The file Tracker opens for a case: archives are copied, because Tracker may extract beside them."""
    if "project_member" not in case:
        return project_path(case, cache), None
    source = local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"]
    isolated = directory / "input" / source.name
    isolated.parent.mkdir(parents=True)
    shutil.copyfile(source, isolated)
    verify_file(isolated, case)
    return isolated, case["project_member"]


def check_state_oracle(manifest, cache, java, *, record=False):
    """Dump Tracker's loaded state for every corpus case and diff it with the committed dumps (O3/O4)."""
    if sys.platform != "linux":
        raise ValueError("The Tracker state oracle requires Linux with Xvfb (it uses the application loader)")
    registry = load_known_defects(manifest)
    whole_file = {entry["case"] for entry in registry["entries"] if is_whole_file(entry)}
    for path, artifact in artifacts(manifest, cache):
        verify_file(path, artifact)
    java_path = Path(shutil.which(java) or java).resolve()
    generated = cache / "state-generated"
    if generated.exists():
        shutil.rmtree(generated)
    generated.mkdir(parents=True)
    dumps, errors, failures, compared, bounded = {}, {}, [], 0, []
    with tempfile.TemporaryDirectory(prefix="state-", dir=cache) as temporary, private_display() as display:
        root = Path(temporary)
        classes = root / "classes"
        classes.mkdir()
        everything = [item["file"] for item in manifest["reference"].get("runtime_dependencies", [])]
        run([str(java_path.with_name("javac")), "-encoding", "UTF-8", "-cp", java_classpath(manifest, cache, dependencies=everything),
             "-d", str(classes), *(str(ROOT / "tools/reference" / name) for name in STATE_JAVA_SOURCES)])

        def dump(case, label, polls):
            directory = root / (case["id"] + label)
            home = directory / "java-home"
            home.mkdir(parents=True)
            source, member = tracker_state_input(case, cache, directory)
            output = directory / "state.json"
            log = run([str(java_path), f"-Duser.home={home}", f"-Djava.io.tmpdir={home}", "-Duser.language=en",
                       "-Duser.country=US", f"-Dmotrack.tableVisitPolls={polls}", "-Xmx256m", "-cp",
                       java_classpath(manifest, cache, classes=classes, dependencies=case.get("reference_dependencies", [])),
                       "TrackerStateDump", str(source), str(output), *([member] if member else [])], display=display) or ""
            if output.stat().st_size > MAX_STATE_BYTES:
                raise ValueError(f"{case['id']}: state dump exceeds its size limit")
            document = load_state_json(output.read_text(encoding="utf-8"), f"{case['id']} Tracker dump")
            validate_state_dump(document, manifest, source=f"{case['id']} Tracker dump")
            return document, STATE_TABLE_VISIT.findall(log)

        for case in manifest["cases"]:
            try:
                document, visits = dump(case, "", STATE_TABLE_POLLS)
            except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                dumps[case["id"]], errors[case["id"]] = STATE_LOAD_FAILURE, str(error)
                if case["id"] in whole_file:
                    # Show the dumper's own failure line, so a changed cause is visible in the log.
                    lines = [line.strip() for line in str(error).splitlines()]
                    cause = [line for line in lines if "waiting=" in line or "Exception" in line]
                    frames = [line for line in lines if "org.opensourcephysics." in line][:12]
                    print(f"{case['id']}: Tracker load failed (whole-file known defect): {' | '.join(cause[:2])[:400]}")
                    print(f"{case['id']}: Tracker frames at the failure: " + " | ".join(frames)[:2000])
                else:
                    failures.append(f"{case['id']}: Tracker state dump failed: {str(error)[-3000:]}")
                continue
            dumps[case["id"]] = document
            if visits:
                print(f"{case['id']}: table visits " + ", ".join(f"{owner}: {result} ({polls} polls)"
                                                                for owner, result, polls in visits))
            if any(result == "bounded" for _, result, _ in visits):
                bounded.append(case)
            (generated / (case["id"] + ".json")).write_text(state_text(document), encoding="utf-8", newline="\n")
            if record or case["id"] in whole_file:
                continue
            committed = state_path(case["id"])
            if not committed.is_file():
                failures.append(f"{case['id']}: no committed dump {shown_path(committed)}")
                continue
            comparison = compare_state(read_state(committed, manifest), document, manifest, case["id"])
            if comparison.failures:
                failures.append(f"{case['id']}: Tracker state differs from the committed dump:\n" + state_report(comparison))
            else:
                compared += 1
        # A visit that ended on the poll bound must dump the same state with a ten-times longer bound.
        for case in bounded:
            try:
                longer, _ = dump(case, "-long-visit", STATE_LONG_TABLE_POLLS)
            except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                failures.append(f"{case['id']}: long table-visit dump failed: {str(error)[-1500:]}")
                continue
            comparison = compare_state(dumps[case["id"]], longer, manifest, case["id"] + " (long visit)")
            if comparison.failures:
                failures.append(f"{case['id']}: a {STATE_LONG_TABLE_POLLS}-poll table visit changes the dump:\n"
                                + state_report(comparison))
            else:
                print(f"{case['id']}: bounded table visit confirmed with {STATE_LONG_TABLE_POLLS} polls")
    expected_files = {state_path(case_id) for case_id in dumps if case_id not in whole_file}
    present = set(STATE_DIR.glob("*.json"))
    if not record and present != expected_files:
        failures.append("Committed dumps do not match the corpus cases and whole-file known defects: unexpected "
                        f"{sorted(path.name for path in present - expected_files)}, missing "
                        f"{sorted(path.name for path in expected_files - present)}")
    reproduced = check_known_defects(registry, manifest, dumps, failures, errors)
    check_known_defect_derivations(registry, manifest, cache, dumps, failures)
    for entry in reproduced:
        print(f"{entry['case']}: KNOWN TRACKER DEFECT reproduced: {entry['id']} ({entry['issue']}; MoTrack must match "
              f"the independent value)")
    if failures:
        raise ValueError(f"Tracker state oracle failed ({len(failures)} problems; generated dumps in {generated}):\n"
                         + "\n".join(failures))
    if record:
        if STATE_DIR.exists():
            for stale in STATE_DIR.glob("*.json"):
                stale.unlink()
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        for case_id, document in dumps.items():
            if document != STATE_LOAD_FAILURE:
                state_path(case_id).write_text(state_text(document), encoding="utf-8", newline="\n")
        print(f"state-oracle: recorded {sum(d != STATE_LOAD_FAILURE for d in dumps.values())} Tracker dumps in "
              f"{shown_path(STATE_DIR)}; review every change")
        return
    print(f"state-oracle: {compared} Tracker loaded-state dumps match {shown_path(STATE_DIR)}; "
          f"{len(reproduced)} known defects reproduced and independently derived")


def check_state_native(manifest, cache, probe, *, record=False):
    """Compare MoTrack's loaded state with the committed Tracker dumps for the subset MoTrack models.

    Unmodeled content is a gap, never a silent pass: the per-case gaps must equal the committed baseline
    (tests/corpus/state-gaps.json), so a value MoTrack stops reporting fails, and a closed gap must be
    removed from the baseline with --record.
    """
    registry = load_known_defects(manifest)
    whole_file = {entry["case"]: entry for entry in registry["entries"] if is_whole_file(entry)}
    expected_files = {state_path(case["id"]) for case in manifest["cases"] if case["id"] not in whole_file}
    present = set(STATE_DIR.glob("*.json"))
    if present != expected_files:
        raise ValueError("Committed Tracker state dumps do not match the corpus cases and whole-file known defects: "
                         f"unexpected {sorted(p.name for p in present - expected_files)}, "
                         f"missing {sorted(p.name for p in expected_files - present)}")
    baseline = None if record else load_gap_baseline(manifest)
    for path, artifact in artifacts(manifest, cache):
        verify_file(path, artifact)
    reports = cache / "reports"
    reports.mkdir(parents=True, exist_ok=True)
    failures, gaps, compared = [], {}, 0
    dumps = {}
    for case in manifest["cases"]:
        project = project_path(case, cache)
        output = reports / (case["id"] + "-native-state.json")
        output.unlink(missing_ok=True)
        try:
            run([str(Path(probe).resolve()), "--state", str(project), str(output)])
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            failures.append(f"  {case['id']}: MoTrack state probe failed: {str(error)[-1500:]}")
            continue
        actual = read_state(output, manifest, "motrack")
        source = whole_file[case["id"]]["expected"]["state_of"] if case["id"] in whole_file else case["id"]
        expected = read_state(state_path(source), manifest)
        dumps[case["id"]] = STATE_LOAD_FAILURE if case["id"] in whole_file else expected
        comparison = compare_state(expected, actual, manifest, case["id"], subset=True)
        apply_known_defects(comparison, registry, case["id"], actual, expected)
        gaps[case["id"]] = {gap: classify_gap(gap, expected, actual) for gap in comparison.gaps}
        if baseline is not None:
            known = baseline.get(case["id"], {})
            for gap in sorted(gaps[case["id"]].keys() - known.keys()):
                comparison.fail(f"gap {gap}", f"new gap ({gaps[case['id']][gap]}): MoTrack no longer reports a value "
                                              "the baseline expects it to report")
            for gap in sorted(known.keys() - gaps[case["id"]].keys()):
                comparison.fail(f"gap {gap}", "gap closed: MoTrack now reports this value; update the baseline "
                                              "with state-check --record")
            for gap in sorted(known.keys() & gaps[case["id"]].keys()):
                if known[gap] != gaps[case["id"]][gap]:
                    comparison.fail(f"gap {gap}", f"reason changed from {known[gap]} to {gaps[case['id']][gap]}")
        if comparison.failures:
            failures.append(state_report(comparison))
        else:
            compared += 1
    derivation_failures = []
    check_known_defect_derivations(registry, manifest, cache, dumps, derivation_failures)
    failures += [f"  {failure}" for failure in derivation_failures]
    if failures:
        raise ValueError("MoTrack loaded state differs from Tracker:\n" + "\n".join(failures))
    if record:
        STATE_GAPS.write_text(gap_baseline_text(gap_baseline(gaps)), encoding="utf-8", newline="\n")
        print(f"state-check: recorded the gaps of {len(gaps)} cases in {shown_path(STATE_GAPS)}; review every change")
        return
    counts = {}
    for items in gaps.values():
        for reason in items.values():
            counts[reason] = counts.get(reason, 0) + 1
    print(f"state-check: MoTrack matches Tracker's loaded state for its modeled subset in {compared} of "
          f"{len(manifest['cases'])} cases, and every gap equals the committed baseline "
          f"(whole-file known defects use their independent expectation)")
    print("state-check: gaps by reason: " + ", ".join(f"{reason} {counts[reason]}" for reason in sorted(counts)))


def diff_state_files(manifest, expected_path_, actual_path_, *, subset=False):
    """Ad hoc comparison of two dump files, printing the bounded difference report."""
    expected = read_state(expected_path_, manifest)
    raw = load_state_json(actual_path_.read_text(encoding="utf-8"), shown_path(actual_path_))
    actual_kind = raw.get("producer", {}).get("kind") if isinstance(raw, dict) else None
    actual = read_state(actual_path_, manifest, "motrack" if actual_kind == "motrack" else "tracker")
    comparison = compare_state(expected, actual, manifest, actual_path_.stem, subset=subset or actual_kind == "motrack")
    if comparison.gaps:
        print("gaps: " + ", ".join(comparison.gaps))
    if comparison.failures:
        raise ValueError("State dumps differ:\n" + state_report(comparison))
    print(f"state-diff: {actual_path_.name} matches {expected_path_.name}")


EXPRESSION_REFERENCE = ROOT / "tests/science/expression-reference.json"
EXPRESSION_ORACLE = ROOT / "tools/reference/ExpressionOracle.java"
EXPRESSION_HEADER_KEYS = ["schema_version", "reference", "tolerance", "function_names", "error_messages"]
EXPRESSION_CASE_KEYS = ["id", "group", "expression", "variables", "compare", "defect", "error", "exception"]


def expression_value(bits):
    if not isinstance(bits, str) or not re.fullmatch(r"[0-9a-f]{16}", bits):
        raise ValueError("Invalid expression oracle value bits")
    return struct.unpack(">d", bytes.fromhex(bits))[0]


def expression_ulps(first, second):
    """Distance in representable doubles; signed zeros are one value."""
    def ordered(value):
        bits = struct.unpack(">q", struct.pack(">d", value))[0]
        return -(bits & 0x7FFFFFFFFFFFFFFF) if bits < 0 else bits
    return abs(ordered(first) - ordered(second))


def compare_expression_values(expected, actual, compare, ulps, location):
    if not isinstance(actual, dict) or expected.keys() != actual.keys():
        raise ValueError(f"{location}: evaluation fields differ")
    for key, value in expected.items():
        if key.endswith("_bits"):
            want, got = expression_value(value), expression_value(actual[key])
            if compare == "exact" or not math.isfinite(want) or not math.isfinite(got):
                if value != actual[key] and not (math.isnan(want) and math.isnan(got)):
                    raise ValueError(f"{location}.{key}: expected {want!r}, got {got!r}")
            elif expression_ulps(want, got) > ulps:
                raise ValueError(f"{location}.{key}: {got!r} is more than {ulps} ulp from {want!r}")
        elif key in {"result", "intended"}:
            # Display text follows the compared bits; within the ulp bound it may legitimately differ.
            if expected[key + "_bits"] == actual[key + "_bits"] and value != actual[key]:
                raise ValueError(f"{location}.{key}: display text differs for identical bits")
        elif value != actual[key]:
            raise ValueError(f"{location}.{key}: expected {value!r}, got {actual[key]!r}")


def compare_expression_reference(expected, actual):
    """Compares a regenerated Java expression report with the committed fixture.

    Exact cases (IEEE arithmetic, exact functions and literals) must match bit for bit; cases using Java
    transcendental functions or pow accept the fixture's declared ulp bound, since Math.sin/exp/log/pow
    are only specified to within 1 ulp and their implementation varies by JVM and CPU.
    """
    if not isinstance(actual, dict) or expected.keys() != actual.keys():
        raise ValueError("Expression oracle report fields differ")
    for key in EXPRESSION_HEADER_KEYS:
        if expected[key] != actual[key]:
            raise ValueError(f"Expression oracle {key} differs")
    ulps = expected["tolerance"]["ulp"]
    if type(ulps) is not int or not 0 < ulps <= 16:
        raise ValueError("Invalid expression ulp tolerance")
    if len(expected["cases"]) != len(actual["cases"]):
        raise ValueError("Expression oracle case count differs")
    for want, got in zip(expected["cases"], actual["cases"]):
        location = f"expression {want['id']}"
        if want.keys() != got.keys() or any(want[key] != got[key] for key in EXPRESSION_CASE_KEYS):
            raise ValueError(f"{location}: case definition, error or exception differs")
        if want["compare"] not in {"exact", "ulp", "random"} or len(want["evaluations"]) != len(got["evaluations"]):
            raise ValueError(f"{location}: evaluation count or comparison differs")
        for index, evaluation in enumerate(want["evaluations"]):
            compare_expression_values(evaluation, got["evaluations"][index], want["compare"], ulps, f"{location}[{index}]")


@kernel_oracle("expression-oracle")
def check_expression_oracle(manifest, cache, java, record=False):
    """Regenerates the expression kernel oracle with the pinned Tracker JAR and diffs it with the fixture."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # The kernel oracle needs no corpus files; keep its JVM home and report out of the shared cache.
    with tempfile.TemporaryDirectory(prefix="motrack-expression-oracle-") as directory:
        home = Path(directory)
        output = home / "expression-oracle.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}", "-Xmx256m",
             "--class-path", java_classpath(manifest, cache), str(EXPRESSION_ORACLE), str(output)])
        text = output.read_text(encoding="utf-8")
    actual = json.loads(text)
    if record:
        EXPRESSION_REFERENCE.write_text(text, encoding="utf-8", newline="\n")
        print(f"Recorded {len(actual['cases'])} expression cases from Tracker {actual['reference']['tracker_version']}")
        return
    compare_expression_reference(json.loads(EXPRESSION_REFERENCE.read_text(encoding="utf-8")), actual)
    evaluations = sum(len(case["evaluations"]) for case in actual["cases"])
    print(f"Expression oracle: {len(actual['cases'])} cases and {evaluations} evaluations match the fixture"
          f" (SuryonoParser, Tracker {actual['reference']['tracker_version']})")


COMMITTED_FIXTURES = ROOT / "tests/fixtures"
OSP_FIXTURES = COMMITTED_FIXTURES / "osp"
XML_DUMP_SCHEMA = "motrack.osp-xmlcontrol-dump/1"
XML_SUFFIXES = {".trk", ".xml"}
ARCHIVE_SUFFIXES = {".trz", ".zip"}
MAX_XML_ORACLE_DOCUMENTS = 4096
MAX_ARCHIVE_MEMBERS = 4096


def xml_oracle_variants(data):
    """Line-ending and byte-order-mark variants that OSP's line reader must read identically."""
    lf = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return {"lf": lf, "crlf": lf.replace(b"\n", b"\r\n"), "cr": lf.replace(b"\n", b"\r"),
            "bom-crlf": b"\xef\xbb\xbf" + lf.replace(b"\n", b"\r\n")}


def xml_documents(path, label):
    """Yield (label, bytes) for an XML document, or for every XML member of an archive, read in memory only."""
    suffix = path.suffix.lower()
    if suffix in ARCHIVE_SUFFIXES:
        with zipfile.ZipFile(path) as archive:
            entries = archive.infolist()
            if len(entries) > MAX_ARCHIVE_MEMBERS:
                raise ValueError(f"{label}: archive has too many members")
            names = [entry.filename for entry in entries]
            for entry in entries:
                if entry.is_dir() or PurePosixPath(entry.filename).suffix.lower() not in XML_SUFFIXES:
                    continue
                if names.count(entry.filename) != 1:
                    raise ValueError(f"{label}: ambiguous archive member {entry.filename[:120]!r}")
                if entry.file_size > MAX_XML_BYTES:
                    raise ValueError(f"{label}: archive member {entry.filename!r} exceeds the XML size limit")
                # Never extract member paths onto the filesystem.
                with archive.open(entry) as stream:
                    data = stream.read(MAX_XML_BYTES + 1)
                if len(data) > MAX_XML_BYTES:
                    raise ValueError(f"{label}: expanded member {entry.filename!r} exceeds the XML size limit")
                yield f"{label}!{entry.filename}", data
    elif suffix in XML_SUFFIXES:
        if path.stat().st_size > MAX_XML_BYTES:
            raise ValueError(f"{label}: document exceeds the XML size limit")
        yield label, path.read_bytes()


def xml_oracle_inputs(manifest, cache):
    """Every corpus case file and XML archive member, the committed fixtures (library documents included), and the
    synthetic OSP reader fixtures with their line-ending variants, each distinct byte sequence once."""
    sources = []
    for case in manifest["cases"]:
        path = local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"]
        sources.append((case["id"], path, False))
    sources.extend((f"fixtures/{path.name}", path, False) for path in sorted(COMMITTED_FIXTURES.iterdir()) if path.is_file())
    sources.extend((f"osp/{path.name}", path, True) for path in sorted(OSP_FIXTURES.glob("*.xml")))
    sources.extend((f"osp/generated/{path.name}", path, True) for path in sorted((OSP_FIXTURES / "generated").glob("*.xml")))
    # Library collections and libraries are OSP documents too (the library oracle checks their loaded model).
    library = COMMITTED_FIXTURES / "library"
    sources.extend((f"library/{path.relative_to(library).as_posix()}", path, False) for path in sorted(library.rglob("*.xml")))
    documents, seen = [], set()
    for label, path, variants in sources:
        for name, data in xml_documents(path, label):
            for variant, payload in (xml_oracle_variants(data).items() if variants else [("", data)]):
                digest = hashlib.sha256(payload).hexdigest()
                if digest in seen:
                    continue
                seen.add(digest)
                documents.append((f"{name}[{variant}]" if variant else name, payload))
                if len(documents) > MAX_XML_ORACLE_DOCUMENTS:
                    raise ValueError("Too many XML oracle documents")
    return documents


def first_json_difference(expected, actual, path="$"):
    """Path of the first difference between two decoded JSON values, or None. Types must match exactly."""
    if type(expected) is not type(actual):
        return f"{path} (type)"
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            return f"{path} (keys {sorted(set(expected) ^ set(actual))})"
        for key in expected:
            if difference := first_json_difference(expected[key], actual[key], f"{path}.{key}"):
                return difference
        return None
    if isinstance(expected, list):
        if len(expected) != len(actual):
            return f"{path} (length {len(expected)} != {len(actual)})"
        for index, (left, right) in enumerate(zip(expected, actual)):
            if difference := first_json_difference(left, right, f"{path}[{index}]"):
                return difference
        return None
    return None if expected == actual else f"{path} ({expected!r} != {actual!r})"


def check_generated_documents(generated):
    """The committed OSP-written fixtures must equal what the pinned OSP writer produces now."""
    committed = OSP_FIXTURES / "generated"
    expected = sorted(path.name for path in committed.glob("*.xml"))
    actual = sorted(path.name for path in generated.glob("*.xml"))
    if expected != actual:
        raise ValueError(f"OSP writer documents differ from {committed}: committed {expected}, generated {actual}")
    for name in actual:
        if (committed / name).read_bytes() != (generated / name).read_bytes():
            raise ValueError(f"{committed / name} differs from OSP's writer output; "
                             "regenerate it with tools/reference/XmlControlDump.java --generate")


# Documents OSP reads but MoTrack deliberately refuses. The probe still dumps the tree it would read (marked
# "motrack_rejects"), so O1 compares it with Java's; any other rejection reason fails the oracle.
XML_DELIBERATE_REJECTIONS = {
    "truncated": "MoTrack #243: a document that ends before its objects close is refused; OSP loads the partial tree",
}


def check_xml_oracle(manifest, cache, java, probe, reports=None, o2_only=False):
    """O1: MoTrack's OSP tree equals the pinned Java XMLControlElement tree. O2: the probe also requires an
    unedited render to reproduce every document byte for byte. `o2_only` runs O2 without Java (all platforms)."""
    for path, artifact in artifacts(manifest, cache):
        verify_file(path, artifact)
    documents = xml_oracle_inputs(manifest, cache)
    reports = Path(reports if reports is not None else cache / "reports" / "xml-oracle").resolve()
    reports.mkdir(parents=True, exist_ok=True)
    mismatches, deliberate, read = [], [], 0
    with tempfile.TemporaryDirectory(prefix="motrack-xml-oracle-") as directory:
        work = Path(directory)
        home = work / "java-home"
        home.mkdir()
        java_command = [java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}", "-Xmx256m",
                        "--class-path", java_classpath(manifest, cache), str(ROOT / "tools/reference/XmlControlDump.java")]
        if not o2_only:
            run(java_command + ["--generate", str(work / "generated")], timeout=600)
            check_generated_documents(work / "generated")
        pairs = []
        for index, (label, data) in enumerate(documents):
            source = work / f"{index:04d}.xml"
            source.write_bytes(data)
            pairs.append((label, source, work / f"{index:04d}-java.json", work / f"{index:04d}-native.json"))
        if not o2_only:
            listing = work / "pairs.txt"
            listing.write_text("".join(f"{source}\t{output}\n" for _, source, output, _ in pairs), encoding="utf-8")
            run(java_command + [str(listing)], timeout=600)
        for index, (label, source, java_output, native_output) in enumerate(pairs):
            # The probe itself fails unless an unedited render reproduces the document (O2).
            run([str(Path(probe).resolve()), "--osp-dump", str(source), str(native_output)])
            actual = json.loads(native_output.read_text(encoding="utf-8"))
            if actual.get("schema") != XML_DUMP_SCHEMA:
                raise ValueError(f"{label}: unexpected XML dump schema")
            rejection = actual.pop("motrack_rejects", None)
            if rejection is not None and rejection not in XML_DELIBERATE_REJECTIONS:
                mismatches.append(f"{label}: unregistered MoTrack rejection {rejection!r}")
                continue
            if o2_only:
                if rejection is not None:
                    deliberate.append(f"{label}: {rejection}")
                else:
                    read += actual["read"] is True
                continue
            expected = json.loads(java_output.read_text(encoding="utf-8"))
            if expected.get("schema") != XML_DUMP_SCHEMA:
                raise ValueError(f"{label}: unexpected XML dump schema")
            if difference := first_json_difference(expected, actual):
                mismatches.append(f"{label}: {difference}")
                for output in (java_output, native_output):
                    shutil.copyfile(output, reports / f"{index:04d}-{output.name.split('-', 1)[1]}")
            elif rejection is not None:
                deliberate.append(f"{label}: {rejection}")
            else:
                read += expected["read"] is True
    summary = {"schema": XML_DUMP_SCHEMA, "oracles": "O2" if o2_only else "O1+O2", "documents": len(documents),
               "read_by_both": read, "deliberate_differences": deliberate,
               "rejected_by_both": len(documents) - read - len(mismatches) - len(deliberate), "mismatches": mismatches}
    (reports / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if mismatches:
        raise ValueError(f"{summary['oracles']} failed for {len(mismatches)} of {len(documents)} documents:\n" + "\n".join(mismatches[:20]))
    if o2_only:
        print(f"xml-oracle --o2-only: {len(documents)} documents rendered byte-identically (O2); {len(deliberate)} truncated "
              f"documents refused as registered differences; {summary['rejected_by_both']} rejected")
        return
    print(f"xml-oracle: {len(documents)} documents matched the Java XMLControlElement tree (O1) and rendered "
          f"byte-identically (O2); {len(deliberate)} are registered deliberate differences; "
          f"{summary['rejected_by_both']} were rejected by both readers")


ODE_REFERENCE = ROOT / "tests/science/ode-reference.json"
# The same agreement classes as tests/science/ode_solver_test.cpp, which justifies them. Fixed-step
# solvers are pure IEEE arithmetic and must be identical. Adaptive solvers call Math.pow, whose
# last bit differs between HotSpot's x86-64 intrinsic, its arm64 fdlibm path and C++ libraries;
# the step-size controller carries that forward, most strongly at the stiff system's stability limit.
ODE_EXACT = {"relative": 0.0, "absolute": 0.0}
ODE_ADAPTIVE_TOLERANCE = {"relative": 1e-8, "absolute": 1e-11}
ODE_STIFF_ADAPTIVE_TOLERANCE = {"relative": 1e-6, "absolute": 1e-10}
MAX_ODE_CASES = 1000


def ode_case_tolerance(case, adaptive):
    if not adaptive[case["solver"]]:
        return ODE_EXACT
    return ODE_STIFF_ADAPTIVE_TOLERANCE if case["system"] == "stiff" else ODE_ADAPTIVE_TOLERANCE


def validate_ode_reference(document, manifest):
    """Check the kernel-oracle schema before comparing or recording it."""
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or document.get("reference_version") != manifest["reference"]["version"]):
        raise ValueError("Unexpected ODE oracle schema or reference version")
    solvers, systems, cases = document.get("solvers"), document.get("systems"), document.get("cases")
    if (not isinstance(solvers, list) or not isinstance(systems, list) or not isinstance(cases, list)
            or not cases or len(cases) > MAX_ODE_CASES):
        raise ValueError("Invalid ODE oracle solver, system or case list")
    solver_ids = [solver.get("id") for solver in solvers]
    system_ids = [system.get("id") for system in systems]
    if len(set(solver_ids)) != len(solver_ids) or len(set(system_ids)) != len(system_ids):
        raise ValueError("Duplicate ODE oracle solver or system")
    seen = set()
    for case in cases:
        if not isinstance(case, dict):
            raise ValueError("Invalid ODE oracle case")
        steps, checkpoints = case.get("steps"), case.get("checkpoints")
        if (not isinstance(case.get("id"), str) or case["id"] in seen or not isinstance(steps, int)
                or not isinstance(checkpoints, list) or not checkpoints
                or case.get("solver") not in solver_ids or case.get("system") not in system_ids
                or any(not isinstance(point, dict) or type(point.get("n")) is not int or not 0 <= point["n"] <= steps
                       for point in checkpoints)):
            raise ValueError(f"Invalid or duplicate ODE oracle case {case.get('id')!r}")
        seen.add(case["id"])
    if {case["solver"] for case in cases} != set(solver_ids):
        raise ValueError("Every ODE solver needs an oracle case")


def ode_reference_text(document):
    """Render the fixture with one case per line so that review diffs stay readable."""
    compact = {"separators": (",", ":"), "ensure_ascii": False}
    lines = ["{"]
    lines.extend(f"  {json.dumps(key)}: {json.dumps(value, **compact)}," for key, value in document.items() if key != "cases")
    lines.append('  "cases": [')
    cases = document["cases"]
    lines.extend(f"    {json.dumps(case, **compact)}{',' if index + 1 < len(cases) else ''}" for index, case in enumerate(cases))
    lines.extend(["  ]", "}"])
    return "\n".join(lines) + "\n"


@kernel_oracle("ode-oracle")
def check_ode_oracle(manifest, cache, java, *, record=False):
    """Integrate the ODE kernel test systems with the pinned OSP solvers and diff the fixture."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # A private home keeps OSP preference files out of the shared reference cache.
    with tempfile.TemporaryDirectory(prefix="ode-oracle-") as temporary:
        home = Path(temporary) / "home"
        home.mkdir()
        output = Path(temporary) / "ode-java.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}",
             "-Xmx256m", "--class-path", java_classpath(manifest, cache),
             str(ROOT / "tools/reference/OdeOracle.java"), str(output)])
        actual = json.loads(output.read_text(encoding="utf-8"))
    validate_ode_reference(actual, manifest)
    location = ODE_REFERENCE.relative_to(ROOT).as_posix() if ODE_REFERENCE.is_relative_to(ROOT) else str(ODE_REFERENCE)
    if record:
        ODE_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        ODE_REFERENCE.write_text(ode_reference_text(actual), encoding="utf-8", newline="\n")
        print(f"ode-oracle: recorded {len(actual['cases'])} Java solver cases in {location}")
        return
    expected = json.loads(ODE_REFERENCE.read_text(encoding="utf-8"))
    validate_ode_reference(expected, manifest)
    compare_ode_reference(expected, actual)
    print(f"ode-oracle: {len(actual['cases'])} Java solver cases match {location}")


def compare_ode_reference(expected, actual):
    """Compare two validated oracle documents with the per-solver agreement classes."""
    compare({key: value for key, value in expected.items() if key != "cases"},
            {key: value for key, value in actual.items() if key != "cases"}, ODE_EXACT, "ode-reference")
    if [case["id"] for case in expected["cases"]] != [case["id"] for case in actual["cases"]]:
        raise ValueError("ode-reference: the Java case list differs from the fixture")
    adaptive = {solver["id"]: solver["adaptive"] for solver in expected["solvers"]}
    for want, got in zip(expected["cases"], actual["cases"]):
        compare(want, got, ode_case_tolerance(want, adaptive), f"ode-reference.{want['id']}")


LIBRARY_FIXTURES = COMMITTED_FIXTURES / "library"


LIBRARY_DUMP_SCHEMA = "motrack.library-dump/1"


LIBRARY_SUFFIXES = {".xml", ".rest"}


# Documents where MoTrack deliberately differs from Tracker 6.3.5 (docs/library-collections.md). Each entry must
# still differ in the stated way, so a stale entry fails instead of hiding a regression.
LIBRARY_KNOWN_DIFFERENCES = {
    "collection-resources-arraylist.xml":
        "Tracker 6.3.5 throws ClassCastException for resources stored as an ArrayList; MoTrack reads them as OSP's "
        "swingJS tip does",
}


# The deepest nesting motrack_osp's depth limit admits (tests/library_limits_test.cpp derives it from the reader).
LIBRARY_DEEPEST_NESTING = 85


def nested_collections(levels):
    """Collections nested `levels` deep in OSP's layout, ending in a resource (as in library_limits_test.cpp)."""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>']
    indent = ""
    for level in range(levels):
        lines += [indent + '<object class="org.opensourcephysics.tools.LibraryCollection">',
                  indent + f'    <property name="name" type="string">Level {level}</property>',
                  indent + '    <property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">',
                  indent + '        <property name="[0]" type="object">']
        indent += "        "
    lines += [indent + '<object class="org.opensourcephysics.tools.LibraryResource">',
              indent + '    <property name="name" type="string">Leaf</property>',
              indent + '    <property name="target" type="string">leaf.trk</property>',
              indent + "</object>"]
    for _ in range(levels):
        indent = indent[:-8]
        lines += [indent + "        </property>", indent + "    </property>", indent + "</object>"]
    return ("\n".join(lines) + "\n").encode("utf-8")


def library_synthetic_documents():
    """Documents too large or too regular to commit: the deepest nesting Tracker and MoTrack both open, and an
    element index that makes Java allocate a large array while MoTrack stores one slot."""
    yield "synthetic/deep-nesting.xml", nested_collections(LIBRARY_DEEPEST_NESTING)
    yield "synthetic/large-index.xml", (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<object class="org.opensourcephysics.tools.LibraryCollection">\n'
        '    <property name="name" type="string">Large index</property>\n'
        '    <property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">\n'
        '        <property name="[200000]" type="object">\n'
        '        <object class="org.opensourcephysics.tools.LibraryResource">\n'
        '            <property name="name" type="string">Far away</property>\n'
        '        </object>\n'
        '        </property>\n'
        '    </property>\n'
        '</object>\n').encode("utf-8")


def library_kind(data):
    """The LibraryDump kind of a document: ComPADRE responses by their element names, OSP documents by root class."""
    if b"<object class=" not in data[:4096]:
        return "compadre-tree" if b"<Identify" in data else "compadre-records"
    return "library" if b'class="org.opensourcephysics.tools.Library"' in data[:4096] else "collection"


def library_oracle_inputs(extra=None):
    """Every committed library fixture in four line-ending variants, plus optional local documents (for example a
    saved ComPADRE response) as they are."""
    sources = [(path.relative_to(LIBRARY_FIXTURES).as_posix(), path, True)
               for path in sorted(LIBRARY_FIXTURES.rglob("*")) if path.is_file() and path.suffix in LIBRARY_SUFFIXES]
    if extra is not None:
        sources.extend((f"extra/{path.name}", path, False)
                       for path in sorted(Path(extra).iterdir()) if path.is_file() and path.suffix in LIBRARY_SUFFIXES)
    documents, seen = [], set()
    loaded = [(label, path.read_bytes(), variants) for label, path, variants in sources]
    loaded += [(label, data, False) for label, data in library_synthetic_documents()]
    for label, data, variants in loaded:
        if len(data) > MAX_XML_BYTES:
            raise ValueError(f"{label}: document exceeds the XML size limit")
        for variant, payload in (xml_oracle_variants(data).items() if variants else [("", data)]):
            digest = hashlib.sha256(payload).hexdigest()
            if digest in seen:
                continue
            seen.add(digest)
            documents.append((label, f"{label}[{variant}]" if variant else label, library_kind(payload), payload))
            if len(documents) > MAX_XML_ORACLE_DOCUMENTS:
                raise ValueError("Too many library oracle documents")
    return documents


def check_library_generated(generated):
    """The committed documents written by OSP's own library loaders must equal a fresh run of the pinned writer."""
    committed = LIBRARY_FIXTURES / "generated"
    expected = sorted(path.name for path in committed.glob("*.xml"))
    actual = sorted(path.name for path in generated.glob("*.xml"))
    if expected != actual:
        raise ValueError(f"OSP library documents differ from {committed}: committed {expected}, generated {actual}")
    for name in actual:
        if (committed / name).read_bytes() != (generated / name).read_bytes():
            raise ValueError(f"{committed / name} differs from OSP's writer output; "
                             "regenerate it with tools/reference/LibraryDump.java --generate")


def check_library_oracle(manifest, cache, java, probe, reports=None, extra=None):
    """Library oracle: MoTrack's collection, Library and ComPADRE models equal what the pinned OSP classes load,
    MoTrack writes what OSP's writer writes for the loaded objects, and unedited saves are byte-identical."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    documents = library_oracle_inputs(extra)
    reports = Path(reports if reports is not None else cache / "reports" / "library-oracle").resolve()
    reports.mkdir(parents=True, exist_ok=True)
    mismatches, differences, loaded = [], [], 0
    with tempfile.TemporaryDirectory(prefix="motrack-library-oracle-") as directory:
        work = Path(directory)
        home = work / "java-home"
        home.mkdir()
        java_command = [java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}", "-Xmx256m",
                        "--class-path", java_classpath(manifest, cache), str(ROOT / "tools/reference/LibraryDump.java")]
        run(java_command + ["--generate", str(work / "generated")], timeout=600)
        check_library_generated(work / "generated")
        items = []
        for index, (name, label, kind, data) in enumerate(documents):
            source = work / f"{index:04d}{Path(name).suffix}"
            source.write_bytes(data)
            items.append((name, label, kind, source, work / f"{index:04d}-java.json", work / f"{index:04d}-native.json"))
        listing = work / "pairs.txt"
        listing.write_text("".join(f"{kind}\t{source}\t{output}\n" for _, _, kind, source, output, _ in items), encoding="utf-8")
        run(java_command + [str(listing)], timeout=600)
        for index, (name, label, kind, source, java_output, native_output) in enumerate(items):
            run([str(Path(probe).resolve()), "--library-dump", kind, str(source), str(native_output)])
            expected = json.loads(java_output.read_text(encoding="utf-8"))
            actual = json.loads(native_output.read_text(encoding="utf-8"))
            if expected.get("schema") != LIBRARY_DUMP_SCHEMA or actual.get("schema") != LIBRARY_DUMP_SCHEMA:
                raise ValueError(f"{label}: unexpected library dump schema")
            expected.pop("error", None)  # Java's exception class; MoTrack reports the location instead.
            difference = first_json_difference(expected, actual)
            known = LIBRARY_KNOWN_DIFFERENCES.get(Path(name).name)
            if known is not None:
                if difference is None or expected.get("loaded") is not False or actual.get("loaded") is not True:
                    mismatches.append(f"{label}: stale known difference ({known})")
                else:
                    differences.append(f"{label}: {known}")
                continue
            if difference:
                mismatches.append(f"{label}: {difference}")
                for output in (java_output, native_output):
                    shutil.copyfile(output, reports / f"{index:04d}-{output.name.split('-', 1)[1]}")
            else:
                loaded += expected.get("loaded") is True
    summary = {"schema": LIBRARY_DUMP_SCHEMA, "documents": len(documents), "loaded_by_both": loaded,
               "known_differences": differences, "mismatches": mismatches}
    (reports / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if mismatches:
        raise ValueError(f"Library oracle failed for {len(mismatches)} of {len(documents)} documents:\n" + "\n".join(mismatches[:20]))
    print(f"library-oracle: {len(documents)} documents matched the pinned OSP library classes ({loaded} loaded by both, "
          f"{len(differences)} known differences)")

FIT_REFERENCE = ROOT / "tests/science/fit-reference.json"


FIT_ORACLE = ROOT / "tools/reference/FitOracle.java"


FIT_BUILT_INS = ["line", "parabola", "cubic", "gaussian", "exponential", "sinusoid", "dampedsine", "power", "log"]


FIT_AGREEMENTS = {"exact", "polynomial", "transcendental", "chaotic"}


FIT_DEFECTS = {"duplicate-x-initial-guess", "dataset-append-duplicates", "dataset-capacity", "singular-polynomial",
               "non-finite-initial-guess", "normal-equation-conditioning", "text-substitution"}


FIT_HEADER_KEYS = ["schema_version", "reference_version", "fits", "user_fits", "minimizers", "format"]


FIT_RESULT_KEYS = {"points", "valid_x", "method", "parameters", "uncertainties", "band", "intended_band", "rms", "correlation",
                   "non_real", "formatted", "start", "tied"}


FIT_METHODS = {"none", "closed-form", "hessian", "levenberg-marquardt", "restored"}


# Main-fit paths that minimize; for a tied fit (ending at its starting deviation) last bits choose among them.
FIT_MINIMIZER_PATHS = {"hessian", "levenberg-marquardt", "restored"}


MAX_FIT_CASES = 1000


# The agreement classes of tests/science/curve_fit_test.cpp (docs/curve-fitting.md#tolerances). Fits using
# only + - * / are bitwise; Math.exp/sin/log/pow differ in the last bit between HotSpot intrinsics on
# x86-64 and arm64 and are amplified by the finite-difference Hessian. A quantity that moves by more than
# 1e-3 when every y changes by one ulp ("sensitivity") is reproducible only bit for bit.
FIT_EXACT = {"relative": 0.0, "absolute": 0.0}


FIT_PARAMETER_TOLERANCE = {"relative": 1e-7, "absolute": 1e-9}


FIT_UNCERTAINTY_TOLERANCE = {"relative": 1e-4, "absolute": 1e-9}


FIT_EXPECT_TOLERANCE = {"relative": 1e-9, "absolute": 1e-12}


FIT_SENSITIVITY_LIMIT = 1e-3


def fit_number(value):
    """Fixture numbers are JSON numbers or the strings NaN, Infinity and -Infinity."""
    if isinstance(value, str) and value in {"NaN", "Infinity", "-Infinity"}:
        return float(value.replace("Infinity", "inf"))
    if type(value) not in {int, float}:
        raise ValueError(f"Invalid fit oracle number {value!r}")
    return float(value)


def validate_fit_reference(document, manifest):
    """Check the curve-fit oracle schema before comparing or recording it."""
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or document.get("reference_version") != manifest["reference"]["version"]):
        raise ValueError("Unexpected fit oracle schema or reference version")
    fits, cases = document.get("fits"), document.get("cases")
    if (not isinstance(fits, list) or [fit.get("id") if isinstance(fit, dict) else None for fit in fits] != FIT_BUILT_INS
            or not isinstance(cases, list) or not cases or len(cases) > MAX_FIT_CASES
            or not isinstance(document.get("user_fits"), list) or not isinstance(document.get("minimizers"), list)
            or not isinstance(document.get("format"), list)):
        raise ValueError("Invalid fit oracle fit, case, minimizer or format list")
    user_ids = {user.get("id") for user in document["user_fits"] if isinstance(user, dict)}
    seen = set()
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("id"), str) or case["id"] in seen:
            raise ValueError(f"Invalid or duplicate fit oracle case {case.get('id') if isinstance(case, dict) else case!r}")
        seen.add(case["id"])
        java = case.get("java")
        data_ok = ("generated" in case) != ("x" in case and "y" in case)
        if (case.get("fit") not in FIT_BUILT_INS and case.get("fit") not in user_ids
                or case.get("agreement") not in FIT_AGREEMENTS or not data_ok
                or ("defect" in case and case["defect"] not in FIT_DEFECTS)
                or not isinstance(java, dict) or set(java) - {"sensitivity"} != FIT_RESULT_KEYS
                or java.get("method") not in FIT_METHODS
                or ("sensitivity" in java) != (case["agreement"] != "exact")
                or ("intended" in case) != ("intended_fit" in case)):
            raise ValueError(f"Invalid fit oracle case {case['id']!r}")
        if "x" in case and len(case["x"]) != len(case["y"]):
            raise ValueError(f"Fit oracle case {case['id']!r} has unpaired data")
    if not set(FIT_BUILT_INS) <= {case["fit"] for case in cases}:
        raise ValueError("Every built-in fit needs an oracle case")


def fit_reference_text(document):
    """Render the fixture with one case per line so that review diffs stay readable."""
    compact = {"separators": (",", ":"), "ensure_ascii": True}
    lines = ["{"]
    lines.extend(f"  {json.dumps(key)}: {json.dumps(value, **compact)}," for key, value in document.items() if key != "cases")
    lines.append('  "cases": [')
    cases = document["cases"]
    lines.extend(f"    {json.dumps(case, **compact)}{',' if index + 1 < len(cases) else ''}" for index, case in enumerate(cases))
    lines.extend(["  ]", "}"])
    return "\n".join(lines) + "\n"


def compare_fit_values(expected, actual, tolerance, location):
    if isinstance(expected, list) or isinstance(actual, list):
        if not isinstance(expected, list) or not isinstance(actual, list) or len(expected) != len(actual):
            raise ValueError(f"{location}: list length differs")
        for index, value in enumerate(expected):
            compare_fit_values(value, actual[index], tolerance, f"{location}[{index}]")
        return
    if expected is None or actual is None:
        if expected is not actual:
            raise ValueError(f"{location}: expected {expected!r}, got {actual!r}")
        return
    want, got = fit_number(expected), fit_number(actual)
    if math.isnan(want) or math.isnan(got) or math.isinf(want) or math.isinf(got):
        if not (want == got or (math.isnan(want) and math.isnan(got))):
            raise ValueError(f"{location}: expected {want!r}, got {got!r}")
    elif tolerance is FIT_EXACT:
        if want != got or math.copysign(1, want) != math.copysign(1, got):
            raise ValueError(f"{location}: expected {want!r}, got {got!r} (bitwise)")
    elif abs(want - got) > tolerance["absolute"] + tolerance["relative"] * max(abs(want), abs(got)):
        raise ValueError(f"{location}: {got!r} differs from {want!r} beyond {tolerance}")


def fit_result_tolerances(case, result):
    """Per-quantity tolerances of a Java result, or None for a quantity that last bits decide."""
    agreement = case["agreement"]
    if agreement == "exact":
        return {"parameters": FIT_EXACT, "rms": FIT_EXACT, "uncertainties": FIT_EXACT}
    sensitivity = result.get("sensitivity", {})

    def relaxed(base, key):
        moved = sensitivity.get(key, 0.0)
        if moved > FIT_SENSITIVITY_LIMIT:
            return None
        return {"relative": max(base["relative"], 100 * moved), "absolute": base["absolute"]}

    if agreement == "chaotic":
        return {"parameters": None, "rms": None, "uncertainties": None}
    uncertainties = relaxed(FIT_UNCERTAINTY_TOLERANCE, "uncertainties")
    if agreement == "polynomial":
        return {"parameters": FIT_EXACT, "rms": FIT_EXACT, "uncertainties": uncertainties}
    return {"parameters": relaxed(FIT_PARAMETER_TOLERANCE, "parameters"), "rms": relaxed(FIT_PARAMETER_TOLERANCE, "rms"),
            "uncertainties": uncertainties}


def compare_fit_result(case, expected, actual, location):
    if not isinstance(actual, dict) or set(expected) != set(actual):
        raise ValueError(f"{location}: result fields differ")
    tolerances = fit_result_tolerances(case, expected)
    for key in ["points", "valid_x", "correlation"]:
        compare_fit_values(expected[key], actual[key], FIT_EXACT, f"{location}.{key}")
    # The start precedes any minimization: bitwise for arithmetic fits, a from-scratch guess otherwise.
    if (expected["start"] is None) != (actual["start"] is None) and case["agreement"] != "chaotic":
        raise ValueError(f"{location}.start: only one run minimized")
    if expected["start"] is not None and actual["start"] is not None:
        start = FIT_EXACT if case["agreement"] == "exact" else FIT_PARAMETER_TOLERANCE
        compare_fit_values(expected["start"]["parameters"], actual["start"]["parameters"], start, f"{location}.start")
        compare_fit_values(expected["start"]["ssr"], actual["start"]["ssr"], start, f"{location}.start.ssr")
    groups = {"parameters": ["parameters"], "rms": ["rms"], "uncertainties": ["uncertainties", "band", "intended_band"]}
    for group, keys in groups.items():
        if tolerances[group] is not None:
            for key in keys:
                compare_fit_values(expected[key], actual[key], tolerances[group], f"{location}.{key}")
    if case["agreement"] != "chaotic":
        # A tied fit ends at the deviation it started from (a start already at the minimum): Newton's result, the
        # Levenberg-Marquardt fallback and the restored start then differ in the last bits, so a transcendental fit
        # may take any of them that reaches the same values. Every other path must match.
        tied = (case["agreement"] == "transcendental" and tolerances["parameters"] is not None
                and (expected["tied"] or actual["tied"]) and {expected["method"], actual["method"]} <= FIT_MINIMIZER_PATHS)
        for key in ["method", "non_real"]:
            if expected[key] != actual[key] and not (key == "method" and tied):
                raise ValueError(f"{location}.{key}: expected {expected[key]!r}, got {actual[key]!r}")
        # Display text follows the compared values; within the tolerance it may legitimately differ.
        if tolerances["parameters"] is FIT_EXACT and tolerances["uncertainties"] is FIT_EXACT and expected["formatted"] != actual["formatted"]:
            raise ValueError(f"{location}.formatted: display text differs for identical values")


def compare_fit_reference(expected, actual):
    """Compares a regenerated Java fit report with the committed fixture."""
    if [key for key in expected if key != "cases"] != FIT_HEADER_KEYS or set(actual) != set(expected):
        raise ValueError("Fit oracle report fields differ")
    for key in ["schema_version", "reference_version", "fits", "user_fits", "format"]:
        if expected[key] != actual[key]:
            raise ValueError(f"fit-reference: {key} differs")
    # Direct minimizer cases use arithmetic objectives only.
    compare_fit_values([m["parameters"] for m in expected["minimizers"]], [m["parameters"] for m in actual["minimizers"]],
                       FIT_EXACT, "fit-reference.minimizers")
    if ([{k: v for k, v in m.items() if k != "parameters"} for m in expected["minimizers"]]
            != [{k: v for k, v in m.items() if k != "parameters"} for m in actual["minimizers"]]):
        raise ValueError("fit-reference: minimizer results differ")
    if [case["id"] for case in expected["cases"]] != [case["id"] for case in actual["cases"]]:
        raise ValueError("fit-reference: the Java case list differs from the fixture")
    for want, got in zip(expected["cases"], actual["cases"]):
        location = f"fit-reference.{want['id']}"
        # Whether Newton diverges depends on the JVM's Math last bits, so either run may classify a transcendental
        # case as chaotic. That case then compares only its data handling and carries no independent expectation.
        classes = {want.get("agreement"), got.get("agreement")}
        chaotic = "chaotic" in classes and classes <= {"chaotic", "transcendental"}
        if (set(want) ^ set(got)) - ({"expect"} if chaotic else set()):
            raise ValueError(f"{location}: case fields differ")
        compared = dict(want, agreement="chaotic") if chaotic else want
        for key, value in want.items():
            if key in {"java", "intended"}:
                compare_fit_result(compared, value, got[key], f"{location}.{key}")
            elif chaotic and key in {"agreement", "expect"}:
                continue
            elif key == "expect":
                # Independent expectations: exact decimal least squares or a double Gauss-Newton minimum.
                if {k: v for k, v in value.items() if k not in {"parameters", "uncertainties"}} != \
                        {k: v for k, v in got[key].items() if k not in {"parameters", "uncertainties"}}:
                    raise ValueError(f"{location}.expect: definition differs")
                compare_fit_values(value["parameters"], got[key]["parameters"], FIT_EXPECT_TOLERANCE, f"{location}.expect")
                compare_fit_values(value["uncertainties"], got[key]["uncertainties"], FIT_EXPECT_TOLERANCE, f"{location}.expect")
            elif value != got[key]:
                raise ValueError(f"{location}.{key}: case definition differs")


@kernel_oracle("fit-oracle")
def check_fit_oracle(manifest, cache, java, *, record=False):
    """Fit the curve-fit kernel cases with the pinned OSP Data Tool classes and diff the fixture."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # A private home keeps OSP preference files out of the shared reference cache.
    with tempfile.TemporaryDirectory(prefix="fit-oracle-") as temporary:
        home = Path(temporary) / "home"
        home.mkdir()
        output = Path(temporary) / "fit-java.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}",
             "-Xmx256m", "--class-path", java_classpath(manifest, cache), str(FIT_ORACLE), str(output)], timeout=300)
        actual = json.loads(output.read_text(encoding="utf-8"))
    validate_fit_reference(actual, manifest)
    location = FIT_REFERENCE.relative_to(ROOT).as_posix() if FIT_REFERENCE.is_relative_to(ROOT) else str(FIT_REFERENCE)
    if record:
        FIT_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        FIT_REFERENCE.write_text(fit_reference_text(actual), encoding="utf-8", newline="\n")
        print(f"fit-oracle: recorded {len(actual['cases'])} Java curve-fit cases in {location}")
        return
    expected = json.loads(FIT_REFERENCE.read_text(encoding="utf-8"))
    validate_fit_reference(expected, manifest)
    compare_fit_reference(expected, actual)
    print(f"fit-oracle: {len(actual['cases'])} Java curve-fit cases and {len(actual['minimizers'])} minimizer runs match {location}")


FILTER_REFERENCE = ROOT / "tests/science/video-filter-reference.json"


FILTER_CLASSES = ["DeinterlaceFilter", "GhostFilter", "StrobeFilter", "DarkGhostFilter", "NegativeFilter", "GrayScaleFilter",
                  "LogFilter", "BrightnessFilter", "BaselineFilter", "SumFilter", "ResizeFilter", "RotateFilter", "PerspectiveFilter",
                  "RadialDistortionFilter"]


FILTER_PATTERNS = {"gradient", "noise", "checker", "blob", "darkblob", "solid", "argb"}


FILTER_OPS = {"apply", "step", "set", "enable", "clear", "baseline", "replace_source"}


FILTER_LINE_KEYS = ("frames", "cases")


MAX_FILTER_CASES = 500


MAX_FILTER_SIDE = 4096


MAX_FILTER_REPEAT = 1000


FULL_FILTER_IMAGE_PIXELS = 48


SHA256_HEX = re.compile(r"[0-9a-f]{64}")


def validate_filter_reference(document, manifest):
    """Check the video-filter oracle schema before comparing or recording it."""
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or document.get("reference_version") != manifest["reference"]["version"]):
        raise ValueError("Unexpected video filter oracle schema or reference version")
    registry = ["org.opensourcephysics.media.core." + name for name in FILTER_CLASSES]
    if document.get("registry") != registry:
        raise ValueError("The video filter registry differs from Tracker 6.3.5's")
    frames, cases = document.get("frames"), document.get("cases")
    if (not isinstance(frames, list) or not frames or not isinstance(cases, list) or not cases
            or len(cases) > MAX_FILTER_CASES or not isinstance(document.get("math"), dict)
            or not isinstance(document.get("composites"), list) or not isinstance(document.get("persistence"), list)
            or not isinstance(document.get("failures"), list)):
        raise ValueError("Invalid video filter oracle frame, case or probe lists")

    def frame_spec(frame, with_hash):
        if (not isinstance(frame, dict) or frame.get("pattern") not in FILTER_PATTERNS
                or any(type(frame.get(key)) is not int for key in ("width", "height", "seed", "t"))
                or not 0 < frame["width"] <= MAX_FILTER_SIDE or not 0 < frame["height"] <= MAX_FILTER_SIDE
                or (with_hash and not SHA256_HEX.fullmatch(str(frame.get("sha256", ""))))):
            raise ValueError(f"Invalid video filter frame {frame!r}")
        return frame["pattern"], frame["width"], frame["height"], frame["seed"], frame["t"]

    known = {frame_spec(frame, True) for frame in frames}
    if len(known) != len(frames):
        raise ValueError("Duplicate video filter frame")
    seen, covered = set(), set()
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("id"), str) or case["id"] in seen:
            raise ValueError(f"Invalid or duplicate video filter case {case.get('id') if isinstance(case, dict) else case!r}")
        seen.add(case["id"])
        filters, case_frames, ops, results = (case.get(key) for key in ("filters", "frames", "ops", "results"))
        if (not isinstance(filters, list) or not filters or not isinstance(case_frames, list) or not case_frames
                or not isinstance(ops, list) or not isinstance(results, list)
                or any(not isinstance(f, dict) or f.get("class") not in registry or not isinstance(f.get("params"), dict)
                       for f in filters)
                or any(frame_spec(frame, False) not in known for frame in case_frames)):
            raise ValueError(f"Invalid video filter case {case['id']!r}")
        covered.update(f["class"] for f in filters)
        applies = 0
        for op in ops:
            if (not isinstance(op, dict) or op.get("op") not in FILTER_OPS
                    or ("frame" in op and not (type(op["frame"]) is int and 0 <= op["frame"] < len(case_frames)))
                    or ("filter" in op and not (type(op["filter"]) is int and 0 <= op["filter"] < len(filters)))
                    or ("repeat" in op and not (type(op["repeat"]) is int and 0 < op["repeat"] <= MAX_FILTER_REPEAT))):
                raise ValueError(f"Invalid video filter operation in {case['id']!r}: {op!r}")
            applies += op["op"] == "apply"
        if len(results) != applies:
            raise ValueError(f"{case['id']}: expected one result per evaluation")
        for result in results:
            if (not isinstance(result, dict) or type(result.get("width")) is not int or type(result.get("height")) is not int
                    or not SHA256_HEX.fullmatch(str(result.get("sha256", "")))
                    or ("rgb" in result) != (result["width"] * result["height"] <= FULL_FILTER_IMAGE_PIXELS)
                    or ("rgb" in result and not re.fullmatch(r"[0-9a-f]{%d}" % (6 * result["width"] * result["height"]),
                                                             str(result["rgb"])))):
                raise ValueError(f"{case['id']}: invalid evaluation result {result!r}")
    if covered != set(registry):
        raise ValueError("Every registered video filter needs an oracle case")
    if {entry.get("class") for entry in document["persistence"] if isinstance(entry, dict)} != set(registry):
        raise ValueError("Every registered video filter needs a persistence probe")


def filter_reference_text(document):
    """Render the fixture with one frame and one case per line so that review diffs stay readable."""
    compact = {"separators": (",", ":"), "ensure_ascii": False}
    lines = ["{"]
    keys = list(document)
    for index, key in enumerate(keys):
        comma = "," if index + 1 < len(keys) else ""
        if key in FILTER_LINE_KEYS:
            items = document[key]
            lines.append(f"  {json.dumps(key)}: [")
            lines.extend(f"    {json.dumps(item, **compact)}{',' if i + 1 < len(items) else ''}" for i, item in enumerate(items))
            lines.append(f"  ]{comma}")
        else:
            lines.append(f"  {json.dumps(key)}: {json.dumps(document[key], **compact)}{comma}")
    lines.append("}")
    return "\n".join(lines) + "\n"


@kernel_oracle("filter-oracle")
def check_filter_oracle(manifest, cache, java, *, record=False):
    """Run the pinned OSP video filters on the synthetic cases and diff the committed fixture exactly."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # A private home keeps OSP preference files out of the shared reference cache.
    with tempfile.TemporaryDirectory(prefix="filter-oracle-") as temporary:
        home = Path(temporary) / "home"
        home.mkdir()
        output = Path(temporary) / "filter-java.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}",
             "-Xmx512m", "--class-path", java_classpath(manifest, cache),
             str(ROOT / "tools/reference/FilterOracle.java"), str(output)])
        actual = json.loads(output.read_text(encoding="utf-8"))
    validate_filter_reference(actual, manifest)
    location = FILTER_REFERENCE.relative_to(ROOT).as_posix() if FILTER_REFERENCE.is_relative_to(ROOT) else str(FILTER_REFERENCE)
    if record:
        FILTER_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        FILTER_REFERENCE.write_text(filter_reference_text(actual), encoding="utf-8", newline="\n")
        print(f"filter-oracle: recorded {len(actual['cases'])} Java filter cases in {location}")
        return
    expected = json.loads(FILTER_REFERENCE.read_text(encoding="utf-8"))
    validate_filter_reference(expected, manifest)
    exact = {"relative": 0.0, "absolute": 0.0}
    compare({key: value for key, value in expected.items() if key != "cases"},
            {key: value for key, value in actual.items() if key != "cases"}, exact, "video-filter-reference")
    if [case["id"] for case in expected["cases"]] != [case["id"] for case in actual["cases"]]:
        raise ValueError("video-filter-reference: the Java case list differs from the fixture")
    for want, got in zip(expected["cases"], actual["cases"]):
        compare(want, got, exact, f"video-filter-reference.{want['id']}")
    print(f"filter-oracle: {len(actual['cases'])} Java filter cases match {location} pixel for pixel")


ANALYSIS_REFERENCE = ROOT / "tests/science/analysis-reference.json"


ANALYSIS_ORACLE = ROOT / "tools/reference/AnalysisOracle.java"


ANALYSIS_KINDS = {"statistics": "column", "plot": "series", "correlation": "series", "area": "series", "measure": "series",
                  "nearest": "series", "function-area": None, "function-slope": None, "fourier": "series",
                  "fft-real": None, "fft-complex": None, "column": None}


# FFT outputs and Fourier coefficients go through Math.sin/Math.cos, whose last bit differs between
# HotSpot's x86-64 intrinsics, its arm64 path and C++ libraries. They may differ by this many units
# of 2^-52 per transform stage, relative to the sum of input magnitudes (docs/data-analysis-kernels.md);
# a wrong twiddle, branch or sign shows at the size of the input. Everything else must be identical.
ANALYSIS_FFT_ULPS = 16


MAX_ANALYSIS_CASES = 2000


def analysis_fft_tolerance(values, n):
    """Absolute tolerance for a length-n transform of `values` (missing values do not count)."""
    scale = sum(abs(value) for value in values if type(value) is float)
    return ANALYSIS_FFT_ULPS * (1 + math.log2(max(n, 1))) * sys.float_info.epsilon * scale


def validate_analysis_reference(document, manifest):
    """Check the analysis oracle schema before comparing or recording it."""
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or document.get("reference_version") != manifest["reference"]["version"]
            or not isinstance(document.get("osp_commit"), str)):
        raise ValueError("Unexpected analysis oracle schema or reference version")
    sources = {}
    for name in ["columns", "series"]:
        items = document.get(name)
        if not isinstance(items, list) or not items or not all(isinstance(item, dict) for item in items):
            raise ValueError(f"Invalid analysis oracle {name}")
        ids = [item.get("id") for item in items]
        if len(set(ids)) != len(ids):
            raise ValueError(f"Duplicate analysis oracle {name} id")
        sources[name[:-1] if name == "columns" else name] = set(ids)
    cases = document.get("cases")
    if not isinstance(cases, list) or not cases or len(cases) > MAX_ANALYSIS_CASES:
        raise ValueError("Invalid analysis oracle case list")
    seen = set()
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("id"), str) or case["id"] in seen:
            identifier = case.get("id") if isinstance(case, dict) else case
            raise ValueError(f"Invalid or duplicate analysis oracle case {identifier!r}")
        source = ANALYSIS_KINDS.get(case.get("kind"), "unknown")
        if source == "unknown" or (source is not None and case.get(source) not in sources[source]):
            raise ValueError(f"Analysis oracle case {case['id']!r} has an unknown kind or source")
        seen.add(case["id"])
    if {case["kind"] for case in cases} != set(ANALYSIS_KINDS):
        raise ValueError("Every analysis oracle kind needs a case")


def analysis_reference_text(document):
    """Render the fixture with one column, series or case per line so that review diffs stay readable."""
    compact = {"separators": (",", ":"), "ensure_ascii": False}
    keys = list(document)
    lines = ["{"]
    for position, key in enumerate(keys):
        suffix = "," if position + 1 < len(keys) else ""
        value = document[key]
        if key in {"columns", "series", "cases"}:
            lines.append(f"  {json.dumps(key)}: [")
            lines.extend(f"    {json.dumps(item, **compact)}{',' if index + 1 < len(value) else ''}"
                         for index, item in enumerate(value))
            lines.append(f"  ]{suffix}")
        else:
            lines.append(f"  {json.dumps(key)}: {json.dumps(value, **compact)}{suffix}")
    lines.append("}")
    return "\n".join(lines) + "\n"


def compare_analysis_case(want, got, series):
    location = f"analysis-reference.{want['id']}"
    if not isinstance(got, dict) or want.keys() != got.keys():
        raise ValueError(f"{location}: case fields differ")
    approximate = {}
    if want["kind"] in {"fft-real", "fft-complex"}:
        length = len(want["input"]) // (2 if want["kind"] == "fft-complex" else 1)
        approximate["output"] = analysis_fft_tolerance(want["input"], length)
    elif want["kind"] == "fourier" and want["spectrum"] is not None:
        points = want["transformed_points"]
        coefficient = analysis_fft_tolerance(series[want["series"]]["y"][:points], points) * 2 / points
        scale = sum(abs(value) for value in series[want["series"]]["y"][:points] if type(value) is float) * 2 / points
        approximate = {"cosine": coefficient, "sine": coefficient, "power": (4 * scale + 2 * coefficient) * coefficient}
    exact = {"relative": 0.0, "absolute": 0.0}
    for key, value in want.items():
        if key == "spectrum" and approximate:
            if not isinstance(got[key], dict) or value.keys() != got[key].keys():
                raise ValueError(f"{location}.spectrum: fields differ")
            for field, values in value.items():
                tolerance = {"relative": 0.0, "absolute": approximate[field]} if field in approximate else exact
                compare(values, got[key][field], tolerance, f"{location}.spectrum.{field}")
        else:
            tolerance = {"relative": 0.0, "absolute": approximate[key]} if key in approximate else exact
            compare(value, got[key], tolerance, f"{location}.{key}")


@kernel_oracle("analysis-oracle")
def check_analysis_oracle(manifest, cache, java, *, record=False):
    """Run the Data Tool analysis cases with the pinned OSP classes and diff the committed fixture."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # A private home keeps OSP preference files out of the shared reference cache.
    with tempfile.TemporaryDirectory(prefix="analysis-oracle-") as temporary:
        home = Path(temporary) / "home"
        home.mkdir()
        output = Path(temporary) / "analysis-java.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}",
             "-Xmx256m", "--class-path", java_classpath(manifest, cache), str(ANALYSIS_ORACLE), str(output)])
        actual = json.loads(output.read_text(encoding="utf-8"))
    validate_analysis_reference(actual, manifest)
    location = (ANALYSIS_REFERENCE.relative_to(ROOT).as_posix() if ANALYSIS_REFERENCE.is_relative_to(ROOT)
                else str(ANALYSIS_REFERENCE))
    if record:
        ANALYSIS_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        ANALYSIS_REFERENCE.write_text(analysis_reference_text(actual), encoding="utf-8", newline="\n")
        print(f"analysis-oracle: recorded {len(actual['cases'])} Java Data Tool cases in {location}")
        return
    expected = json.loads(ANALYSIS_REFERENCE.read_text(encoding="utf-8"))
    validate_analysis_reference(expected, manifest)
    exact = {"relative": 0.0, "absolute": 0.0}
    compare({key: value for key, value in expected.items() if key != "cases"},
            {key: value for key, value in actual.items() if key != "cases"}, exact, "analysis-reference")
    if [case["id"] for case in expected["cases"]] != [case["id"] for case in actual["cases"]]:
        raise ValueError("analysis-reference: the Java case list differs from the fixture")
    series = {item["id"]: item for item in expected["series"]}
    for want, got in zip(expected["cases"], actual["cases"]):
        compare_analysis_case(want, got, series)
    print(f"analysis-oracle: {len(actual['cases'])} Java Data Tool cases match {location}")


MOTION_REFERENCE = ROOT / "tests/science/motion-reference.json"


# Java-to-fixture agreement for the motion kernel oracle (tools/reference/MotionOracle.java). Its inputs
# are built with IEEE arithmetic only, and the kernels are arithmetic, sqrt and StrictMath.atan2, so
# results must be identical. Butterworth designs and OSP's x log x sin x cos x test function call
# Math.tan/sin/cos/log, and FilterSelfTest's signals Math.sin, which HotSpot implements with intrinsics
# specified only to within 1 ulp; those allow a last-bit change from a different JDK build.
MOTION_EXACT = {"relative": 0.0, "absolute": 0.0}


MOTION_INTRINSIC_TOLERANCE = {"relative": 1e-12, "absolute": 1e-14}


MOTION_CASE_SECTIONS = ("derivative_cases", "difference_cases", "angular_cases", "filter_signals", "filter_cases",
                        "numerical_cases")


MOTION_ALGORITHMS = ("finite_difference", "smooth_finite_difference", "bounce_detection")


MOTION_FILTER_TYPES = ("moving_average", "butterworth", "savitzky_golay")


MOTION_SELF_TESTS = ("moving_average_reduces_noise", "moving_average_handles_invalid_gap",
                     "butterworth_attenuates_above_cutoff", "butterworth_preserves_dc", "butterworth_zero_phase",
                     "savitzky_golay_preserves_quadratic")


MAX_MOTION_CASES = 1000


def motion_case_tolerance(section, case):
    if section == "filter_cases" and case.get("type") == "butterworth":
        return MOTION_INTRINSIC_TOLERANCE
    if section == "numerical_cases" and case.get("function") == "x-log-sin-cos":
        return MOTION_INTRINSIC_TOLERANCE
    return MOTION_EXACT


def validate_motion_reference(document, manifest):
    """Check the motion-oracle schema and its coverage before comparing or recording it."""
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or document.get("reference_version") != manifest["reference"]["version"]
            or not isinstance(document.get("tracker"), dict)):
        raise ValueError("Unexpected motion oracle schema or reference version")
    self_test = document.get("self_test")
    if (not isinstance(self_test, list) or [check.get("name") for check in self_test if isinstance(check, dict)]
            != list(MOTION_SELF_TESTS) or any(check.get("passed") is not True for check in self_test)):
        raise ValueError("The motion oracle must record all six passing FilterSelfTest checks")
    for section in MOTION_CASE_SECTIONS:
        cases = document.get(section)
        if not isinstance(cases, list) or not cases or len(cases) > MAX_MOTION_CASES:
            raise ValueError(f"Invalid motion oracle section {section}")
        if any(not isinstance(case, dict) for case in cases):
            raise ValueError(f"Invalid motion oracle case in {section}")
        ids = [case.get("id") for case in cases if section != "numerical_cases"]
        if any(not isinstance(case_id, str) for case_id in ids) or len(set(ids)) != len(ids):
            raise ValueError(f"Missing or duplicate motion oracle case id in {section}")
    for case in document["derivative_cases"]:
        clip, results = case.get("clip"), case.get("results")
        count = clip.get("step_count") if isinstance(clip, dict) else None
        if (type(count) is not int or not isinstance(results, list) or len(case.get("positions", ())) != count
                or len(case.get("times", ())) != count):
            raise ValueError(f"Invalid motion derivative case {case.get('id')!r}")
        expected = list(MOTION_ALGORITHMS) + (["bounce_detection_compacted"] if clip.get("step_size", 1) > 1 else [])
        if [result.get("algorithm") for result in results if isinstance(result, dict)] != expected:
            raise ValueError(f"Motion derivative case {case['id']!r} lacks an algorithm result")
        for result in results:
            series = ("first", "second", "velocity", "acceleration")
            if result["error"] is None and any(len(result.get(name, ())) != count for name in series):
                raise ValueError(f"Invalid motion derivative result in {case['id']!r}")
    signals = {signal["id"] for signal in document["filter_signals"]}
    for case in document["filter_cases"]:
        if case.get("type") not in MOTION_FILTER_TYPES or case.get("signal") not in signals:
            raise ValueError(f"Invalid motion filter case {case.get('id')!r}")
    if {case["type"] for case in document["filter_cases"]} != set(MOTION_FILTER_TYPES):
        raise ValueError("Every motion filter needs an oracle case")
    if not any(result["error"] for case in document["derivative_cases"] for result in case["results"]):
        raise ValueError("The motion oracle must record Tracker's Bounce Detection failures")


def motion_reference_text(document):
    """Render the fixture with one case per line so that review diffs stay readable."""
    compact = {"separators": (",", ":"), "ensure_ascii": False}
    keys = [key for key in document if key not in MOTION_CASE_SECTIONS]
    lines = ["{"]
    lines.extend(f"  {json.dumps(key)}: {json.dumps(document[key], **compact)}," for key in keys)
    for number, section in enumerate(MOTION_CASE_SECTIONS):
        cases = document[section]
        lines.append(f"  {json.dumps(section)}: [")
        lines.extend(f"    {json.dumps(case, **compact)}{',' if index + 1 < len(cases) else ''}" for index, case in enumerate(cases))
        lines.append("  ]" + ("," if number + 1 < len(MOTION_CASE_SECTIONS) else ""))
    lines.append("}")
    return "\n".join(lines) + "\n"


@kernel_oracle("motion-oracle")
def check_motion_oracle(manifest, cache, java, *, record=False):
    """Run the motion kernel cases with the pinned Tracker classes and diff the committed fixture."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # A private home keeps OSP preference files out of the shared reference cache.
    with tempfile.TemporaryDirectory(prefix="motion-oracle-") as temporary:
        home = Path(temporary) / "home"
        home.mkdir()
        output = Path(temporary) / "motion-java.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}",
             "-Xmx256m", "--class-path", java_classpath(manifest, cache),
             str(ROOT / "tools/reference/MotionOracle.java"), str(output)])
        actual = json.loads(output.read_text(encoding="utf-8"))
    validate_motion_reference(actual, manifest)
    location = MOTION_REFERENCE.relative_to(ROOT).as_posix() if MOTION_REFERENCE.is_relative_to(ROOT) else str(MOTION_REFERENCE)
    total = sum(len(actual[section]) for section in MOTION_CASE_SECTIONS)
    if record:
        MOTION_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        MOTION_REFERENCE.write_text(motion_reference_text(actual), encoding="utf-8", newline="\n")
        print(f"motion-oracle: recorded {total} Java motion cases in {location}")
        return
    expected = json.loads(MOTION_REFERENCE.read_text(encoding="utf-8"))
    validate_motion_reference(expected, manifest)
    for key in expected:
        if key in MOTION_CASE_SECTIONS:
            continue
        compare(expected[key], actual.get(key), MOTION_INTRINSIC_TOLERANCE if key == "self_test" else MOTION_EXACT,
                f"motion-reference.{key}")
    if set(expected) != set(actual):
        raise ValueError("motion-reference: the Java sections differ from the fixture")
    for section in MOTION_CASE_SECTIONS:
        if len(expected[section]) != len(actual[section]):
            raise ValueError(f"motion-reference: the Java {section} list differs from the fixture")
        for index, (want, got) in enumerate(zip(expected[section], actual[section])):
            compare(want, got, motion_case_tolerance(section, want), f"motion-reference.{section}[{want.get('id', index)}]")
    print(f"motion-oracle: {total} Java motion cases match {location}")


SAMPLING_REFERENCE = ROOT / "tests/science/sampling-reference.json"
SAMPLING_FRAME_KINDS = {"noise", "bright", "sparse", "gradient", "checker", "edge", "vedge", "band", "white"}
SAMPLING_SHAPES = {"ellipse", "rectangle", "polygon"}
SAMPLING_REGION_COLUMNS = ["R", "G", "B", "luma", "pixels", "Rsd", "Gsd", "Bsd"]
SAMPLING_PROFILE_COLUMNS = ["x", "y", "R", "G", "B", "luma", "pixels", "n"]
# The same agreement classes as tests/science/sampling_reference_test.cpp. Region statistics and
# horizontal profiles are integer sums and IEEE arithmetic, identical on every JVM. Tilted probes
# start from Math.atan2/cos/sin, whose last bit may differ between HotSpot intrinsics and CPUs.
SAMPLING_EXACT = {"relative": 0.0, "absolute": 0.0}
SAMPLING_TILTED_TOLERANCE = {"relative": 0.0, "absolute": 1e-9}
SAMPLING_HORIZONTAL_SINE = 1e-5
MAX_SAMPLING_CASES = 500


def sampling_profile_is_tilted(case):
    """LineProfileStep.getProfileData's choice between its horizontal and tilted algorithms."""
    return not case["always_horizontal"] and abs(math.sin(case["axis_angle"])) >= SAMPLING_HORIZONTAL_SINE


def _sampling_numeric(value):
    return (type(value) in {int, float} and math.isfinite(value)) or value == "NaN"


def _sampling_point(value):
    return isinstance(value, list) and len(value) == 2 and all(type(v) in {int, float} and math.isfinite(v) for v in value)


def _valid_sampling_region(case, frame_ids):
    result, vertices = case.get("result"), case.get("vertices")
    return (isinstance(case.get("id"), str) and case.get("frame") in frame_ids and case.get("shape") in SAMPLING_SHAPES
            and _sampling_point(case.get("position"))
            and isinstance(case.get("size"), list) and len(case["size"]) == 2 and all(type(v) is int for v in case["size"])
            and (vertices is None) == (case["shape"] != "polygon")
            and (vertices is None or (isinstance(vertices, list) and all(_sampling_point(v) for v in vertices)))
            and isinstance(case.get("bounds"), list) and len(case["bounds"]) == 4 and all(type(v) is int for v in case["bounds"])
            and type(case.get("mask_pixels")) is int and re.fullmatch(r"[0-9a-f]{16}", str(case.get("mask_fnv1a64"))) is not None
            and (result is None or (isinstance(result, dict) and list(result) == SAMPLING_REGION_COLUMNS
                                    and all(_sampling_numeric(v) for v in result.values()))))


def _valid_sampling_profile(case, frame_ids):
    result, ends = case.get("result"), case.get("ends")
    columns = list(result.values()) if isinstance(result, dict) else []
    return (isinstance(case.get("id"), str) and case.get("frame") in frame_ids
            and type(case.get("always_horizontal")) is bool and type(case.get("axis_angle")) in {int, float}
            and isinstance(ends, list) and len(ends) == 2 and all(_sampling_point(end) for end in ends)
            and type(case.get("spread")) is int and 0 <= case["spread"] <= 100
            and (result is None or (isinstance(result, dict) and list(result) == SAMPLING_PROFILE_COLUMNS
                                    and all(isinstance(column, list) and all(_sampling_numeric(v) for v in column) for column in columns)
                                    and len({len(column) for column in columns}) == 1)))


def validate_sampling_reference(document, manifest):
    """Check the sampling oracle schema before comparing or recording it."""
    if (not isinstance(document, dict) or document.get("schema_version") != 1
            or document.get("reference_version") != manifest["reference"]["version"]):
        raise ValueError("Unexpected sampling oracle schema or reference version")
    frames, regions, profiles = document.get("frames"), document.get("regions"), document.get("profiles")
    if (not isinstance(frames, list) or not isinstance(regions, list) or not isinstance(profiles, list)
            or not frames or not regions or not profiles or len(regions) + len(profiles) > MAX_SAMPLING_CASES
            or not isinstance(document.get("tracker"), dict)):
        raise ValueError("Invalid sampling oracle frame, region or profile list")
    frame_ids = set()
    for frame in frames:
        if (not isinstance(frame, dict) or frame.get("id") in frame_ids or frame.get("kind") not in SAMPLING_FRAME_KINDS
                or any(type(frame.get(key)) is not int or not 0 < frame[key] <= 4096 for key in ("width", "height"))
                or re.fullmatch(r"[0-9a-f]{16}", str(frame.get("fnv1a64"))) is None):
            raise ValueError("Invalid or duplicate sampling frame")
        frame_ids.add(frame["id"])
    seen = set()
    for key, valid in (("regions", _valid_sampling_region), ("profiles", _valid_sampling_profile)):
        for case in document[key]:
            if not isinstance(case, dict) or case.get("id") in seen or not valid(case, frame_ids):
                raise ValueError(f"Invalid or duplicate sampling case {case.get('id') if isinstance(case, dict) else case!r}")
            seen.add(case["id"])
    if not any(sampling_profile_is_tilted(case) for case in profiles) or all(sampling_profile_is_tilted(case) for case in profiles):
        raise ValueError("The sampling oracle needs both horizontal and tilted profiles")


def sampling_reference_text(document):
    """Render the fixture with one frame or case per line so that review diffs stay readable."""
    compact = {"separators": (",", ":"), "ensure_ascii": False}
    lists = ("frames", "regions", "profiles")
    lines = ["{"]
    lines.extend(f"  {json.dumps(key)}: {json.dumps(value, **compact)}," for key, value in document.items() if key not in lists)
    for position, key in enumerate(lists):
        items = document[key]
        lines.append(f"  {json.dumps(key)}: [")
        lines.extend(f"    {json.dumps(item, **compact)}{',' if index + 1 < len(items) else ''}" for index, item in enumerate(items))
        lines.append("  ]," if position + 1 < len(lists) else "  ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


@kernel_oracle("sampling-oracle")
def check_sampling_oracle(manifest, cache, java, *, record=False):
    """Measure the sampling kernel's synthetic frames with the pinned RGBStep/LineProfileStep and diff the fixture."""
    verify_file(cache / manifest["reference"]["file"], manifest["reference"])
    # A private home keeps OSP preference files out of the shared reference cache.
    with tempfile.TemporaryDirectory(prefix="sampling-oracle-") as temporary:
        home = Path(temporary) / "home"
        home.mkdir()
        output = Path(temporary) / "sampling-java.json"
        run([java, "-Djava.awt.headless=true", f"-Duser.home={home}", f"-Djava.io.tmpdir={home}",
             "-Xmx256m", "--class-path", java_classpath(manifest, cache),
             str(ROOT / "tools/reference/SamplingOracle.java"), str(output)])
        actual = json.loads(output.read_text(encoding="utf-8"))
    validate_sampling_reference(actual, manifest)
    location = (SAMPLING_REFERENCE.relative_to(ROOT).as_posix() if SAMPLING_REFERENCE.is_relative_to(ROOT)
                else str(SAMPLING_REFERENCE))
    cases = len(actual["regions"]) + len(actual["profiles"])
    if record:
        SAMPLING_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
        SAMPLING_REFERENCE.write_text(sampling_reference_text(actual), encoding="utf-8", newline="\n")
        print(f"sampling-oracle: recorded {cases} Java sampling cases in {location}")
        return
    expected = json.loads(SAMPLING_REFERENCE.read_text(encoding="utf-8"))
    validate_sampling_reference(expected, manifest)
    compare({key: value for key, value in expected.items() if key not in {"regions", "profiles"}},
            {key: value for key, value in actual.items() if key not in {"regions", "profiles"}}, SAMPLING_EXACT, "sampling-reference")
    for key in ("regions", "profiles"):
        if [case["id"] for case in expected[key]] != [case["id"] for case in actual[key]]:
            raise ValueError(f"sampling-reference: the Java {key} list differs from the fixture")
        for want, got in zip(expected[key], actual[key]):
            tilted = key == "profiles" and sampling_profile_is_tilted(want)
            compare(want, got, SAMPLING_TILTED_TOLERANCE if tilted else SAMPLING_EXACT, f"sampling-reference.{want['id']}")
    print(f"sampling-oracle: {cases} Java sampling cases match {location}")


LIBRARY_COMMANDS = ("discover-library", "fetch-library", "verify-library", "library-census", "library-projects")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # Library commands are corpus-level commands with their own dispatch (tools/corpus_library.py).
    parser.add_argument("command", choices=[*CORPUS_COMMANDS, *LIBRARY_COMMANDS, *sorted(KERNEL_ORACLES)])
    parser.add_argument("dumps", nargs="*", type=Path, help="state-diff only: expected and actual dump files")
    parser.add_argument("--cache", type=Path, default=ROOT / ".dev-tools/tracker-reference")
    parser.add_argument("--java", help="Java 17+ JDK executable (reference, session, roundtrip, state-oracle and *-oracle "
                                       "commands; optional for library-census)")
    parser.add_argument("--probe", help="Built motrack_corpus_probe executable "
                                        "(check/roundtrip/xml-oracle/state-check/library-census commands)")
    parser.add_argument("--record", action="store_true", help="Explicitly replace committed Java baselines for review")
    parser.add_argument("--reports", type=Path, help="Report directory for xml-oracle/library-oracle (default: CACHE/reports/COMMAND)")
    parser.add_argument("--library-extra", type=Path, help="Directory of further documents for library-oracle (not committed)")
    parser.add_argument("--o2-only", action="store_true", help="xml-oracle without Java: byte-identical renders only")
    parser.add_argument("--source", action="append", help="Limit library commands to this source ID (repeatable)")
    parser.add_argument("--redownload", action="store_true", help="discover-library: re-pin every file instead of reusing verified ones")
    parser.add_argument("--check", action="store_true", help="library-census: compare with the committed census instead of writing it")
    parser.add_argument("--jobs", type=int, default=1, help="library-census: experiments processed concurrently (local only)")
    args = parser.parse_args()
    if args.record and args.command not in {"reference", "state-oracle", "state-check"} and args.command not in KERNEL_ORACLES:
        parser.error("--record is only valid for the reference, state-oracle and state-check commands and the kernel oracles")
    if args.reports is not None and args.command not in {"xml-oracle", "library-oracle"}:
        parser.error("--reports is only valid for the xml-oracle and library-oracle commands")
    if args.library_extra is not None and args.command != "library-oracle":
        parser.error("--library-extra is only valid for the library-oracle command")
    if args.o2_only and args.command != "xml-oracle":
        parser.error("--o2-only is only valid for the xml-oracle command")
    if args.command in {"check", "roundtrip", "xml-oracle", "library-oracle", "state-check", "library-census"} and not args.probe:
        parser.error(f"{args.command} requires --probe")
    if (args.command == "state-diff") != bool(args.dumps) or (args.dumps and len(args.dumps) != 2):
        parser.error("state-diff takes exactly two dump files; other commands take none")
    if args.source and args.command not in {"discover-library", "fetch-library", "verify-library"}:
        parser.error("--source is only valid for library discovery, fetching and verification")
    if args.redownload and args.command != "discover-library":
        parser.error("--redownload is only valid for discover-library")
    if (args.check or args.jobs != 1) and args.command != "library-census":
        parser.error("--check and --jobs are only valid for library-census")
    if args.command in LIBRARY_COMMANDS:
        # The library module reuses this module's helpers; share one instance when run as a script.
        sys.modules.setdefault("corpus", sys.modules[__name__])
        import corpus_library
        corpus_library.main(args)
        return
    args.java = args.java or "java"
    manifest = load_manifest()
    cache = args.cache.resolve()
    if args.command == "state-oracle":
        check_state_oracle(manifest, cache, args.java, record=args.record)
    elif args.command == "state-check":
        check_state_native(manifest, cache, args.probe, record=args.record)
    elif args.command == "state-diff":
        diff_state_files(manifest, *args.dumps)
    elif args.command in {"fetch", "verify"}:
        for path, artifact in artifacts(manifest, cache):
            (fetch_file if args.command == "fetch" and "local_file" not in artifact else verify_file)(path, artifact)
        print(f"Verified {len(manifest['cases'])} cases and Tracker {manifest['reference']['version']}")
    elif args.command in KERNEL_ORACLES:
        globals()[KERNEL_ORACLES[args.command]](manifest, cache, args.java, record=args.record)
    elif args.command in {"session", "roundtrip"}:
        check_sessions(manifest, cache, args.java, args.probe if args.command == "roundtrip" else None)
    elif args.command == "xml-oracle":
        check_xml_oracle(manifest, cache, args.java, args.probe, args.reports, args.o2_only)
    elif args.command == "library-oracle":
        check_library_oracle(manifest, cache, args.java, args.probe, args.reports, args.library_extra)
    else:
        check_cases(manifest, cache, java=args.java if args.command == "reference" else None,
                    probe=args.probe, record=args.record)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error)) from error
