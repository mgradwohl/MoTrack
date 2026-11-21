"""Install the project-local Qt SDK and developer Python tools.

Qt comes from aqtinstall (PyPI) when it is reachable. When PyPI or aqt fails, or with
``--qt-source direct``, the same essential archives aqt would install come straight from the Qt
online repository: the package list and each archive's SHA-256 are read from download.qt.io itself
(never through a mirror redirect), every archive is checked before it is extracted with a
libarchive ``bsdtar`` (7z support), and the result is moved into the prefix ``qt-prefix`` prints.
"""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import venv
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
AQT_VERSION = "3.3.0"
QT_REPOSITORY = "https://download.qt.io/online/qtsdkrepository"
QT_HOST = "download.qt.io"
TIMEOUT = 120


def toolchain():
    """The KEY=value pins in tools/toolchain.env."""
    pins = {}
    for line in (ROOT / "tools" / "toolchain.env").read_text(encoding="utf-8").splitlines():
        if match := re.fullmatch(r"([A-Z0-9_]+)=(\S*)\s*", line):
            pins[match[1]] = match[2]
    return pins


QT_VERSION = toolchain()["MOTRACK_QT_VERSION"]


def platform_config():
    """(tools directory, aqt host, aqt arch, Qt directory, online-repository OS) for this machine."""
    machine = platform.machine().lower()
    if sys.platform == "win32" and machine in {"amd64", "x86_64"}:
        return "windows", "windows", "win64_msvc2022_64", "msvc2022_64", "windows_x86"
    if sys.platform == "darwin" and machine in {"arm64", "x86_64"}:
        return "macos", "mac", "clang_64", "macos", "mac_x64"
    if sys.platform == "linux" and machine == "x86_64":
        return "linux", "linux", "linux_gcc_64", "gcc_64", "linux_x64"
    raise SystemExit(f"Automatic Qt setup is unavailable for {sys.platform}/{machine}.")


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(req.full_url, code, f"refusing redirect to {newurl}", headers, fp)


def fetch_trusted(url):
    """The body of a small metadata or checksum file, from download.qt.io itself."""
    if urllib.parse.urlsplit(url).hostname != QT_HOST:
        raise SystemExit(f"Qt metadata must come from {QT_HOST}: {url}")
    with urllib.request.build_opener(_NoRedirect).open(url, timeout=TIMEOUT) as response:
        return response.read()


def download(url, target):
    """Download a (possibly mirrored) archive to target."""
    partial = target.with_name(target.name + ".part")
    with urllib.request.urlopen(url, timeout=TIMEOUT) as response, open(partial, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)
    partial.replace(target)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def published_sha256(url):
    """The SHA-256 download.qt.io publishes beside url (its '<file>.sha256')."""
    text = fetch_trusted(url + ".sha256").decode("ascii", "replace").split()
    if not text or not re.fullmatch(r"[0-9a-f]{64}", text[0].lower()):
        raise SystemExit(f"Unreadable checksum for {url}")
    return text[0].lower()


def repository_url(repository_os, version):
    compact = version.replace(".", "")
    return f"{QT_REPOSITORY}/{repository_os}/desktop/qt6_{compact}/qt6_{compact}/"


def essential_archives(updates, package, version, directory):
    """[(archive file name, target relative to the qt directory)] for package's archives.

    Qt 6.8 archives carry no '<version>/<directory>' prefix; the repository's Extract operations name
    each archive's target (d3dcompiler_47 and opengl32sw go to bin/), and the prefix is the default.
    """
    record = next((p for p in ET.fromstring(updates).iter("PackageUpdate") if p.findtext("Name") == package), None)
    if record is None:
        raise SystemExit(f"The Qt repository has no {package}.")
    build = record.findtext("Version", "").strip()
    names = [a.strip() for a in record.findtext("DownloadableArchives", "").split(",") if a.strip()]
    if not build or not names:
        raise SystemExit(f"The Qt repository lists no archives for {package}.")
    prefix = f"{version}/{directory}"
    targets = {}
    for operation in record.iter("Operation"):
        arguments = [a.text or "" for a in operation.findall("Argument")]
        if operation.get("name") == "Extract" and len(arguments) == 2:
            target = arguments[0].removeprefix("@TargetDir@").strip("/")
            if (target != prefix and not target.startswith(prefix + "/")) or ".." in target.split("/"):
                raise SystemExit(f"Unexpected Qt extract target {arguments[0]} for {arguments[1]}.")
            targets[arguments[1]] = target
    return [(build + name, targets.get(name, prefix)) for name in names]


def find_bsdtar():
    """A tar that reads 7z (libarchive's bsdtar), or exit with how to get one."""
    if sys.platform == "win32":
        candidates = [Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "tar.exe"]
    else:
        candidates = [shutil.which("bsdtar"), "/usr/bin/tar" if sys.platform == "darwin" else None]
    for candidate in filter(None, candidates):
        try:
            version = subprocess.run([candidate, "--version"], capture_output=True, text=True, check=False).stdout
        except OSError:
            continue
        if "bsdtar" in version:
            return str(candidate)
    hint = "sudo apt-get install libarchive-tools" if sys.platform == "linux" else "libarchive's bsdtar"
    raise SystemExit(f"Extracting Qt's 7z archives needs bsdtar ({hint}).")


