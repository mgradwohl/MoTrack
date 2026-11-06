# MoTrack

MoTrack is a planned desktop application for measuring motion in video, plotting the results, and comparing measurements with mathematical models. It aims to bring workflows and existing experiments from [Open Source Physics Tracker](https://github.com/OpenSourcePhysics/tracker) to a C++ application for Windows, macOS, and Linux.

## Project status

MoTrack is in early development. The desktop prototype opens local movies directly or imports Tracker `.trk` files with attached local movies and image sequences. It displays point-mass marks on the frame and in a table, imports saved image calibration, supports distance calibration and manual marking with undo, plots position and velocity, and exports the selected clip's measurements to CSV. See the [migration plan](docs/migration-plan.md) for scope and acceptance criteria.

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

Run `build/dev/motrack` (`motrack.exe` on Windows) from an [activated developer shell](CONTRIBUTING.md#developer-setup) and open `tests/fixtures/image-sequence.trk` or `tests/fixtures/movie.trk` from the File menu. Use the frame controls below the image to step through the clip. The initial interface is available in English and Spanish.

Choose a point mass from the toolbar or create one in the Tracks menu. Shift-click an image to mark the selected point mass and advance one clip step. Shift+Enter repeats the previous mark. Use File → Export measurements to save the current measurements as CSV. Use File → Save project to keep the current tracks, calibration, and media references in a MoTrack project. Keep the referenced media in place when reopening the saved project.

Choose a quantity above the plot. Select a plotted point to jump to its video frame. Position uses the frame number when the project has no known timebase. Velocity uses frame timestamps from directly opened movies or saved constant image-sequence timing, and is left blank at the clip edges and across gaps in the marks.

To set a known distance, select Calibration Stick, click two distinct points in the displayed image, and enter their distance and unit. Undo restores the previous scale and unit.

## License

MoTrack is licensed under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE). Dependencies and any materials reused from Tracker retain their applicable notices and terms.
