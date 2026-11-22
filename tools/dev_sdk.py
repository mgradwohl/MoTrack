"""Install the project-local Qt SDK and developer Python tools.

Qt comes from aqtinstall (PyPI) when it is reachable. When PyPI or aqt fails, or with
``--qt-source direct``, the same essential archives aqt would install come straight from the Qt
online repository: the package list is read from download.qt.io itself (never through a mirror
redirect), every archive is checked against the SHA-256 pinned in tools/qt-archives.sha256 before it
is extracted with a libarchive ``bsdtar`` (7z support), and the result is moved into the prefix
``qt-prefix`` prints.

Both ways also install the add-on modules in ``QT_MODULES`` (Qt Multimedia, for camera capture), whose
archives are pinned the same way. A prefix installed before a module was added gets just that module:
its pinned archives are added without replacing or deleting any file already in the prefix
(``--qt-source aqt`` uses aqt's ``--noarchives`` amendment instead).
"""

import argparse
import datetime
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
QT_ARCHIVE_PINS = ROOT / "tools" / "qt-archives.sha256"
# Qt add-on modules beyond the essentials, by aqt module name, with the CMake package each provides.
# Qt Multimedia supplies cameras and their frames; it bundles its own FFmpeg (docs/camera-capture.md).
QT_MODULES = {"qtmultimedia": "lib/cmake/Qt6Multimedia/Qt6MultimediaConfig.cmake"}
# Every platform's packages (essentials and QT_MODULES), as (aqt arch, Qt directory, online-repository OS).
QT_PACKAGES = (("win64_msvc2022_64", "msvc2022_64", "windows_x86"), ("linux_gcc_64", "gcc_64", "linux_x64"),
               ("clang_64", "macos", "mac_x64"))


def pinned_archives(path=QT_ARCHIVE_PINS):
    """{'<repository OS>/<package>/<archive>': sha256} from tools/qt-archives.sha256."""
    pins = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            if not (match := re.fullmatch(r"([0-9a-f]{64})  (\S+)", line)):
                raise SystemExit(f"Malformed line in {path.name}: {line}")
            pins[match[2]] = match[1]
    return pins


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


class QtPrefixConflict(SystemExit):
    """A module archive would replace a file in the installed prefix; nothing was added."""


def missing_modules(prefix):
    """The QT_MODULES whose CMake package the Qt prefix lacks, in QT_MODULES order."""
    return [module for module, config in QT_MODULES.items() if not (Path(prefix) / config).is_file()]


def qt_packages(archive, version=QT_VERSION, modules=tuple(QT_MODULES), essentials=True):
    """The essential package for archive and its add-on module packages (qt.qt6.683.addons.<module>.<arch>)."""
    compact = version.replace(".", "")
    return ([f"qt.qt6.{compact}.{archive}"] if essentials else []) + \
        [f"qt.qt6.{compact}.addons.{module}.{archive}" for module in modules]


def repository_archives(archive, directory, repository_os, version=QT_VERSION, modules=tuple(QT_MODULES), essentials=True):
    """(base URL, [(package, [(archive file name, target)])]) from download.qt.io's current Updates.xml."""
    base = repository_url(repository_os, version)
    updates = fetch_trusted(base + "Updates.xml")
    if hashlib.sha256(updates).hexdigest() != published_sha256(base + "Updates.xml"):
        raise SystemExit("Qt's Updates.xml does not match its published SHA-256.")
    return base, [(package, essential_archives(updates, package, version, directory))
                  for package in qt_packages(archive, version, modules, essentials)]


def print_archive_checksums():
    """Print a new tools/qt-archives.sha256 from download.qt.io's published checksums."""
    lines = [line for line in QT_ARCHIVE_PINS.read_text(encoding="utf-8").splitlines() if line.startswith("#")]
    lines = [re.sub(r"on \d{4}-\d{2}-\d{2}", f"on {datetime.date.today().isoformat()}", line) for line in lines]
    for archive, directory, repository_os in QT_PACKAGES:
        base, packages = repository_archives(archive, directory, repository_os)
        for package, archives in packages:
            lines += [f"{published_sha256(f'{base}{package}/{name}')}  {repository_os}/{package}/{name}" for name, _ in archives]
    print("\n".join(lines))


