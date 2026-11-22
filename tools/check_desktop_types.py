"""Check the document types a staged MoTrack build declares to the desktop (#68).

`check STAGE` validates the installed macOS Info.plist or Linux desktop entry and shared-mime-info
package; the platform validators run when installed. `launch STAGE` additionally opens a file whose
folder and name contain spaces and non-ASCII characters through the desktop's own launcher
(macOS LaunchServices, GLib's desktop-entry expansion) and requires the staged app to report it.
`launch` registers the bundle with LaunchServices on macOS, so it only runs in CI.
"""

import argparse
import configparser
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
import xml.etree.ElementTree as ElementTree

APP_ID = "com.github.mgradwohl.motrack"
# Extension -> (macOS type identifier, declared as exported, document-type rank, Linux MIME type).
TYPES = {
    "motrack": ("com.github.mgradwohl.motrack.project", True, "Owner", "application/x-motrack-project"),
    "trk": ("org.opensourcephysics.tracker.trk", False, "Default", "application/x-tracker-project"),
    "trz": ("org.opensourcephysics.tracker.trz", False, "Default", "application/x-tracker-archive"),
}
FOLDER = "Launch folder — Ünïcødé 測定"
LSREGISTER = Path(
    "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
)
LAUNCH_SECONDS = 60


def require(condition, message):
    if not condition:
        raise SystemExit(f"check_desktop_types: {message}")


def check_plist(bundle):
    path = bundle / "Contents" / "Info.plist"
    require(path.is_file(), f"{path} is missing")
    if shutil.which("plutil"):
        subprocess.run(["plutil", "-lint", str(path)], check=True)
    info = plistlib.loads(path.read_bytes())
    require(info.get("CFBundleIdentifier") == APP_ID, "unexpected bundle identifier")
    minimum = info.get("LSMinimumSystemVersion", "")
    require(all(part.isdigit() for part in minimum.split(".")) and minimum, f"LSMinimumSystemVersion is {minimum!r}")
    executable = bundle / "Contents" / "MacOS" / info.get("CFBundleExecutable", "")
    require(executable.is_file(), f"bundle executable {executable} is missing")
    declarations = {}
    for key, exported in (("UTExportedTypeDeclarations", True), ("UTImportedTypeDeclarations", False)):
        for declaration in info.get(key, []):
            tags = declaration.get("UTTypeTagSpecification", {}).get("public.filename-extension", [])
            declarations[declaration["UTTypeIdentifier"]] = (exported, tags)
    claims = {}
    for document in info.get("CFBundleDocumentTypes", []):
        for identifier in document.get("LSItemContentTypes", []):
            claims[identifier] = (document.get("CFBundleTypeRole"), document.get("LSHandlerRank"))
    for extension, (identifier, exported, rank, _) in TYPES.items():
        require(declarations.get(identifier) == (exported, [extension]), f"{identifier} does not declare .{extension}")
        require(claims.get(identifier) == ("Editor", rank), f"{identifier} is not opened as an editor with rank {rank}")
    for media in ("public.movie", "public.image"):
        require(claims.get(media, (None, None))[1] == "Alternate", f"{media} is not an alternate document type")
    print(f"{path}: declares {', '.join('.' + extension for extension in TYPES)}")


def unescape(value):
    """Desktop Entry string-value escapes: \\s, \\n, \\t, \\r and \\\\."""
    result, escaped = [], False
    for character in value:
        if escaped:
            result.append({"s": " ", "n": "\n", "t": "\t", "r": "\r"}.get(character, character))
            escaped = False
        elif character == "\\":
            escaped = True
        else:
            result.append(character)
    return "".join(result)


def read_desktop_entry(path):
    parser = configparser.ConfigParser(interpolation=None, delimiters=("=",))
    parser.optionxform = str
    parser.read(path, encoding="utf-8")
    require(parser.has_section("Desktop Entry"), f"{path} has no [Desktop Entry] group")
    return parser["Desktop Entry"]


