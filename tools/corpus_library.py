"""Discover, fetch and census real-world Tracker experiments from the Library Browser collections.

Only metadata, hashes and the census summary are committed (tests/corpus/library-manifest.json and
library-census.json). Experiment files stay in the ignored cache under ``<cache>/library/``. Downloads are
untrusted data: every request goes to an allowlisted HTTPS host, sizes are capped per file and in total,
archives are read member by member with bounded sizes, nothing is extracted by its archive path and
nothing downloaded is executed.
"""

import base64
import collections
import concurrent.futures
import copy
import datetime
import hashlib
import html
import html.parser
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

import corpus

LIBRARY_MANIFEST = corpus.CORPUS / "library-manifest.json"
LIBRARY_CENSUS = corpus.CORPUS / "library-census.json"
README = corpus.CORPUS / "README.md"
USER_AGENT = "MoTrack-corpus/1.0"
MIN_REQUEST_DELAY_SECONDS = 1.0
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 1536 * 1024 * 1024
MAX_METADATA_BYTES = 16 * 1024 * 1024
MAX_ARCHIVE_MEMBERS = 4096
MAX_COLLECTION_NESTING = 32
EXPERIMENT_SUFFIXES = (".trz", ".trk", ".zip")
RETRY_STATUS = {429, 500, 502, 503, 504}
# Experiment files may only come from these hosts. Collection metadata may also be read from the
# others; anything a collection references elsewhere is recorded as skipped for a human decision.
DOWNLOAD_HOSTS = frozenset({"www.compadre.org", "opensourcephysics.github.io", "iwant2study.org"})
METADATA_HOSTS = DOWNLOAD_HOSTS | {"api.github.com", "rbtsps.ieee-saopen.org", "home.phys.ntnu.no"}
COMPADRE_NS = "{https://www.compadre.org/osp/services/REST/}"
OSP_PERMALINK = "https://github.com/OpenSourcePhysics/osp/blob/d49c6ecf41f48dc53fa85af1806241b46f7a90b4/src/org/opensourcephysics/tools/"
TRACKER_PERMALINK = "https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/"
SOURCES = (
    {"id": "compadre-tracker", "kind": "compadre-rest", "name": "ComPADRE OSP Tracker Collection",
     "url": "https://www.compadre.org/osp/services/REST/osp_tracker.cfm?OSPType=Tracker",
     "loaded_by": TRACKER_PERMALINK + "TFrame.java#L2267-L2268 (LibraryComPADRE.TRACKER_SERVER_TREE + PRIMARY_ONLY; "
                  "the browser queries this record service per subject, this unfiltered query is their union)",
     "selection": OSP_PERMALINK + "LibraryComPADRE.java#L449-L476"},
    {"id": "osp-tracker-library", "kind": "osp-library", "name": "Tracker Home Library",
     "url": "https://opensourcephysics.github.io/resources/CAB/tracker_library.xml",
     "loaded_by": TRACKER_PERMALINK + "TFrame.java#L2265 (LibraryBrowser.TRACKER_LIBRARY)",
     "rights_api": "https://api.github.com/repos/OpenSourcePhysics/resources/license"},
    {"id": "osp-shared-library", "kind": "osp-library", "name": "Shared Library",
     "url": "https://opensourcephysics.github.io/resources/CAB/shared_library.xml",
     "loaded_by": TRACKER_PERMALINK + "TFrame.java#L2266 (LibraryBrowser.SHARED_LIBRARY)",
     "rights_api": "https://api.github.com/repos/OpenSourcePhysics/resources/license"},
)
SOURCE_IDS = tuple(source["id"] for source in SOURCES)
TRACKER = "org.opensourcephysics.cabrillo.tracker."


def check_url(url, hosts):
    """Accept only plain HTTPS URLs on an allowlisted host (no credentials, default port)."""
    parts = urllib.parse.urlsplit(url)
    if (parts.scheme != "https" or parts.hostname is None or parts.hostname.lower() not in hosts
            or parts.username is not None or parts.password is not None or parts.port not in {None, 443}):
        raise ValueError(f"URL is not an allowlisted HTTPS host: {url[:200]}")
    return url


def quote_url(url):
    """Percent-encode spaces and other unsafe characters that collection listings leave raw."""
    parts = urllib.parse.urlsplit(url)
    return urllib.parse.urlunsplit(parts._replace(path=urllib.parse.quote(parts.path, safe="/%:@!$&'()*+,;=-._~"),
                                                  query=urllib.parse.quote(parts.query, safe="=&%:@!$'()*+,;/?-._~")))


class _RedirectGuard(urllib.request.HTTPRedirectHandler):
    def __init__(self, hosts):
        self.hosts = hosts

    def redirect_request(self, request, stream, code, message, headers, new_url):
        check_url(new_url, self.hosts)
        return super().redirect_request(request, stream, code, message, headers, new_url)