def stage_packages(root, archive, directory, repository_os, version, extract, pins, modules, essentials):
    """Download, check and extract the packages' archives under root/qt.partial; the archive count.

    Each package's archive list must equal its entries in tools/qt-archives.sha256, and every archive
    must match its pinned SHA-256, before anything is extracted from it.
    """
    base, packages = repository_archives(archive, directory, repository_os, version, modules, essentials)
    pins = pinned_archives() if pins is None else pins
    for package, archives in packages:
        prefix_key = f"{repository_os}/{package}/"
        listed = {prefix_key + name for name, _ in archives}
        pinned = {key for key in pins if key.startswith(prefix_key)}
        if listed != pinned:
            names = ", ".join(sorted(key.removeprefix(prefix_key) for key in listed ^ pinned))
            raise SystemExit(f"download.qt.io's {package} archives differ from tools/qt-archives.sha256 ({names}). "
                             "If Qt republished them, review the change, then run python tools/dev_sdk.py "
                             "qt-archive-checksums.")
    downloads = root / "qt-downloads"
    staging = root / "qt.partial"
    downloads.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(staging, ignore_errors=True)
    tar = None if extract else find_bsdtar()
    count = 0
    for package, archives in packages:
        for name, target in archives:
            url = f"{base}{package}/{name}"
            checksum = pins[f"{repository_os}/{package}/{name}"]
            path = downloads / name
            if not path.is_file() or sha256(path) != checksum:
                print(f"Downloading {name}", flush=True)
                download(url, path)
            if (actual := sha256(path)) != checksum:
                path.unlink()
                raise SystemExit(f"{name} has SHA-256 {actual}, but tools/qt-archives.sha256 pins {checksum}.")
            destination = staging / target
            destination.mkdir(parents=True, exist_ok=True)
            print(f"Extracting {name}", flush=True)
            if extract:
                extract(path, destination)
            else:
                subprocess.run([tar, "-xf", path, "-C", destination], check=True)
            count += 1
    return count


def install_qt_direct(root, archive, directory, repository_os, version=QT_VERSION, extract=None, pins=None,
                      modules=tuple(QT_MODULES)):
    """Install Qt's essential archives and modules for archive (win64_msvc2022_64, ...) from download.qt.io."""
    count = stage_packages(root, archive, directory, repository_os, version, extract, pins, modules, True)
    staging = root / "qt.partial"
    staged = staging / version / directory
    for config in ["lib/cmake/Qt6/Qt6Config.cmake"] + [QT_MODULES[module] for module in modules]:
        if not (staged / config).is_file():
            raise SystemExit(f"The Qt archives did not provide {config} under {staged}.")
    mark_open_source(staged)
    prefix = root / "qt" / version / directory
    shutil.rmtree(prefix, ignore_errors=True)
    prefix.parent.mkdir(parents=True, exist_ok=True)
    staged.replace(prefix)
    shutil.rmtree(staging, ignore_errors=True)
    shutil.rmtree(root / "qt-downloads", ignore_errors=True)
    print(f"Installed Qt {version} ({count} verified archives) in {prefix}", flush=True)