def check_linux(stage):
    desktop = stage / "share" / "applications" / f"{APP_ID}.desktop"
    mime = stage / "share" / "mime" / "packages" / f"{APP_ID}.xml"
    require(desktop.is_file() and mime.is_file(), "the desktop entry or MIME package was not installed")
    entry = read_desktop_entry(desktop)
    binary = (stage / "bin" / "motrack").resolve()
    require(entry.get("Type") == "Application", "the desktop entry is not an application")
    require(entry.get("Exec", "").endswith(" %F"), "Exec must pass each local file as its own argument (%F)")
    require(Path(unescape(entry.get("TryExec", ""))).resolve() == binary, "TryExec does not name the staged binary")
    declared = {value for value in entry.get("MimeType", "").split(";") if value}
    namespace = {"m": "http://www.freedesktop.org/standards/shared-mime-info"}
    globs = {}
    for item in ElementTree.parse(mime).getroot().findall("m:mime-type", namespace):
        globs[item.get("type")] = [glob.get("pattern") for glob in item.findall("m:glob", namespace)]
    for extension, (_, _, _, mime_type) in TYPES.items():
        require(mime_type in declared, f"the desktop entry does not open {mime_type}")
        require(globs.get(mime_type) == [f"*.{extension}"], f"{mime_type} does not match *.{extension}")
    if shutil.which("desktop-file-validate"):
        subprocess.run(["desktop-file-validate", str(desktop)], check=True)
    print(f"{desktop}: opens {', '.join(sorted(declared))}")


def write_desktop_entry(prefix):
    """Run the install-time desktop entry script for another prefix."""
    source = Path(__file__).resolve().parents[1]
    return subprocess.run(
        [
            "cmake",
            f"-DCMAKE_INSTALL_PREFIX={prefix}",
            f"-DMOTRACK_DESKTOP_TEMPLATE={source / 'desktop' / 'linux' / f'{APP_ID}.desktop.in'}",
            f"-DMOTRACK_DESKTOP_OUTPUT={APP_ID}.desktop",
            "-DMOTRACK_DESKTOP_BINARY=bin/motrack",
            "-P",
            str(source / "cmake" / "MoTrackDesktopEntry.cmake"),
        ],
        capture_output=True,
        text=True, encoding="utf-8",
    )


def registered_data_home(stage, root):
    """Per-user registration as documented in the README, inside a private XDG data directory."""
    data = root / "share"
    (data / "applications").mkdir(parents=True)
    (data / "mime" / "packages").mkdir(parents=True)
    shutil.copy2(stage / "share" / "applications" / f"{APP_ID}.desktop", data / "applications")
    shutil.copy2(stage / "share" / "mime" / "packages" / f"{APP_ID}.xml", data / "mime" / "packages")
    subprocess.run(["update-mime-database", str(data / "mime")], check=True)
    subprocess.run(["update-desktop-database", str(data / "applications")], check=True)
    return data


def sample_files(root):
    folder = root / FOLDER
    folder.mkdir()
    fixture = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "point-mass.trk"
    sample = folder / "point mass — é ü.trk"
    shutil.copy2(fixture, sample)
    return sample


def same_path(path):
    """macOS reports file names decomposed (NFD); its file systems treat both forms as one name."""
    return unicodedata.normalize("NFC", str(Path(path).resolve()))


def wait_for_report(output, errors, sample):
    deadline = time.monotonic() + LAUNCH_SECONDS
    expected = f"opened\t{sample.resolve()}"
    while time.monotonic() < deadline:
        lines = [line.strip() for line in output.read_text(encoding="utf-8", errors="replace").splitlines()]
        opened = [same_path(line.split("\t", 1)[1]) for line in lines if line.startswith("opened\t")]
        if opened:
            require(opened == [same_path(sample)], f"opened {opened}, expected {expected}")
            return
        if errors.exists() and "MoTrack:" in errors.read_text(encoding="utf-8", errors="replace"):
            raise SystemExit(errors.read_text(encoding="utf-8", errors="replace"))
        time.sleep(0.5)
    raise SystemExit(f"the launched app did not report {expected}")


