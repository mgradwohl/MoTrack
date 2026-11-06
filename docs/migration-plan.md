# MoTrack migration plan

## Architecture

- **Project model.** Store clips, calibration, units, tracks, marks, and model definitions in plain C++ types. Keep the playhead, selection, and window layout in workspace state. Use stable track IDs and per-document undoable commands.
- **Analysis.** Keep coordinate transforms, derivatives, filters, tracking, and model evaluation callable without a GUI. Preserve Tracker behavior during the port. Changes that affect scientific results need explicit decisions and reference tests.
- **Media.** Reuse a background decoder for playback and stepping, with cancellation and a bounded cache. Track decoded frame ordinals, clip steps, and presentation timestamps separately. Add indexed keyframe seeking for long movies and run larger automatic tracking searches off the UI thread.
- **Project I/O.** Read Tracker XML (`.trk`) and experiment archives (`.trz`) through an adapter that maps known legacy types, resolves relative and archive-local media, and reports missing or unsupported content. Save supported edits in versioned MoTrack documents. Validate legacy writes against the Java app before offering them.
- **Desktop UI.** Use Qt 6 Widgets for the main window, canvas, tables, and plot controls. Keep view state and drawing outside the analysis library. Evaluate native camera capture separately.
- **Familiar workflows.** Use Tracker's layout, tool names, and common interactions as the starting point. Change them where the existing workflow makes controls hard to find, unclear, or difficult to use. Record meaningful differences in the user guide.
- **Localization.** Route user-facing strings through Qt Linguist from the start. Use Tracker's locale catalogs as a terminology reference where their license permits reuse. Grow translated coverage with each feature and check layouts with longer labels and right-to-left languages.

The C++ port uses Qt for desktop widgets, XML parsing, and localization, FFmpeg for video decoding, and libarchive for experiment archives. Port the OSP algorithms and behaviors needed for compatibility with reference tests.

## Delivery milestones

1. **Foundation.** Establish CMake presets, Clang/LLVM, GoogleTest, CTest, and native CI for Windows, macOS, and Linux. Add a small legacy `.trk` reader and fixtures so the first coverage gate exercises production code.
2. **First experiment workflow.** Open a representative legacy experiment, step forward and backward, calibrate distance, mark a point mass, undo an edit, plot position and velocity, and export CSV. Run the packaged app on all three platforms.
3. **Compatibility and feature parity.** Expand the experiment corpus. Archive import and template-based point-mass tracking have initial implementations. Continue with additional track and measurement tools, particle models, video filters, richer data analysis, multi-tab workspaces, and verified legacy project writes. Expand the language catalog and verify representative workflows across locales.

## Verification

- Build and test with Clang on all three OSes. Choose minimum OS versions and CPU architectures before pinning the release toolchain.
- Follow TaskSmack's LLVM instrumentation and reporting approach. Combine coverage from the three platforms by source line and enforce a hard 90% minimum on handwritten production C++, including UI and platform code. Exclude generated files, dependencies, and test sources. Include all production targets in the report so untested code counts.
- Freeze a Java Tracker reference revision and record expected frame assignments, calibration, and exported measurements. Cover constant and variable frame rates, backward seeking, skipped clip steps, missing marks, rotated video, and missing media. Compare numerical results with explicit tolerances.
- Land the documentation bootstrap on `main`. Use pull requests and required build, test, and coverage checks after CI is in place.
- Before packaging, verify the license terms for Tracker and OSP code and assets carried forward, the chosen Qt modules, and the exact FFmpeg configuration.
