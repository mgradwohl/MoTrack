# Upstream changes after Tracker 6.3.5

MoTrack's parity target tracks upstream `SwingJS` ([full-port plan](migration-plan.md#goal-and-completion-contract)).
This inventory, audited 2026-11-21 against the tip below, lists the desktop behavior and file-format changes made
after the 6.3.5 release. Each in-scope item (U*n*) and format change (F*n*) maps to requirement IDs; implement
and test it with the corresponding slice. Refresh this file when the reference is re-pinned. No upstream code was
built or run for the audit.

## 1. Scope, revisions and method

| Repo | Baseline | Compared tip | Commits after baseline |
| --- | --- | --- | --- |
| Tracker | `version_6.3.5` = `dc064f922fd924826fb42030b9cd5f5bbc720c9f` | `SwingJS` = `34b1b503f6cb55bee031d64ea916bf4f3262504d` (2026-11-20 "Update data table when dragging a marker.") | 297 (241 non-merge, 56 merges). The history is linear from the tag (merge-base = tag), with 142 files changed. |
| OSP | `d56207b0ae737458a31365d368f1c28e4bd3202a` (= OSP `version_6.3.5`) | `swingJS` = `d49c6ecf…` (2026-11-19) | 197 (171 non-merge, 26 merges). 306 files changed. |

**OSP pairing.** Neither repo has a submodule or pinned OSP jar. Tracker's `.classpath` points to the Eclipse sibling project `/osp`. `jarscripts/build-tracker-macosx-jar.xml` reads `${workspace}/osp/src` or `${workspace}/osp/bin`. The Tracker tip will only build against OSP **`swingJS`**, for three reasons:

- It uses `OSPRuntime.isCompact`, `getHTMLPageSize`, `saveFileSafely`, `org.opensourcephysics.js.AIPatch` and `javajs.api.JSUtilI`.
- None of these exist on OSP's default branch `master` (`37b7072`, 6 commits after `d56207b`, not merged into `swingJS`). I checked this directly.
- The prebuilt tip `tracker.jar` embeds `OSPRuntime.VERSION = 6.3.5-261118`, the value set by OSP `swingJS` `ddb5410a` on 2026-11-18.

**Method.**

- Made blobless clones and ran `git diff -w --ignore-cr-at-eol` per file. Large raw diffs in AutoTracker, TFrame, Tracker, PrefsDialog, TrackDataBuilder and TViewChooser are mostly CRLF and re-indentation noise. For example, AutoTracker is 9,139 changed lines raw but 5 lines whitespace-insensitive.
- Compared normalized line sets to separate moved code from real changes. TrackerPanel was reorganized: its constructors and `Loader` moved.
- Compared the sets of literal XML property keys used by `setValue/get*/getChildControl` in every changed loader, for both repos.
- Read the relevant source in both revisions.

**Evidence tags used below.**

- **[V]**: verified by reading the diff or source in both revisions.
- **[I]**: inferred from commit messages or from code paths I did not trace end to end, including runtime effects I did not execute.

**Plan cross-check.** `docs/migration-plan.md` names `020692a3` as "the source inventory reviewed for this plan", and its video-filter inventory link uses that same hash. `020692a3` is a **post-6.3.5 `SwingJS` commit** (2026-11-06, "Fixed bug in TToolBar"), not the 6.3.5 tag. The help file and filter registry are unchanged between the tag and the tip [V], so the filter inventory is unaffected. The wording should still say so explicitly once the baseline moves.

## 2. Counts per category

The primary category is per non-merge commit; the per-commit classification was kept with the audit notes, not in the repository. Several commits mix categories. File-format effects are tracked separately in section 4.

| Category | Tracker | OSP |
| --- | --- | --- |
| Desktop feature | 59 | 20 |
| Desktop bug fix | 23 | 16 |
| File-format/XML (primary) | 2 | 1 |
| SwingJS/browser-only | 79 | 62 |
| Build/infra/assets (incl. pure refactors, logging, jars, version bumps) | 66 | 69 |
| Unclear | 12 | 3 |
| **Total** | **241** | **171** |

Commits with any file-format effect: Tracker `b08f34ab`, `0fc26bee`, `73603b6a` (and the follow-on JavaCV commits), `28421616`, `d0bbcaf2`, `b0e9b68f`; OSP `ce0aad60`, `82b07d15`, `c10b192f`, `29c69c05`, `f5462c3a` (net none).

## 3. In-scope items (desktop behavior added or fixed after 6.3.5)

The impact column says whether an item **changes** an existing requirement (Δ), adds a **new sub-requirement** (+), or supplies a **regression case** (RC).

