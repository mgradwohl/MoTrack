"""Fetch pinned Tracker examples and compare native imports with Java reference snapshots."""

import argparse
import copy
from contextlib import contextmanager
import hashlib
import json
import math
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import xml.dom.minidom as minidom
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "tests/corpus"
MAX_XML_BYTES = 32 * 1024 * 1024


def load_manifest(path=CORPUS / "manifest.json"):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest["schema_version"] != 1:
        raise ValueError("Unsupported corpus manifest version")
    seen = set()
    for case in manifest["cases"]:
        if not re.fullmatch(r"[a-z0-9-]+", case["id"]) or case["id"] in seen:
            raise ValueError("Invalid or duplicate corpus case ID")
        seen.add(case["id"])
        if "session_exclusion" in case and ("session_tracks" in case or any(
                key.startswith("native_") and key.endswith("_roundtrip") and value for key, value in case.items())):
            raise ValueError(f"{case['id']}: session_exclusion conflicts with session_tracks or an enabled native round-trip flag")
        expected_path(case)
        if "native_reference_index" in case:
            expected_path({"expected": case["native_reference_index"]})
    for artifact in [manifest["reference"], *manifest["cases"]]:
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
    for case in manifest["cases"]:
        yield local_fixture_path(case) if "local_file" in case else cache / "fixtures" / case["file"], case


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
        if not isinstance(actual, list) or len(expected) != len(actual):
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


def run(command, *, display=None):
    environment = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    if display is not None:
        environment["DISPLAY"] = display
        environment.pop("WAYLAND_DISPLAY", None)
        environment.pop("XAUTHORITY", None)
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                            timeout=45, env=environment)
    if result.returncode:
        raise ValueError(f"Probe failed ({result.returncode}):\n{(result.stdout + result.stderr)[-5000:]}")


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
            run([java, "-Djava.awt.headless=true", f"-Duser.home={reference_home}", "-Xmx256m", "--class-path",
                 str(cache / manifest["reference"]["file"]), str(ROOT / "tools/reference/TrackerReference.java"),
                 str(project), str(output)])
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
            or report.get("view_preparation") != "visit_custom_tables_restore_selection"):
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
    if calibration is not None:
        compare(calibration, before["calibration"], tolerance, case["id"] + ".calibration")
    vectors = [{"name": track["name"], "samples": track["vector_samples"]} for track in before["tracks"]
               if track["class"] == "org.opensourcephysics.cabrillo.tracker.Vector"]
    compare(expected["vectors"], vectors, tolerance, case["id"] + ".vectors")
    if "session_vector_data" in case:
        calibrated = [{"name": track["name"], "samples": track.get("calibrated_vector_samples")}
                      for track in before["tracks"] if track["class"] == "org.opensourcephysics.cabrillo.tracker.Vector"]
        compare(case["session_vector_data"], calibrated, tolerance, case["id"] + ".calibrated_vectors")
    if key_frames is not None:
        actual = [track["key_frames"] for track in before["tracks"]
                  if track["class"] == "org.opensourcephysics.cabrillo.tracker.PointMass"]
        compare(key_frames, actual, tolerance, case["id"] + ".key_frames")


def validate_known_session_issue(case, report, manifest, saved):
    """Reproduce one exact upstream defect; never count it as round-trip acceptance."""
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


