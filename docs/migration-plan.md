# MoTrack migration plan

## Architecture

- **Project model.** Store clips, calibration, units, tracks, marks, and model definitions in plain C++ types. Keep the playhead, selection, and window layout in workspace state. Use stable track IDs and per-document undoable commands.
- **Analysis.** Keep coordinate transforms, derivatives, filters, tracking, and model evaluation callable without a GUI. Preserve Tracker behavior during the port. Changes that affect scientific results need explicit decisions and reference tests.
- **Media.** Use one frame source for playback, stepping, and analysis. Track the decoded frame ordinal, clip step, presentation timestamp, and any analysis timing override separately. For random access, seek to an appropriate keyframe and decode forward. Run decoding and auto-tracking off the UI thread with cancellation and a bounded cache.
- **Project I/O.** Read Tracker XML (`.trk`) and experiment archives (`.trz`) through an adapter that maps known legacy types, resolves relative and archive-local media, and reports missing or unsupported content. Validate legacy writes against the Java app before offering them.
- **Desktop UI.** Use Qt 6 Widgets for the main window, canvas, tables, and plot controls. Keep view state and drawing outside the analysis library. Evaluate native camera capture separately.
- **Familiar workflows.** Use Tracker's layout, tool names, and common interactions as the starting point. Change them where the existing workflow makes controls hard to find, unclear, or difficult to use. Record meaningful differences in the user guide.
- **Localization.** Route user-facing strings through Qt Linguist from the start. Use Tracker's locale catalogs as a terminology reference where their license permits reuse. Grow translated coverage with each feature and check layouts with longer labels and right-to-left languages.

The C++ port replaces Java-only dependencies with native equivalents. Qt supplies desktop widgets, XML parsing, and localization. FFmpeg is the planned video decoder. Port the OSP algorithms and behaviors needed for compatibility with reference tests. Choose additional libraries when a workflow calls for them.

## Delivery milestones

1. **Foundation.** Establish CMake presets, Clang/LLVM, GoogleTest, CTest, and native CI for Windows, macOS, and Linux. Add a small legacy `.trk` reader and fixtures so the first coverage gate exercises production code.
2. **First experiment workflow.** Open a representative legacy experiment, step forward and backward, calibrate distance, mark a point mass, undo an edit, plot position and velocity, and export CSV. Run the packaged app on all three platforms.
3. **Compatibility and feature parity.** Expand the corpus and add archive support, auto-tracking, measurement tools, particle models, and legacy project writes. Prioritize features based on the experiments and workflows they unlock.

## Verification

- Build and test with Clang on all three OSes. Choose minimum OS versions and CPU architectures before pinning the release toolchain.
- Follow TaskSmack's LLVM instrumentation and reporting approach. Combine coverage from the three platforms by source line and enforce a hard 90% minimum on handwritten production C++, including UI and platform code. Exclude generated files, dependencies, and test sources. Include all production targets in the report so untested code counts.
- Freeze a Java Tracker reference revision and record expected frame assignments, calibration, and exported measurements. Cover constant and variable frame rates, backward seeking, skipped clip steps, missing marks, rotated video, and missing media. Compare numerical results with explicit tolerances.
- Land the documentation bootstrap on `main`. Use pull requests and required build, test, and coverage checks after CI is in place.
- Before packaging, verify the license terms for Tracker and OSP code and assets carried forward, the chosen Qt modules, and the exact FFmpeg configuration.