class PoliteClient:
    """Serial HTTP client: one request at a time, a fixed delay between requests and bounded retries."""

    def __init__(self, hosts, *, delay=MIN_REQUEST_DELAY_SECONDS, retries=3, backoff=2.0, timeout=60,
                 opener=None, sleep=time.sleep, clock=time.monotonic):
        if delay < MIN_REQUEST_DELAY_SECONDS or retries < 0 or backoff < 1:
            raise ValueError("Library requests need at least a one-second delay and bounded retries")
        self.hosts = frozenset(hosts)
        self.delay, self.retries, self.backoff, self.timeout = delay, retries, backoff, timeout
        self.open_url = opener or urllib.request.build_opener(_RedirectGuard(self.hosts)).open
        self.sleep, self.clock = sleep, clock
        self.last = None
        self.requests = 0

    def request(self, url, consume, hosts=None):
        hosts = self.hosts if hosts is None else self.hosts & hosts
        check_url(url, hosts)
        for attempt in range(self.retries + 1):
            if self.last is not None and (remaining := self.last + self.delay - self.clock()) > 0:
                self.sleep(remaining)
            retry_after = None
            try:
                self.requests += 1
                request = urllib.request.Request(quote_url(url), headers={"User-Agent": USER_AGENT})
                with self.open_url(request, timeout=self.timeout) as response:
                    check_url(response.geturl(), hosts)
                    return consume(response)
            except urllib.error.HTTPError as error:
                if error.code not in RETRY_STATUS or attempt == self.retries:
                    raise ValueError(f"HTTP {error.code} for {url[:200]}") from error
                retry_after = error.headers.get("Retry-After") if error.headers else None
            except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException) as error:
                if attempt == self.retries:
                    raise ValueError(f"Request failed for {url[:200]}: {str(error)[:200]}") from error
            finally:
                self.last = self.clock()
            wait = self.delay * self.backoff ** (attempt + 1)
            if retry_after and retry_after.isdigit():
                wait = max(wait, int(retry_after))
            self.sleep(min(wait, 120))
        raise AssertionError("unreachable")

    def get(self, url, max_bytes=MAX_METADATA_BYTES):
        def consume(response):
            length = response.headers.get("Content-Length") if getattr(response, "headers", None) else None
            if length and length.isdigit() and int(length) > max_bytes:
                raise ValueError(f"Response exceeds {max_bytes} bytes: {url[:200]}")
            data = response.read(max_bytes + 1)
            if len(data) > max_bytes:
                raise ValueError(f"Response exceeds {max_bytes} bytes: {url[:200]}")
            return data
        return self.request(url, consume)

    def download(self, url, destination, *, max_bytes=MAX_FILE_BYTES, expected=None):
        """Stream to a private temporary file, hash and only then publish ``destination``."""
        limit = expected["bytes"] if expected else max_bytes
        if limit > max_bytes:
            raise ValueError(f"Pinned size exceeds the per-file limit: {destination.name}")
        destination.parent.mkdir(parents=True, exist_ok=True)

        def consume(response):
            length = response.headers.get("Content-Length") if getattr(response, "headers", None) else None
            if length and length.isdigit() and int(length) > limit:
                raise ValueError(f"Download exceeds {limit} bytes: {url[:200]}")
            digest, size = hashlib.sha256(), 0
            with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=destination.name + ".",
                                             suffix=".part", delete=False) as output:
                pending = Path(output.name)
                try:
                    while block := response.read(min(65536, limit - size + 1)):
                        size += len(block)
                        if size > limit:
                            raise ValueError(f"Download exceeds {limit} bytes: {url[:200]}")
                        digest.update(block)
                        output.write(block)
                except BaseException:
                    output.close()
                    pending.unlink(missing_ok=True)
                    raise
            return pending, {"bytes": size, "sha256": digest.hexdigest()}

        pending, result = self.request(url, consume, DOWNLOAD_HOSTS)
        try:
            if result["bytes"] == 0:
                raise ValueError(f"Empty download: {url[:200]}")
            if expected:
                corpus.verify_file(pending, expected)
            pending.replace(destination)
        finally:
            pending.unlink(missing_ok=True)
        return result


def parse_xml(data):
    """Parse metadata or project XML; document type declarations (entity expansion) are refused."""
    if len(data) > max(MAX_METADATA_BYTES, corpus.MAX_XML_BYTES):
        raise ValueError("XML exceeds the size limit")
    if re.search(rb"<!(DOCTYPE|ENTITY)", data[:65536], re.IGNORECASE) or b"<!ENTITY" in data:
        raise ValueError("XML document type declarations are not accepted")
    return ET.fromstring(data)


def _text(node, tag, namespace=COMPADRE_NS):
    child = node.find(namespace + tag)
    return child.text.strip() if child is not None and child.text else ""


def _suffix(name):
    return PurePosixPath(name.lower()).suffix


def library_browser_attachment(record, types):
    """OSP's LibraryComPADRE.getAttachment: first downloadable attachment of the given types,
    replaced by one whose file identifier matches the record's."""
    record_file = _text(record, "file-identifier")
    chosen = None
    for document in record.findall(COMPADRE_NS + "attached-document"):
        if _text(document, "file-type") in types and document.find(COMPADRE_NS + "download-url") is not None:
            if chosen is None or _text(document, "file-identifier") == record_file:
                chosen = document
    return chosen


def parse_compadre(data):
    """One candidate per distinct downloadable Tracker document (ComPADRE item ID, document ID)."""
    root = parse_xml(data)
    records = root.findall(f".//{COMPADRE_NS}record")
    declared = (root.findtext(f".//{COMPADRE_NS}recordCount") or "").strip()
    if declared and (not declared.isdigit() or int(declared) != len(records)):
        raise ValueError(f"ComPADRE declared {declared} records but returned {len(records)}")
    documents = {}
    for record in records:
        item = _text(record, "compadre-identifier")
        if not item.isdigit():
            raise ValueError("ComPADRE record without a numeric identifier")
        chosen = library_browser_attachment(record, {"Main", "Primary"})
        if chosen is None:
            chosen = library_browser_attachment(record, {"Supplemental"})
        record_title = _text(record, "title")
        authors = [contributor.text.strip() for contributor in record.iter(COMPADRE_NS + "contributor")
                   if contributor.get("role") == "Author" and contributor.text and contributor.text.strip()]
        subjects = [subject.text.strip() for subject in record.findall(COMPADRE_NS + "osp-subject")
                    if subject.text and subject.text.strip()]
        for document in record.findall(COMPADRE_NS + "attached-document"):
            name_node = document.find(COMPADRE_NS + "file-name")
            name = (name_node.text or "").strip() if name_node is not None else ""
            url = _text(document, "download-url")
            if not url or _suffix(name) not in EXPERIMENT_SUFFIXES:
                continue
            number = _text(document, "document-identifier")
            if not number.isdigit():
                raise ValueError(f"ComPADRE item {item} has a document without a numeric identifier")
            experiment_id = f"compadre-{item}-{number}"
            entry = documents.setdefault(experiment_id, {
                "id": experiment_id, "source": "compadre-tracker", "file": experiment_id + _suffix(name),
                "original_name": name, "url": check_url(url, DOWNLOAD_HOSTS),
                "listed_bytes": int(name_node.get("file-size")) if (name_node.get("file-size") or "").isdigit() else None,
                "title": _text(document, "title") or record_title, "authors": [], "subjects": [],
                "information_url": check_url(_text(record, "information-url"), METADATA_HOSTS),
                "compadre": {"item": int(item), "document": int(number), "file": int(_text(document, "file-identifier") or 0),
                             "attachment_type": _text(document, "file-type"), "records": [], "library_browser_records": []}})
            if entry["url"] != url:
                raise ValueError(f"{experiment_id}: conflicting download URLs")
            for author in authors:
                if author not in entry["authors"]:
                    entry["authors"].append(author)
            for subject in subjects:
                if subject not in entry["subjects"]:
                    entry["subjects"].append(subject)
            if record_title not in entry["compadre"]["records"]:
                entry["compadre"]["records"].append(record_title)
            if chosen is document and record_title not in entry["compadre"]["library_browser_records"]:
                entry["compadre"]["library_browser_records"].append(record_title)
    return len(records), sorted(documents.values(), key=lambda entry: (entry["compadre"]["item"], entry["compadre"]["document"]))