| # | Item | Commits | Classes / files | Category | R-IDs | Impact |
| --- | --- | --- | --- | --- | --- | --- |
| U1 | **Single-view ("maximized") layout, now persisted.** `TrackerPanel` saves `maximized_view` and `divider_fractions`. `TrackerIO.save()` no longer calls `restoreViews()` first, so a maximized layout is saved as-is. View menu has **Show Single View** (Main, Window 1–4), **Show Split Views**, and a "show another view" step button. Creating a track while a non-main view is maximized switches to the main view. [V] | T: `11e17565` `0fc26bee` `b08f34ab` `98bca4ea` `1a0344e2` `8e622e93` `b5c03ad1` `c5fdf3e5` `583493e6` | TrackerPanel(.Loader), TFrame (`maximizeView`, `restoreViews`, `saveCurrentDividerLocations`), TViewChooser, TMenuBar, TTrackBar, TrackerIO | File-format + desktop feature | R04, R21 (+), R02/R03 (preserve) | + persisted layout sub-requirement; RC for preservation of the new keys |
| U2 | **JavaCV video engine** (JavaCV 1.5.14, FFmpeg 8.1.2, OpenCV 4.14.0). Covers `CVVideo`, `CVVideoType` (reads avi/flv/mov/mp4/wmv; records mp4), `CVVideoRecorder`, `CVDiagnostics` and `CVThumbnailTool`. `MovieFactory` **prefers CV when its diagnostics pass and falls back to Xuggle otherwise** [V]. Adds Help ▸ Diagnostics ▸ About JavaCV, the "(JavaCV)" type in the Properties dialog, and `VideoIO.preferredVideoEngine` ordering. CV frame times are uniform: `t_i = i·duration/frameCount`, rounded to µs. The background runner that would collect real grabber timestamps is constructed but never started (its `AsyncSwingWorker` is commented out) [V]. The recorder hard-codes **H.264 MP4 at 30 fps**, ignoring the export frame duration [V]. | T: `73603b6a` `fc7cb2e0` `da72ac56` `9f51aba9` `f0dd5bf2` `dcc486ed` `bb252baf` (+ build `6ec1babf` `e85b66d1` `058bcb34`); O: `eed617fa` `2a63bff9` `27a09282` `56752bcb` `331e500d` `592539b5` | `media/cv/*`, MovieFactory, MovieVideo, VideoIO, PropertiesDialog, Tracker | Desktop feature + file-format | R05 (Δ timing semantics per engine), R23 (Δ export timing), R02/R03 (+ new video class), R27 (dependency policy) | + new engine/class acceptance; RC: variable-frame-rate timing and exported fps (both upstream bug candidates) |
| U3 | **MovieVideo `start_times` omitted when null.** In 6.3.5, `setValue("start_times", null, 3)` wraps a null array in `WrappedArray`, whose `toString()` dereferences `val.length`, so the save would throw an NPE. The tip skips the property. `CVVideo.Loader` explicitly removes `start_times`. [V] | O: `ce0aad60`; T: `73603b6a` (CVVideo.Loader) | MovieVideo.Loader, CVVideo.Loader, XMLProperty.WrappedArray | Desktop bug fix + file-format | R03, R05 | RC (reader must accept a movie object without `start_times`) |
| U4 | **Desktop camera capture** (Video ▸ Capture…). Starts a localhost `HttpServer` on `127.0.0.1` with an ephemeral port and opens the system browser to `/capture` (`ES6/tracker-capture.html`, `tracker-mp4-import.js`). It receives either an MP4 (imported using its encoded timing) or a JPEG image stack (written to temp files, imported at the chosen frame rate) into the current tab. [V] | T: `3ed7cc93` `e635c5a2` `dda3a891` `acc47e86` `24ea5da6` `441199ac` `f8088960` `a694deeb`; O: `33394dc8` (ImageVideo frame-count limit) | TrackerCamera, TrackerCameraServer, TActions `captureVideo`, Tracker `importVideoCapture`/`importMP4Capture`, TFrame.loadVideo | Desktop feature | R05 (+ media acquisition); no current R-ID fits exactly, nearest R05/R23; security review under R26/R27 | + new sub-requirement (or an explicit scope decision) |
| U5 | **Compact trackbar is now the desktop default.** `OSPRuntime.isCompact = true`, and `isCompact(c) = isSmallScreen(c) \|\| isCompact` [V]. Track and point numeric fields move into floating `NumberFieldPanel`s opened from trackbar toggle buttons. View ▸ Compact Trackbar toggles the mode, but the setting is not persisted (pref keys unchanged) [V]. | T: `286596d6` `a8f674e5` `7485bfc4` `cc937e49` `86121fd7` `e10adffe` `97a3922c` `dfca1215` `dfca65e8` `36970117` `5129f7e6` `ea354a52` `c66cf091` `9c348b71` `5907958f` `a1785a1d` `dc8b8f00` `ec54a18d` `575d84a3` `035af68b`; O: `10506416` `64f1a76d` | TTrackBar.Compact, NumberFieldPanel, TTrack (field classes), most track classes' toolbar components | Desktop feature (default UI change) | R06, R09, R10, R21, R25 | Δ "familiar Tracker workflow" baseline; needs a maintainer decision |
| U6 | **Crosshair marking tool** (Track ▸ Crosshair). A magnifier dial with marker button, 1-px nudge buttons and zoom control. It can mark point masses, vectors, calibration sticks, line profiles and RGB polygons. Not drawn in the world view. Shown automatically when there is no keyboard. Not persisted. [V] | T: `880caec0` `699ab9dd` `5d338049` `de0b7ad5` `5ec1f136` `50d39015` `005c3df1` `840959a4` `df09feca` `56752e5b` `e6972ce4` `181b9c75` `159c2e65` `610554c8` | Crosshair (new), TrackerPanel (interactive order, marking), TMouseHandler, TMenuBar | Desktop feature | R06 (+), R07, R09, R13 | + sub-requirement (marking aid) |
| U7 | **Tape measure / calibration stick numeric-entry fixes.** New `commitMagField`/`commitAngleField` reject NaN and ≤0 values and compare against the current value. The angle fix refreshes the key-step angle via `getTapeLength()` before applying the new angle. A zero length with distinct ends is recomputed. Fixed-position tapes propagate ends, length and angle to all steps, and `refreshStep` takes length from step 0 for fixed-position tapes. The input-field commit was reworked. [V diff; numerical effect I] | T: `723da762` `71b5b5c2` `dd0d153c` `e26da4d8` (+ guards `e10adffe` `97a3922c`) | TapeMeasure, TapeStep | Desktop bug fix | R09, R10 | RC: stick/tape length and angle entry, fixed-position propagation, scale result |
| U8 | **Hit-test tolerance.** `Step.hitRect` grows from 8×8 to 24×24 px. The axes x-axis hit path now starts at +25 px (was 15), and the simple-axes tick moves to 25. [V] | T: `29ed1983` | Step, CoordAxesStep, PointShapeFootprint | Desktop feature (selection behavior) | R06, R09 | Δ selection tolerance |
| U9 | **Table/data refresh and selection during drag.** `PositionStep` fires `PROPERTY_ADJUSTING` before the step-changed event. Mouse release clears adjusting on every selected step. Table popup ▸ Go to step now stops playback, steps there, selects the step's default point and shows its coordinates; it is cancelable, has a 10 s timeout and rejects stale requests. The table popup checks row/column bounds. [V] | T: `34b1b503` `351ff1ab` (`f71dbd6d` partly) | PositionStep, TMouseHandler, TableTrackView | Desktop bug fix + feature | R15, R06 | RC (table values after multi-select drag); + go-to-step selection |
| U10 | **Angle-unit popup** (degrees/radians on angle fields) now calls `TrackerPanel.setAnglesInRadians()`. That fires `PROPERTY_TRACKERPANEL_RADIANANGLES` and sets `changed`. In 6.3.5 the popup assigned the field directly, with no refresh and no dirty flag. [V] | T: `286596d6` | TTrack, Protractor | Desktop bug fix | R09, R25, R26 (dirty state) | RC |
| U11 | **Paste filter validation.** Video ▸ Paste filter now rejects clipboard XML that fails to read or is not a `Filter` subclass. In 6.3.5 it cast and loaded blindly. [V] | T: `ead10cff` | TMenuBar | Desktop bug fix | R12 | RC |
| U12 | **RGB region polygon editing.** Vertex-removal bounds fix (`vertex+2`). The graphics composite is restored after filling an open polygon. The vertex handle is selected after adding or removing a vertex. Toolbar components are split into track-level and point-level. Polygons can be marked with the crosshair. [V] | T: `50d39015` `cc937e49` | RGBRegion, RGBStep | Desktop bug fix | R13 | RC |
| U13 | **Robustness fixes.** PencilDrawing null-arrowhead NPE; `TTrack.delete` with a null panel; TapeStep layout/bounds null; TapeMeasure rotator shapes null; DynamicSystem toolbar mass refresh; CircleFitter data-point selection suppresses the step-edit trigger and resets its popup; Calibration reuses its axis dropdown. [V] | T: `286596d6` `dfca65e8` `e10adffe` `71b5b5c2` `97a3922c` | PencilDrawing, TTrack, TapeStep, TapeMeasure, DynamicSystem, CircleFitter, Calibration | Desktop bug fix | R10, R19, R21, R09 | RC (small) |
| U14 | **TRZ export with an exported video.** 6.3.5 copied the exported video to `new File(tempDir, videoTarget)`, where `videoTarget` is already an absolute temp path, then zipped that copy. The tip zips the real file. The zip goes through `BrowserZipExport.compress`, which on desktop calls `JarTool.compress` unchanged (`saveFileSafely` is a no-op off JS) [V]. In 6.3.5 the video member was probably missing (Windows invalid path) or nested under a bogus path (Unix) [I]. | T: `b0e9b68f` (`d0bbcaf2` `0b40fec1` `51d2efda`) | ExportZipDialog, TrackerIO.BrowserZipExport | Desktop bug fix (packaging) | R03, R23 | RC (exported-video member present and correctly named) |
| U15 | **Image-sequence export.** The `.xml` sidecar is now written directly: class `ImageVideo`, `paths`, `path`, `delta_t`, basepath. 6.3.5 instead reopened the exported images as an `ImageVideo` and serialized that. Zipped-image export now builds the archive from scratch frames with numbered entry names and errors on missing or empty frames. Video saves are wrapped in `BrowserZipExport.saveVideo`. [V] | O: `82b07d15` `c10b192f` `1a0d5d38`; T: `d0bbcaf2` | ImageVideoRecorder, ExportVideoDialog | Desktop bug fix (file content equivalent) | R23, R05 | RC (sidecar properties, entry naming) |
| U16 | **Version strings.** Tip builds write `semantic_version` = `OSPRuntime.VERSION` = `6.3.5.YYMMDD` (betas through 2026-10-26) or `6.3.5-YYMMDD` (current `6.3.5-261118`). `compareVersions` now splits on `[.-]`, and TrackerStarter/TrackerJarFilter do the same. 6.3.5's parser truncates the third field to one digit, so it also reads these as 6.3.5 and shows no "newer version" warning. [V] | T: `28421616`; O: `29c69c05` plus version bumps | Tracker.compareVersions, TrackerStarter, TrackerJarFilter, OSPRuntime | File-format (value) + desktop bug fix | R02, R03 | RC: accept non-numeric/dated `semantic_version` |
| U17 | **StepperClipControl.** When the video is already on the requested frame but the step number differs, the tip updates `stepNumber` and fires `PROPERTY_CLIPCONTROL_STEPNUMBER`. In 6.3.5 it only set `stepDisplayed`, leaving a stale step. [V; user-visible effect I] | O: `263298ab` | StepperClipControl | Desktop bug fix | R05 | RC (step/frame sync for stepper clip control) |
| U18 | **Drag target reset.** `InteractivePanel` clears `iaDraggable` on every press, so the press is hit-tested again rather than reusing the previous drag target. TMouseHandler resolves the interactive on press. [V; effect I] | O: `99e4f306`; T: `7674d2cd` `f71dbd6d` | InteractivePanel, TMouseHandler | Desktop bug fix / unclear | R06 | RC |
| U19 | **NumberField changes.** `setValue` is no longer skipped while the field is invisible (`3ec498fe`), and the unit suffix is trimmed before parsing. **DatasetCurveFitter** parameter, RMS and shift/selected fields now subclass `media.core.NumberField` (`51fa5f6f`, whose message says "untested"), so parsing and display formatting follow the media NumberField sig-fig formatter rather than the fitter's private DecimalFormat. [V code; display/parse differences I] | O: `51fa5f6f` `094831b6` `3ec498fe` `60df3143` | NumberField, DatasetCurveFitter, DataToolTab | Unclear / desktop bug fix | R17, R25 | RC: fit-parameter formatting/parsing (re-verify against reference) |
| U20 | **Library Browser.** Search is rewritten with an RPN boolean grammar: AND/OR/NOT/&/\|/!, parentheses, quotes and `a !b`. A syntax error aborts the search, and the result node is named `'phrase' (N)` in lower case. Also fixed: missing search results, concurrent modification in the HTML display worker, and the "Open" button being disabled (`showInfoEnabled` now defaults to true). ResourceLoader no longer recurses when downloading zip URLs (`faefa8f0`). Downloaded library paths keep spaces instead of `%20` (T `a6fddcc0`). Adds `LibraryHistory` and `Library.ListMap`. `LibraryCollection` `resources`: after a temporary regression the tip writes an array again and reads array or ArrayList, so there is **no net format change** vs 6.3.5. [V] | O: `9ab6ce8c` `9b70b54e` `b8c5eddf` `70208888` `4112a20c` `805376ee` `91691367` `faefa8f0` `f5462c3a` `c0f53603`; T: `a6fddcc0` | LibraryBrowser, LibraryTreePanel, LibraryCollection, LibraryResource, ResourceLoader, TrackerIO | Desktop feature + bug fix | R24 | Δ search semantics; RC |
| U21 | **Desktop menu/UI reorganization.** [V] | T: `785a73e7` `13efda4f` `e7cbd112` `1903c696` `e473a3e5` `35ed8a57` `97681509` `43a513b6` `df2d0f42` | TMenuBar, TToolBar, Tracker, `tracker.properties` | Desktop feature | R21, R25, R26 | Δ menu inventory (phase-1 menu audit) |
| U22 | **Filipino catalog.** `tracker_fil_PH.properties` (1,783 keys) was added, but `"fil"` is not in `Tracker.getLocales()` [V]. It is therefore not selectable from the language menu and is reachable only through the default-locale bundle lookup [I]. | T: `bad5a689` | resources | Unclear | R25 | Possible + (baseline catalog list) |
| U23 | **Visual defaults.** [V] | O: `27a09282` `853d3b30` `d1ea5703` `0373bc57` `b96e4e6d` `49644f1e`; T: `785a73e7` | TextLine, ResizableIcon, VideoPlayer, OSPRuntime, Tracker | Desktop feature | R21, R25 | Δ minor |
| U24 | **Window chrome.** `FullFrameDragHandler` makes empty toolbar, viewport and tab areas (TFrame, Library Browser) drag the whole window. Panning the TrackerPanel inside the viewport without a video is restored (`acda4fa7`). [V] | T: `6c38e5a8` `acda4fa7` `d0d9e7f9` `32b43efc` `69e2b1e7` `486af0b0`; O: `2cfeae91` | GUIUtils.FullFrameDragHandler, TrackerPanel, TMouseHandler | Unclear (`acda4fa7` = desktop bug fix) | R21 (panning) | Recommend porting only `acda4fa7`; the window drag duplicates the native window manager |