def add_qt_modules_direct(root, archive, directory, repository_os, modules, version=QT_VERSION, extract=None, pins=None):
    """Add modules' pinned archives to an installed prefix without replacing or deleting its files.

    A staged file that already exists in the prefix with other content stops the amendment before
    anything is added; identical files are left as they are.
    """
    count = stage_packages(root, archive, directory, repository_os, version, extract, pins, modules, False)
    prefix = root / "qt" / version / directory
    staging = root / "qt.partial"
    staged = staging / version / directory
    for module in modules:
        if not (staged / QT_MODULES[module]).is_file():
            raise SystemExit(f"The Qt archives did not provide {QT_MODULES[module]} under {staged}.")
    # Links (macOS framework Versions/Current, Linux library sonames) move as links, never followed.
    entries = []
    for directory_path, directories, files in os.walk(staged):
        entries += [Path(directory_path) / name for name in files]
        entries += [Path(directory_path) / name for name in directories if (Path(directory_path) / name).is_symlink()]
    additions = []
    for source in sorted(entries):
        target = prefix / source.relative_to(staged)
        if target.is_symlink() or target.exists():
            same_link = source.is_symlink() and target.is_symlink() and os.readlink(source) == os.readlink(target)
            same_file = not source.is_symlink() and not target.is_symlink() and target.is_file() and sha256(target) == sha256(source)
            if same_link or same_file:
                continue
            raise QtPrefixConflict(f"{target} already exists with other content; the {', '.join(modules)} archives were not added.")
        additions.append((source, target))
    for source, target in additions:
        target.parent.mkdir(parents=True, exist_ok=True)
        source.replace(target)
    shutil.rmtree(staging, ignore_errors=True)
    shutil.rmtree(root / "qt-downloads", ignore_errors=True)
    print(f"Added {', '.join(modules)} ({count} verified archives, {len(additions)} new files) to {prefix}", flush=True)


def install_qt_aqt(python, root, host, archive, modules=tuple(QT_MODULES), amend=False):
    """aqt install-qt with the modules; amend adds only the modules to an existing prefix (--noarchives)."""
    subprocess.run([python, "-m", "pip", "install", "--disable-pip-version-check", f"aqtinstall=={AQT_VERSION}"],
                   check=True)
    command = [python, "-m", "aqt", "install-qt", host, "desktop", QT_VERSION, archive, "-O", root / "qt"]
    if modules:
        command += ["-m", *modules]
    if amend:
        command.append("--noarchives")
    subprocess.run(command, cwd=root, check=True)


def amend(python, root, host, archive, directory, repository_os, source, prefix):
    """Add the modules an existing prefix lacks: pinned add-only archives first (aqt with --qt-source aqt)."""
    modules = missing_modules(prefix)
    if not modules:
        return
    print(f"Adding {', '.join(modules)} to {prefix}", flush=True)
    if source == "aqt":
        install_qt_aqt(python, root, host, archive, modules, amend=True)
        return
    try:
        add_qt_modules_direct(root, archive, directory, repository_os, modules)
    except QtPrefixConflict:
        raise
    except (OSError, SystemExit) as error:
        if source == "direct":
            raise
        print(f"The download.qt.io amendment failed ({error}); trying aqtinstall instead.", flush=True)
        install_qt_aqt(python, root, host, archive, modules, amend=True)


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
        amend(python, root, host, archive, directory, repository_os, source, prefix)
    else:
        if source in {"auto", "aqt"}:
            try:
                install_qt_aqt(python, root, host, archive)
            except subprocess.CalledProcessError as error:
                if source == "aqt":
                    raise
                print(f"aqtinstall failed ({error}); installing Qt from download.qt.io instead.", flush=True)
        if not qt_config.is_file() or missing_modules(prefix):
            install_qt_direct(root, archive, directory, repository_os)
    if not qt_config.is_file():
        raise SystemExit(f"Qt installation is missing {qt_config}.")
    if modules := missing_modules(prefix):
        raise SystemExit(f"Qt installation in {prefix} is missing {', '.join(modules)}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["install", "qt-prefix", "tools-root", "qt-archive-checksums"])
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
    elif args.command == "qt-archive-checksums":
        print_archive_checksums()
    elif args.command == "qt-prefix":
        print(prefix)
    else:
        print(root)


if __name__ == "__main__":
    main()