class _RightsParser(html.parser.HTMLParser):
    """Collect <dt>/<dd> pairs from the ComPADRE item detail list (text and links, tags dropped)."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.pairs, self.current, self.capture, self.links = [], None, None, []

    def handle_starttag(self, tag, attributes):
        if tag in {"dt", "dd"}:
            self.capture, self.current = tag, []
            if tag == "dd":
                self.links = []
        elif tag == "a" and self.capture == "dd":
            href = dict(attributes).get("href")
            if href:
                self.links.append(href)

    def handle_endtag(self, tag):
        if tag == self.capture:
            text = " ".join("".join(self.current).split())
            if tag == "dt":
                self.pairs.append([text, None, []])
            elif self.pairs and self.pairs[-1][1] is None:
                self.pairs[-1][1], self.pairs[-1][2] = text, list(self.links)
            self.capture = None

    def handle_data(self, data):
        if self.capture:
            self.current.append(data)


def parse_compadre_rights(page):
    """The item's rights statements exactly as worded on its detail page; no interpretation."""
    parser = _RightsParser()
    parser.feed(page.decode("utf-8", errors="replace"))
    rights = {}
    for label, value, links in parser.pairs:
        key = label.rstrip(":").strip()
        if value is not None and re.search(r"right|licen|copyright|restrict", key, re.IGNORECASE) and key not in rights:
            rights[key] = {"statement": value, **({"links": links} if links else {})}
    return rights


def _collection_property(node, name):
    return next((item for item in node.findall("property") if item.get("name") == name), None)


def _collection_text(node, name):
    item = _collection_property(node, name)
    return item.text.strip() if item is not None and item.text else ""


def resolve_osp_path(target, base):
    """OSP XML.getResolvedPath for library targets: absolute URLs stay, others join the inherited base."""
    if re.match(r"^[a-z][a-z0-9+.-]*://", target, re.IGNORECASE):
        return target
    return urllib.parse.urljoin(base.rstrip("/") + "/", target)


def parse_osp_library(data):
    root = parse_xml(data)
    if root.get("class") != "org.opensourcephysics.tools.Library":
        raise ValueError("Not an OSP library document")
    paths = _collection_property(root, "collection_paths")
    names = _collection_property(root, "collection_names")
    urls = [item.text.strip() for item in (paths.findall("property") if paths is not None else []) if item.text]
    labels = [item.text.strip() for item in (names.findall("property") if names is not None else []) if item.text]
    return _collection_text(root, "name"), [(url, labels[index] if index < len(labels) else url) for index, url in enumerate(urls)]


def parse_osp_collection(data, url, trail):
    """Tracker resources in an OSP LibraryCollection, with OSP's inherited base-path resolution."""
    root = parse_xml(data)
    if root.get("class") != "org.opensourcephysics.tools.LibraryCollection":
        raise ValueError("Not an OSP library collection")
    found = []

    def walk(node, base, path, depth):
        if depth > MAX_COLLECTION_NESTING:
            raise ValueError("Library collection nesting is too deep")
        base = _collection_text(node, "base_path") or base
        path = path + [_collection_text(node, "name")] if _collection_text(node, "name") else path
        target = _collection_text(node, "target")
        if target and node.get("class") == "org.opensourcephysics.tools.LibraryResource":
            kind = _collection_text(node, "type")
            if _suffix(urllib.parse.urlsplit(target).path) in EXPERIMENT_SUFFIXES and kind in {"Tracker", ""}:
                html_path = _collection_text(node, "html_path")
                found.append({"url": resolve_osp_path(target, base), "name": " / ".join(path), "type": kind,
                              "html": resolve_osp_path(html_path, base) if html_path else None})
        resources = _collection_property(node, "resources")
        for item in resources.findall("property") if resources is not None else []:
            child = item.find("object")
            if child is not None:
                walk(child, base, path, depth + 1)

    walk(root, url.rsplit("/", 1)[0] + "/", list(trail), 0)
    return found


def _slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def osp_experiment_id(url):
    """Host prefix, the trailing path segments that fit in 56 characters, and a URL digest for uniqueness."""
    parts = urllib.parse.urlsplit(url)
    prefix = {"opensourcephysics.github.io": "osp", "iwant2study.org": "iwant2study"}.get(parts.hostname, _slug(parts.hostname))
    segments = [slug for slug in (_slug(part) for part in PurePosixPath(urllib.parse.unquote(parts.path)).with_suffix("").parts[1:])
                if slug]
    stem = segments.pop()[-56:].strip("-") if segments else "experiment"
    while segments and len(segments[-1]) + 1 + len(stem) <= 56:
        stem = segments.pop() + "-" + stem
    return f"{prefix}-{stem}-{hashlib.sha256(url.encode('utf-8')).hexdigest()[:8]}"


def discover_compadre(client, source, log):
    count, candidates = parse_compadre(client.get(source["url"]))
    log(f"{source['id']}: {count} records, {len(candidates)} downloadable Tracker documents")
    rights_by_page = {}
    for candidate in candidates:
        page = candidate["information_url"]
        if page not in rights_by_page:
            rights_by_page[page] = parse_compadre_rights(client.get(page, 4 * 1024 * 1024))
        candidate["rights"] = rights_by_page[page] or None
    return {"records": count}, candidates


