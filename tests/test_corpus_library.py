"""Discovery, manifest, download-limit and census tests for the real-world library corpus (no network)."""

import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import corpus
import corpus_library as library

FIXTURES = Path(__file__).resolve().parent / "corpus" / "fixtures" / "library"
COMPADRE = library.SOURCES[0]["url"]


class Response(io.BytesIO):
    def __init__(self, data, url, headers=None):
        super().__init__(data)
        self.url, self.headers = url, headers or {}

    def geturl(self):
        return self.url


class FakeNetwork:
    """Serves canned bytes per URL and records every request (URL, User-Agent, time)."""

    def __init__(self, routes, clock):
        self.routes, self.clock, self.calls = routes, clock, []

    def __call__(self, request, timeout):
        url = request.full_url
        self.calls.append((url, request.get_header("User-agent"), self.clock.now))
        route = self.routes[url]
        if isinstance(route, list):
            route = route.pop(0)
        if isinstance(route, Exception):
            raise route
        if isinstance(route, tuple):
            return Response(route[0], route[1], route[2] if len(route) > 2 else None)
        return Response(route, url)


class Clock:
    def __init__(self):
        self.now = 100.0
        self.sleeps = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


def tracker_project(tracks=("PointMass",), version="6.1.1", extra=""):
    items = "".join(f'<property name="item" type="object"><object class="org.opensourcephysics.cabrillo.tracker.{name}">'
                    f'<property name="name" type="string">{name}</property></object></property>' for name in tracks)
    return (f'<?xml version="1.0" encoding="UTF-8"?>\n<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">'
            f'<property name="semantic_version" type="string">{version}</property>'
            f'<property name="tracks" type="collection" class="java.util.ArrayList">{items}</property>{extra}</object>\n').encode()


def archive(members):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as output:
        for name, data in members.items():
            output.writestr(name, data)
    return stream.getvalue()


class LibraryTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="motrack-library-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.clock = Clock()

    def http_error(self, code):
        error = urllib.error.HTTPError("https://www.compadre.org/x", code, "status", {}, None)
        self.addCleanup(error.close)
        return error

    def client(self, routes, **options):
        self.network = FakeNetwork(routes, self.clock)
        return library.PoliteClient(library.METADATA_HOSTS, opener=self.network, sleep=self.clock.sleep, clock=self.clock,
                                    **options)

    def test_compadre_discovery_matches_the_library_browser_and_keeps_every_tracker_document(self):
        count, candidates = library.parse_compadre((FIXTURES / "compadre-tracker-sample.xml").read_bytes())
        self.assertEqual(count, 7)
        by_id = {entry["id"]: entry for entry in candidates}
        self.assertEqual(sorted(by_id), ["compadre-10311-1797", "compadre-11617-2517", "compadre-12037-5886",
                                         "compadre-12037-5888", "compadre-16672-5833", "compadre-16672-5878",
                                         "compadre-16833-5923"])
        main = by_id["compadre-10311-1797"]
        self.assertEqual(main["url"], "https://www.compadre.org/osp/document/ServeFile.cfm?ID=10311&DocID=1797&TrackerSet=1")
        self.assertEqual((main["file"], main["original_name"], main["listed_bytes"]),
                         ("compadre-10311-1797.trz", "video-tracker-uniform-motion-ball-on-track.trz", 1165166))
        self.assertEqual(main["authors"], ["Aaron Titus"])
        self.assertIn("Classical Mechanics / Motion in One Dimension / Velocity", main["subjects"])
        self.assertEqual(main["information_url"], "https://www.compadre.org/osp/items/detail.cfm?ID=10311")
        # All three records of the item open the Main TRZ in Tracker's browser, including both video records.
        self.assertEqual(main["compadre"]["library_browser_records"],
                         ["Uniform Motion TRZ", "Fast uniform motion video", "Slow uniform motion video"])
        # Tracker's browser never opens "Source Code" TRZ attachments: they are kept here, but marked unreachable.
        for unreachable in ["compadre-12037-5886", "compadre-12037-5888", "compadre-16672-5878", "compadre-16833-5923"]:
            self.assertEqual(by_id[unreachable]["compadre"]["attachment_type"], "Source Code")
            self.assertEqual(by_id[unreachable]["compadre"]["library_browser_records"], [])
        self.assertEqual(by_id["compadre-12037-5886"]["compadre"]["records"], ["uniform-motion-ball-slow TRZ"])
        self.assertEqual(by_id["compadre-11617-2517"]["file"], "compadre-11617-2517.zip")

    def test_compadre_selection_reproduces_tracker_attachment_rules(self):
        root = library.parse_xml((FIXTURES / "compadre-tracker-sample.xml").read_bytes())
        records = root.findall(f".//{library.COMPADRE_NS}record")
        chosen = []
        for record in records:
            attachment = library.library_browser_attachment(record, {"Main", "Primary"})
            if attachment is None:
                attachment = library.library_browser_attachment(record, {"Supplemental"})
            chosen.append(None if attachment is None else attachment.findtext(library.COMPADRE_NS + "file-name"))
        self.assertEqual(chosen, ["video-tracker-uniform-motion-ball-on-track.trz"] * 3 + [
            "tracker_solar_rotation.zip", "uniform-motion-ball-slow.mp4", "explosiveCollision.trz", None])

    def test_compadre_response_must_be_consistent_and_free_of_entity_declarations(self):
        data = (FIXTURES / "compadre-tracker-sample.xml").read_bytes()
        for broken in [data.replace(b"<recordCount>7<", b"<recordCount>8<"),
                       data.replace(b"<compadre-identifier>10311<", b"<compadre-identifier>x<", 1),
                       b'<?xml version="1.0"?><!DOCTYPE x [<!ENTITY a "aaaa">]><x>&a;</x>',
                       data.replace(b"https://www.compadre.org/osp/document/ServeFile.cfm?ID=10311&amp;DocID=1797&amp;TrackerSet=1",
                                    b"http://www.compadre.org/osp/document/ServeFile.cfm?ID=10311", 1)]:
            with self.subTest(prefix=broken[:60]), self.assertRaises(ValueError):
                library.parse_compadre(broken)

    def test_rights_statements_are_copied_verbatim_with_their_links(self):
        rights = library.parse_compadre_rights((FIXTURES / "compadre-detail-sample.html").read_bytes())
        self.assertEqual(rights, {
            "Access Rights": {"statement": "Free access"},
            "License": {"statement": "This material is released under a Creative Commons Attribution-Noncommercial-Share Alike 3.0 license.",
                        "links": ["http://creativecommons.org/licenses/by-nc-sa/3.0/"]},
            "Rights Holder": {"statement": "Aaron Titus"}})
        # Line breaks separate words as the page renders them (ComPADRE items 13020 and 13021).
        page = (b'<dl><dt>Access Rights:</dt><dd><i>Free access</i> and<br /><i>Available by subscription</i></dd>'
                b'<dt>Restriction:</dt><dd>&copy; 2013 Paulo  Sime\xc3\xa3o Carvalho<br /></dd></dl>')
        self.assertEqual(library.parse_compadre_rights(page), {
            "Access Rights": {"statement": "Free access and Available by subscription"},
            "Restriction": {"statement": "\u00a9 2013 Paulo Sime\u00e3o Carvalho"}})

    def test_osp_collections_resolve_inherited_base_paths_and_keep_only_tracker_files(self):
        library_xml = b"""<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.Library">
    <property name="name" type="string">Home</property>
    <property name="collection_paths" type="array" class="[Ljava.lang.String;">
        <property name="[0]" type="string">https://opensourcephysics.github.io/resources/CAB/a.xml</property>
        <property name="[1]" type="string">https://elsewhere.example/b.xml</property>
    </property>
    <property name="collection_names" type="array" class="[Ljava.lang.String;">
        <property name="[0]" type="string">Sampler</property>
    </property>
</object>"""
        self.assertEqual(library.parse_osp_library(library_xml), ("Home", [
            ("https://opensourcephysics.github.io/resources/CAB/a.xml", "Sampler"),
            ("https://elsewhere.example/b.xml", "https://elsewhere.example/b.xml")]))
        collection = b"""<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryCollection">
    <property name="name" type="string">Sampler</property>
    <property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">
        <property name="[0]" type="object">
        <object class="org.opensourcephysics.tools.LibraryCollection">
            <property name="name" type="string">Mechanics</property>
            <property name="base_path" type="string">https://opensourcephysics.github.io/resources/CAB/experiments/</property>
            <property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">
                <property name="[0]" type="object">
                <object class="org.opensourcephysics.tools.LibraryResource">
                    <property name="name" type="string">Ball toss</property>
                    <property name="html_path" type="string">BallToss.html</property>
                    <property name="target" type="string">BallToss.zip</property>
                    <property name="type" type="string">Tracker</property>
                </object>
                </property>
                <property name="[1]" type="object">
                <object class="org.opensourcephysics.tools.LibraryResource">
                    <property name="name" type="string">Video</property>
                    <property name="target" type="string">BallToss.mp4</property>
                    <property name="type" type="string">Video</property>
                </object>
                </property>
            </property>
        </object>
        </property>
        <property name="[1]" type="object">
        <object class="org.opensourcephysics.tools.LibraryResource">
            <property name="name" type="string">Relative</property>
            <property name="target" type="string">../shared/My Run.trk</property>
            <property name="type" type="string">Tracker</property>
        </object>
        </property>
    </property>
</object>"""
        found = library.parse_osp_collection(collection, "https://opensourcephysics.github.io/resources/CAB/a.xml", ["Home"])
        self.assertEqual(found, [
            {"url": "https://opensourcephysics.github.io/resources/CAB/experiments/BallToss.zip",
             "name": "Home / Sampler / Mechanics / Ball toss", "type": "Tracker",
             "html": "https://opensourcephysics.github.io/resources/CAB/experiments/BallToss.html"},
            {"url": "https://opensourcephysics.github.io/resources/shared/My Run.trk", "name": "Home / Sampler / Relative",
             "type": "Tracker", "html": None}])
        identifier = library.osp_experiment_id(found[1]["url"])
        self.assertRegex(identifier, r"^osp-resources-shared-my-run-[0-9a-f]{8}$")
        self.assertEqual(identifier, library.osp_experiment_id(found[1]["url"]))
        with self.assertRaises(ValueError):
            library.parse_osp_collection(library_xml, "https://opensourcephysics.github.io/a.xml", [])

    def test_urls_must_be_https_on_an_allowlisted_host(self):
        library.check_url("https://www.compadre.org/osp/document/ServeFile.cfm?ID=1", library.DOWNLOAD_HOSTS)
        for url in ["http://www.compadre.org/x.trz", "https://evil.example/x.trz", "https://user@www.compadre.org/x.trz",
                    "https://www.compadre.org:8443/x.trz", "file:///etc/passwd", "https://api.github.com/x.trz"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                library.check_url(url, library.DOWNLOAD_HOSTS)
        self.assertEqual(library.quote_url("https://iwant2study.org/a b/c(1).trz?x=1&y=a b"),
                         "https://iwant2study.org/a%20b/c(1).trz?x=1&y=a%20b")
        guard = library._RedirectGuard(library.DOWNLOAD_HOSTS)
        with self.assertRaises(ValueError):
            guard.redirect_request(None, None, 302, "Found", {}, "https://evil.example/x.trz")

    def test_client_waits_between_requests_retries_transient_errors_and_sends_a_generic_agent(self):
        url = "https://www.compadre.org/a"
        unavailable = self.http_error(503)
        client = self.client({url: [unavailable, urllib.error.URLError("reset"), b"ok"], "https://www.compadre.org/b": b"two"})
        self.assertEqual(client.get(url), b"ok")
        self.assertEqual(client.get("https://www.compadre.org/b"), b"two")
        times = [call[2] for call in self.network.calls]
        self.assertEqual(len(times), 4)
        self.assertTrue(all(later - earlier >= library.MIN_REQUEST_DELAY_SECONDS for earlier, later in zip(times, times[1:])))
        self.assertEqual(self.clock.sleeps[:2], [2.0, 4.0])
        self.assertEqual({call[1] for call in self.network.calls}, {"MoTrack-corpus/1.0"})
        for status in [404, 403]:
            client = self.client({url: [self.http_error(status), b"never"]})
            with self.subTest(status=status), self.assertRaisesRegex(ValueError, f"HTTP {status}"):
                client.get(url)
            self.assertEqual(len(self.network.calls), 1)
        client = self.client({url: [urllib.error.URLError("down")] * 4})
        with self.assertRaisesRegex(ValueError, "Request failed"):
            client.get(url)
        self.assertEqual(len(self.network.calls), 4)
        with self.assertRaises(ValueError):
            library.PoliteClient(library.METADATA_HOSTS, delay=0.5)

    def test_downloads_are_capped_hashed_and_published_only_when_complete(self):
        data = b"tracker archive"
        url = "https://www.compadre.org/osp/document/ServeFile.cfm?ID=1&DocID=2&TrackerSet=1"
        target = self.root / "files" / "compadre-1-2.trz"
        client = self.client({url: data})
        self.assertEqual(client.download(url, target), {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        self.assertEqual(target.read_bytes(), data)
        expected = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, route, options in [
                ("too large", data * 3, {"max_bytes": len(data) * 2}),
                ("declared too large", (data, url, {"Content-Length": str(10 ** 9)}), {}),
                ("redirected", (data, "https://evil.example/x.trz"), {}),
                ("metadata host", (data, "https://api.github.com/x.trz"), {}),
                ("checksum", b"x" * len(data), {"expected": expected}),
                ("longer than pinned", data + b"!", {"expected": expected}),
                ("empty", b"", {})]:
            destination = self.root / name / "file.trz"
            client = self.client({url: route})
            with self.subTest(name=name), self.assertRaises(ValueError):
                client.download(url, destination, **options)
            self.assertEqual(list(destination.parent.iterdir()) if destination.parent.exists() else [], [])
        with self.assertRaises(ValueError):
            self.client({}).download("https://rbtsps.ieee-saopen.org/x.trz", self.root / "x.trz")

    def manifest(self, **changes):
        data = b"tracker"
        entry = {"id": "compadre-1-2", "source": "compadre-tracker", "file": "compadre-1-2.trz",
                 "url": "https://www.compadre.org/osp/document/ServeFile.cfm?ID=1&DocID=2&TrackerSet=1",
                 "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "projects": 1, "discovered": "2026-11-21"}
        entry.update(changes)
        return {"schema_version": 1, "refreshed": "2026-11-21", "sources": [{"id": "compadre-tracker"}], "experiments": [entry]}

    def test_manifest_validation_rejects_unsafe_or_inconsistent_entries(self):
        library.validate_library_manifest(self.manifest())
        for changes in [{"id": "../escape"}, {"id": "Upper"}, {"file": "other.trz"}, {"file": "compadre-1-2.exe", "id": "compadre-1-2"},
                        {"url": "http://www.compadre.org/x"}, {"url": "https://evil.example/x.trz"},
                        {"bytes": 0}, {"bytes": library.MAX_FILE_BYTES + 1}, {"bytes": "7"}, {"sha256": "abc"},
                        {"projects": 0}, {"discovered": "yesterday"}, {"source": "unknown"},
                        {"duplicate_of": "compadre-9-9"}]:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                library.validate_library_manifest(self.manifest(**changes))
        manifest = self.manifest()
        manifest["experiments"].append(copy.deepcopy(manifest["experiments"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            library.validate_library_manifest(manifest)
        second = copy.deepcopy(manifest["experiments"][0])
        second.update(id="compadre-1-3", file="compadre-1-3.trz")
        manifest["experiments"][1] = second
        with self.assertRaisesRegex(ValueError, "identical content"):
            library.validate_library_manifest(manifest)
        second["duplicate_of"] = "compadre-1-2"
        library.validate_library_manifest(manifest)
        many = self.manifest()
        many["experiments"] = []
        for index in range(library.MAX_TOTAL_BYTES // library.MAX_FILE_BYTES + 1):
            many["experiments"].append({**self.manifest()["experiments"][0], "id": f"compadre-1-{index}",
                                        "file": f"compadre-1-{index}.trz", "bytes": library.MAX_FILE_BYTES,
                                        "sha256": f"{index:064x}"})
        with self.assertRaisesRegex(ValueError, "total size"):
            library.validate_library_manifest(many)
        known = {"issue": 211, "summary": "heap overflow", "experiments": ["compadre-1-2"]}
        library.validate_library_manifest({**self.manifest(), "known_issues": [known]})
        self.assertEqual(library.known_issues({**self.manifest(), "known_issues": [known]}), {"compadre-1-2": 211})
        for broken in [{**known, "issue": "211"}, {**known, "experiments": []}, {**known, "experiments": ["compadre-9-9"]},
                       {key: value for key, value in known.items() if key != "summary"}]:
            with self.subTest(known=broken), self.assertRaises(ValueError):
                library.validate_library_manifest({**self.manifest(), "known_issues": [broken]})
        with self.assertRaises(ValueError):
            library.validate_library_manifest({**self.manifest(), "known_issues": [known, {**known, "issue": 212}]})

    def test_committed_manifest_and_census_are_valid_and_consistent(self):
        manifest = library.load_library_manifest()
        census = json.loads(library.LIBRARY_CENSUS.read_text(encoding="utf-8"))
        self.assertEqual(census["manifest_refreshed"], manifest["refreshed"])
        unique = [entry["id"] for entry in manifest["experiments"] if not entry.get("duplicate_of")]
        self.assertEqual([record["id"] for record in census["experiments"]], unique)
        self.assertEqual(census["aggregate"]["experiments"], len(unique))
        self.assertEqual(census["aggregate"], library.aggregate(census["experiments"]))
        readme = library.README.read_text(encoding="utf-8")
        self.assertIn(library.census_markdown(census), readme)
        self.assertEqual({source["id"] for source in manifest["sources"]}, set(library.SOURCE_IDS))
        self.assertTrue(all(entry["url"].startswith("https://") for entry in manifest["experiments"]))

    def test_fetch_verifies_existing_files_downloads_missing_ones_and_reports_every_failure(self):
        data = b"tracker"
        manifest = self.manifest()
        manifest["experiments"].append({**manifest["experiments"][0], "id": "compadre-1-3", "file": "compadre-1-3.trz",
                                        "url": "https://www.compadre.org/osp/document/ServeFile.cfm?ID=1&DocID=3",
                                        "sha256": hashlib.sha256(b"other!!").hexdigest()})
        first, second = (library.cache_path(self.root, entry) for entry in manifest["experiments"])
        self.assertEqual(first.name, f"compadre-1-2-{hashlib.sha256(data).hexdigest()[:12]}.trz")
        client = self.client({manifest["experiments"][0]["url"]: data, manifest["experiments"][1]["url"]: b"other!!"})
        self.assertEqual(library.fetch(manifest, self.root, client=client, log=lambda _: None), (2, 2))
        self.assertEqual(library.fetch(manifest, self.root, client=self.client({}), log=lambda _: None), (2, 0))
        # A corrupt cached copy is deleted and downloaded again; only verified bytes are published.
        first.write_bytes(b"x" * len(data))
        second.unlink()
        client = self.client({manifest["experiments"][0]["url"]: data, manifest["experiments"][1]["url"]: b"changed"})
        with self.assertRaisesRegex(ValueError, "(?s)compadre-1-3: Checksum mismatch") as raised:
            library.fetch(manifest, self.root, client=client, log=lambda _: None)
        self.assertNotIn("compadre-1-2:", str(raised.exception))
        self.assertEqual(first.read_bytes(), data)
        self.assertEqual(sorted(path.name for path in first.parent.iterdir()), [first.name])

    def test_a_repinned_file_is_fetched_under_its_new_name_and_stale_files_are_pruned(self):
        data, replacement = b"tracker", b"tracker, version 2"
        old = self.manifest()
        files = library.library_files(self.root)
        files.mkdir(parents=True)
        stale = library.cache_path(self.root, old["experiments"][0])
        stale.write_bytes(data)
        (files / "unlisted.trz").write_bytes(b"left over")
        (files / "compadre-1-2-0123.trz.abc.part").write_bytes(b"partial")
        repinned = self.manifest(bytes=len(replacement), sha256=hashlib.sha256(replacement).hexdigest())
        client = self.client({repinned["experiments"][0]["url"]: replacement})
        self.assertEqual(library.fetch(repinned, self.root, client=client, log=lambda _: None), (1, 1))
        current = library.cache_path(self.root, repinned["experiments"][0])
        self.assertNotEqual(current, stale)
        self.assertEqual([path.name for path in files.iterdir()], [current.name])
        self.assertEqual(current.read_bytes(), replacement)
        # A verified copy under the old ``<id>.<ext>`` cache name is adopted without a download.
        current.replace(files / "compadre-1-2.trz")
        self.assertEqual(library.fetch(repinned, self.root, client=self.client({}), log=lambda _: None), (1, 0))
        self.assertEqual([path.name for path in files.iterdir()], [current.name])
        # A wrong legacy copy is never adopted; the pinned bytes are downloaded and the stray is pruned.
        current.unlink()
        (files / "compadre-1-2.trz").write_bytes(b"x" * len(replacement))
        client = self.client({repinned["experiments"][0]["url"]: replacement})
        self.assertEqual(library.fetch(repinned, self.root, client=client, log=lambda _: None), (1, 1))
        self.assertEqual([path.name for path in files.iterdir()], [current.name])

    def discovery_routes(self, project_zip):
        rest = (FIXTURES / "compadre-tracker-sample.xml").read_bytes()
        detail = (FIXTURES / "compadre-detail-sample.html").read_bytes()
        _, candidates = library.parse_compadre(rest)
        routes = {COMPADRE: rest}
        for candidate in candidates:
            routes[candidate["information_url"]] = detail
            routes[candidate["url"]] = project_zip
        solar = next(candidate["url"] for candidate in candidates if candidate["id"] == "compadre-11617-2517")
        routes[solar] = archive({"readme.txt": b"no project"})
        routes[next(candidate["url"] for candidate in candidates if candidate["id"] == "compadre-16833-5923")] = \
            self.http_error(404)
        return routes, candidates

    def test_discovery_pins_files_excludes_non_projects_and_reuses_verified_pins(self):
        project_zip = archive({"lab.trk": tracker_project(), "video.mp4": b"\0" * 8})
        routes, candidates = self.discovery_routes(project_zip)
        client = self.client(routes)
        manifest = library.discover(self.root, sources={"compadre-tracker"}, client=client, today="2026-11-21", log=lambda _: None)
        pinned = {entry["id"]: entry for entry in manifest["experiments"]}
        self.assertEqual(sorted(pinned), ["compadre-10311-1797", "compadre-12037-5886", "compadre-12037-5888",
                                          "compadre-16672-5833", "compadre-16672-5878"])
        first = pinned["compadre-10311-1797"]
        self.assertEqual((first["bytes"], first["sha256"], first["projects"]),
                         (len(project_zip), hashlib.sha256(project_zip).hexdigest(), 1))
        self.assertEqual(first["rights"]["License"]["links"], ["http://creativecommons.org/licenses/by-nc-sa/3.0/"])
        self.assertNotIn("duplicate_of", first)
        self.assertEqual(pinned["compadre-12037-5886"]["duplicate_of"], "compadre-10311-1797")
        self.assertEqual({item["id"]: item["reason"][:20] for item in manifest["excluded"]},
                         {"compadre-11617-2517": "Archive contains no ", "compadre-16833-5923": "HTTP 404 for https:/"})
        self.assertEqual(manifest["sources"][0]["records"], 7)
        detail_requests = [call for call in self.network.calls if "detail.cfm" in call[0]]
        self.assertEqual(len(detail_requests), len({candidate["information_url"] for candidate in candidates}))
        files = sorted(path.name for path in library.library_files(self.root).iterdir())
        self.assertEqual(files, sorted(library.cache_path(self.root, entry).name for entry in manifest["experiments"]))

        # Refreshing reuses verified pins (no file downloads) and records experiments that disappeared upstream.
        previous = copy.deepcopy(manifest)
        gone = "https://www.compadre.org/osp/document/ServeFile.cfm?ID=1&DocID=1&TrackerSet=1"
        previous["experiments"].append({**first, "id": "compadre-1-1", "file": "compadre-1-1.trz", "url": gone, "sha256": "1" * 64})
        # A revised identifier scheme renames the verified cache file instead of downloading it again.
        legacy = previous["experiments"][0]
        legacy.update(id="legacy-10311", file="legacy-10311.trz")
        files = library.library_files(self.root)
        current = library.cache_path(self.root, first)
        current.replace(files / "legacy-10311.trz")
        routes, _ = self.discovery_routes(project_zip)
        for candidate in candidates:
            routes.pop(candidate["url"])
        routes[solar := next(candidate["url"] for candidate in candidates if candidate["id"] == "compadre-11617-2517")] = \
            archive({"readme.txt": b"no project"})
        routes[next(candidate["url"] for candidate in candidates if candidate["id"] == "compadre-16833-5923")] = \
            self.http_error(404)
        refreshed = library.discover(self.root, sources={"compadre-tracker"}, client=self.client(routes), today="2026-12-01",
                                     previous=previous, log=lambda _: None)
        self.assertEqual([entry["sha256"] for entry in refreshed["experiments"]],
                         [entry["sha256"] for entry in manifest["experiments"]])
        self.assertEqual({entry["discovered"] for entry in refreshed["experiments"]}, {"2026-11-21"})
        self.assertEqual(refreshed["withdrawn"], [{"id": "compadre-1-1", "url": gone, "sha256": "1" * 64,
                                                   "withdrawn": "2026-12-01"}])
        self.assertEqual(refreshed["experiments"][0]["id"], "compadre-10311-1797")
        self.assertTrue(current.is_file())
        self.assertFalse((files / "legacy-10311.trz").exists())
        self.assertFalse(any("ServeFile" in call[0] and "TrackerSet" in call[0] and "11617" not in call[0]
                             and "16833" not in call[0] for call in self.network.calls))

        # A failed forced refresh keeps the previous verified pin instead of excluding the experiment.
        routes, _ = self.discovery_routes(project_zip)
        for candidate in candidates:
            routes[candidate["url"]] = self.http_error(404)
        kept = library.discover(self.root, sources={"compadre-tracker"}, client=self.client(routes, retries=0),
                                today="2026-12-02", previous=refreshed, redownload=True, log=lambda _: None)
        self.assertEqual([entry["sha256"] for entry in kept["experiments"]],
                         [entry["sha256"] for entry in refreshed["experiments"]])
        self.assertTrue(current.is_file())

    def test_archive_projects_are_read_without_extraction_and_with_bounds(self):
        path = self.root / "sample.trz"
        path.write_bytes(archive({"../escape.trk": b"<object/>", "nested/b.trk": b"<object/>", "video.mp4": b"v",
                                  "__MACOSX/nested/._b.trk": b"\0\5\26\7", "._top.trk": b"\0"}))
        self.assertEqual(library.archive_projects(path), ["../escape.trk", "nested/b.trk"])
        self.assertEqual(list(library.read_projects(path)), [("../escape.trk", b"<object/>"), ("nested/b.trk", b"<object/>")])
        self.assertFalse((self.root.parent / "escape.trk").exists())
        self.assertEqual(library.archive_file_types(path), {"(appledouble)": 2, "mp4": 1, "trk": 2})
        with self.assertRaisesRegex(ValueError, "size limit"):
            list(library.read_projects(path, limit=3))
        with patch.object(library, "MAX_ARCHIVE_MEMBERS", 2), self.assertRaisesRegex(ValueError, "too many"):
            library.archive_projects(path)
        self.assertTrue(library.is_appledouble("__MACOSX/x/._y.trk") and library.is_appledouble("._y.trk"))
        self.assertFalse(library.is_appledouble("macosx/y.trk"))

    def test_library_projects_materialize_unique_experiments_for_oracles(self):
        manifest = self.manifest()
        data = archive({"a.trk": tracker_project(), "b.trk": tracker_project(("Vector",))})
        manifest["experiments"][0].update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), projects=2)
        manifest["experiments"].append({**manifest["experiments"][0], "id": "compadre-1-3", "file": "compadre-1-3.trz",
                                        "duplicate_of": "compadre-1-2"})
        files = library.library_files(self.root)
        files.mkdir(parents=True)
        cached = library.cache_path(self.root, manifest["experiments"][0])
        cached.write_bytes(data)
        items = library.library_projects(manifest, self.root)
        self.assertEqual([(item["experiment"], item["member"]) for item in items], [("compadre-1-2", "a.trk"), ("compadre-1-2", "b.trk")])
        self.assertEqual(Path(items[1]["path"]).read_bytes(), tracker_project(("Vector",)))
        self.assertEqual(items[1]["sha256"], hashlib.sha256(tracker_project(("Vector",))).hexdigest())
        cached.write_bytes(data[:-1] + b"!")
        with self.assertRaisesRegex(ValueError, "Checksum"):
            library.library_projects(manifest, self.root)
        # An unreadable archive names its experiment and cached path.
        broken = b"PK\3\4 not really a zip"
        manifest["experiments"][0].update(bytes=len(broken), sha256=hashlib.sha256(broken).hexdigest())
        broken_path = library.cache_path(self.root, manifest["experiments"][0])
        broken_path.write_bytes(broken)
        with self.assertRaisesRegex(library.ArchiveError, "compadre-1-2 .*BadZipFile"):
            library.library_projects(manifest, self.root)

    def test_project_analysis_reports_versions_tracks_media_filters_models_and_functions(self):
        extra = ('<property name="referenceframe" type="string">mass A</property>'
                 '<property name="videoclip" type="object"><object class="org.opensourcephysics.media.core.VideoClip">'
                 '<property name="video" type="object"><object class="org.opensourcephysics.media.xuggle.XuggleVideo">'
                 '<property name="path" type="string">videos/run.MP4</property>'
                 '<property name="filters" type="collection" class="java.util.ArrayList">'
                 '<property name="item" type="object"><object class="org.opensourcephysics.media.core.GhostFilter"/></property>'
                 '<property name="item" type="object"><object class="org.opensourcephysics.media.core.BrightnessFilter"/></property>'
                 '</property></object></property></object></property>')
        data = tracker_project(("PointMass", "PointMass", "DynamicParticle", "CoordAxes"), "5.1.5", extra)
        data = data.replace(b"<property name=\"name\" type=\"string\">DynamicParticle</property>",
                            b"<property name=\"name\" type=\"string\">model</property><property name=\"data_functions\" type=\"array\">"
                            b"<property name=\"[0]\" type=\"object\"><object class=\"org.opensourcephysics.display.DataFunction\"/></property>"
                            b"</property>")
        self.assertEqual(library.analyze_project(data), {
            "root": "TrackerPanel", "semantic_version": "5.1.5", "version": None,
            "tracks": {"CoordAxes": 1, "DynamicParticle": 1, "PointMass": 2}, "models": ["DynamicParticle"],
            "data_functions": 1, "video": "xuggle.XuggleVideo", "video_type": "mp4",
            "filters": ["BrightnessFilter", "GhostFilter"], "reference_frame": True, "datatool_tabs": 0, "custom_views": False})
        self.assertIn("xml_error", library.analyze_project(b"<object class='x'><unclosed></object>"))
        self.assertIn("xml_error", library.analyze_project(b'<!DOCTYPE x [<!ENTITY a "b">]><x>&a;</x>'))
        tabset = (b'<object class="org.opensourcephysics.cabrillo.tracker.TFrame"><property name="tabs" type="array">'
                  b'<property name="[0]" type="string">a.trk</property><property name="[1]" type="string">b.trk</property>'
                  b'</property></object>')
        self.assertEqual(library.analyze_project(tabset)["tabset_tabs"], 2)

    def test_native_and_reference_outcomes_are_platform_neutral(self):
        crt_text = "HEAP CORRUPTION DETECTED: after Normal block (#1) at 0x1.\nCRT detected that the application wrote"
        cache = Path(tempfile.gettempdir()) / "motrack-cache"
        output = self.root / "open.json"
        with patch.object(library, "run_tool", return_value=(1, f"warning\nCould not read {cache}/library/x.trz from C:\\Users\\a\\b.trz")):
            self.assertEqual(library.native_open("probe", "x.trz", output, cache),
                             {"status": "rejected", "message": "Could not read <cache>/library/x.trz from <path>"})

        def opened(command, timeout):
            Path(command[-1]).write_text(json.dumps({"experiments": [
                {"movie": True, "unsupported_tracks": [library.TRACKER + "CoordAxes", library.TRACKER + "TapeMeasure"]},
                {"movie": False, "unsupported_tracks": [library.TRACKER + "CoordAxes"]}]}), encoding="utf-8")
            return 0, ""
        for code, text, status in [(None, "timed out after 180 s", "timed_out"), (3, crt_text, "crashed"),
                                   (3221225477, "", "crashed"), (-11, "", "crashed"), (1, "Invalid Tracker XML.", "rejected")]:
            with self.subTest(code=code), patch.object(library, "run_tool", return_value=(code, text)):
                self.assertEqual(library.native_open("probe", "x.trz", output, cache)["status"], status)
        with patch.object(library, "run_tool", side_effect=opened):
            self.assertEqual(library.native_open("probe", "x.trz", output, cache),
                             {"status": "opened", "experiments": 2, "movies": 1,
                              "unsupported_tracks": {"CoordAxes": 2, "TapeMeasure": 1}})

        project = self.root / "compadre-1-2" / "000.trk"
        project.parent.mkdir()
        manifest = corpus.load_manifest()
        snapshot = {"frame": 3, "world": [1.0, 2.0]}

        def tools(java_project, native_project):
            def run(command, timeout):
                if command[0] == "java":
                    if java_project is None:
                        return 1, "java.lang.IllegalArgumentException: Reference rejected\n\tat X"
                    Path(command[-1]).write_text(json.dumps({"project": java_project}), encoding="utf-8")
                elif native_project is None:
                    return 1, "Unsupported"
                else:
                    Path(command[-1]).write_text(json.dumps(native_project), encoding="utf-8")
                return 0, ""
            return run
        for java_project, native_project, expected in [
                (snapshot, snapshot, {"status": "match"}),
                (snapshot, {"frame": 3, "world": [1.0, 2.5]}, {"status": "differs", "detail": "project.world[1]: expected 2.0, got 2.5"}),
                (None, snapshot, {"status": "java_rejected", "java": "java.lang.IllegalArgumentException: Reference rejected"}),
                (snapshot, None, {"status": "native_rejected", "native": "Unsupported"}),
                (None, None, {"status": "both_rejected", "native": "Unsupported"})]:
            with self.subTest(expected=expected), patch.object(library, "run_tool", side_effect=tools(java_project, native_project)):
                self.assertEqual(library.reference_compare("java", "cp", "probe", project, self.root, self.root, manifest), expected)

    def test_census_visits_unique_experiments_in_manifest_order_and_retries_zip_as_trz(self):
        files = library.library_files(self.root)
        files.mkdir(parents=True)
        payloads = {"compadre-1-2.trz": archive({"one.trk": tracker_project(), "videos/a.mp4": b"v"}),
                    "osp-sampler-a1b2c3d4.zip": archive({"x.trk": tracker_project(("Vector",)), "y.trk": tracker_project(),
                                                         "__MACOSX/._x.trk": b"\0\5\26\7"}),
                    "compadre-9-9.trz": b"not an archive"}
        entries = []
        for name, data in payloads.items():
            identifier = name.rsplit(".", 1)[0]
            entries.append({"id": identifier, "source": "compadre-tracker", "file": name, "bytes": len(data),
                            "sha256": hashlib.sha256(data).hexdigest()})
            library.cache_path(self.root, entries[-1]).write_bytes(data)
        entries.append({**entries[0], "id": "compadre-1-3", "file": "compadre-1-3.trz", "duplicate_of": "compadre-1-2"})
        names = {entry["id"]: library.cache_path(self.root, entry).name for entry in entries}
        opened = []

        def tool(command, timeout):
            self.assertEqual(command[1], "--open")
            opened.append(Path(command[2]).name)
            if command[2].endswith(".zip"):
                return 1, "Choose a Tracker or MoTrack project file."
            Path(command[-1]).write_text(json.dumps({"experiments": [{"movie": False, "unsupported_tracks": []}]}),
                                         encoding="utf-8")
            return 0, ""
        with patch.object(library, "run_tool", side_effect=tool), patch.object(library, "platform_key", return_value="linux"):
            report = library.census({"refreshed": "2026-11-21", "experiments": entries,
                                     "known_issues": [{"issue": 211, "summary": "x", "experiments": ["compadre-1-2"]}]},
                                    self.root, "probe", jobs=2, log=lambda _: None)
        self.assertEqual(sorted(opened), sorted([names["compadre-1-2"], names["compadre-9-9"], "osp-sampler-a1b2c3d4.trz",
                                                 names["osp-sampler-a1b2c3d4"]]))
        self.assertEqual([record["id"] for record in report["experiments"]], ["compadre-1-2", "osp-sampler-a1b2c3d4", "compadre-9-9"])
        self.assertEqual(report["experiments"][0]["native"]["linux"]["known_issue"], 211)
        broken = report["experiments"][2]
        self.assertRegex(broken["archive_error"], r"^compadre-9-9 \(<cache>.*\): BadZipFile")
        self.assertEqual((broken["projects"], broken["tabs"], report["aggregate"]["archive_errors"]), ([], 0, 1))
        self.assertEqual(report["aggregate"]["native"]["linux"]["open"], {"known issue #211": 1, "opened": 1, "rejected": 1})
        self.assertEqual(report["duplicates_skipped"], 1)
        zipped = report["experiments"][1]
        self.assertEqual(zipped["native"], {"linux": {"status": "rejected", "message": "Choose a Tracker or MoTrack project file.",
                                                      "as_trz": {"status": "opened", "experiments": 1, "movies": 0,
                                                                 "unsupported_tracks": {}}}})
        self.assertEqual(([project["member"] for project in zipped["projects"]], zipped["tabs"]), (["x.trk", "y.trk"], 2))
        self.assertEqual(report["experiments"][0]["archive_file_types"], {"mp4": 1, "trk": 1})
        self.assertEqual(zipped["archive_file_types"], {"(appledouble)": 1, "trk": 2})
        self.assertFalse(any(path.suffix == ".trz" for path in (self.root / "library" / "reports").iterdir()))
        with self.assertRaises(ValueError):
            library.census({"refreshed": "2026-11-21", "experiments": []}, self.root, "probe", jobs=0)

    def test_census_aggregates_feature_use_and_renders_a_bounded_table(self):
        experiments = [
            {"id": "a", "sha256": "a" * 64, "bytes": 10, "archive_file_types": {"mp4": 1, "trk": 2}, "tabs": 2,
             "native": {"windows": {"status": "opened", "unsupported_tracks": {"CoordAxes": 2}}},
             "projects": [{"semantic_version": "6.1.1", "tracks": {"PointMass": 2, "CoordAxes": 1}, "video": "xuggle.XuggleVideo",
                           "video_type": "mp4", "filters": ["GhostFilter"], "models": [], "data_functions": 2,
                           "reference": {"status": "match"}},
                          {"version": "4.92", "tracks": {"PointMass": 1}, "video": None, "video_type": None, "filters": [],
                           "models": ["DynamicParticle"], "data_functions": 0, "reference": {"status": "differs"}}]},
            {"id": "b", "sha256": "b" * 64, "bytes": 5, "archive_file_types": {"zip": 1}, "tabs": 1,
             "native": {"windows": {"status": "rejected", "message": "Choose a Tracker or MoTrack project file.",
                                    "as_trz": {"status": "opened"}}},
             "projects": [{"xml_error": "bad"}]}]
        total = library.aggregate(experiments)
        self.assertEqual(total["track_classes"], {"PointMass": {"projects": 2, "instances": 3}, "CoordAxes": {"projects": 1, "instances": 1}})
        self.assertEqual(total["tracker_versions"], {"(not recorded)": 1, "4.92": 1, "6.1.1": 1})
        self.assertEqual(total["data_functions"], {"projects": 1, "functions": 2})
        self.assertEqual(total["native"], {"windows": {
            "open": {"opened": 1, "rejected": 1}, "rejections": {"Choose a Tracker or MoTrack project file.": 1},
            "unsupported_tracks": {"CoordAxes": 1}, "zip_as_trz": {"opened": 1}}})
        self.assertEqual(total["reference"], {"differs": 1, "match": 1})
        self.assertEqual(total["archive_file_types"], {"mp4": 1, "trk": 1, "zip": 1})
        markdown = library.census_markdown({"manifest_refreshed": "2026-11-21", "aggregate": total})
        self.assertTrue(markdown.startswith(library.CENSUS_START) and markdown.endswith(library.CENSUS_END))
        self.assertIn("| Track classes (projects using) | PointMass (2), CoordAxes (1) |", markdown)
        self.assertIn("| ZIP experiments renamed .trz, windows | opened (1) |", markdown)
        self.assertEqual(list(library.diff({"a": [{"id": "x", "v": 1}]}, {"a": [{"id": "x", "v": 2}]})),
                         ["census.a[x].v: expected 1, got 2"])

        # Another platform's committed outcomes survive a regeneration only for byte-identical experiments.
        report = {"experiments": copy.deepcopy(experiments)}
        committed = {"experiments": copy.deepcopy(experiments)}
        committed["experiments"][0]["native"] = {"linux": {"status": "opened"}, "windows": {"status": "stale"}}
        committed["experiments"][1]["native"] = {"linux": {"status": "opened"}}
        committed["experiments"][1]["sha256"] = "c" * 64
        merged = library.merge_platforms(report, committed, "windows")
        self.assertEqual(merged["experiments"][0]["native"], {"linux": {"status": "opened"}, **experiments[0]["native"]})
        self.assertEqual(merged["experiments"][1]["native"], experiments[1]["native"])
        self.assertEqual(merged["aggregate"]["native"]["linux"]["open"], {"opened": 1})
        stripped = library.without_platform(merged, "windows")
        self.assertEqual([record["native"] for record in stripped["experiments"]], [{"linux": {"status": "opened"}}, {}])
        self.assertEqual(list(stripped["aggregate"]["native"]), ["linux"])

    def census_check(self, report, committed, *, java=None, platform="linux"):
        census_file, readme = self.root / "census.json", self.root / "README.md"
        readme.write_text("intro\n" + library.census_markdown(committed) + "\noutro\n", encoding="utf-8")
        library.write_json(census_file, committed)
        args = type("Args", (), {"check": True, "probe": "probe", "java": java, "jobs": 1})()
        with patch.object(library, "LIBRARY_CENSUS", census_file), patch.object(library, "README", readme), \
                patch.object(library, "platform_key", return_value=platform), \
                patch.object(library, "census", return_value=copy.deepcopy(report)), \
                patch("builtins.print"):
            library.census_main({"refreshed": "2026-11-21", "experiments": []}, self.root, args)

    def test_census_check_compares_the_committed_report_and_publishes_a_new_platform_baseline(self):
        def record(identifier, status, **extra):
            return {"id": identifier, "sha256": identifier * 64, "bytes": 1, "archive_file_types": {"trk": 1}, "tabs": 1,
                    "projects": [{"root": "TrackerPanel", "version": "4.92"}],
                    "native": {"linux": {"status": status, **extra}}}
        committed = library.census_report({"refreshed": "2026-11-21", "experiments": []},
                                          [record("a", "opened"), record("b", "crashed", message="memory error", known_issue=211)], None)
        self.census_check(committed, committed)
        # A known issue's raw outcome may flap (undefined behaviour) without failing the check.
        flapped = copy.deepcopy(committed)
        flapped["experiments"][1]["native"]["linux"] = {"status": "opened", "experiments": 1, "known_issue": 211}
        flapped["aggregate"] = library.aggregate(flapped["experiments"])
        self.census_check(flapped, committed)
        drifted = copy.deepcopy(committed)
        drifted["experiments"][0]["native"]["linux"] = {"status": "rejected", "message": "Invalid Tracker XML."}
        drifted["aggregate"] = library.aggregate(drifted["experiments"])
        with self.assertRaisesRegex(ValueError, r"census.experiments\[a\].native.linux"):
            self.census_check(drifted, committed)
        # No committed baseline for this platform: everything else is still compared.
        windows = copy.deepcopy(committed)
        for item in windows["experiments"]:
            item["native"] = {"windows": item["native"]["linux"]}
        windows["aggregate"] = library.aggregate(windows["experiments"])
        merged = copy.deepcopy(windows)
        for item, linux in zip(merged["experiments"], committed["experiments"]):
            item["native"]["linux"] = linux["native"]["linux"]
        merged["aggregate"] = library.aggregate(merged["experiments"])
        self.census_check(merged, windows)
        changed = copy.deepcopy(merged)
        changed["experiments"][0]["tabs"] = 2
        changed["aggregate"] = library.aggregate(changed["experiments"])
        with self.assertRaisesRegex(ValueError, "tabs"):
            self.census_check(changed, windows)
        # A census recorded with the Java reference can only be checked with it.
        with_java = {**committed, "java_reference": "Tracker 6.3.5 TrackerReference (xml_and_coordinate_api)"}
        with self.assertRaisesRegex(ValueError, "pass --java"):
            self.census_check(with_java, with_java)
        self.census_check(with_java, with_java, java="java")

    def test_memory_errors_are_classified_without_addresses(self):
        crt = ("HEAP CORRUPTION DETECTED: after Normal block (#71854) at 0x000001DB0B867070.\n"
               "CRT detected that the application wrote to memory after end of heap buffer.")
        self.assertEqual(library._diagnostic(crt, self.root, 3),
                         "memory error: HEAP CORRUPTION DETECTED: after Normal block (<n>) at <n>.")
        self.assertEqual(library._diagnostic("==4242==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x6020", self.root, 1),
                         "memory error: <n>ERROR: AddressSanitizer: heap-buffer-overflow on address <n>")
        self.assertEqual(library._diagnostic("", self.root, None), "exit None")


if __name__ == "__main__":
    unittest.main()
