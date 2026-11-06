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

Install CMake 3.25 or later, Ninja, Clang, and Qt 6.8 with Linguist Tools. Set `CMAKE_PREFIX_PATH` to the Qt installation. On Windows, use an x64 Visual Studio Developer PowerShell with LLVM and the Qt `bin` directory on `PATH`. The configure step fetches GoogleTest.

```sh
cmake --workflow --preset dev
```

Run `build/dev/motrack` (`motrack.exe` on Windows) and open `tests/fixtures/point-mass.trk` from the File menu. The initial interface is available in English and Spanish.