def discover_osp(client, source, log):
    name, collections = parse_osp_library(client.get(source["url"]))
    rights = None
    if source.get("rights_api"):
        license_info = json.loads(client.get(source["rights_api"], 1024 * 1024))
        text = (license_info.get("content") or "")
        if license_info.get("encoding") == "base64":
            text = base64.b64decode(text).decode("utf-8", errors="replace")
        heading = [line.strip() for line in text.splitlines() if line.strip()][:2]
        rights = {"Repository license": {"statement": " ".join(heading), "links": [license_info.get("html_url", "")]}}
    found, skipped = {}, []
    for url, label in collections:
        try:
            check_url(url, METADATA_HOSTS)
            resources = parse_osp_collection(client.get(url), url, [name, label])
        except ValueError as error:
            skipped.append({"url": url, "name": label, "reason": str(error)[:240]})
            log(f"{source['id']}: skipped {label}: {str(error)[:120]}")
            continue
        for resource in resources:
            try:
                check_url(resource["url"], DOWNLOAD_HOSTS)
            except ValueError as error:
                skipped.append({"url": resource["url"], "name": resource["name"], "reason": str(error)[:240]})
                continue
            experiment_id = osp_experiment_id(resource["url"])
            entry = found.setdefault(experiment_id, {
                "id": experiment_id, "source": source["id"], "file": experiment_id + _suffix(urllib.parse.urlsplit(resource["url"]).path),
                "original_name": urllib.parse.unquote(PurePosixPath(urllib.parse.urlsplit(resource["url"]).path).name),
                "url": resource["url"], "listed_bytes": None, "title": resource["name"].rsplit(" / ", 1)[-1],
                "authors": [], "subjects": [], "information_url": resource["html"],
                "rights": rights if urllib.parse.urlsplit(resource["url"]).hostname == "opensourcephysics.github.io" else None,
                "osp": {"collection": url, "paths": []}})
            if resource["name"] not in entry["osp"]["paths"]:
                entry["osp"]["paths"].append(resource["name"])
        log(f"{source['id']}: {label}: {len(resources)} Tracker resources")
    return {"collections": len(collections), "skipped": skipped}, sorted(found.values(), key=lambda entry: entry["id"])


def archive_projects(path):
    """Names of Tracker project members in a ZIP/TRZ, bounded; never extracts."""
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) > MAX_ARCHIVE_MEMBERS:
            raise ValueError("Archive has too many members")
        return [entry.filename for entry in entries if not entry.is_dir() and _suffix(entry.filename) == ".trk"]


def library_files(cache):
    return cache / "library" / "files"


def discover(cache, *, sources=SOURCE_IDS, client=None, today=None, previous=None, redownload=False, log=print):
    """Query every selected collection root, pin each experiment file and return a new manifest."""
    client = client or PoliteClient(METADATA_HOSTS)
    today = today or datetime.date.today().isoformat()
    previous = previous or {"experiments": [], "sources": []}
    pinned = {entry["id"]: entry for entry in previous.get("experiments", [])}
    pinned_urls = {entry["url"]: entry for entry in previous.get("experiments", [])}
    manifest = {"schema_version": 1, "refreshed": today, "user_agent": USER_AGENT,
                "limits": {"max_file_bytes": MAX_FILE_BYTES, "max_total_bytes": MAX_TOTAL_BYTES,
                           "min_request_delay_seconds": MIN_REQUEST_DELAY_SECONDS},
                "redistribution": "Experiment files are fetched unchanged into the ignored cache; none are committed. "
                                  "Rights statements are copied verbatim from their source without interpretation.",
                "sources": [], "experiments": [], "excluded": [], "withdrawn": []}
    candidates = []
    for source in SOURCES:
        if source["id"] not in sources:
            kept = next((item for item in previous.get("sources", []) if item["id"] == source["id"]), None)
            if kept:
                manifest["sources"].append(kept)
                candidates.extend(entry for entry in previous.get("experiments", []) if entry["source"] == source["id"])
                manifest["excluded"].extend(item for item in previous.get("excluded", []) if item.get("source") == source["id"])
            continue
        summary, found = (discover_compadre if source["kind"] == "compadre-rest" else discover_osp)(client, source, log)
        manifest["sources"].append({**{key: value for key, value in source.items() if key != "rights_api"},
                                    **summary, "queried": today})
        candidates.extend(found)
    seen_ids, by_digest, total = set(), {}, 0
    directory = library_files(cache)
    for candidate in candidates:
        if candidate["id"] in seen_ids:
            continue
        seen_ids.add(candidate["id"])
        old = pinned.get(candidate["id"])
        if old is None or old["url"] != candidate["url"]:
            old = pinned_urls.get(candidate["url"], old)
        path = directory / candidate["file"]
        if old and old["id"] != candidate["id"] and (directory / old["file"]).is_file() and not path.exists():
            # Same URL under a revised identifier: keep the verified bytes (checked below) under the new name.
            (directory / old["file"]).replace(path)
        # A changed listed size means the upstream file changed; otherwise keep the verified pin.
        reuse = (not redownload and old and old["url"] == candidate["url"] and "sha256" in old
                 and candidate.get("listed_bytes") == old.get("listed_bytes"))
        try:
            if reuse:
                if not path.exists():
                    client.download(candidate["url"], path, expected=old)
                corpus.verify_file(path, old)
                pin = {"bytes": old["bytes"], "sha256": old["sha256"]}
            else:
                path.unlink(missing_ok=True)
                pin = client.download(candidate["url"], path)
            projects = archive_projects(path) if candidate["file"].endswith((".trz", ".zip")) else [candidate["original_name"]]
        except (ValueError, zipfile.BadZipFile, OSError) as error:
            manifest["excluded"].append({"id": candidate["id"], "source": candidate["source"], "url": candidate["url"], "reason": str(error)[:240]})
            log(f"excluded {candidate['id']}: {str(error)[:120]}")
            continue
        if not projects:
            manifest["excluded"].append({"id": candidate["id"], "source": candidate["source"], "url": candidate["url"],
                                         "reason": "Archive contains no Tracker project (.trk) member"})
            path.unlink(missing_ok=True)
            continue
        if total + pin["bytes"] > MAX_TOTAL_BYTES:
            manifest["excluded"].append({"id": candidate["id"], "source": candidate["source"], "url": candidate["url"], "reason": "Total size limit reached"})
            path.unlink(missing_ok=True)
            continue
        total += pin["bytes"]
        entry = {**{key: value for key, value in candidate.items() if key != "duplicate_of"}, **pin, "projects": len(projects),
                 "discovered": old["discovered"] if old and old.get("sha256") == pin["sha256"] else today}
        if pin["sha256"] in by_digest:
            entry["duplicate_of"] = by_digest[pin["sha256"]]
        else:
            by_digest[pin["sha256"]] = entry["id"]
        manifest["experiments"].append(entry)
        log(f"pinned {entry['id']} ({entry['bytes']} bytes, {len(projects)} projects)")
    listed = {entry["url"] for entry in manifest["experiments"]} | {item["url"] for item in manifest["excluded"]}
    manifest["withdrawn"] = [item for item in previous.get("withdrawn", []) if item["url"] not in listed]
    for old in previous.get("experiments", []):
        if old["url"] not in listed:
            manifest["withdrawn"].append({"id": old["id"], "url": old["url"], "sha256": old["sha256"], "withdrawn": today})
    validate_library_manifest(manifest)
    return manifest


