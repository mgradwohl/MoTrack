# MoTrack

MoTrack is a planned desktop application for measuring motion in video, plotting the results, and comparing measurements with mathematical models. It aims to bring workflows and existing experiments from [Open Source Physics Tracker](https://github.com/OpenSourcePhysics/tracker) to a C++ application for Windows, macOS, and Linux.

## Project status

MoTrack is in early development. The desktop prototype opens Tracker `.trk` files and displays saved point-mass marks in image coordinates. The first experiment milestone adds video stepping, distance calibration, manual marking, plotting, and CSV export. See the [migration plan](docs/migration-plan.md) for scope and acceptance criteria.

## Development

- C++23 and CMake, with Clang/LLVM on Windows, macOS, and Linux.
- Qt 6 Widgets for the desktop interface and FFmpeg for indexed video decoding.
- GoogleTest and CTest, with a 90% minimum line coverage gate for handwritten production C++ from the first code change.
- [TaskSmack](https://github.com/mgradwohl/tasksmack) is the reference for build presets, test organization, and LLVM coverage reporting.
- See [Contributing](CONTRIBUTING.md) for coding conventions and LLVM checks.

The project targets desktop workflows. Browser support is out of scope.

### Build the prototype

Run the setup script after cloning. It installs build tools through the platform package manager, downloads Qt into the checkout's ignored `.dev-tools` directory, and builds and tests the app.

**Windows x64** (PowerShell with WinGet available)

```powershell
.\tools\setup-dev.ps1 -Build
```

**Ubuntu 24.04 x64 or macOS** (Bash, with Homebrew on macOS)

```sh
./tools/setup-dev.sh --build
```

Run `build/dev/motrack` (`motrack.exe` on Windows) from an [activated developer shell](CONTRIBUTING.md#developer-setup) and open `tests/fixtures/point-mass.trk` from the File menu. The initial interface is available in English and Spanish.

## License

MoTrack is licensed under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE). Dependencies and any materials reused from Tracker retain their applicable notices and terms.