U21 details:

- Font Size is now added to the View menu. Edit still adds it too, but it is a single `JMenu` instance, so whichever menu is rebuilt last holds it.
- The Track Control menu item was removed from both the View and Track menus; Track Control is now toolbar-only.
- The Track menu gains Crosshair and a Select submenu. The View menu gains a Tab submenu plus **Mobile Layout** and **Compact Trackbar** check items.
- File ▸ Save Video As was removed from the menu (commented out).
- Label renames: "Import…"→"Open…" (video), "Plot View"→"Plot", "Restore Views"→"Show Split Views", "Maximize this view"→"Show single view", "angle from horizontal/x-axis"→"angle", Overflow "Maximize"→"Full Screen". New keys are English-only.
- Opening a Recent entry whose file no longer exists now beeps and does nothing.
- Preferences are written with `writeNoSearch`, so the prefs directory is no longer added to the resource search path.

U23 details:

- The default `TextLine` font changes from TimesRoman to SansSerif. This affects plot and label text.
- `ResizableIcon` scales by non-integer font factors.
- `VideoPlayer` drops the rate spinner and loop button when the frame is narrower than 500 px × font factor.
- A "small screen" (frame width < 500 px × font factor) forces compact mode, maximizes the main view at startup, and suppresses the default views when the first track is created.

