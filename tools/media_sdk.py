"""Build MoTrack's pinned media and archive libraries into the ignored developer SDK."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1] / ".dev-tools" / "vcpkg"
VCPKG_RELEASE = "2026.10.16"
VCPKG_COMMIT = "9eec002df1cd109f8f104cd966a763daf9751489"
FFMPEG_FEATURES = "ffmpeg[core,avcodec,avformat,swscale]"
LIBARCHIVE_FEATURES = "libarchive[core]"


def triplet():
    machine = platform.machine().lower()
    if sys.platform == "win32" and machine in {"amd64", "x86_64"}:
        return "x64-windows"
    if sys.platform == "darwin" and machine in {"arm64", "x86_64"}:
        return "arm64-osx" if machine == "arm64" else "x64-osx"
    if sys.platform == "linux" and machine == "x86_64":
        return "x64-linux"
    raise SystemExit(f"Automatic FFmpeg setup is unavailable for {sys.platform}/{machine}.")


def bootstrap():
    binary = ROOT / ("vcpkg.exe" if sys.platform == "win32" else "vcpkg")
    if not (ROOT / ".git").is_dir():
        ROOT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "-c", "advice.detachedHead=false", "clone", "--depth", "1",
                        "--branch", VCPKG_RELEASE, "https://github.com/microsoft/vcpkg.git", ROOT],
                       check=True)
    revision = subprocess.check_output(["git", "-C", ROOT, "rev-parse", "HEAD"], text=True).strip()
    if revision != VCPKG_COMMIT:
        raise SystemExit(f"Unexpected vcpkg revision {revision}. Expected {VCPKG_COMMIT}.")
    if not binary.is_file():
        command = ["cmd.exe", "/d", "/c", "bootstrap-vcpkg.bat"] if sys.platform == "win32" else ["./bootstrap-vcpkg.sh"]
        subprocess.run([*command, "-disableMetrics"], cwd=ROOT, check=True)
    return binary


def install(target):
    binary = bootstrap()
    binary_cache = ROOT.with_name("vcpkg-binary-cache")
    binary_cache.mkdir(exist_ok=True)
    env = {**os.environ, "VCPKG_DEFAULT_BINARY_CACHE": str(binary_cache),
           "VCPKG_DOWNLOADS": str(ROOT / "downloads"), "VCPKG_DISABLE_METRICS": "1"}
    if sys.platform == "darwin":
        env["SDKROOT"] = subprocess.check_output(
            ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True
        ).strip()
        kept = env.get("VCPKG_KEEP_ENV_VARS", "")
        env["VCPKG_KEEP_ENV_VARS"] = ";".join(filter(None, (kept, "SDKROOT")))
    env.setdefault("VCPKG_MAX_CONCURRENCY", str(min(os.cpu_count() or 1, 8)))
    subprocess.run(
        [binary, "install", f"{FFMPEG_FEATURES}:{target}", f"{LIBARCHIVE_FEATURES}:{target}", "--classic"],
        env=env,
        check=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["bootstrap", "install", "prefix"])
    args = parser.parse_args()
    target = triplet()
    if args.command == "bootstrap":
        bootstrap()
    elif args.command == "install":
        install(target)
    else:
        print((ROOT / "installed" / target).as_posix())


if __name__ == "__main__":
    main()