def validate_library_manifest(manifest):
    if manifest.get("schema_version") != 1:
        raise ValueError("Unsupported library manifest version")
    sources = {source["id"] for source in manifest.get("sources", [])}
    if not sources <= set(SOURCE_IDS):
        raise ValueError("Unknown library source")
    seen, digests, total = set(), {}, 0
    for entry in manifest.get("experiments", []):
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str):
            raise ValueError("Invalid library experiment")
        identifier = entry["id"]
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,95}", identifier) or identifier in seen:
            raise ValueError(f"Invalid or duplicate library experiment ID: {identifier[:100]}")
        seen.add(identifier)
        suffix = _suffix(entry.get("file", ""))
        if entry.get("file") != identifier + suffix or suffix not in EXPERIMENT_SUFFIXES:
            raise ValueError(f"{identifier}: cache file must be the ID plus a Tracker suffix")
        check_url(entry.get("url", ""), DOWNLOAD_HOSTS)
        if entry.get("source") not in sources:
            raise ValueError(f"{identifier}: unknown source")
        if (not isinstance(entry.get("sha256"), str) or not re.fullmatch(r"[a-f0-9]{64}", entry["sha256"])
                or type(entry.get("bytes")) is not int or not 0 < entry["bytes"] <= MAX_FILE_BYTES
                or type(entry.get("projects")) is not int or entry["projects"] < 1):
            raise ValueError(f"{identifier}: invalid pinned size, digest or project count")
        duplicate = entry.get("duplicate_of")
        if duplicate is not None and digests.get(entry["sha256"]) != duplicate:
            raise ValueError(f"{identifier}: duplicate_of must name the first experiment with the same digest")
        if duplicate is None and entry["sha256"] in digests:
            raise ValueError(f"{identifier}: identical content must name its first experiment")
        digests.setdefault(entry["sha256"], identifier)
        if not isinstance(entry.get("discovered"), str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", entry["discovered"]):
            raise ValueError(f"{identifier}: missing discovery date")
        total += entry["bytes"]
    if total > MAX_TOTAL_BYTES:
        raise ValueError("Library corpus exceeds its total size limit")
    return manifest


def load_library_manifest(path=LIBRARY_MANIFEST):
    return validate_library_manifest(json.loads(Path(path).read_text(encoding="utf-8")))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def fetch(manifest, cache, *, client=None, sources=None, log=print):
    """Download missing pinned files and verify every present one; existing files are never replaced."""
    client = client or PoliteClient(DOWNLOAD_HOSTS)
    directory = library_files(cache)
    verified = downloaded = 0
    failures = []
    for entry in manifest["experiments"]:
        if sources and entry["source"] not in sources:
            continue
        path = directory / entry["file"]
        try:
            if path.exists():
                corpus.verify_file(path, entry)
            else:
                client.download(entry["url"], path, expected=entry)
                downloaded += 1
            verified += 1
        except (ValueError, OSError) as error:
            failures.append(f"{entry['id']}: {str(error)[:200]}")
    log(f"Verified {verified} library experiments ({downloaded} downloaded)")
    if failures:
        raise ValueError("Library fetch failed:\n" + "\n".join(failures))
    return verified, downloaded


def read_projects(path, limit=corpus.MAX_XML_BYTES):
    """Yield (member, bytes) for each Tracker project in a .trk/.trz/.zip; bounded, never extracted."""
    if _suffix(path.name) == ".trk":
        if path.stat().st_size > limit:
            raise ValueError("Project exceeds the XML size limit")
        yield None, path.read_bytes()
        return
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) > MAX_ARCHIVE_MEMBERS:
            raise ValueError("Archive has too many members")
        names = [entry.filename for entry in entries]
        for entry in entries:
            if entry.is_dir() or _suffix(entry.filename) != ".trk":
                continue
            if names.count(entry.filename) != 1:
                raise ValueError(f"Ambiguous archive member: {entry.filename[:120]}")
            if entry.file_size > limit:
                raise ValueError(f"Archive member exceeds the XML size limit: {entry.filename[:120]}")
            with archive.open(entry) as stream:
                data = stream.read(limit + 1)
            if len(data) > limit:
                raise ValueError(f"Expanded archive member exceeds the XML size limit: {entry.filename[:120]}")
            yield entry.filename, data


def library_projects(manifest, cache, *, include_duplicates=False):
    """Materialize every library project as a private .trk under ``<cache>/library/projects/<id>/``.

    This is the entry point for corpus oracles (for example the A1 O1/O2 XMLControl checks): each item
    names its experiment, original archive member, written path, size and SHA-256 of the exact bytes.
    """
    directory = cache / "library" / "projects"
    items = []
    for entry in manifest["experiments"]:
        if entry.get("duplicate_of") and not include_duplicates:
            continue
        source = library_files(cache) / entry["file"]
        corpus.verify_file(source, entry)
        target = directory / entry["id"]
        if target.exists():
            shutil.rmtree(target)
        target.mkdir(parents=True)
        for index, (member, data) in enumerate(read_projects(source)):
            path = target / f"{index:03d}.trk"
            path.write_bytes(data)
            items.append({"experiment": entry["id"], "member": member, "path": str(path), "bytes": len(data),
                          "sha256": hashlib.sha256(data).hexdigest()})
    return items


MODEL_CLASSES = {"AnalyticParticle", "DynamicParticle", "DynamicParticlePolar", "DynamicSystem", "ParticleDataTrack"}
CENSUS_START = "<!-- library-census:start -->"
CENSUS_END = "<!-- library-census:end -->"


def _short(name):
    for prefix in (TRACKER, "org.opensourcephysics.media.core.", "org.opensourcephysics.media.", "org.opensourcephysics."):
        if name.startswith(prefix):
            return name[len(prefix):]
    return name


def _child_property(node, name):
    return next((item for item in node.findall("property") if item.get("name") == name), None)


def _objects(node):
    """Direct object values of an OSP array/collection property."""
    return [] if node is None else [item.find("object") for item in node.findall("property") if item.find("object") is not None]