**Debug noise (no functional change)** [V]:

- `TTrack.setLocked` prints to stderr.
- `TableTrackView` prints every single-cell selection via `TTrack.notifyTableCellSelected`.
- `TrackerIO.loadTRK` prints progress.
- `OSPRuntime.beep(msg)` now also prints the message to stderr before beeping.

Reference-harness log comparisons should tolerate these lines.

**Verified desktop no-ops** (commit messages suggest a desktop change, but the code is JS-gated) [V]:

- Data Tool and Data Builder mutual exclusion ("only one tool window"): `TFrame.allowOnlyOneTool = OSPRuntime.isJS`.
- Closing the Library Browser after opening a project: `closeLibraryBrowser()` is gated on `isJS`.
- The frame maximize toolbar button needs `TFrame.isLayoutAdaptive`, which is set only in `jsInitAdaptive`.
- Mobile layout applies only when `OSPRuntime.isMobile()` is true. On desktop that requires choosing View ▸ Mobile Layout; I classified it as SwingJS-only (see the open question below).
- The resize handler's `setupResizer` returns early when not JS.
- The `getStackPaths` refactor and the `MovieVideo.setStartTimes` change and revert have no net change.

**Maintainer decisions (2026-11-21).**

1. Mobile Layout stays outside desktop scope (browser/mobile presentation), even though the desktop View menu can reach it.
2. U5: MoTrack defaults to the compact trackbar with a View toggle, and remembers the choice in preferences (upstream does not).
3. U4: camera capture is in scope, implemented natively (Qt Multimedia preview and recording into a new experiment), without a localhost server or browser page.
4. U2: JavaCV's uniform frame times and fixed 30 fps export are upstream bugs. MoTrack uses real timestamps and the requested export timing, with independent expected values, and records the difference.

