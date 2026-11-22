# MoTrack full-port plan

This is the canonical scope, requirements, architecture, status, and execution order for completing
the native desktop port. It is not a claim that the port is complete. Contributor commands live in
[CONTRIBUTING.md](../CONTRIBUTING.md), agent workflow in [AGENTS.md](../AGENTS.md), and per-change
checks in [code-review.md](code-review.md). The [Tracker issue audit and triage](upstream-issues.md)
maps upstream reports to native issues, regression requirements and accepted/deferred feature
decisions. Consult it as each slice is ported.

**Status upkeep.** Keep this document concise. When a slice lands, update its requirement row, the
user-facing documentation and the merged-slice list with the PR link. Detailed validation evidence
(commands, case counts, coverage numbers, CI runs) belongs in the PR description, not here.

## Goal and completion contract

Deliver Tracker's desktop video-analysis and modeling workflows in a C++23 application on Windows,
macOS, and Linux. Users must be able to open existing experiments, perform the corresponding edits and
analysis, save standard Tracker files, and continue in Tracker. A `.motrack` companion alongside the
standard files is acceptable; it cannot substitute for a usable `.trk` or `.trz` output.

**Baseline.** On 2026-11-21 the maintainer set the target to the current upstream
[OpenSourcePhysics/tracker](https://github.com/OpenSourcePhysics/tracker) default branch (`SwingJS`),
tracked as it moves, rather than the frozen 6.3.5 release. Desktop behavior added or fixed upstream
after 6.3.5 is in scope; browser/SwingJS execution and experimental side branches are not.
The [maintainer's fork](https://github.com/mgradwohl/tracker) has no commits of its own; on 2026-11-21 it
trailed upstream `SwingJS` by 126 commits, so upstream is the tracked source. Tracker desktop **6.3.5** is
still the newest tagged desktop release. Its official JAR (SHA-256
`3b655f4370613afa5c4224c8e2e435d21a1f1fdcc28274937890feb90e1460d0`) remains the pinned *validation
executable* until a reference build of a newer upstream revision is pinned. The post-6.3.5 desktop and file-format changes are
inventoried in [upstream-delta.md](upstream-delta.md). Its candidate sources are the
`version_6.3.5` tags (Tracker [`dc064f9`](https://github.com/OpenSourcePhysics/tracker/tree/dc064f922fd924826fb42030b9cd5f5bbc720c9f),
OSP [`d56207b`](https://github.com/OpenSourcePhysics/osp/tree/d56207b0ae737458a31365d368f1c28e4bd3202a)); see the
audit checkpoint for the remaining build-provenance gap. Those upstream changes become requirements or regression cases here; each re-pin records its revision.

Full completion requires every requirement below to be implemented and verified on the supported
desktop platforms, or explicitly resolved by a maintainer-approved compatibility decision. A gap cannot
be removed from scope merely by preserving its XML, hiding its control, documenting it as unsupported,
or reaching the coverage threshold. Include the OSP services exposed by Tracker, such as Data Tool,
function/model builders, and the Library Browser; porting only Tracker's own Java classes is insufficient.

Replacing Swing, Java VM management, and Xuggle with native components is an implementation change,
not permission to omit the user workflows they support. MoTrack never requires Java: Java appears only
in development/reference validation. Record deliberate behavior changes, including corrections to
reference bugs.

**Maintainer decisions of 2026-11-21.** The compact trackbar is the default, as upstream, with a View toggle
that MoTrack remembers. Camera capture is in scope as a native Qt Multimedia workflow. Clear upstream bugs are
fixed rather than reproduced, with independent expected values and a recorded difference. Several independent
PRs on disjoint components may be open at once. Installers, signing/notarization, SBOM and release
publication (R27) are outside the current completion contract; local staging remains. Every UI locale
that Tracker ships is in scope: catalogs are seeded from Tracker's own translations where strings
correspond, remaining strings are machine-translated and marked `unfinished` for later human review.

**Maintainer decisions of 2026-11-22.** Machine-translated strings ship visible, without a label; they stay
`unfinished` in the catalogs for review, so builds must not pass `-nounfinished`. The interface keeps every
Tracker capability but may modernize its layout: menus, toolbars and panels can be reorganized where a
cleaner design helps, and the user guide maps each Tracker command to its MoTrack location. Where MoTrack
corrects a Tracker defect, the user guide lists the difference and an opened experiment shows a quiet,
non-blocking indicator (status or diagnostics, no pop-up) naming the corrections that apply to it.

## Current status

Snapshot **2026-11-21**, main at [`b22029b`](https://github.com/mgradwohl/MoTrack/commit/b22029b4e1bdf9eb5911d3a2ca53d3b779fb3edc)
(after [#165](https://github.com/mgradwohl/MoTrack/pull/165)). Each listed PR passed review and the
required checks in force at its merged revision; its description records the evidence. The edited
actual-Tracker round-trip suite has run on Linux CI for every writer PR since #45.

Implemented, tested workflows: local movie/image opening, playback and indexed stepping, clip limits,
a shared background decoder with a bounded cache, point-mass creation/marking/rename/deletion,
independent vector editing, undo/redo, distance calibration, axes, transient distance/angle tools,
position/velocity/speed/acceleration/momentum/energy plots, a straight-line fit, CSV export,
template-based AutoTracker off the UI thread, native save/reopen, audited standard `.trk`/`.trz` saving
with a paired `.motrack` companion and recovery, portable archive export, a tabbed multi-experiment
workspace and per-experiment Revert. English, French, German and Spanish catalogs exist.

| Area | Merged PRs |
| --- | --- |
| Foundation, vectors and reference corpus | [#34](https://github.com/mgradwohl/MoTrack/pull/34), [#36](https://github.com/mgradwohl/MoTrack/pull/36), [#38](https://github.com/mgradwohl/MoTrack/pull/38), [#47](https://github.com/mgradwohl/MoTrack/pull/47), [#151](https://github.com/mgradwohl/MoTrack/pull/151) |
| Source preservation, IDs, associations, archive payloads, sessions | [#37](https://github.com/mgradwohl/MoTrack/pull/37), [#40](https://github.com/mgradwohl/MoTrack/pull/40)–[#44](https://github.com/mgradwohl/MoTrack/pull/44) |
| Media-free standard writer: mass, marks, vectors, clip, calibration, units, timing | [#45](https://github.com/mgradwohl/MoTrack/pull/45), [#46](https://github.com/mgradwohl/MoTrack/pull/46), [#48](https://github.com/mgradwohl/MoTrack/pull/48)–[#54](https://github.com/mgradwohl/MoTrack/pull/54), [#108](https://github.com/mgradwohl/MoTrack/pull/108), [#110](https://github.com/mgradwohl/MoTrack/pull/110), [#112](https://github.com/mgradwohl/MoTrack/pull/112) |
| Source references, views and dependent-member indexing | [#55](https://github.com/mgradwohl/MoTrack/pull/55)–[#62](https://github.com/mgradwohl/MoTrack/pull/62), [#96](https://github.com/mgradwohl/MoTrack/pull/96)–[#99](https://github.com/mgradwohl/MoTrack/pull/99) |
| Renames, track creation, deletion and selection/plot/table cleanup | [#100](https://github.com/mgradwohl/MoTrack/pull/100)–[#104](https://github.com/mgradwohl/MoTrack/pull/104), [#107](https://github.com/mgradwohl/MoTrack/pull/107), [#113](https://github.com/mgradwohl/MoTrack/pull/113)–[#118](https://github.com/mgradwohl/MoTrack/pull/118) |
| Reference-harness reliability and bounded test groups | [#106](https://github.com/mgradwohl/MoTrack/pull/106), [#109](https://github.com/mgradwohl/MoTrack/pull/109), [#111](https://github.com/mgradwohl/MoTrack/pull/111), [#138](https://github.com/mgradwohl/MoTrack/pull/138) |
| Linked images, image archives, paired save and recovery, portable images | [#119](https://github.com/mgradwohl/MoTrack/pull/119), [#120](https://github.com/mgradwohl/MoTrack/pull/120), [#121](https://github.com/mgradwohl/MoTrack/pull/121), [#124](https://github.com/mgradwohl/MoTrack/pull/124)–[#128](https://github.com/mgradwohl/MoTrack/pull/128), [#130](https://github.com/mgradwohl/MoTrack/pull/130), [#134](https://github.com/mgradwohl/MoTrack/pull/134) |
| Movies: baseline, mass/marks, clip, MOV archives, packaging, calibration, authoring | [#132](https://github.com/mgradwohl/MoTrack/pull/132), [#133](https://github.com/mgradwohl/MoTrack/pull/133), [#136](https://github.com/mgradwohl/MoTrack/pull/136), [#139](https://github.com/mgradwohl/MoTrack/pull/139), [#141](https://github.com/mgradwohl/MoTrack/pull/141)–[#144](https://github.com/mgradwohl/MoTrack/pull/144) |
| Vectors on image/MOV media and varying calibration | [#145](https://github.com/mgradwohl/MoTrack/pull/145), [#147](https://github.com/mgradwohl/MoTrack/pull/147) |
| Multi-member archives, group persistence, save state and checkpoints | [#152](https://github.com/mgradwohl/MoTrack/pull/152), [#154](https://github.com/mgradwohl/MoTrack/pull/154)–[#156](https://github.com/mgradwohl/MoTrack/pull/156), [#158](https://github.com/mgradwohl/MoTrack/pull/158), [#159](https://github.com/mgradwohl/MoTrack/pull/159) |
| Workspace registry, shared services, bound editors, input preparation | [#160](https://github.com/mgradwohl/MoTrack/pull/160)–[#163](https://github.com/mgradwohl/MoTrack/pull/163) |
| Tabbed desktop shell and per-experiment Revert | [#164](https://github.com/mgradwohl/MoTrack/pull/164), [#165](https://github.com/mgradwohl/MoTrack/pull/165) |

The [corpus](../tests/corpus/README.md) holds 80 distinct pinned cases. Its actual-Tracker acceptance
suite (237 edited load/save/fresh-reopen cases plus native reimport) runs in CI on every PR. Known
Tracker 6.3.5 defects stay explicitly unaccepted with strict MoTrack guards and independent expected
values: vector-sum member loss ([#63](https://github.com/mgradwohl/MoTrack/issues/63)), unvisited
custom-table columns ([#64](https://github.com/mgradwohl/MoTrack/issues/64)), archived AVI extraction
([#140](https://github.com/mgradwohl/MoTrack/issues/140)) and negative-time vector rows
([#146](https://github.com/mgradwohl/MoTrack/issues/146)). See [upstream-issues.md](upstream-issues.md#defects-discovered-by-the-reference-harness).

Major current limits: standard writes cover only the audited schemas in the
[format guide](native-format.md); older native files cannot reconstruct lost source associations;
saved views, tabsets and cross-tab import are unfinished; the track/coordinate models are reduced;
AutoTracker and plotting are basic; there are no particle models, persistent measurement tools,
video filters, Data Tool, Library Browser or video export. The README describes available interactions.

### Phase 4 remaining work

1. **Saved views.** Persist and restore view layout, selection and playhead per experiment in the
   native workspace, and read/write Tracker's saved view state where Tracker stores it. See
   [shell ownership](workspace-sessions.md#desktop-workspace-shell).
2. **Tracker tabsets ([#150](https://github.com/mgradwohl/MoTrack/issues/150)).** TFrame-root `.trk`
   files of absolute/relative path pairs, distinct from a TrackerPanel experiment and a multi-member
   `.trz`. Cover relative resolution, relocation, missing/duplicate/cyclic references and actual Tracker reopen.
3. **Cross-tab import** of tracks and data between experiments.

Group acceptance includes edit A/save/close A/edit B/save/reopen both, reverse order, undo after
saving, original-input removal, native close/reopen, Save As, edits during saving, canceled/partial
commits and stale recovery. Two independent native companions do not establish shared group recovery.

### Reference inventory audit checkpoint

The 2026-11-09 audit examined the pinned 6.3.5 JAR (9,659,919 bytes, SHA-256
`3b655f4370613afa5c4224c8e2e435d21a1f1fdcc28274937890feb90e1460d0`) against the `version_6.3.5`
tags above. All 2,732 class SourceFile attributes map to 629 source paths; 783 of 788 nonclass
resources match exactly (four differ by line endings, one is the generated manifest); 32 UI locales,
14 menu filters, 335 resource keys, 45 action definitions and all 76 full-configuration flags agree.
This does **not** prove full source/binary equivalence: the tag's bundled `distribution/tracker.jar`
is an earlier May 27 build (SHA-256 `e1698b89083969a5bbefc7c88a025389dfe7e95613848d0c043e72be6e13459c`)
and the packaging target copies prebuilt `bin` outputs. The audit also found the tabset workflow
(#150) and moving reference frames in the real Diabolo archive ([#148](https://github.com/mgradwohl/MoTrack/issues/148)).
The tag's bundled JAR must not replace the tested reference merely because it is inside the tag. Constant-pool
presence and registration agreement do not establish live workflow coverage; dynamic menus and other
classes still need inventory work. Post-6.3.5 upstream changes need the same inventory before the baseline is re-pinned.

## Requirements and status matrix

**Implemented** means the explicitly described slice is in merged code with tests. **Partial** means
some of the complete requirement remains. **Missing** means there is no supported end-to-end workflow.
Phase numbers are former phase numbers, mapped to waves under the [implementation order](#implementation-order-and-exit-gates); they are not dates or percent completion. Each
requirement needs a source/reference case, public-entry-point test, persistence test when relevant,
and platform evidence before it can be marked complete.

| ID | Required desktop behavior | Status and remaining work | Phase |
| --- | --- | --- | --- |
| R01 | Reproducible native builds, CI, review, numerical tests, at least 90% combined C++ line coverage | Implemented foundation: three-OS CI, static analysis, coverage gate, corpus and actual-Tracker acceptance harness. Applies to every new component. | 0, every phase |
| R02 | Open `.trk`/`.trz`; preserve all supported and unknown content, references, metadata, and archive tabs | Partial: point masses, independent vectors and calibration import; original XML, persistent IDs and source associations; exact original archives (native v5) and multi-member groups (v6); every archive member opens as a lazy tab; a read-only reference index covers names, selections, views and dependent members. Table dataset rows have source-structure evidence only; media-backed application acceptance remains. Remaining: editing for every other track type (now preserved only) and tabsets (#150). | 1–3 |
| R03 | Save edited standard files plus optional `.motrack`; reopen in both applications without data loss | Partial: depending on the source schema, audited writers cover mass/marks, independent vectors, clip range/stride, media-free timing, fixed/partly fixed/varying calibration, unit labels, audited renames, track creation/removal and selection/plot/table cleanup. Routes are media-free, linked-image, image-archive, constant-rate linked-movie, MOV-archive, portable and multi-member archive, with paired save and recovery; the [format guide](native-format.md) defines which edits each schema and route accepts (for example, movie mass/time-label and image numeric-timing limits). Remaining: every feature below as it becomes editable, dataset-owner removal, broader movie containers and timing; #63/#64/#140 guards stay. | 2–3, every feature |
| R04 | Multiple experiment tabs, independent undo/dirty state, tab/project Save As, cross-tab import and reusable views | Partial: tabbed workspace with independent histories/playheads/jobs, lazy editors, reorder/hide/reopen, Save All, whole-source close decisions (#164) and per-experiment Revert (#165). Remaining: saved views/layouts, cross-tab import, tabsets (#150). | 2–4 |
| R05 | Movies, stills, image sequences and zipped sequences; accurate clip/time/frame controls and media properties | Partial: local movies/images, trim/stride, indexing, orientation, bounded caching, playback; audited constant-rate movies save. Remaining: variable-rate timing, zipped sequences, codec/container matrix (#73), broader movie packaging, media properties and time controls, numeric media timing edits. The synthetic AVI does not reproduce or resolve the historical MOV/Open Recent report in #65. | 1, 3–4, 6 |
| R06 | Complete point-mass editing and appearance: selection, dragging/nudging, numeric positions, multiselect, locks, labels, trails, paths, and vector overlays | Partial: manual marking/replacement, repeat, rename/delete, mass, undo, movie authoring. Remaining: the crosshair marking aid and hit tolerances ([upstream](upstream-delta.md) U6, U8), selection behavior, dragging/nudging, numeric entry, multiselect, locks, labels, trails/footprints/vector overlays, path length (#91). | 4–5 |
| R07 | Independent vectors, linked vectors, sums, their data, editing, and persistence | Partial: independent vectors import/display/edit/undo/save on all routes. Remaining: attached/linked vectors, vector-sum evaluation (#63), negative-time rows (#146), mixed-track snapping, full attachment/display semantics. | 3–5 |
| R08 | Center-of-mass tracks, constituents, dependent updates, and moving reference frames | Missing native evaluation/editing: membership is preserved and indexed, constituent renames export, and a Java reference case checks total mass and weighted positions. Moving reference frames: #148. | 2, 5 |
| R09 | Fixed or varying calibration, axes, calibration sticks/points, offset origins, attachments, coordinate locks, and L/M/T/angle units | Partial: calibration keys, stick, axes and every existing fixed/varying/partly fixed form save. Remaining: calibration points, offset origin, fixed-state toggles, key deletion, locks, attachments, units UI/defaults including angle units, reference frames. | 3–5 |
| R10 | Persistent tape measures, protractors, circle fitting, attachments, and their changing measurements | Partial: transient distance/angle tools only; source attachments are indexed and a three-tape Java reference exists. Remaining: persistent tools, geometry edits, attachments, round trips. | 5 |
| R11 | Full AutoTracker: masks, search areas, keyframes, target offsets, match scores, subpixel positions, evolution/tethering, look-ahead, line search, stop/retry/skip, and live/fast modes | Partial: fixed grayscale patch, bounded search, background operation, uncertainty stop. Reference algorithm and interaction parity remain (#71, #90). | 5–7 |
| R12 | Ordered, editable video filter stack with saved parameters and reproducible filtered frames | Missing; see the filter inventory below. | 6 |
| R13 | RGB region and line-profile tracks, shapes/widths, changing image statistics, spatial/time data, and persistence | Missing; define sampling relative to the filter pipeline first. | 6–7 |
| R14 | Complete motion data and derivative choices, gaps/interpolation, motion smoothing, filtered clones, and correct units | Partial: translational quantities with gap handling. Remaining: audit all variables/algorithms, angular quantities, interpolation, derivative options, Moving Average/Butterworth/Savitzky-Golay filtered clones. | 5, 8 |
| R15 | Plot/table parity: selectable axes/columns, synchronized plots, comparisons, ranges, selection, sorting, formatting, text columns, clipboard, and data refresh | Partial: one selected series and a basic marks table; saved table/plot owners, guests and formats are preserved, indexed and safely renamed/removed. Remaining: native plot/table editing and saved views, media-backed dataset behavior, #64, text columns (#80). | 4, 8, 10 |
| R16 | Data Builder expressions, parameters, custom variables, saved function sets, and autoloading | Missing; needs a compatible expression engine and dependency tracking (#74). | 8 |
| R17 | Data Tool and Fit Builder workflows: statistics, automatic/manual/custom fits, uncertainty, residuals, integration and Fourier analysis, saved analysis state | Partial: straight-line slope/intercept/R² only. Inventory the bundled OSP tool and match its exposed operations. | 1, 8 |
| R18 | Analytic/kinematic particle models, parameter editing/synchronization, initial conditions, model overlays, and stamping | Missing. | 8–9 |
| R19 | Cartesian/polar dynamic particles and interacting two-body systems, numerical solvers, launch conditions, and model data | Missing natively; boosters and system members are preserved/indexed with a Java reference case. Kernel K2 ports every OSP solver Tracker can instantiate ([ODE solvers](ode-solvers.md)), checked against the pinned JAR by a Java oracle; it corrects the reference's Adams6 predictor ([#178](https://github.com/mgradwohl/MoTrack/issues/178)) and backward multistep loop ([#179](https://github.com/mgradwohl/MoTrack/issues/179)). Native models, polar/interacting dynamics (including the two-body external-force defect, [#192](https://github.com/mgradwohl/MoTrack/issues/192)), model editing and broader export remain. | 8–9 |
| R20 | External multi-point models from text/clipboard and external modeling programs, time mapping, refresh/auto-paste, and stamping | Missing; use a tested native data adapter, never an embedded Java application. | 8–9 |
| R21 | Main/world/plot/table/page views, layouts, track controls, notes, drawings, labels, and supporting documents | Partial: main video, table, plot, zoom/pan and axes. Remaining: world/page views, annotations, notes, layout persistence including the saved single-view layout (upstream U1/F1), compact trackbar by default with a remembered toggle (upstream U5). | 4, 10 |
| R22 | Data export with track/column selection, formatting, raw precision, delimiters and clipboard; image/plot/print export | Partial: point-mass and selected-series CSV. Remaining: other data, graphics and print workflows (#81). | 8, 10 |
| R23 | Video/clip export and transcoding, image sequences, overlays, correct output timing, thumbnails and portable experiment packaging | Partial: portable archive packaging (R03). Video/image-sequence export missing. | 3, 6, 10 |
| R24 | Library Browser: local/remote collections, metadata/search, recent resources, cache management, downloads and authoring/sharing collection files | Missing; offline/cancel/error behavior and collection-format compatibility required (#92). | 11 |
| R25 | Preferences, shortcuts, accessible focus/labels, font/icon sizing, locale-aware numbers, all baseline language catalogs, searchable help and diagnostics | Partial: four UI languages, basic keyboard controls. Remaining: every Tracker locale, preferences (#75), RTL/accessibility, help, diagnostics, locale-aware input (#66, #67). | every phase, 10–12 |
| R26 | Robust desktop lifecycle: open/drop/file association, close/cancel, recovery, resource limits, responsive long tasks, and clean shutdown | Partial: worker/cancel/dirty-state, whole-source close and save recovery. Remaining: drop and file-association opening with spaces/Unicode paths (#68), OS file-type registration (including the staged macOS bundle's declared types), multi-document recovery, resource regressions (#72, #76). | 2–4, 12 |
| R27 | Installable releases, runtime deployment, signing/notarization, upgrades, notices, SBOM and provenance | **Deferred by maintainer decision (2026-11-21)**; local staging only. Installer issues #82, #84, #85, #93 and #95 wait for this decision to be revisited; the clean-system launch part of #83 (no Java or development SDK) stays in wave 4 (former phase 12). | outside current contract |
| R28 | Native camera capture: preview, record and open the recording as a new experiment with its real timing | Missing. Upstream's localhost-server/browser capture (U4) is replaced by a native Qt Multimedia workflow; no network listener. | wave 3 (I8) |

The reference inventory is grounded in Tracker's
[desktop feature overview](https://opensourcephysics.github.io/tracker-website/),
[point-mass help](https://opensourcephysics.github.io/tracker-website/help/pointmass.html),
[AutoTracker help](https://opensourcephysics.github.io/tracker-website/help/autotracker.html),
[coordinates](https://opensourcephysics.github.io/tracker-website/help/coords.html),
[tables](https://opensourcephysics.github.io/tracker-website/help/tableview.html),
[plots](https://opensourcephysics.github.io/tracker-website/help/plotview.html),
[models](https://opensourcephysics.github.io/tracker-website/help/particles.html),
[two-body systems](https://opensourcephysics.github.io/tracker-website/help/systems.html),
[external models](https://opensourcephysics.github.io/tracker-website/help/datatrack.html), and
[Library Browser](https://opensourcephysics.github.io/tracker-website/help/library_browser.html).
Slice A2 (former phase 1) reconciles every menu, track class, saved property, and bundled OSP tool against this matrix;
newly discovered baseline requirements extend it rather than disappearing between broad categories.
[Upstream changes after 6.3.5](upstream-delta.md) add sub-requirements and regression cases to these rows,
including the `CVVideo` class and movies without `start_times` (R05), dated `semantic_version` values
(R02/R03), single-view layout keys (R04/R21) and the upstream regression fixes U7–U19.

The [video-filter inventory](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/resources/help/video_filters.html)
(that revision is a post-6.3.5 `SwingJS` commit; the help file and filter registry are unchanged between the
6.3.5 tag and the current tip) includes brightness/contrast, highlight/shadow, deinterlace, strobe, ghost/dark ghost, grayscale,
negative, baseline subtraction, sum/average, perspective, radial distortion, resize, and rotate/reflect.
Parameters, ordering, enable/disable, copy/paste, temporal reset behavior, and saved state are part of
parity. Motion filters in R14 are a separate numerical-data pipeline.

## Architecture

Re-planned on 2026-11-21 after an architecture review: the reduced `LegacyProject` model plus per-field
"audited" XML patches and bespoke acceptance cases cannot scale to full parity. MoTrack instead mirrors how
Tracker itself persists objects, on top of an exact model of OSP's file format. Keep the existing C++23 /
Qt 6 Widgets / FFmpeg / libarchive choices. The shipped application never needs Java or Swift; native
C/C++ and operating-system libraries are allowed, and Java is confined to development/reference tests.

### Current code

The [CMake targets](../CMakeLists.txt) are `motrack_project`, `motrack_media`, `motrack_image_package`,
`motrack_input`, `motrack_workspace`, and the application. `motrack_project` stays independent of GUI and
decoder APIs. `DocumentSession` and `ArchiveGroupSession` own editable document data, source/archive
preservation, history and save bookkeeping; `WorkspaceSession` owns them as a registry; `WorkspaceWindow`/
`WorkspaceContext` own input preparation, shared workers and whole-source saving
([workspace guide](workspace-sessions.md)); each `MainWindow` is a per-experiment editor. These session,
workspace, paired-save and recovery layers treat the project model as an opaque value, which makes it
replaceable. The model itself (`LegacyProject`) is reduced to point masses and independent vectors, and
the standard writers patch the original XML through QDom with many layout guards.

### Target layers

| Layer | Design |
| --- | --- |
| OSP XMLControl (`motrack_osp`, new) | Lossless parse/render of OSP's format with OSP's **line-oriented reader** semantics, not a generic XML DOM: root `<object class=` within the first lines, `</object>` and self-closing objects consume the following line, arrays/collections stop at the first non-`<property` line, `>null</property` is null, CDATA ends only at `]]></property>`, no entity decoding, repeated names resolve to the last. The writer follows OSP: CDATA for `" < > & '`, compact `{a,b}` primitive arrays, `[n]` elements skipping nulls, four-space indent, the file's own line endings. An immutable source keeps the original bytes and a node table with line spans; rendering copies untouched lines verbatim, so unedited content stays byte-identical even next to edits. Every render is checked by re-parsing it with the same reader. QDom remains for diagnostics only. Tabsets, function sets, preferences, Data Tool files and library collections use the same layer. |
| Typed Tracker model (`TrackerDocument`, new) | One C++ type per Tracker class, loaded and saved like Tracker's `XML.ObjectLoader`s. The typed value is the editable state and the parsed tree the immutable baseline: saving re-renders only properties whose decoded value changed, so an unchanged value keeps its original text. Unknown classes stay `UnknownObject` nodes. Per-class field descriptors drive load/save, generic edit commands, inspectors, the native sidecar and oracle dumps. References resolve to IDs at load and render back to names at save; export of a rename/delete is refused only where an unknown object could hold a reference. |
| Identities and commands | `ObjectId` generalizes `TrackId` to every addressable object (tracks, functions, parameters, filters, views) with the same never-rewinding counter. Commands are `SetField`, `Insert`, `Remove` and `Batch` (AutoTracker runs, multi-selection); a removed object keeps its source binding, so undo restores its original bytes. Copy-on-write object sharing keeps saved baselines and worker snapshots cheap. |
| Science (`motrack_science`, new) | Qt-free kernels on immutable snapshots: track data columns (Tracker's DatasetManager), coordinate systems and reference frames, derivatives and motion filters, an OSP-compatible expression parser, fitting, ODE solvers, statistics/integration/FFT, video filter kernels, template matching, RGB/line sampling. Each kernel is checked against the OSP/Tracker class it ports, called headless in Java. |
| Media, workspace and views | The existing media pipeline, workspace sessions, paired save and recovery remain. Views (plot/table/world/page, track controls, inspectors, Data Tool, builders, Library Browser) consume the typed model and science results; only the GUI thread updates widgets. |
| Native format | Version 7 stores the current rendered Tracker XML, an identity sidecar (object ID ↔ node path), the ID counter and MoTrack-only extension data. Versions 2–6 still open: their original XML imports into `TrackerDocument` and the reduced JSON edits apply on top; unknown associations from versions 2–3 remain explicit. Version 1 retained no source XML, so its documents keep native-only editing and standard export stays refused, as today. Group envelopes keep their UUIDs. |
| Dependencies | Explicit references and a checked dependency graph drive sums, centers of mass, reference frames, attachments, custom functions, filtered clones and models. Recompute only affected results, reject cycles and dangling references, and invalidate by document revision. Names are labels, not identity; see the [source name/reference design](tracker-name-references.md). |
| Saves and services | Stage and validate both outputs of a paired save, retain recoverable valid files and report partial failures; standard exports never depend on a `.motrack` reader. Filesystem dialogs, clipboard, network/library access and printing stay at adapters. User documents never execute imported code; external-model exchange uses a documented data boundary. |

During the switch-over `LegacyProject` becomes a read-only view derived from `TrackerDocument`, so there is
only ever one editable model; it is deleted once commands and UI are ported.

### Acceptance oracles

Tracker rebuilds XML from its own objects and drops content it does not model, so "MoTrack's save equals
Tracker's save" is not a valid compatibility test, and a lossy Tracker resave is never accepted as evidence.
Tracker is used as a **loader** oracle, and preservation is checked against MoTrack's own baseline:

| Oracle | Check |
| --- | --- |
| O1 | MoTrack's parsed tree equals a Java `XMLControlElement` dump of the same file (headless, every corpus file). |
| O2 | Rendering an unedited parse reproduces the file byte for byte, on Windows, macOS and Linux. |
| O3 | MoTrack's typed-load dump equals a dump of Tracker's loaded `TrackerPanel`. |
| O4 | Track data columns at every frame match Tracker's DatasetManager within the declared tolerances. |
| O5 | Tracker loading MoTrack's save equals Tracker loading the original plus the intended edits, for every save route: linked `.trk`, `.trz` repackaging including other tabs and media members, and native v7 reopen followed by export. Tracker's own resave of MoTrack's output reimports into MoTrack with the edited values intact. |
| O6 | Generic edit operations applied by MoTrack and by Java produce equal Tracker loads, including random edit sequences per file. |
| O7 | For objects MoTrack *creates*, and only for the properties Tracker models, the XML equals what Tracker writes for the same object (fixpoint). Edited existing objects are covered by O8 instead. |
| O8 | After any edit, every node outside the edit set re-renders byte-identical and re-parses equal to the source, including content Tracker ignores; the edited nodes decode to the intended values. |

A known-defect registry records where Tracker is wrong (#63, #64, #140, #146) instead of accepting its output.
Entries are keyed per corpus case and object (or per case for whole-file failures such as #140), carry the
upstream issue and an independent expected value, and require MoTrack to equal that value. An entry fails when
Tracker stops deviating, so stale entries surface when the reference is re-pinned; no entry waives a property
across files. The existing 237 edited actual-Tracker cases stay as the gate for the switch-over. Cases that
assert a refusal caused only by QDom layout limits convert to acceptance cases checked in actual Tracker when
B1 removes that limit; every other case is unchanged. Their bespoke verifiers retire only after mutation testing
shows the generic oracles catch the same faults.

All long I/O, decoding, search, filtering and model work needs cancellation, bounded memory and
request/document generation checks. Only the UI thread modifies widgets or commits live document
edits. Keep RAII for non-Qt resources and Qt parent ownership for widgets. Do not run local system
stress tests as routine feature validation; use representative bounded workloads.

## Implementation order and exit gates

Work proceeds in waves. Within a wave, independent slices on disjoint components may be developed and
reviewed in parallel; dependent stacks stay at most one PR deep. Each slice lands with tests, UI where
applicable, native and standard serialization, translations and documentation. Exit gates are required
results, not statements that their harnesses already exist; each gate below carries the former phase gate it
replaces word for word where one existed.

| Wave | Slices (parallel within a group unless noted) | Exit gate |
| --- | --- | --- |
| 0 — Foundations and kernels | **A1** OSP XMLControl parser/renderer with fuzzing, a Java dump and O1/O2 over every corpus file (critical path). **A2** reference and corpus (former phase 1): a hash-pinned fetch manifest of real experiments across Tracker 4.x–6.3.5 and later upstream builds; a Java generator covering every saved class; the menu/action/class/OSP-tool inventory that I3 checks against; runtime/dependency provenance and the source-built reference for post-6.3.5 behavior; the platform/codec matrix. **A3** generic Java state/data dumper, differ and known-defect registry (O3/O4). Kernels **K1** expression engine, **K2** ODE solvers, **K3** fitting, **K4** derivatives and motion filters, **K5** statistics/integration/FFT, **K6** video filter kernels, **K7** template matching, **K8** RGB/line sampling, each with a Java kernel oracle. **L1** library collection model on A1. | Every corpus file passes O1/O2 on all three platforms; each kernel matches its OSP/Tracker counterpart within declared tolerances, including degenerate and near-zero inputs. Former phase 1: reproducible baseline report with provenance, expected values/tolerances, every requirement mapped to a case, and explicit gaps; no claims based only on synthetic XML. |
| 1 — Model switch-over (sequential) | **B1** typed loaders for the classes already supported (panel subset, clip/clip control, coordinate system, point mass, vector), with the standard writer and tabsets rebased on A1; guards that encode real Tracker behavior stay, QDom layout guards go. **B2** `TrackerDocument` behind `WorkspaceDocument::project`, ID-addressed commands, native v7 with v1–6 readers; `LegacyProject` read-only. **B3** ID-based track list/selection and descriptor-driven inspector; delete `LegacyProject` and the QDom writer. **B4** O3–O8 become required CI gates. | The 237 edited actual-Tracker cases (converted only as described under the oracles), all native migrations, paired save/recovery and workspace tests pass throughout. Former phase 3: the round-trip suite passes in actual Java Tracker, including untouched objects and other tabs; reopen after native close/reload, and reimport Tracker's resave; no silent loss. Former phase 4: two experiments with different timing/units can be edited, switched, saved, canceled and reopened without state or worker results crossing documents. Former phase 2: native save/close/reopen retains unknown objects and associations; duplicate names, reorder/delete/undo, old-native migration and cancellation tests pass. Source-input removal does not break future export metadata. |
| 2 — Track and object families (after B2; each slice also requires O3–O8 to pass for its classes) | **C0** dependency evaluation (sequential, first): explicit references and a checked graph that reject cycles and dangling references and invalidate by document revision; names are labels, not identity ([name/reference design](tracker-name-references.md)). Then in parallel: **C1** coordinates: fixed-flag toggles, key deletion, axes, offset origin, calibration points and stick, units/angle settings. **C2** complete point mass, data columns, derivatives and filtered clones (K4). **C3** tape measure, protractor, circle fitter and attachments. **C4** centers of mass, vector sums and chains, then reference frames (#148). **C5** models: analytic, dynamic, polar, two-body, external data tracks (K1/K2). **C6** media: format/sequence/property/control matrix, full clip timing, ordered filter stack (K6), perspective, RGB region and line profile (K8), full AutoTracker (K7), engine/class changes from [upstream](upstream-delta.md). **C7** persisted views and documents (TViews, single-view layout, drawings, notes), cross-tab import and track copy/paste between experiments. **C8** data functions and function-set autoload (K1). | Former phase 5: every mechanical type is creatable/editable/undoable, produces reference numerical data, and round-trips with relationships intact; interleaved edits and frame changes update all views correctly. Former phase 6: reference image/coordinate tests and playback/seek tests pass for ordered filter combinations; repeated seeking gives the defined result within memory/time budgets; filter settings round-trip. Former phase 7: golden tracking/statistics cases cover subpixel matches, occlusion/low confidence, template evolution, keyframes, stop/retry/cancel and calibrated sampling; UI stays responsive and results round-trip. Former phase 8 (data part): reference quantities, smoothing, expression errors and analysis results match declared tolerances; saved functions reopen correctly. Former phase 9: reference projectile, oscillator, collision/two-body and externally supplied trajectories agree at original frame times; parameters, dependencies and solver settings survive both save formats. |
| 3 — Interface | **I1** plot/table parity (C2). **I2** world/page views and layouts. **I3** toolbars, menus and track controls against the A2 inventory, including the compact trackbar default with a remembered toggle and the crosshair tool. **I4** Data Tool and Fit Builder (K3/K5). **I5** Data and Model Builders (K1/K2/C5/C8). **I6** Library Browser network, search and cache (L1). **I7** data/image/print/video export. **I8** preferences, every Tracker locale, accessibility, help, diagnostics, drop/command-line/file-association opening, native camera capture. | Former phase 8 (tools part): fits, uncertainties and Data Tool results match declared tolerances; saved analysis and view state reopen correctly. Former phase 10: representative lessons can be authored, annotated, exported and reopened; rendered/exported timing, coordinates, units and saved layouts remain correct. Former phase 11: reference collections and portable experiments open with relative paths and documents intact; offline and interrupted transfers preserve existing local work. |
| 4 — Close parity | Reconcile the current upstream inventory, finish every locale (Tracker's translations where they map, the rest machine-translated and marked `unfinished`), keyboard/accessibility/RTL checks, diagnostics/help, command-line/drop opening and licenses/notices for staged builds. Installers, signing and release publication are deferred (R27). | All in-contract requirements closed with evidence; full corpus and interactive acceptance pass on staged Windows/macOS/Linux builds, and each staged build launches and opens the corpus on a clean machine or container with no Java runtime or development SDK. |
| 5 — Resolve the remaining issue backlog | After full-port acceptance, refresh every open MoTrack issue and group related work by shared cause/component. Address data loss, incorrect science, security and crashes first, then compatibility/workflow defects, usability and remaining feature work. Keep upstream/native links and evidence current. | Every applicable issue has a verified fix and linked PR/evidence. Duplicates and investigated non-reproductions have explicit evidence-based dispositions; unresolved decisions or external blockers remain visible and are not closed just to empty the queue. |
| 6 — Audit the complete codebase and resolve findings | After the existing issues are fully addressed, conduct a thorough review of production code, adapters, workers/UI lifetimes, scientific algorithms, persistence/migrations, archive/XML/media boundaries, tests, dependencies, setup and CI. File actionable findings as MoTrack issues, prioritize them and group coherent fixes into reviewable PRs. | All audit findings are addressed with regression evidence and reviewed current-head CI. Recheck affected areas after fixes and refresh the issue inventory before declaring the expanded task complete. |

Former phases map to waves as follows. The requirement matrix's phase column, the decisions table below, the
[issue triage](upstream-issues.md) and the [upstream inventory](upstream-delta.md) still use former phase
numbers until each row is re-mapped with its slice.

| Former phase | Delivered by | Status |
| --- | --- | --- |
| 0 Stabilize | every wave | Ongoing gate for every slice: three-platform builds/tests/static analysis and coverage pass; the current supported workflow remains usable. Fix data loss, crashes and incorrect science before unrelated features. |
| 1 Reference and corpus | wave 0 (A2) | Partial: [#238](https://github.com/mgradwohl/MoTrack/pull/238) adds the hash-pinned real-world library corpus (Tracker 3.00–6.3.3) and its census. Gap: no file from 6.3.5 or a later upstream build yet, so the gate's "4.x–6.3.5 and later upstream builds" range is not covered. Open: post-6.3.5 reference build, inventory, provenance, source mapping, codec matrix. |
| 2 Preserve documents and identities | waves 0–1 (A1, B2) | Audited slices merged; ObjectId generalization in B2. |
| 3 Standard round trips | wave 1 | Audited slices merged; open until its gate above passes for the complete baseline. |
| 4 Multi-document and shared UI | waves 1–2 (B3, C7) | Workspace, shell and Revert merged; Tracker tabsets (#150) under development on the current XML layer, moving to A1 in B1; saved views and cross-tab import open. |
| 5 Coordinates and mechanical tracks | wave 2 (C0–C4) | Open. |
| 6 Media and image transformations | wave 2 (C6) | Open. |
| 7 Automated and image-based measurement | wave 2 (C6) | Open. |
| 8 Analysis and expressions | waves 0, 2, 3 (K1, K3–K5, C2, C8, I1, I4, I5) | Open. |
| 9 Modeling | waves 0, 2 (K2, C5) | Open. |
| 10 Presentation and output | wave 3 (I2, I7) | Open. |
| 11 Library workflows | waves 0, 3 (L1, I6) | Open. |
| 12 Close parity | wave 4 | Open. |
| 13 Issue backlog | wave 5 | Not started. |
| 14 Codebase audit | wave 6 | Not started. |

Localization, accessibility, performance checks and serialization accompany every slice.

The [upstream issue triage](upstream-issues.md) adds concrete regression cases to these exit gates,
including historical issues already fixed in Tracker. Resolve applicable native filings as their slices
land; record an investigated disposition when the original report cannot be reproduced. Intentional
corrections to reference bugs require independent expected results and explicit compatibility limits.
Deferred upstream proposals are separate from the baseline and do not postpone its completion.
The [corpus and reference harness](../tests/corpus/README.md) pins Tracker 6.3.5, real upstream
experiments and Java-authored synthetic fixtures with independent expected values; it remains the
authority for procedures, tolerances and deliberately unaccepted reference results.

### Decisions that must be closed before their dependent phase

| Decision | Current position | Needed by |
| --- | --- | --- |
| Executable/source reference | Baseline tracks upstream `SwingJS` (2026-11-21); no desktop release newer than 6.3.5 exists. The 6.3.5 JAR's URL, bytes and SHA-256 are pinned for validation and run under Temurin 17. Post-6.3.5 behavior needs a source-built reference from paired Tracker `SwingJS` and OSP `swingJS` revisions ([build steps](upstream-delta.md#5-reference-build-feasibility-desktop-jar-from-the-current-tip)); the tip's committed `tracker.jar` has no recorded source revision and cannot be pinned. Matching source mapping and the full GUI/media dependency stack remain to verify. | 1 (wave 0, A2), before golden results for post-6.3.5 behavior |
| OS and CPU support | In-contract verification uses the CI runners: Windows Server 2025 x64, macOS 15 arm64 and Ubuntu 24.04 x64. These are test environments, not declared minimum end-user OS versions; release minimums and further Intel/Arm coverage are decided with R27. | 1; release minimums with R27 |
| Codec coverage and export | FFmpeg import works for tested fixtures; pin a format/codec/container test matrix and identify any licensing or platform limitations. Do not label untested formats supported because FFmpeg recognizes their names. | 1, 6 and 10 |
| Numerical equivalence | Freeze quantity-specific tolerances, derivative/solver choices, units and gap semantics from actual reference results, including boundary and near-zero values. | 1, extended per feature |
| External-model exchange and supporting documents | Verify the reference's data protocols and choose native adapters/viewers without silently excluding those workflows or executing arbitrary document content. | 9–11 |

Closed: native preservation representation ([native format](native-format.md)) and paired-save
recovery ([paired saving](paired-saving.md)). Unresolved decisions stay visible here. Routine
implementation choices can be made within the authorized port scope; any change to required behavior
or declared compatibility must be recorded.

## Post-port completion work

The maintainer extended the objective on 2026-11-08: completing the full Tracker port is followed by
resolving the remaining MoTrack issue backlog, then a complete code review, issue filing and resolution
of the review findings. These are additional completion gates, not replacements for unfinished parity.
Waves 5–6 (formerly phases 13–14) have not started. Continue fixing defects discovered during implementation before adding
unrelated features; do not defer a current data-loss or scientific failure to the later audit.

At each sweep, record the issue inventory and priority, relevant reproduction/expected behavior,
related upstream report, affected requirements and intended PR grouping. Group issues when one
coherent change resolves the same cause; keep unrelated changes separate. Close issues with linked
fixes and concrete validation, or a documented investigated disposition where appropriate. Do not
silently discard applicable work or treat an unverified report as a proven fix.

The final audit applies the [review checklist](code-review.md) across the whole repository, including
cross-component interactions and representative user workflows. Record actionable findings with a
reproduction or concrete failure scenario, severity and acceptance criteria before implementing fixes.
Preserve the [upstream triage](upstream-issues.md) distinction between native fixes and Tracker's own
resolution. Deferred upstream proposals remain outside the parity baseline; the post-port
sweep tracks the MoTrack issues that remain after the port.

Every resulting PR still requires passing review, current-revision Windows/macOS/Linux CI and the
unchanged 90% combined C++ coverage gate. Clean verified merged branches and obsolete task outputs
after merging. Release publication and signing still require separate authorization.

## Verification and definition of done

- Keep the hard **90% combined handwritten production-C++ line coverage** gate, Clang formatting/static
  analysis, numerical/unit tests, public workflow tests and staged smoke tests on all three platforms.
  Exclude generated/dependency/test sources, not inconvenient production modules. A reference runner's
  results and real interoperability checks are additional evidence, not substitutes for these gates.
- Use exact comparisons for identities/frame assignments and explicit per-quantity absolute/relative
  tolerances for scientific results. Reference differences must be explained; do not increase a tolerance
  merely to hide an error. Test absent data, invalid units/expressions, gaps, singular fits and solver failure.
- Test failures and lifecycle operations: cancel/supersede, reopen, undo/redo, mixed-document edits,
  missing/relocated media, read-only destinations, interrupted paired writes, corrupt/truncated archives,
  oversized input and shutdown. Keep parsing and archive resource limits explicit and fuzz those boundaries.
- Use small deterministic CI fixtures, a separately fetched real corpus, and bounded longer scenarios.
  Record machine/workload/time/memory when measuring performance. Establish budgets in slice A2 (former phase 1) and keep
  regression baselines; never infer safety from an unbounded local stress test.
- For each requirement, link the merged PR in its row; the PR records reference version/case,
  tests/tolerances, platform results and known differences. Update status when code lands;
  an open PR or local demonstration is not a completed main-branch feature.
- Before declaring the **full port complete**, reconcile the current upstream inventory again, close every
  row and subfeature, pass real Tracker round trips, verify representative interactive teaching workflows
  on a staged build, and finish every locale's catalog (machine-translated entries may remain `unfinished`).
  Installers are outside the current contract (R27). Any exception needs an explicit maintainer
  decision recorded with its effect on the parity claim.
- Under the maintainer's standing authorization, reviewed PRs with green current-revision CI may be merged
  autonomously. Then fast-forward local `main`, remove verified merged branches locally/remotely, and remove
  obsolete task-owned outputs while retaining active builds, caches, SDKs and useful corpus samples.

## Tracker round-trip compatibility

The maintainer requires this workflow: open a Tracker experiment in MoTrack, edit it, save it, and
reopen the edited experiment in Tracker. A `.motrack` companion may be saved alongside the standard
`.trk` project or `.trz` archive. Tracker must not need the companion to read the standard file.

**Current status:** see R02/R03. Imports retain the original XML and archive; audited edits are
patched into that preserved source, and unsupported edited fields are rejected rather than lost.
Version-1 native documents cannot recover previously discarded content. Treat full round-trip support
as a compatibility milestone, not a completed capability.

Tracker's XML object format and ZIP experiment packaging are workable. The main implementation risk
is losing data or changing relationships when translating only the types MoTrack understands. The
[Tracker I/O source](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/TrackerIO.java)
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

Current inventory, checked on 2026-11-06:

| Material | Availability | What it proves today |
| --- | --- | --- |
| `point-mass.trk`, `image-sequence.trk`, `movie.trk` and `archive.trz` | Committed under [tests/fixtures](../tests/fixtures), with tiny generated media and additional inline XML tests. | Targeted parser, timing, path and application regressions; not reference-app round-trip proof. |
| `independent-vectors.trk` | Java-authored synthetic fixture, reproducible generator and checksum recorded in [the corpus](../tests/corpus/README.md). | Sparse/zero independent vectors and nonuniform calibration; native import and actual Java session resave, with tail/component checks. Native vector export adds moved/added/deleted samples and empty/first-sample round-trip cases; clip export covers trim/stride and retention of samples outside the clip; fixed-calibration export checks origin/rotation/nonuniform scale without moving image samples. A length-unit case preserves a Unicode label without moving geometry or changing calibration. Snapping cases explicitly verify rejected unsafe exports. CI evidence belongs to each writer PR. |
| `center-of-mass.trk` | Java-authored synthetic fixture and independent weighted-position expectations in the corpus manifest. | Two named point masses and their center at sparse frames. Native import reports the center as unsupported; the Java application baseline checks resolved members, total mass and image/world positions across resave. |
| `keyed-calibration.trk` | Java-authored synthetic fixture and independent Java snapshot with pinned bytes; reproducible generator in the corpus. | Fully varying origin/rotation/nonuniform scale at frames 0/4/9, with point-mass image/world coordinates checked before and at transitions. Native import and Java application resave reference; a standard-export case adds/edits keys with unchanged image marks and independent world values. See its writer PR for CI evidence. |
| `BallToss.trz`, `protractor.trk`, `asinewave.trk` | Reproducibly fetched using [tests/corpus/manifest.json](../tests/corpus/manifest.json), with committed independent Java snapshots. | Native import/coordinate comparisons for real point masses, clip/calibration values and reporting unsupported models/tools. No complete GUI load or standard-file export claim. |
| `binary_star.trz` | Local development sample; project records Tracker 6.2.0 and contains `DynamicSystem` and `DynamicParticle` objects. | Useful future model/preservation case; no automated corpus baseline yet. |
| `bouncing_cart.trz` | Local development sample; project records Tracker 6.1.1 and contains `DynamicParticle` and `TapeMeasure` objects. | Useful future model/tool/preservation case; displaying its media does not implement those tracks. |
| `tab_video.trz` | Local development sample; project records Tracker 4.62 and contains a `TapeMeasure`. | Useful older-format case; despite its name, the inspected archive contains only one `.trk`. |
| Library corpus (514 files, 492 unique, 0.83 GB; refreshed 2026-11-21) | Real experiments from Tracker's default Library Browser collections (ComPADRE, Tracker Home Library, Shared Library). They are pinned by URL, size and SHA-256 in [library-manifest.json](../tests/corpus/library-manifest.json) together with verbatim rights statements, fetched into the ignored cache and checked weekly by `library-corpus.yml`. | A census of the Tracker features that real files use, MoTrack's desktop open result for each file and the 6.3.5 XML/coordinate reference for each project. Findings: #211, #212, #213, #224, #229. This is not round-trip or parity evidence (slice A2, corpus part). |

The older `binary_star`, `bouncing_cart` and `tab_video` downloads are not in the reproducible corpus.
Record provenance, redistribution terms, download location and hashes before promoting them into automated reference tests.
Add real point-mass/vector experiments and a genuinely multi-tab archive; the existing samples cannot
stand in for those cases.

The existing small synthetic fixtures test importer behavior, not broad Tracker compatibility. Extend
the versioned corpus with source URL, redistribution permission, checksum, producing Tracker version,
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