def _value(node, name):
    item = _child_property(node, name)
    return item.text.strip() if item is not None and item.text and item.text.strip() else None


def analyze_project(data):
    """Tracker features one saved project uses, read structurally from its XML (no Tracker code)."""
    try:
        root = parse_xml(data)
    except (ET.ParseError, ValueError) as error:
        return {"xml_error": str(error)[:200]}
    report = {"root": _short(root.get("class", "")), "semantic_version": _value(root, "semantic_version"),
              "version": _value(root, "version")}
    if root.get("class") == TRACKER + "TFrame":
        tabs = _child_property(root, "tabs")
        report["tabset_tabs"] = 0 if tabs is None else len(tabs.findall("property"))
        return report
    tracks = collections.Counter()
    functions = 0
    for track in _objects(_child_property(root, "tracks")):
        tracks[_short(track.get("class", ""))] += 1
        functions += len(_objects(_child_property(track, "data_functions")))
    clip = _child_property(root, "videoclip")
    clip_object = clip.find("object") if clip is not None else None
    video_property = _child_property(clip_object, "video") if clip_object is not None else None
    video = video_property.find("object") if video_property is not None else None
    filters, video_type = [], None
    if video is not None:
        filters = sorted({_short(item.get("class", "")) for item in _objects(_child_property(video, "filters"))})
        path = _value(video, "path")
        paths = _child_property(video, "paths")
        if path is None and paths is not None:
            first = next((item.text.strip() for item in paths.findall("property") if item.text and item.text.strip()), None)
            path = first
        video_type = (_suffix(path.split("!")[-1].split("?")[0]).lstrip(".") or None) if path else None
    report.update({
        "tracks": dict(sorted(tracks.items())),
        "models": sorted(set(tracks) & MODEL_CLASSES),
        "data_functions": functions,
        "video": _short(video.get("class", "")) if video is not None else None,
        "video_type": video_type,
        "filters": filters,
        "reference_frame": _value(root, "referenceframe") is not None,
        "datatool_tabs": len(_objects(_child_property(root, "datatool_tabs"))),
        "custom_views": _child_property(root, "views") is not None,
    })
    return report


def archive_file_types(path):
    if _suffix(path.name) == ".trk":
        return {"trk": 1}
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) > MAX_ARCHIVE_MEMBERS:
            raise ValueError("Archive has too many members")
        types = collections.Counter(_suffix(entry.filename).lstrip(".") or "(none)" for entry in entries if not entry.is_dir())
    return dict(sorted(types.items()))


def _sanitize(message, cache):
    """Platform-neutral diagnostic: no machine-specific paths, addresses or allocation numbers; bounded."""
    text = " ".join(str(message).split())
    for root, label in ((str(cache), "<cache>"), (tempfile.gettempdir(), "<temp>")):
        text = text.replace(root, label).replace(root.replace("\\", "/"), label)
    text = re.sub(r"(?<![\w<])(?:[A-Za-z]:[\\/]|/(?:tmp|home|Users|var|private)/)[^\s'\"]*", "<path>", text)
    return re.sub(r"0x[0-9A-Fa-f]+|#\d+|==\d+==", "<n>", text)[:300]


MEMORY_ERROR = re.compile(r"CRT detected that|AddressSanitizer|HEAP CORRUPTION|free\(\): invalid|double free|corrupted")


def platform_key():
    return {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux" if sys.platform.startswith("linux") else sys.platform)


def _diagnostic(text, cache, code):
    lines = [line for line in text.splitlines() if line.strip()]
    memory = next((line for line in lines if MEMORY_ERROR.search(line)), None)
    if memory:
        return "memory error: " + _sanitize(memory, cache)
    return _sanitize(lines[-1] if lines else f"exit {code}", cache)


def run_tool(command, timeout):
    """Run a probe without a shell; return (exit code, combined output). Untrusted inputs are arguments only."""
    environment = {**os.environ, "QT_QPA_PLATFORM": "offscreen"}
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=timeout, env=environment)
    except subprocess.TimeoutExpired:
        return None, f"timed out after {timeout} s"
    return result.returncode, (result.stdout + result.stderr).strip()


def native_open(probe, path, output, cache):
    output.unlink(missing_ok=True)
    code, text = run_tool([str(probe), "--open", str(path), str(output)], 180)
    if code != 0 or not output.is_file():
        return {"status": "rejected", "message": _diagnostic(text, cache, code)}
    experiments = json.loads(output.read_text(encoding="utf-8"))["experiments"]
    unsupported = collections.Counter(_short(name) for item in experiments for name in item["unsupported_tracks"])
    return {"status": "opened", "experiments": len(experiments),
            "movies": sum(1 for item in experiments if item["movie"]),
            "unsupported_tracks": dict(sorted(unsupported.items()))}


def compile_reference(java, cache):
    """Compile the existing TrackerReference once instead of per file (source-launcher compiles each run)."""
    manifest = corpus.load_manifest()
    classes = cache / "library" / "classes"
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir(parents=True)
    javac = Path(java).with_name("javac" + Path(java).suffix)
    code, text = run_tool([str(javac), "-d", str(classes), "--class-path", corpus.java_classpath(manifest, cache),
                           str(corpus.ROOT / "tools/reference/TrackerReference.java")], 300)
    if code != 0:
        raise ValueError(f"Could not compile TrackerReference: {text[-2000:]}")
    return manifest, corpus.java_classpath(manifest, cache, classes=classes)


def reference_compare(java, classpath, probe, project, reports, cache, manifest):
    """Compare Java's XML/coordinate snapshot (corpus.py reference) with MoTrack's import of one project."""
    name = f"{project.parent.name}-{project.stem}"
    home = reports / "java-home" / name  # private per project, so concurrent JVMs never share preferences
    home.mkdir(parents=True, exist_ok=True)
    java_output, native_output = reports / (name + "-java.json"), reports / (name + "-native.json")
    for output in (java_output, native_output):
        output.unlink(missing_ok=True)
    java_code, java_text = run_tool(corpus.java_reference_command(java, home, classpath, "TrackerReference", project, java_output), 120)
    native_code, native_text = run_tool([str(probe), str(project), str(native_output)], 120)
    java_ok, native_ok = java_code == 0 and java_output.is_file(), native_code == 0 and native_output.is_file()
    if not java_ok and not native_ok:
        return {"status": "both_rejected", "native": _diagnostic(native_text, cache, native_code)}
    if not java_ok:
        lines = [line for line in java_text.splitlines() if line.strip() and not line.startswith("\tat ")]
        return {"status": "java_rejected", "java": _sanitize(lines[0] if lines else f"exit {java_code}", cache)}
    if not native_ok:
        return {"status": "native_rejected", "native": _diagnostic(native_text, cache, native_code)}
    expected = json.loads(java_output.read_text(encoding="utf-8"))["project"]
    actual = json.loads(native_output.read_text(encoding="utf-8"))
    try:
        corpus.compare(expected, actual, manifest["tolerance"], "project")
    except ValueError as error:
        return {"status": "differs", "detail": str(error)[:300]}
    return {"status": "match"}