## 4. File-format / XML changes

I compared the sets of literal property names in every changed `XML.ObjectLoader` (Tracker and OSP) [V]. Only the items below differ. Everything else is unchanged:

- Every other track, step, coordinate, view, toolbar, filter and VideoClip loader key.
- The filter registry (16 filters, same order).
- `XMLControlElement` serialization. The only changes are the new `writeNoSearch` and removal of a debug `System.out.println` in `arrayValue` that printed every array string during load in 6.3.5.
- Tracker preference keys.

| # | Class / property | Written when | Differs from 6.3.5 output? | 6.3.5 reading tip output | MoTrack action |
| --- | --- | --- | --- | --- | --- |
| F1 | `TrackerPanel`: `maximized_view` (int, `TView.VIEW_UNSET`=-1 … `VIEW_MAIN`=4: 0 plot, 1 table, 2 world, 3 page, 4 main) and `divider_fractions` (`double[4]`, restore fractions). Read back only when loading completes (`progress == PROGRESS_COMPLETE`). | Only when a single view is maximized at save time. In that case `dividers` holds the *maximized* divider positions. | **Yes**: new properties. 6.3.5 could never save a maximized layout because `TrackerIO.save` restored views first. | Unknown properties are ignored. The file opens with collapsed panes (from `dividers`) and no "restore" state [I]. | Preserve the keys in the source envelope (covered by unknown-content retention). When R04/R21 layout persistence is implemented, read and write them. Add a corpus case saved from a tip build. |
| F2 | Video object class `org.opensourcephysics.media.cv.CVVideo` (inside `VideoClip.video`). Properties come from `VideoAdapter.Loader` + `MovieVideo.Loader`: `path` etc., `duration`, `frame_count`, `frame_rate`, `platform`="Java", with **no `start_times`**. | Whenever the JavaCV engine is active. That is the default when the JavaCV jars are present and diagnostics pass. | **Yes**: new class name; `start_times` absent. | `VideoClip.Loader` opens the child by `path` and extension and reads only `path`/`paths`/`absolutePath`/`filters`/`delta_t`; it does not instantiate the saved class. 6.3.5 would reopen with Xuggle [V by reading]. Java platform ignores saved timing anyway (`setFromControl` returns null for platform "Java") [V]. | MoTrack's audited writers require `…xuggle.XuggleVideo` (`src/tracker_writer.cpp` lines 168 and 1085, `src/tracker_rename.cpp` line 228). CV-saved projects will therefore be preserved but refused for edited export. Add an explicit decision and acceptance case for `CVVideo`, and for `JSMovieVideo` if desired. |
| F3 | `MovieVideo` `start_times`: omitted when `startTimesMS == null` (fixes a 6.3.5 save NPE). | Null start times; always for `CVVideo`. | Yes (absent vs. 6.3.5 crash). | Reader tolerates absence (`getObject` returns null) [V]. | Reader/oracle must accept a movie object without `start_times`. |
| F4 | `TrackerPanel` `semantic_version` value: `6.3.5.YYMMDD` or `6.3.5-YYMMDD`. | Every save by a tip build. | Yes (value only). | Parsed as 6.3.5 [V]. | Accept dated/suffixed versions; do not require numeric triples. |
| F5 | Exported image-sequence sidecar `.xml` (`ImageVideo`): `paths`, `path`, `delta_t`, basepath. Now always written directly instead of from a reopened video, so `filters` never appears. | Export ▸ Video as image sequence. | Equivalent properties, same order. `delta_t` now comes from the recorder's `frameDuration` rather than the reopened video [I]. | Same. | If MoTrack exports image sequences (R23), match these properties. |
| F6 | TRZ packaging with an exported video: the exported file is now included (see U14). | File ▸ Export ▸ TRZ with "export video". | Archive inventory differs from 6.3.5 [I]. | n/a | RC for R03/R23 packaging. |
| F7 | `LibraryCollection.resources`. | Saving collections. | **No net change** (array in both; tip also reads ArrayList). | Same. | None. |

