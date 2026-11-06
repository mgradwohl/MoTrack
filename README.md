# MoTrack

MoTrack is a planned desktop application for measuring motion in video, plotting the results, and comparing measurements with mathematical models. It aims to bring workflows and existing experiments from [Open Source Physics Tracker](https://github.com/OpenSourcePhysics/tracker) to a C++ application for Windows, macOS, and Linux.

## Project status

MoTrack is in planning. The first milestone will open a representative Tracker experiment, step through its video, calibrate distances, mark a point mass, and export measurements. See the [migration plan](docs/migration-plan.md) for scope and acceptance criteria.

## Development

- C++20 and CMake, with Clang/LLVM on Windows, macOS, and Linux.
- Qt 6 Widgets for the desktop interface and FFmpeg for indexed video decoding.
- GoogleTest and CTest, with a 90% minimum line coverage gate for handwritten production C++ from the first code change.
- [TaskSmack](https://github.com/mgradwohl/tasksmack) is the reference for build presets, test organization, and LLVM coverage reporting.

The project targets desktop workflows. Browser support is out of scope.