def census(manifest, cache, probe, java=None, *, jobs=1, log=print):
    """Per-experiment Tracker feature use plus MoTrack's (and optionally Java's) handling of each file.

    ``jobs`` experiments are processed concurrently (local processes only; no network). Results keep
    manifest order, and every output file and Java home is private to its experiment.
    """
    if not 1 <= jobs <= 16:
        raise ValueError("Census jobs must be between 1 and 16")
    probe = Path(probe).resolve()
    reports = cache / "library" / "reports"
    if reports.exists():
        shutil.rmtree(reports)
    reports.mkdir(parents=True)
    projects = collections.defaultdict(list)
    for item in library_projects(manifest, cache):
        projects[item["experiment"]].append(item)
    reference = compile_reference(java, cache) if java else None

    def experiment(entry):
        path = library_files(cache) / entry["file"]
        record = {"id": entry["id"], "source": entry["source"], "sha256": entry["sha256"],
                  "file_type": _suffix(entry["file"]).lstrip("."), "bytes": entry["bytes"],
                  "archive_file_types": archive_file_types(path), "projects": []}
        native = native_open(probe, path, reports / (entry["id"] + "-open.json"), cache)
        if record["file_type"] == "zip":
            # Tracker opens ZIP experiments like TRZ (ResourceLoader.isJarZipTrz); MoTrack only accepts .trz.
            renamed = reports / (entry["id"] + ".trz")
            shutil.copyfile(path, renamed)
            native["as_trz"] = native_open(probe, renamed, reports / (entry["id"] + "-open-trz.json"), cache)
            renamed.unlink()
        # Media decoding and memory-error detection are platform dependent; keep each platform's outcome.
        record["native"] = {platform_key(): native}
        for item in projects[entry["id"]]:
            project = {"member": item["member"], **analyze_project(Path(item["path"]).read_bytes())}
            if reference:
                project["reference"] = reference_compare(java, reference[1], probe, Path(item["path"]),
                                                         reports, cache, reference[0])
            record["projects"].append(project)
        record["tabs"] = sum(project.get("tabset_tabs", 1) for project in record["projects"])
        log(f"{entry['id']}: {native['status']}, {len(record['projects'])} projects")
        return record

    unique = [entry for entry in manifest["experiments"] if not entry.get("duplicate_of")]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        experiments = list(pool.map(experiment, unique))
    return census_report(manifest, experiments, java)


def census_report(manifest, experiments, java):
    return {"schema_version": 1, "manifest_refreshed": manifest["refreshed"],
            "probe": "motrack_corpus_probe --open (desktop open path) and default import snapshot",
            "java_reference": "Tracker 6.3.5 TrackerReference (xml_and_coordinate_api)" if java else None,
            "duplicates_skipped": sum(1 for entry in manifest["experiments"] if entry.get("duplicate_of")),
            "aggregate": aggregate(experiments), "experiments": experiments}


def aggregate(experiments):
    projects = [project for record in experiments for project in record["projects"]]
    versions, tracks, instances = collections.Counter(), collections.Counter(), collections.Counter()
    videos, types, filters, models, files = (collections.Counter() for _ in range(5))
    for project in projects:
        versions[project.get("semantic_version") or project.get("version") or "(not recorded)"] += 1
        for name, count in project.get("tracks", {}).items():
            tracks[name] += 1
            instances[name] += count
        videos[project.get("video") or "(none)"] += 1
        types[project.get("video_type") or "(none)"] += 1
        filters.update(project.get("filters", []))
        models.update(project.get("models", []))
    for record in experiments:
        files.update({name: 1 for name in record["archive_file_types"]})
    reference = collections.Counter(project["reference"]["status"] for project in projects if "reference" in project)

    def ordered(counter):
        return dict(sorted(counter.items(), key=lambda item: (-item[1], item[0])))

    native = {}
    for platform in sorted({key for record in experiments for key in record["native"]}):
        results = [record["native"][platform] for record in experiments if platform in record["native"]]
        native[platform] = {
            "open": ordered(collections.Counter(result["status"] for result in results)),
            "rejections": ordered(collections.Counter(result["message"] for result in results if result["status"] == "rejected")),
            "unsupported_tracks": ordered(collections.Counter(name for result in results for name in result.get("unsupported_tracks", {}))),
            "zip_as_trz": ordered(collections.Counter(result["as_trz"]["status"] for result in results if "as_trz" in result))}
    return {"experiments": len(experiments), "projects": len(projects), "bytes": sum(record["bytes"] for record in experiments),
            "tracker_versions": ordered(versions),
            "track_classes": {name: {"projects": tracks[name], "instances": instances[name]} for name in ordered(tracks)},
            "video_classes": ordered(videos), "video_types": ordered(types), "filters": ordered(filters),
            "models": ordered(models),
            "data_functions": {"projects": sum(1 for project in projects if project.get("data_functions")),
                               "functions": sum(project.get("data_functions", 0) for project in projects)},
            "archive_file_types": ordered(files),
            "tabs": ordered(collections.Counter(str(record["tabs"]) for record in experiments)),
            "native": native, **({"reference": ordered(reference)} if reference else {})}