## 5. Reference-build feasibility (desktop jar from the current tip)

**Feasible.** The steps are mechanical, but they have not been executed here and some risks are unverified.

**Prebuilt jar in the repo is not a pinnable reference.** At the tip, `distribution/tracker.jar` is:

- 10,693,757 bytes, SHA-256 `afc0caee1bcbd2796acb53ca936be7dd27ff17b0aaea1f1e2209c240754f7046`.
- Manifest: Build-Date Nov 19 2026; Created-By `25.0.3+9-LTS (Eclipse Adoptium)`; Ant 1.10.17; class file version 55 (Java 11 target); `OSPRuntime.VERSION` `6.3.5-261118`.
- The manifest Class-Path lists the JavaCV generic jars, the macOS arm64/x86_64 classifier jars, and `xuggle-xuggler-server-all.jar`.

It was committed in `13d8cbbb` and its entries are timestamped 16:04 on 2026-11-19. So it most likely corresponds to roughly Tracker `d19bf86c` plus OSP `31179833` [I]. It predates Tracker `610554c8`, `f71dbd6d`, `8e366258`, `035af68b`, `9a343784`, `e01469b9` and `34b1b503`, and OSP `99e4f306`, `263298ab`, `2bf0e3c7` and `d49c6ecf`. It was likely built from a developer working tree with no recorded source revision. Under the plan's provenance rules it is not acceptable as a pinned reference.

**Packaging script.** `jarscripts/build-tracker-jar.xml` was renamed to `build-tracker-macosx-jar.xml` in `058bcb34` (84% similar).

- `basedir=".\"` is Windows-style and `workspace = ../../`.
- The default target `tracker_jar` depends on `copy_bin_for_tracker`. That target copies precompiled Eclipse outputs `${workspace}/osp/bin` and `${workspace}/tracker/bin` into a temp dir, excluding test/demo/zip/xml/txt.
- It writes the manifest and jars `**/epsgraphics/ … **/media/, **/tracker/, **/ES6/, **/opensourcephysics/resources/` into `distribution/tracker.jar`.
- The script's `compile` target runs `javac` **without a classpath**, so it would fail on `media/xuggle` and `media/cv` [I]. A separate explicit compile step is therefore required.