@contextmanager
def private_display():
    if sys.platform != "linux":
        raise ValueError("Session checks currently require Linux with Xvfb")
    executable = shutil.which("Xvfb")
    if not executable:
        raise ValueError("Install Xvfb to run the isolated session checks")
    reader, writer = os.pipe()
    server = None
    try:
        try:
            server = subprocess.Popen([executable, "-displayfd", str(writer), "-screen", "0", "1280x800x24",
                                       "-nolisten", "tcp"], pass_fds=(writer,),
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        finally:
            os.close(writer)
        if not select.select([reader], [], [], 5)[0]:
            raise ValueError("Private Xvfb display did not become ready")
        number = os.read(reader, 32).decode("ascii")
        if not re.fullmatch(r"[0-9]+\n", number) or server.poll() is not None:
            raise ValueError("Private Xvfb display failed to start")
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
                               f'<property name="x" type="double">{x}</property><property name="y" type="double">{y}</property>'
                               '</object></property>' for frame, x, y in [(1, 330, 220), (4, 340, 210), (8, 360, 230)])
                     + '</property><property name="keyFrames" type="array" class="[I">'
                       '<property name="array" type="string">{1,4,8}</property></property>')
    added = minidom.parseString(
        '<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.PointMass">'
        '<property name="name" type="string"><![CDATA[nouvelle β & <C>]]></property><property name="mass" type="double">3</property>'
        '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
        '<property name="locked" type="boolean">false</property><property name="footprint" type="string"><![CDATA[Footprint.Diamond]]></property>'
        '<property name="color" type="object"><object class="java.awt.Color">'
        '<property name="red" type="int">255</property><property name="green" type="int">0</property>'
        '<property name="blue" type="int">0</property><property name="alpha" type="int">255</property></object></property>'
        + framedata + '</object></property>')
    tracks[0].appendChild(expected.importNode(added.documentElement, True))
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    for document in [expected, actual, added]:
        document.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested new point mass")


CREATED_VECTOR_NAME = "vecteur β & <C>"
CREATED_VECTOR_APPEARANCE = {"visible": True, "trail": True, "locked": False, "footprint": "Footprint.BoldArrow", "argb": -65281}


PLOT_DELETION_NAMES = {"delete-plot-owner": "mass A", "delete-plot-guest": "mass B", "delete-first-plot-guest": "mass C"}
TABLE_DELETION_NAMES = {"delete-first-table-owner": {"mass A"}, "delete-selected-table-owner": {"mass B"},
                        "delete-last-table-owner": {"mass C"}, "delete-all-table-owners": {"mass A", "mass B", "mass C"}}


