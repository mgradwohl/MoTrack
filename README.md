# MoTrack

MoTrack is a planned desktop application for measuring motion in video, plotting the results, and comparing measurements with mathematical models. It aims to bring workflows and existing experiments from [Open Source Physics Tracker](https://github.com/OpenSourcePhysics/tracker) to a C++ application for Windows, macOS, and Linux.

## Project status

MoTrack is in early development. The desktop prototype opens local movies directly or imports Tracker `.trk` files with attached local movies and image sequences. It displays point-mass marks on the frame and in a table, draws imported Tracker vectors as arrows, imports saved image calibration, supports distance calibration and manual marking with undo, plots position and velocity, and exports the selected clip's measurements to CSV. See the [migration plan](docs/migration-plan.md) for scope and acceptance criteria.

## Development

The [full-port plan](docs/migration-plan.md) records requirements, current completion status, proposed
architecture, implementation dependencies, and the tests required before claiming Tracker parity.

- C++23 and CMake, with Clang/LLVM on Windows, macOS, and Linux.
- Qt 6 Widgets for the desktop interface, FFmpeg for video decoding, and libarchive for Tracker experiment archives.
- GoogleTest and CTest, with a 90% minimum line coverage gate for handwritten production C++ from the first code change.
- [TaskSmack](https://github.com/mgradwohl/tasksmack) is the reference for build presets, test organization, and LLVM coverage reporting.
- See [Contributing](CONTRIBUTING.md) for coding conventions and LLVM checks.

The project targets desktop workflows. Browser support is out of scope.

The application runs without Java or Swift installed. It uses native Qt, FFmpeg, libarchive, and
operating-system libraries. Java is used only in development tests against the original Tracker;
it is not shipped with MoTrack or used to read and write experiments.

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

Choose a point mass from the toolbar or create one in the Tracks menu. Shift-click an image to mark the selected point mass and advance one clip step. Shift+Enter repeats the previous mark. The Tracks menu also lets you rename a point mass, delete it, or remove its mark on the current frame. Use File → Export measurements to save the current marks as CSV, or File → Export plot data to save the selected series with its frame numbers, times when available, and units. Use File → Save MoTrack copy as to keep the current tracks, calibration, and media references in a `.motrack` project. Keep externally linked media in place when reopening it.

Open a Tracker `.trz` experiment archive from the same project chooser. If it contains several Tracker tabs, choose the one to open. Saving a `.motrack` copy embeds the complete original archive, including its media, other tabs and unknown files, so that copy can reopen after the original archive is removed. Only the selected tab is editable today.

Choose Tracks → New vector to create an independent vector track, or select an imported vector in the toolbar. Shift-click the tail, then Shift-click the tip to create or replace its arrow on the current frame and advance one clip step. Escape cancels a pending tail; switching frames, tracks, or tools also cancels it. Shift+Enter copies the preceding arrow to the current frame. Rename track, Delete track, Delete mark on this frame, and Undo/Redo work with vectors as well as point masses. Vector tails and components are saved in image pixels and survive reopening a `.motrack` project. Motion plots and measurement CSV export still cover point masses; dependent vector sums are not yet supported.

New Tracker imports also keep their original XML inside the saved `.motrack` file, including settings MoTrack cannot yet edit. Supported imported projects can also be saved back to standard Tracker files as described below; full interchange coverage remains unfinished. Standalone movies and images still use external files. Older native projects still open; archive-backed ones need their original `.trz` until resaved with this version. Data discarded by earlier versions cannot be recovered. See [native document compatibility](docs/native-format.md).

### Save a Tracker project and its MoTrack companion

For a supported imported experiment, choose **File → Save Tracker project…**. Save a local-image or
media-free project as `.trk`, or an imported image archive as `.trz`. MoTrack also writes a sibling
`.motrack` file with the same name. Tracker opens the standard file directly. Existing archives retain
other experiments and supporting files; the desktop edits only the selected experiment.

After choosing a Tracker destination, ordinary **Save** updates both files for the rest of that session.
**Save MoTrack copy as…** remains available; once paired saving is in use, a native-only copy does not
clear outstanding Tracker changes. After reopening a project, choose **Save Tracker project…** again
to select paired saving. MoTrack asks before replacing an unrelated existing companion.

Saving shows a cancelable progress window. The files are replaced separately, with the Tracker file
first. If interruption leaves one file unfinished, the next save offers to finish the recorded save and
then write your current edits. Changes made by another program are preserved and reported as a
conflict. Recovery never marks newer edits saved without writing them to both files.

This workflow currently covers audited media-free and local-image experiments, plus selected-member
updates to existing image archives. Movie export, filters and broader Tracker
settings are still being ported. An unsupported export is refused; save a MoTrack copy to retain work.
The [format guide](docs/native-format.md) records the exact supported edits and compatibility evidence.

### Export a portable image experiment

For a supported imported image experiment, choose **File → Export portable image archive…** and save
as `.trz`. MoTrack copies the experiment and its linked images into one file that can be moved and
opened in Tracker. The current project keeps its original save destinations, unsaved edits and undo
history; exporting a copy does not replace ordinary Save or create a companion automatically.

Export runs in the background with a Cancel button. Missing images are named in the error message,
and failures or cancellation before replacement keep an existing archive intact. This command currently
supports the audited local-image schemas described in the [format guide](docs/native-format.md#creating-a-portable-image-archive).
Movie packaging, arbitrary supporting documents and newly created standalone image projects remain unfinished.

### Analyze motion

Choose position, velocity, speed, acceleration, momentum, or kinetic energy above the plot. Set the selected point mass’s value in the Tracks menu. Select a plotted point to jump to its video frame. Enable Linear fit to see the slope, intercept, and R² for the plotted samples. Position uses the frame number when the project has no known timebase. Derived quantities use frame timestamps or saved image-sequence timing, and are left blank at the clip edges and across gaps in the marks. Imported movies are indexed in the background and keep Tracker’s saved clip start time.

Use Clip Settings to choose the starting frame, ending frame, and frame interval for an experiment. Existing marks keep their original frame numbers when the clip changes.

To start AutoTracker, return to a frame with a marked point mass and choose AutoTracker. It follows a small textured patch on later frames, stops at existing marks, and asks for a new manual mark when the match becomes uncertain.

To set a known distance, select Calibration Stick, click two distinct points in the displayed image, and enter their distance and unit. Undo restores the previous scale and unit.

Use Set Axes to place the coordinate origin and choose the positive x direction. The existing distance scale is preserved. Toggle the overlay from the View menu.

Use Tools → Measure distance or Measure angle to make a measurement on the current frame. The tools use calibrated coordinates when available and otherwise measure in image pixels.

Zoom from the View menu or hold Ctrl while scrolling over the video. Drag with the middle mouse button or hold Alt while dragging to pan. Fit video to view resets the display.

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