**Inputs.**

1. Tracker `34b1b503f6cb55bee031d64ea916bf4f3262504d` and OSP `swingJS` `d49c6ecf` (use full blobs).
2. Tracker `libraries/`:
   - `xuggle-xuggler-server-all-5.7.0-20210422.000415-2.jar` (same 46,328,130-byte jar MoTrack already pins).
   - `slf4j`/`logback` (as now).
   - JavaCV 1.5.14 set: `javacv`, `javacpp`, `ffmpeg` 8.1.2-1.5.14, `opencv` 4.14.0-1.5.14, `openblas`, plus classifier jars for **macOS arm64/x86_64 and Windows x86_64 only**. There are **no Linux JavaCV natives** in the repo.
3. A JDK. Upstream used Temurin 25 with a Java 11 target. Temurin 17 (MoTrack's current reference JDK) or 21 with `javac --release 11` should work [I].
4. Optionally Ant 1.10.x.

**Steps** (do not run any upstream scripts other than reviewed ant/javac steps):

1. Lay out `W/tracker` and `W/osp` as siblings.
2. Compile OSP: `javac --release 11 -encoding UTF-8 -d W/osp/bin` over `W/osp/src/**/*.java`. The jar excludes `demo`, `demoJS`, `test`, `testing`, `csm`, `davidson` and `debugging`, so those can be skipped. Then copy the non-Java resources from both `W/osp/src` and `W/osp/resources` (the swingJS branch added a top-level `resources/org/opensourcephysics/resources/...` tree) into `bin`.
3. Compile Tracker: `javac --release 11 -cp W/osp/bin:W/tracker/libraries/*` with output to `W/tracker/bin`. Copy `src/**` resources, plus `W/tracker/resources/ES6/*`, which the camera server serves from the classpath.
4. Package: run `ant -f jarscripts/build-tracker-macosx-jar.xml tracker_jar` on Windows (because of the `.\` basedir), or an equivalent `jar` command with the same includes, excludes and manifest.
5. Run in the existing harness exactly as now, with an explicit classpath of jar + Xuggle + SLF4J and **no JavaCV jars**. `MovieFactory` then falls back to Xuggle [V], which keeps 6.3.5-equivalent movie timing and the `XuggleVideo` class name. A separate, explicitly selected JavaCV variant (Windows/macOS only) can characterize U2/F2.
6. Pin the bytes and SHA-256, record JDK/ECJ provenance, and re-run the corpus plus the 237 edited actual-Tracker cases.

Expected deltas on re-run:

- `semantic_version` string.
- Unknown `maximized_view`/`divider_fractions` only if a fixture maximizes a view.
- stderr debug lines.
- Possible UI-session differences from the compact trackbar default, if any case drives the trackbar.

**Unverified risks.**

- Upstream compiles with Eclipse (ECJ). Plain `javac` acceptance of the tip sources is untested; the `504055ad` "duplicate getText() method" incident shows the branch moves fast.
- The tip is an active beta branch with very frequent commits (2026-11-19/20). A reference should pin an exact pair rather than "latest".

## 6. Newer desktop releases

- **None.** The Tracker tags end at `version_6.3.5` [V, tag list via API]. The only GitHub Release is `version_6.1.6` ("Tracker Online JavaScript core", 2024).
- The official change log's newest entry is **Version 6.3.5 (released 22 Jun 2026)** (fetched 2026-11-21). It mentions no 6.3.6, 6.4, 6.5 or 7.0, beta, JavaCV, capture, compact trackbar or crosshair.
- Upstream treats the tip as **6.3.5 betas**. OSP commits titled "update beta version number and release date" stamp `6.3.5.260914` → `6.3.5-261118` [V].
- One build-script target names its jar `tracker-6.5.${date}.jar` (`tracker_jar_javacv`). That hints at a future 6.5 [I], but nothing is announced.

## Appendix A: Out-of-scope commits (brief)

**Tracker, SwingJS/browser-only (79).**

| Theme | Commits |
| --- | --- |
| Mobile/iPad layout, toolbar and menus | `0ca9a406` `f630484e` `51a6b796` `7dd3229f` `473163c7` `02691ee2` `2b68e6e3` `4b587f66` `5070a6d0` `ead04316` `3b6f964e` `04348687` `b1e0a18c` `2691b663` `f360c481` `6dd50728` `353bcbdf` `45bc5108` `fd164606` `4a3e2738` `756fbe71` |
| Online splash and HTML | `b3427b37` `04922947` `30760821` `8441ac39` |
| Online camera UI | `20ef47cb` `d3dc7232` `a694deeb` `5da82cc9` `1ae72f40` `b76f74ed` `5ff66d3a` `e1a90e44` `d34f47ad` |
| AIPatch / sticky menus / gestures | `95ba2ccf` `aa237663` `5180d0bb` `d2259e3a` `238db12a` `fe5c1247` `3984417d` `3ee87838` `84764e29` `de9695e6` `89450ae8` `3e7829f7` `0b40fec1` `a5c4972c` `a351f567` `a9a57b01` `29a4a2aa` `d4257946` `80dcbb47` `732414a5` `68ba3602` `2be3c972` `8b1cebf5` `f4d58405` `12728efb` `bb525f9f` `7723661a` |
| JS window/maximize, touch and repaint | `45bcdcb0` `4d0a5cce` `d3942581` `0cb3221e` `6c086fa2` `95496745` `ad09c882` `32746f93` `8e366258` `9a343784` `e01469b9` |
| JS one-tool-window policy | `1dd5d04a` `5a97bb77` `9ca7f940` `304285df` `5d569d2a` |
| JS file save / export | `51d2efda` `5cbb2dfd` |

**Tracker, build/infra/assets (66).**

| Theme | Commits |
| --- | --- |
| Jars and manifest | `9334c974` `88e9cbeb` `8f15bff3` `13d8cbbb` `058bcb34` |
| `tracker-assets.zip` and images | `ef9cf341` `807b0d15` `17f2a63d` `d39f25eb` `a9e5a84a` `33fa9396` `ec43ed29` `1c89b9eb` `f99fe149` `4630f4fb` |
| Classpath, project and `.j2s` | `6ec1babf` `e85b66d1` `8d0f8b86` `088fdace` `8121622c` |
| Class lists | `1b9af875` `12f69872` `74c3b0c2` |
| j2s annotations and refactors | `4c4917ae` `024b0d2e` `d9a328b1` `73c5f9fa` `d56b4f7c` `f94407b5` `19966c21` `ccc16f07` `c6474266` `c402b42f` `6e321d2f` `10698e8a` `b7b2ecb8` `d19bf86c` `2758716b` `baff7c05` `43a99a7e` `324f7300` `4b73a200` |
| Logging, comments and debug hooks | `27f46e8a` `19e11738` `1e2282a3` `0eb06412` `69363167` `07d2180e` `b7678b1e` `27067689` `c808c7a3` `e35f7400` `09a23998` `e1a09d36` `7c8ac460` `d933cb2d` `f25fbb75` `f43946a7` `e72f7684` `504055ad` |
| Analytics count reader | `70be500e` |
| Test sample `green_ss_laser.trk` | `cfda7612` |
| HTML spellcheck | `49502e39` |
| Shortcut-key revert | `ad876d40` |
| Resource strings | `97681509` |
| Help-event refactor | `26049940` |

**Tracker, unclear (12).** `7674d2cd` `6c38e5a8` `74b588d2` `38a274e1` `020692a3` `d0d9e7f9` `32b43efc` `69093b48` `89c3c911` `69e2b1e7` `486af0b0` `f71dbd6d`. These are touch-motivated input changes, window-drag chrome, welcome-screen logic later disabled, Data-window check sync, and a toolbar fix.

**OSP, SwingJS/browser-only (62).**

| Theme | Commits |
| --- | --- |
| `SwingJS-site.zip` / `j2s.core` updates | `7fbf82bb` `7b9c60ab` `90a56ea7` `f133d89e` `e4678a42` `eaefa2e8` `6a17ffd3` `233bd195` `442864da` `a7b14909` `3d6df20c` `a48e5507` `57314ce0` `22feccb2` `2576aeef` `5fbd5771` `8456e1b1` `a730dc0d` `31179833` `d49c6ecf` `de1f0ff3` |
| AIPatch and JSUtilI | `ca0c2e7a` `9c6ae183` `3e20e84e` `e6a6362d` `44970f20` `9c95956d` `a6251927` `dfb95d91` `24203235` `92c0c6e0` `2a753baa` `11f3c7cb` `ce39f0cd` `ecdc1e6d` `3c77962c` `6a3b63d4` `5e1e1dac` `d059b573` `c9850702` |
| Mobile flags | `094831b6` `722dd5b3` `b71f12c4` `c075b00c` |
| JS movie timing | `c91ce9c3` `84b9513b` |
| iPad CSS / InteractivePanel | `83d3c450` `6758bfe0` |
| JS Library Browser sizing and loadAsync | `43a4a7b0` `732eb08b` `b1530258` `163aced7` `61e020ba` `485cf73e` |
| JS resources and encoding | `3a3d174d` `4da43445` `f4c1d35a` `b646a579` |
| JS saves and paste | `587612fa` `c604e349` |
| JS VideoPlayer | `f98ccf16` `fbeee912` |

**OSP, build/infra (69).** Version bumps, EJS and `controls` cleanup, Library privatization and refactors, `@j2sNative`/`@j2sIgnore` annotation passes, test and demo files, Eclipse prefs, assets, the DataTool privatization (`bb34bf48`), hashCode additions, and import cleanup. `ec29f295` (MovieVideo `setStartTimes` change, then reverted) has no net change.

**OSP, unclear (3).** `51fa5f6f` (fitter NumberField swap, "untested"; see U19), `2cfeae91` (FullFrameDragHandler), `60df3143` (NumberField string property).