def launch_linux(stage, root):
    data = registered_data_home(stage, root)
    environment = {**os.environ, "XDG_DATA_HOME": str(data), "QT_QPA_PLATFORM": "offscreen"}
    sample = sample_files(root)
    for extension, (_, _, _, mime_type) in TYPES.items():
        probe = sample.with_suffix("." + extension)
        if not probe.exists():
            probe.write_bytes(b"probe")  # GIO reports any empty file as application/x-zerosize.
        # The fast type comes from the name alone, as a file manager lists it before reading content.
        info = subprocess.run(
            ["gio", "info", "--attributes=standard::fast-content-type", str(probe)],
            env=environment, check=True, capture_output=True, text=True, encoding="utf-8",
        ).stdout
        require(f"standard::fast-content-type: {mime_type}" in info, f"{probe.name} is not {mime_type}: {info}")
        handlers = subprocess.run(["gio", "mime", mime_type], env=environment, check=True, capture_output=True, text=True, encoding="utf-8").stdout
        require(f"{APP_ID}.desktop" in handlers, f"{mime_type} is not registered to MoTrack: {handlers}")
    # Write the entry again for a prefix that needs the Exec escapes, reaching the staged binary via a link.
    # CMake turns a backslash into a path separator, so an install prefix cannot contain one.
    prefix = root / 'Odd "prefix" $HOME `tick` ü'
    prefix.mkdir()
    (prefix / "bin").symlink_to(stage / "bin", target_is_directory=True)
    require(write_desktop_entry(prefix).returncode == 0, f"no desktop entry was written for {prefix}")
    # GLib looks up the Exec program before expanding %%, so the install refuses a path containing %.
    percent = root / "100% done"
    percent.mkdir()
    refused = write_desktop_entry(percent)
    require(refused.returncode != 0 and "contains %" in " ".join(refused.stderr.split()), "an install path with % was accepted")
    # desktop-file-validate cannot open a file by this path; the entry's content is what needs validating.
    unusual = root / "unusual-prefix.desktop"
    shutil.copy2(prefix / f"{APP_ID}.desktop", unusual)
    subprocess.run(["desktop-file-validate", str(unusual)], check=True)
    # GLib expands Exec and %F exactly as a file manager does; only --smoke-test is added so the app exits.
    for entry in (data / "applications" / f"{APP_ID}.desktop", unusual):
        smoke = root / f"{APP_ID}-smoke.desktop"
        smoke.write_text(entry.read_text(encoding="utf-8").replace(" %F", " --smoke-test %F"), encoding="utf-8")
        output = root / "stdout.txt"
        errors = root / "stderr.txt"
        with output.open("wb") as stdout, errors.open("wb") as stderr:
            subprocess.run(["gio", "launch", str(smoke), str(sample)], env=environment, check=True, stdout=stdout, stderr=stderr)
        wait_for_report(output, errors, sample)
        print(f"GLib launched {entry} with {sample}")


def launch_macos(stage, root):
    bundle = (stage / "motrack.app").resolve()
    subprocess.run([str(LSREGISTER), "-f", str(bundle)], check=True)
    sample = sample_files(root)
    for extension in TYPES:
        probe = sample.with_suffix("." + extension)
        if not probe.exists():
            probe.write_bytes(b"probe")
        script = (
            "ObjC.import('AppKit');"
            f"var url = $.NSURL.fileURLWithPath({str(probe)!r});"
            "var app = $.NSWorkspace.sharedWorkspace.URLForApplicationToOpenURL(url);"
            "app.isNil() ? '' : app.path.js"
        )
        handler = subprocess.run(["osascript", "-l", "JavaScript", "-e", script], check=True, capture_output=True, text=True, encoding="utf-8")
        require(same_path(handler.stdout.strip()) == same_path(bundle), f".{extension} opens with {handler.stdout.strip() or 'nothing'}")
    output = root / "stdout.txt"
    errors = root / "stderr.txt"
    # LaunchServices delivers the document as an Apple Event, which Qt turns into a QFileOpenEvent.
    subprocess.run(
        ["open", "-n", "-W", "--stdout", str(output), "--stderr", str(errors), "-a", str(bundle), str(sample), "--args", "--smoke-test"],
        check=True, timeout=LAUNCH_SECONDS,
    )
    wait_for_report(output, errors, sample)
    print(f"LaunchServices opened {sample} in the staged bundle")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["check", "launch"])
    parser.add_argument("stage", type=Path, help="install prefix of the staged build")
    arguments = parser.parse_args()
    stage = arguments.stage.resolve()
    bundle = stage / "motrack.app"
    if sys.platform == "darwin":
        check_plist(bundle)
    elif sys.platform.startswith("linux"):
        check_linux(stage)
    else:
        raise SystemExit("Windows file associations are not installed until the deferred installer work (R27).")
    if arguments.command == "launch":
        require(os.environ.get("GITHUB_ACTIONS") == "true", "launch changes the desktop's association database; run it in CI")
        with tempfile.TemporaryDirectory() as directory:
            (launch_macos if sys.platform == "darwin" else launch_linux)(stage, Path(directory))


if __name__ == "__main__":
    main()
