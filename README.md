# MoTrack

MoTrack is a planned desktop application for measuring motion in video, plotting the results, and comparing measurements with mathematical models. It aims to bring workflows and existing experiments from [Open Source Physics Tracker](https://github.com/OpenSourcePhysics/tracker) to a C++ application for Windows, macOS, and Linux.

## Project status

MoTrack is in early development. The desktop prototype opens local movies directly or imports Tracker `.trk` files with attached local movies and image sequences. It displays point-mass marks on the frame and in a table, imports saved image calibration, supports distance calibration and manual marking with undo, plots position and velocity, and exports the selected clip's measurements to CSV. See the [migration plan](docs/migration-plan.md) for scope and acceptance criteria.

## Development

- C++23 and CMake, with Clang/LLVM on Windows, macOS, and Linux.
- Qt 6 Widgets for the desktop interface, FFmpeg for video decoding, and libarchive for Tracker experiment archives.
- GoogleTest and CTest, with a 90% minimum line coverage gate for handwritten production C++ from the first code change.
- [TaskSmack](https://github.com/mgradwohl/tasksmack) is the reference for build presets, test organization, and LLVM coverage reporting.
- See [Contributing](CONTRIBUTING.md) for coding conventions and LLVM checks.

The project targets desktop workflows. Browser support is out of scope.

### Build the prototype

Run the setup script after cloning. It installs build tools through the platform package manager, installs Qt, FFmpeg, and libarchive in the checkout's ignored `.dev-tools` directory, and builds and tests the app.

**Windows x64** (PowerShell with WinGet available)

```powershell
.\tools\setup-dev.ps1 -Build
```

**Ubuntu 24.04 x64 or macOS** (Bash, with Homebrew on macOS)

```sh
./tools/setup-dev.sh --build
```

Run `build/dev/motrack` (`motrack.exe` on Windows) from an [activated developer shell](CONTRIBUTING.md#developer-setup) and open `tests/fixtures/image-sequence.trk` or `tests/fixtures/movie.trk` from the File menu. Use the controls below the image to play, pause, or step through the clip. The interface follows the system locale and is available in English, French, German, and Spanish.

Directly opened movies use quarter-turn rotation and reflection metadata. Tracker project imports retain their encoded frame coordinates.

Use File → Open images to start from an image sequence. Enter its frame rate when known, or enter zero to analyze by frame number.

Choose a point mass from the toolbar or create one in the Tracks menu. Shift-click an image to mark the selected point mass and advance one clip step. Shift+Enter repeats the previous mark. The Tracks menu also lets you rename a point mass, delete it, or remove its mark on the current frame. Use File → Export measurements to save the current measurements as CSV. Use File → Save project to keep the current tracks, calibration, and media references in a MoTrack project. Keep the referenced media in place when reopening the saved project.

Open a Tracker `.trz` experiment archive from the same project chooser. If it contains several Tracker tabs, choose the one to open. A MoTrack project saved from an archive references that archive for its media.

Choose position, velocity, speed, or acceleration above the plot. Select a plotted point to jump to its video frame. Position uses the frame number when the project has no known timebase. Derived quantities use frame timestamps or saved image-sequence timing, and are left blank at the clip edges and across gaps in the marks.

Use Clip Settings to choose the starting frame, ending frame, and frame interval for an experiment. Existing marks keep their original frame numbers when the clip changes.

To start AutoTracker, return to a frame with a marked point mass and choose AutoTracker. It follows a small textured patch on later frames, stops at existing marks, and asks for a new manual mark when the match becomes uncertain.

To set a known distance, select Calibration Stick, click two distinct points in the displayed image, and enter their distance and unit. Undo restores the previous scale and unit.

Use Set Axes to place the coordinate origin and choose the positive x direction. The existing distance scale is preserved. Toggle the overlay from the View menu.

### Stage a local desktop build

From the activated developer shell, run:

```sh
cmake --preset stage
cmake --build --preset stage
cmake --install build/stage --prefix "$PWD/build/stage/installed"
```

Launch `motrack.app` from the install directory on macOS or run `bin/motrack` (`bin/motrack.exe` on Windows). This is a local deployment for development and smoke testing.

## License

MoTrack is licensed under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE). Dependencies and any materials reused from Tracker retain their applicable notices and terms.
