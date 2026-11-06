"""Install the project-local Qt SDK and developer Python tools."""

import argparse
from pathlib import Path
import platform
import subprocess
import sys
import venv


ROOT = Path(__file__).resolve().parents[1]
QT_VERSION = "6.8.3"
AQT_VERSION = "3.3.0"
FORMAT_VERSION = "18.1.8"


def platform_config():
    machine = platform.machine().lower()
    if sys.platform == "win32" and machine in {"amd64", "x86_64"}:
        return "windows", "windows", "win64_msvc2022_64", "msvc2022_64"
    if sys.platform == "darwin" and machine in {"arm64", "x86_64"}:
        return "macos", "mac", "clang_64", "macos"
    if sys.platform == "linux" and machine == "x86_64":
        return "linux", "linux", "linux_gcc_64", "gcc_64"
    raise SystemExit(f"Automatic Qt setup is unavailable for {sys.platform}/{machine}.")


def install(root, host, archive, prefix):
    environment = root / "venv"
    python = environment / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
    formatter = python.with_name("clang-format.exe" if sys.platform == "win32" else "clang-format")
    if not python.is_file():
        venv.EnvBuilder(with_pip=True).create(environment)
    qt_config = prefix / "lib/cmake/Qt6/Qt6Config.cmake"
    if not qt_config.is_file() or not formatter.is_file():
        subprocess.run([python, "-m", "pip", "install", f"aqtinstall=={AQT_VERSION}",
                        f"clang-format=={FORMAT_VERSION}"], check=True)
    if not qt_config.is_file():
        subprocess.run([python, "-m", "aqt", "install-qt", host, "desktop", QT_VERSION,
                        archive, "-O", root / "qt"], cwd=root, check=True)
    if not qt_config.is_file():
        raise SystemExit(f"Qt installation is missing {qt_config}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["install", "qt-prefix", "tools-root"])
    args = parser.parse_args()
    name, host, archive, directory = platform_config()
    root = ROOT / ".dev-tools" / name
    prefix = root / "qt" / QT_VERSION / directory
    if args.command == "install":
        install(root, host, archive, prefix)
    elif args.command == "qt-prefix":
        print(prefix)
    else:
        print(root)


if __name__ == "__main__":
    main()