def deletion_roundtrip_expected(case, mode):
    mass_modes = {"delete-mass", "delete-selected-mass", "delete-all-masses"}
    if (mode in mass_modes and case["id"] != "protractor"
            or mode in {"delete-vector", "delete-all-vectors"} and case["id"] != "independent-vectors"
            or mode in PLOT_DELETION_NAMES and case["id"] != "plot-guests"
            or mode in TABLE_DELETION_NAMES and case["id"] != "table-owners"
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
    if [[text(value) for value in cells(row)] for row in rows] != [["mass A", "t", "x", "y", "frame"], ["mass B", "t", "y", "x", "frame"], ["mass C", "t", "x", "y", "frame"]]:
        raise ValueError("Expected the pinned original table columns")
    formats = prop(table, "column_formats")
    groups = cells(formats)
    if [[[text(value) for value in cells(row)] for row in cells(group)] for group in groups] != [
            [["mass A", "x", "0.000"], ["mass A", "y", "0.00"]],
            [["mass B", "y", "0.0000"], ["mass B", "t", "0.00"]], [["mass C", "x", "0.00000"]]]:
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
        else:
            for child in list(selected.childNodes):
                selected.removeChild(child)
            selected.appendChild(document.createTextNode("mass A"))


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
                               f'<property name="xtail" type="double">{x}</property><property name="ytail" type="double">{y}</property>'
                               f'<property name="xcomponent" type="double">{cx}</property><property name="ycomponent" type="double">{cy}</property>'
                               '</object></property>' for frame, x, y, cx, cy in [(1, 100, 120, 4, -8), (6, 200, 180, 0, 0), (8, 160, 100, -6, 12)])
                     + '</property>')
    added = minidom.parseString(
        '<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.Vector">'
        '<property name="name" type="string"><![CDATA[vecteur β & <C>]]></property>'
        '<property name="visible" type="boolean">true</property><property name="trail" type="boolean">true</property>'
        '<property name="locked" type="boolean">false</property><property name="footprint" type="string"><![CDATA[Footprint.BoldArrow]]></property>'
        '<property name="color" type="object"><object class="java.awt.Color">'
        '<property name="red" type="int">255</property><property name="green" type="int">0</property>'
        '<property name="blue" type="int">255</property><property name="alpha" type="int">255</property></object></property>'
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
    for name, value in [("xorigin", "100"), ("yorigin", "120"), ("angle", "90"), ("xscale", "4"), ("yscale", "8")]:
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
            nodes[0].data = str(value)
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
                node.data = format(value, ".17g")
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
    changes = [("VideoClip", "starttime", "double", "-500"), ("StepperClipControl", "delta_t", "double", "250")]
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
    value = format(float("".join(node.data for node in values)) * 2.5, ".17g")
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
            coordinate.appendChild(expected.createTextNode(format(value, ".17g")))
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
        text(prop(original_mark, "x"), format(image[0], ".17g"))
        text(prop(original_mark, "y"), format(image[1], ".17g"))
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
                    values[0].data = format(value, ".17g")
                else:
                    coordinate = expected.createElement("property")
                    coordinate.setAttribute("name", name)
                    coordinate.setAttribute("type", "double")
                    coordinate.appendChild(expected.createTextNode(format(value, ".17g")))
                    obj.appendChild(coordinate)
    actual = minidom.parse(str(exported))
    matches = canonical_tracker_xml(expected) == canonical_tracker_xml(actual)
    expected.unlink()
    actual.unlink()
    if not matches:
        raise ValueError("Standard export changed content beyond the requested vector edits")


def check_sessions(manifest, cache, java, probe=None):
    if sys.platform != "linux":
        raise ValueError("Session checks currently require Linux with Xvfb")
    modes = [("mass", "native_mass_roundtrip"), ("marks", "native_mark_roundtrip"),
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
    cases = [(case, mode) for case in manifest["cases"] if "session_tracks" in case
             for mode, flag in modes if case.get(flag)]
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
        run([str(javac), "-cp", str(jar), "-d", str(classes),
             str(ROOT / "tools/reference/TrackerReference.java"),
             str(ROOT / "tools/reference/TrackerSessionReference.java"), str(ROOT / "tools/reference/VectorFixture.java"),
             str(ROOT / "tools/reference/TrackerSessionDiagnosticsTest.java")])
        diagnostic_home = root / "diagnostic-home"
        diagnostic_home.mkdir()
        run([str(java_path), "-Djava.awt.headless=true", f"-Duser.home={diagnostic_home}", "-Xmx256m",
             "-cp", os.pathsep.join([str(classes), str(jar)]), "TrackerSessionDiagnosticsTest"])
        for case, mode in cases:
            if probe and case.get("session_known_issue"):
                raise ValueError("Known Tracker defects cannot be used as native export acceptance")
            output = root / (case["id"] + ("-" + mode if probe else ""))
            java_home = output / "java-home"
            java_home.mkdir(parents=True)
            project = project_path(case, cache)
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
                     "-cp", os.pathsep.join([str(classes), str(jar)]), "VectorFixture", str(project), mode])
                case = {**case, "session_tracks": [track for track in case["session_tracks"]
                                                   if track["class"] == "org.opensourcephysics.cabrillo.tracker.CoordAxes"]}
            expected = None
            if probe and mode.startswith("snap-"):
                # Rejected exports preserve their existing destination. Java independently authors
                # the unstable geometry so the actual application proves why rejection is required.
                destination = output / "rejected-destination.trk"
                run([str(Path(probe).resolve()), "--export-" + mode, str(project), str(destination)])
                if destination.read_bytes() != project.read_bytes():
                    raise ValueError("Rejected snapping export changed the destination")
                project = output / "java-snapping-source.trk"
                run([str(java_path), "-Djava.awt.headless=true", f"-Duser.home={java_home}", "-Xmx256m",
                     "-cp", os.pathsep.join([str(classes), str(jar)]), "VectorFixture", str(project), mode])
                expected = snapping_roundtrip_expected(case, mode)
            elif probe:
                exported = output / "motrack-export.trk"
                run([str(Path(probe).resolve()), "--export-" + mode, str(project), str(exported)])
                if mode == "mass":
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
            # Tracker has static per-panel drawing state but allocates IDs separately in each TFrame.
            # A fresh JVM prevents two windows' vector snapping registries from sharing the same IDs.
            for stage in ["save", "reopen"]:
                home = java_home if stage == "save" else output / "java-reopen-home"
                home.mkdir(exist_ok=True)
                source = project if stage == "save" else output / "tracker-resaved.trk"
                try:
                    run([str(java_path), f"-Duser.home={home}", "-Duser.language=en", "-Duser.country=US",
                         "-Xmx256m", "-cp", os.pathsep.join([str(classes), str(jar)]), "TrackerSessionReference",
                         str(source), str(output), stage], display=display)
                except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                    diagnostic = format_session_file_diagnostic(source)
                    resave = ("\nTracker resave diagnostic (bounded): " + format_session_file_diagnostic(output / "tracker-resaved.trk")
                              if stage == "save" else "")
                    raise ValueError(f"{case['id']}/{mode}/{stage}: Tracker reference failed for {source.name}: {error}"
                                     f"\nTracker input diagnostic (bounded): {diagnostic}{resave}") from error
                stages[stage] = json.loads((output / ("session-" + stage + ".json")).read_text(encoding="utf-8"))
                if (stages[stage]["reference_version"] != manifest["reference"]["version"]
                        or stages[stage]["verification_level"] != "tracker_session_" + stage
                        or stages[stage].get("view_preparation") != "visit_custom_tables_restore_selection"):
                    raise ValueError("Unexpected Tracker session stage")
                callbacks = stages[stage].get("loader_callbacks")
                if type(callbacks) is not int or callbacks < 1:
                    raise ValueError("Missing Tracker loader callback evidence")
                if callbacks > 1:
                    print(f"{case['id']}/{mode}/{stage}: observed {callbacks} loader callbacks; reference action ran once")
            report = {"reference_version": stages["save"]["reference_version"], "verification_level": "tracker_session_resave",
                      "view_preparation": stages["save"]["view_preparation"],
                      "loader_callbacks": {stage: stages[stage]["loader_callbacks"] for stage in stages},
                      "before": stages["save"]["loaded"], "after_save": stages["save"]["after_save"],
                      "after": stages["reopen"]["loaded"]}
            keys = {"marks": [[0, 3], [7]], "empty-marks": [[], [0]], "first-mark": [[4], [0]]}.get(mode)
            if mode in {"create-mass", "create-empty-mass"}:
                keys = [[0], [0], [] if mode == "create-empty-mass" else [1, 4, 8]]
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
                    print(f"{case['id']}/{mode}: passed (MoTrack edit/native reopen/TRK export; Tracker load/save/reopen; native reimport)")
            elif known_report is not None:
                saved_report, suffix = known_report, "-known-issue.json"
                print(f"{case['id']}: KNOWN TRACKER DEFECT reproduced (members and samples lost; round trip NOT accepted)")
            else:
                saved_report, suffix = report, "-session.json"
                print(f"{case['id']}: passed (Tracker application load/save/reopen; MoTrack export untested)")
            (reports / (case["id"] + suffix)).write_text(
                json.dumps(saved_report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["fetch", "verify", "reference", "check", "session", "roundtrip"])
    parser.add_argument("--cache", type=Path, default=ROOT / ".dev-tools/tracker-reference")
    parser.add_argument("--java", default="java", help="Java 17+ JDK executable (reference/session/roundtrip commands)")
    parser.add_argument("--probe", help="Built motrack_corpus_probe executable (check/roundtrip commands)")
    parser.add_argument("--record", action="store_true", help="Explicitly replace committed Java baselines for review")
    args = parser.parse_args()
    if args.record and args.command != "reference":
        parser.error("--record is only valid for the reference command")
    if args.command in {"check", "roundtrip"} and not args.probe:
        parser.error(f"{args.command} requires --probe")
    manifest = load_manifest()
    cache = args.cache.resolve()
    if args.command in {"fetch", "verify"}:
        for path, artifact in artifacts(manifest, cache):
            (fetch_file if args.command == "fetch" and "local_file" not in artifact else verify_file)(path, artifact)
        print(f"Verified {len(manifest['cases'])} cases and Tracker {manifest['reference']['version']}")
    elif args.command in {"session", "roundtrip"}:
        check_sessions(manifest, cache, args.java, args.probe if args.command == "roundtrip" else None)
    else:
        check_cases(manifest, cache, java=args.java if args.command == "reference" else None,
                    probe=args.probe, record=args.record)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, zipfile.BadZipFile, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error)) from error
