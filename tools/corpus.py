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
        expected_path(case)
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


def session_units(case, expected):
    # Java materializes the pinned application's default when the original XML omits a label.
    # The corpus records that default independently; do not infer expectations from resaved output.
    return {"length_unit": expected["length_unit"] or case["session_default_length_unit"],
            "mass_unit": expected["mass_unit"]}


def validate_session(case, report, manifest, expected=None, key_frames=None, calibration=None):
    if (report["reference_version"] != manifest["reference"]["version"]
            or report["verification_level"] != "tracker_session_resave"):
        raise ValueError("Unexpected session reference version or verification level")
    before = report["before"]
    tolerance = manifest["tolerance"]
    compare(before, report["after_save"], tolerance, case["id"] + ".save")
    compare(before, report["after"], tolerance, case["id"] + ".resave")
    identities = [{"class": track["class"], "name": track["name"]} for track in before["tracks"]]
    compare(case["session_tracks"], identities, tolerance, case["id"] + ".tracks")
    if expected is None:
        expected = json.loads(expected_path(case).read_text(encoding="utf-8"))["project"]
    compare(session_units(case, expected), {key: before[key] for key in ["length_unit", "mass_unit"]},
            tolerance, case["id"] + ".units")
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
    if key_frames is not None:
        actual = [track["key_frames"] for track in before["tracks"]
                  if track["class"] == "org.opensourcephysics.cabrillo.tracker.PointMass"]
        compare(key_frames, actual, tolerance, case["id"] + ".key_frames")


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
             ("keyed-calibration", "native_keyed_calibration_roundtrip")] if probe else [("session", "session_tracks")]
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
             str(ROOT / "tools/reference/TrackerSessionReference.java"), str(ROOT / "tools/reference/VectorFixture.java")])
        for case, mode in cases:
            output = root / (case["id"] + ("-" + mode if probe else ""))
            java_home = output / "java-home"
            java_home.mkdir(parents=True)
            project = project_path(case, cache)
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
                run([str(java_path), f"-Duser.home={home}", "-Duser.language=en", "-Duser.country=US",
                     "-Xmx256m", "-cp", os.pathsep.join([str(classes), str(jar)]), "TrackerSessionReference",
                     str(source), str(output), stage], display=display)
                stages[stage] = json.loads((output / ("session-" + stage + ".json")).read_text(encoding="utf-8"))
                if (stages[stage]["reference_version"] != manifest["reference"]["version"]
                        or stages[stage]["verification_level"] != "tracker_session_" + stage):
                    raise ValueError("Unexpected Tracker session stage")
            report = {"reference_version": stages["save"]["reference_version"], "verification_level": "tracker_session_resave",
                      "before": stages["save"]["loaded"], "after_save": stages["save"]["after_save"],
                      "after": stages["reopen"]["loaded"]}
            keys = {"marks": [[0, 3], [7]], "empty-marks": [[], [0]], "first-mark": [[4], [0]]}.get(mode)
            calibration = keyed_calibration_expected(mode == "keyed-calibration") if case["id"] == "keyed-calibration" else None
            try:
                validate_session(case, report, manifest, expected, keys, calibration)
            except ValueError as error:
                raise ValueError(f"{error}\nTracker session snapshots (bounded): {json.dumps(report)[:24000]}") from error
            if probe:
                reimport = output / "native-reimport.json"
                run([str(Path(probe).resolve()), str(output / "tracker-resaved.trk"), str(reimport)])
                expected["source_version"] = manifest["reference"]["version"]
                expected.update(session_units(case, expected))
                compare(expected, json.loads(reimport.read_text(encoding="utf-8")), manifest["tolerance"], case["id"] + ".reimport")
                saved_report = {"verification_level": "motrack_" + mode + "_roundtrip", "tracker_session": report, "expected_reimport": expected}
                suffix = "-" + mode + "-roundtrip.json"
                if mode.startswith("snap-"):
                    saved_report["verification_level"] = "motrack_rejected_" + mode
                    print(f"{case['id']}/{mode}: passed (MoTrack rejected unsafe export; actual Tracker snapping confirmed; resave/reimport)")
                else:
                    print(f"{case['id']}/{mode}: passed (MoTrack edit/native reopen/TRK export; Tracker load/save/reopen; native reimport)")
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