def census_markdown(report):
    total = report["aggregate"]

    def top(counter, limit=8):
        items = list(counter.items())
        shown = ", ".join(f"{name} ({value['projects'] if isinstance(value, dict) else value})" for name, value in items[:limit])
        return shown + (f", … {len(items) - limit} more" if len(items) > limit else "") if items else "none"

    rows = [
        ("Experiments / projects / bytes", f"{total['experiments']} / {total['projects']} / {total['bytes']:,}"),
        ("Tracker versions (projects)", top(total["tracker_versions"])),
        ("Track classes (projects using)", top(total["track_classes"], 12)),
        ("Video classes (projects)", top(total["video_classes"])),
        ("Video file types (projects)", top(total["video_types"])),
        ("Video filters (projects)", top(total["filters"])),
        ("Models (projects)", top(total["models"])),
        ("Data functions", f"{total['data_functions']['functions']} in {total['data_functions']['projects']} projects"),
        ("Archive member types (experiments)", top(total["archive_file_types"])),
        ("Tabs per experiment (experiments)", top(total["tabs"])),
    ]
    for platform, native in total["native"].items():
        rows += [(f"MoTrack desktop open, {platform} (experiments)", top(native["open"])),
                 (f"MoTrack open rejections, {platform}", top(native["rejections"], 6)),
                 (f"Unsupported tracks reported, {platform} (experiments)", top(native["unsupported_tracks"], 10))]
        if native["zip_as_trz"]:
            rows.append((f"ZIP experiments renamed .trz, {platform}", top(native["zip_as_trz"])))
    if "reference" in total:
        rows.append(("Java 6.3.5 XML/coordinate vs MoTrack import (projects)", top(total["reference"])))
    lines = [CENSUS_START, "", f"Generated from manifest refresh {report['manifest_refreshed']} by "
             "`python tools/corpus.py library-census`; see `library-census.json` for every experiment.", "",
             "| Census | Result |", "| --- | --- |"]
    lines += [f"| {label} | {html.escape(value, quote=False).replace('|', '/')} |" for label, value in rows]
    return "\n".join(lines + ["", CENSUS_END])


def merge_platforms(report, committed, platform):
    """Carry other platforms' committed native outcomes for byte-identical experiments into ``report``."""
    previous = {record["id"]: record for record in (committed or {}).get("experiments", [])}
    for record in report["experiments"]:
        old = previous.get(record["id"])
        if old and old.get("sha256") == record["sha256"]:
            for key, value in old.get("native", {}).items():
                if key != platform:
                    record["native"][key] = value
        record["native"] = dict(sorted(record["native"].items()))
    report["aggregate"] = aggregate(report["experiments"])
    return report


def without_platform(report, platform):
    stripped = copy.deepcopy(report)
    for record in stripped["experiments"]:
        record["native"].pop(platform, None)
    stripped["aggregate"] = aggregate(stripped["experiments"])
    return stripped


def census_main(manifest, cache, args):
    platform = platform_key()
    committed = json.loads(LIBRARY_CENSUS.read_text(encoding="utf-8")) if LIBRARY_CENSUS.exists() else None
    report = merge_platforms(census(manifest, cache, args.probe, args.java, jobs=args.jobs), committed, platform)
    readme = README.read_text(encoding="utf-8")
    if CENSUS_START not in readme or CENSUS_END not in readme:
        raise ValueError("README is missing the library census markers")

    def with_table(value):
        return readme[:readme.index(CENSUS_START)] + census_markdown(value) + readme[readme.index(CENSUS_END) + len(CENSUS_END):]

    # Keep the regenerated report beside the cache for review (CI uploads it as an artifact).
    write_json(cache / "library" / "library-census.json", report)
    if args.check:
        if committed is None:
            raise ValueError("No committed library census to check")
        expected, actual = committed, report
        if not any(platform in record.get("native", {}) for record in committed["experiments"]):
            # No baseline for this platform yet: check everything else and publish this platform's results.
            actual = without_platform(report, platform)
            print(f"No committed {platform} native baseline; its results are in {cache / 'library' / 'library-census.json'}")
        differences = list(diff(expected, actual))[:40]
        if differences or with_table(actual) != readme:
            raise ValueError("Library census differs from the committed report:\n" + "\n".join(differences or ["README table"]))
        print(f"Library census matches ({report['aggregate']['experiments']} experiments)")
        return
    write_json(LIBRARY_CENSUS, report)
    README.write_text(with_table(report), encoding="utf-8", newline="\n")
    print(f"Wrote library census for {report['aggregate']['experiments']} experiments")


def diff(expected, actual, location="census"):
    if isinstance(expected, dict) and isinstance(actual, dict):
        for key in sorted(set(expected) | set(actual)):
            if key not in actual or key not in expected:
                yield f"{location}.{key}: {'missing' if key not in actual else 'unexpected'}"
            else:
                yield from diff(expected[key], actual[key], f"{location}.{key}")
    elif isinstance(expected, list) and isinstance(actual, list) and len(expected) == len(actual):
        for index, (left, right) in enumerate(zip(expected, actual)):
            label = left.get("id") or left.get("member") if isinstance(left, dict) else index
            yield from diff(left, right, f"{location}[{label}]")
    elif expected != actual:
        yield f"{location}: expected {json.dumps(expected)[:120]}, got {json.dumps(actual)[:120]}"


def main(args):
    cache = args.cache.resolve()
    sources = set(args.source or SOURCE_IDS)
    if not sources <= set(SOURCE_IDS):
        raise ValueError(f"Unknown library source; choose from {', '.join(SOURCE_IDS)}")
    if args.command == "discover-library":
        previous = load_library_manifest() if LIBRARY_MANIFEST.exists() else None
        manifest = discover(cache, sources=sources, previous=previous, redownload=args.redownload)
        write_json(LIBRARY_MANIFEST, manifest)
        print(f"Pinned {len(manifest['experiments'])} library experiments "
              f"({sum(entry['bytes'] for entry in manifest['experiments'])} bytes); "
              f"{len(manifest['excluded'])} excluded, {len(manifest['withdrawn'])} withdrawn")
        return
    manifest = load_library_manifest()
    if args.command == "fetch-library":
        fetch(manifest, cache, sources=sources)
    elif args.command == "verify-library":
        selected = [entry for entry in manifest["experiments"] if entry["source"] in sources]
        for entry in selected:
            corpus.verify_file(library_files(cache) / entry["file"], entry)
        print(f"Verified {len(selected)} library experiments")
    elif args.command == "library-projects":
        items = library_projects(manifest, cache)
        index = cache / "library" / "projects" / "index.json"
        write_json(index, {"schema_version": 1, "manifest_refreshed": manifest["refreshed"], "projects": items})
        print(f"Wrote {len(items)} library projects; index: {index}")
    else:
        census_main(manifest, cache, args)
