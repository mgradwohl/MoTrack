# MoTrack migration plan

## Architecture

- **Project model.** Store clips, calibration, units, tracks, marks, and model definitions in plain C++ types. Keep the playhead, selection, and window layout in workspace state. Use stable track IDs and per-document undoable commands.
- **Analysis.** Keep coordinate transforms, derivatives, filters, tracking, and model evaluation callable without a GUI. Preserve Tracker behavior during the port. Changes that affect scientific results need explicit decisions and reference tests.
- **Media.** Reuse a background decoder for playback and stepping, with cancellation and a bounded cache. Track decoded frame ordinals, clip steps, and presentation timestamps separately. Add indexed keyframe seeking for long movies and run larger automatic tracking searches off the UI thread.
- **Project I/O.** Read Tracker XML (`.trk`) and experiment archives (`.trz`) through an adapter that maps known legacy types, resolves relative and archive-local media, and reports missing or unsupported content. Saving a versioned MoTrack document alongside the standard Tracker file is acceptable; native persistence alone does not fulfill the required [round-trip workflow](#tracker-round-trip-compatibility). Validate legacy writes against the Java app before offering them.
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

## Tracker round-trip compatibility

The maintainer requires this workflow: open a Tracker experiment in MoTrack, edit it, save it, and
reopen the edited experiment in Tracker. A `.motrack` companion may be saved alongside the standard
`.trk` project or `.trz` archive. Tracker must not need the companion to read the standard file.

**Current status:** MoTrack imports a supported subset of Tracker data and saves that subset in its
native format. It does not yet write Tracker-compatible files or retain all unknown XML content.
Existing native documents therefore cannot be assumed to contain enough information for a lossless
Tracker export. Treat round-trip support as a compatibility milestone, not a completed capability.

Tracker's XML object format and ZIP experiment packaging are workable. The main implementation risk
is losing data or changing relationships when translating only the types MoTrack understands. The
[Tracker I/O source](https://github.com/mgradwohl/tracker/blob/SwingJS/src/org/opensourcephysics/cabrillo/tracker/TrackerIO.java)
is a reference for project saving and archive discovery; pin the actual tested desktop release and
its source revision in the compatibility corpus.

### Required behavior

- Preserve the original XML objects and unknown properties, including unsupported track types,
  models, filters, view settings, and references. Patch supported edits into the preserved document
  rather than reconstructing it solely from the reduced import model. XML whitespace need not be
  identical; the untouched data and its meaning must survive.
- Keep stable associations between editable tracks and their original objects, including duplicate
  track names, reordering, rename, deletion, and undo/redo. Edits affecting dependent or unsupported
  objects must update their references correctly or report that the edit cannot be exported safely.
  Retaining an XML node alone does not prove that its relationships remain valid.
- Persist the source information required for later exports with the native project. Closing MoTrack
  and reopening the `.motrack` companion must not discard unknown content or require the original
  imported project to still exist. Media references remain explicit and relocatable.
- For `.trz`, retain all untouched project tabs, media, and supplemental members. Update the intended
  project and any required references without silently dropping other archive content. A `.trk` export
  must resolve its media from the chosen destination; a portable `.trz` must carry its required media.
- Treat paired saves as one user operation with explicit results for both files. Do not report full
  success or clear unsaved state if one required output fails. Stage and validate writes before
  replacement, preserve existing valid files on failure, and test partial-failure recovery. Do not
  imply that two separate file replacements are inherently atomic.
- Distinguish supported editing from preservation of unsupported content. Show concrete compatibility
  limits before an operation would lose data; never advertise a successful open as proof of fidelity.

### Corpus and acceptance tests

The existing small synthetic fixtures test importer behavior, not broad Tracker compatibility. Build
a versioned corpus with source URL, redistribution permission, checksum, producing Tracker version,
features exercised, and expected values. Keep tiny synthetic edge cases in ordinary CI and provide a
reproducible fetch/check process for larger real experiments where redistribution is not appropriate.

For each supported workflow, load the original in the pinned Java Tracker, open and edit it in
MoTrack, save both formats, close MoTrack, reopen the native companion, and export again. Reopen each
standard output in Tracker and verify edited and untouched values: original frame numbers, clip time,
calibration, units, track samples, dependencies, media, and other archive tabs. Save again from Tracker
and check that MoTrack can reopen the result. Merely parsing XML or reading a ZIP is insufficient.

Cover point masses and independent vectors first, alongside unsupported objects that must survive.
Include multiple Tracker versions, variable frame rates, calibration keyframes, Unicode and relocated
paths, missing media, multiple archive tabs, unknown properties, and failed paired writes. Run the
corpus through the public import/save paths on all three platforms; record the tested Tracker versions
and known gaps instead of making an unqualified claim of compatibility with every Tracker file.