def mark_open_source(prefix):
    """The edits Qt's own installer makes: qt.conf for a relocated prefix, an open-source qconfig.pri."""
    (prefix / "bin").mkdir(parents=True, exist_ok=True)
    (prefix / "bin" / "qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="utf-8")
    qconfig = prefix / "mkspecs" / "qconfig.pri"
    if qconfig.is_file():
        lines = qconfig.read_text(encoding="utf-8").splitlines()
        lines = ["QT_EDITION = OpenSource" if line.startswith("QT_EDITION =") else
                 "QT_LICHECK =" if line.startswith("QT_LICHECK =") else line for line in lines]
        qconfig.write_text("\n".join(lines) + "\n", encoding="utf-8")


def install_qt_direct(root, archive, directory, repository_os, version=QT_VERSION, extract=None):
    """Install Qt's essential archives for archive (win64_msvc2022_64, ...) from download.qt.io."""
    base = repository_url(repository_os, version)
    updates = fetch_trusted(base + "Updates.xml")
    expected = published_sha256(base + "Updates.xml")
    if hashlib.sha256(updates).hexdigest() != expected:
        raise SystemExit("Qt's Updates.xml does not match its published SHA-256.")
    package = f"qt.qt6.{version.replace('.', '')}.{archive}"
    archives = essential_archives(updates, package, version, directory)
    downloads = root / "qt-downloads"
    staging = root / "qt.partial"
    downloads.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(staging, ignore_errors=True)
    tar = None if extract else find_bsdtar()
    for name, target in archives:
        url = f"{base}{package}/{name}"
        checksum = published_sha256(url)
        path = downloads / name
        if not path.is_file() or sha256(path) != checksum:
            print(f"Downloading {name}", flush=True)
            download(url, path)
        if (actual := sha256(path)) != checksum:
            path.unlink()
            raise SystemExit(f"{name} has SHA-256 {actual}, but download.qt.io publishes {checksum}.")
        destination = staging / target
        destination.mkdir(parents=True, exist_ok=True)
        print(f"Extracting {name}", flush=True)
        if extract:
            extract(path, destination)
        else:
            subprocess.run([tar, "-xf", path, "-C", destination], check=True)
    staged = staging / version / directory
    if not (staged / "lib/cmake/Qt6/Qt6Config.cmake").is_file():
        raise SystemExit(f"The Qt archives did not provide lib/cmake/Qt6/Qt6Config.cmake under {staged}.")
    mark_open_source(staged)
    prefix = root / "qt" / version / directory
    shutil.rmtree(prefix, ignore_errors=True)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    staged.replace(prefix)
    shutil.rmtree(staging, ignore_errors=True)
    shutil.rmtree(downloads, ignore_errors=True)
    print(f"Installed Qt {version} ({len(archives)} verified archives) in {prefix}", flush=True)


def install_qt_aqt(python, root, host, archive):
    subprocess.run([python, "-m", "pip", "install", "--disable-pip-version-check", f"aqtinstall=={AQT_VERSION}"],
                   check=True)
    subprocess.run([python, "-m", "aqt", "install-qt", host, "desktop", QT_VERSION, archive, "-O", root / "qt"],
                   cwd=root, check=True)


def install(root, host, archive, directory, repository_os, source):
    environment = root / "venv"
    python = environment / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
    if not python.is_file():
        venv.EnvBuilder(with_pip=True).create(environment)
    # Older setups installed PyPI's clang-format 18 here; the pinned LLVM provides clang-format now.
    if python.with_name("clang-format.exe" if sys.platform == "win32" else "clang-format").exists():
        subprocess.run([python, "-m", "pip", "uninstall", "--yes", "clang-format"], check=True)
    prefix = root / "qt" / QT_VERSION / directory
    qt_config = prefix / "lib/cmake/Qt6/Qt6Config.cmake"
    if qt_config.is_file():
        return
    if source in {"auto", "aqt"}:
        try:
            install_qt_aqt(python, root, host, archive)
        except subprocess.CalledProcessError as error:
            if source == "aqt":
                raise
            print(f"aqtinstall failed ({error}); installing Qt from download.qt.io instead.", flush=True)
    if not qt_config.is_file():
        install_qt_direct(root, archive, directory, repository_os)
    if not qt_config.is_file():
        raise SystemExit(f"Qt installation is missing {qt_config}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["install", "qt-prefix", "tools-root"])
    parser.add_argument("--qt-source", choices=["auto", "aqt", "direct"],
                        default=os.environ.get("MOTRACK_QT_SOURCE", "auto"),
                        help="aqt, the verified download.qt.io archives, or aqt with that fallback (default; "
                             "MOTRACK_QT_SOURCE overrides it)")
    args = parser.parse_args()
    name, host, archive, directory, repository_os = platform_config()
    root = ROOT / ".dev-tools" / name
    prefix = root / "qt" / QT_VERSION / directory
    if args.command == "install":
        install(root, host, archive, directory, repository_os, args.qt_source)
    elif args.command == "qt-prefix":
        print(prefix)
    else:
        print(root)


if __name__ == "__main__":
    main()
