# Track list, selection and inspector (slice B3)

This is the design and implementation plan for slice B3 of wave 1 in the
[full-port plan](migration-plan.md#implementation-order-and-exit-gates): "ID-based track list/selection and
descriptor-driven inspector; delete `LegacyProject` and the QDom writer." **Status:** design only; nothing here is
implemented. B3 follows B2 ([design](tracker-document.md)): B2.1 has landed
([PR #337](https://github.com/mgradwohl/MoTrack/pull/337)); B2.2 (commands and history) and B2.3 (export
composition) are being built; B2.4a, B2.4b and B2.5 are not started. Names this document takes from the B2
design, such as `SetField`, `Batch`, `DocumentHistory` and the document revision, mean what that design says;
they are not merged code. Wave 2 lands each family's interface PR after B3 (the plan in
[#172](https://github.com/mgradwohl/MoTrack/issues/172)), so the inspector must take any family's descriptors
without a widget written for that family.

The wave-1 exit gate holds after every slice: the 237 edited actual-Tracker cases, all native migrations, paired
save and recovery, and the workspace tests pass. B3's user-visible changes are the track list panel and the
inspector, read-only rows for tracks MoTrack preserves but does not edit, and the selection rules below. Export
decisions and bytes do not change.

## Requirements

| Requirement | What B3 advances |
| --- | --- |
| R04 | Each experiment view keeps its own selection by `ObjectId`, independent of other tabs, and edits reach the document's own history as commands. |
| R06 | Track selection by identity through undo, redo, rename, deletion and restoration; the per-frame inspector section is the hook for numeric positions, which C2 enables. |
| R21 | A track list and an inspector replace the toolbar combo box and the per-property dialogs. They show the fields of Tracker's trackbar in a modernized layout; the compact trackbar default is I3. |
| R25 | Translated labels and choices, locale-aware numbers, labelled and keyboard-reachable editors, errors shown without pop-ups. |
| R02, R03 | Deleting `LegacyProject` and B1's edit composition changes no export decision or byte and keeps every native file opening to the same document. |

## Starting point after B2

- `MainWindow` lists tracks in `m_TrackPicker`, a `QComboBox` in the Project toolbar: point masses, then vectors
  with a "(vector)" suffix, read from the legacy view. Every track action reads `currentIndex()` and splits it into
  a point-mass or vector index (`selectedVector`). `refreshEditViews(int)` rebuilds the combo box after each edit,
  undo and redo and keeps the old index: with A, B and C, deleting A, selecting C and undoing selects B. AutoTracker
  keeps the index in `AutoTrackSession::track`.
- Rename track… and Set mass… open modal `QInputDialog`s guarded by `m_RequestGeneration`; calibration, axes and
  clip settings have their own dialogs.
- After B2.4a, edits pass through `TrackEdits`' index-based façade over B2.2's commands and the consumers in
  [What LegacyProject keeps until B3](tracker-document.md#what-legacyproject-keeps-until-b3) read `legacyView`.
  After B2.4b export composes from typed state, with B1's composition as a required comparison; after B2.5
  native files are version 7, and versions 1–6 open through the reduced model. `trackOrder()` holds only the live
  point masses and vectors, point masses first.
- Production code has had no QDom since B1 ([#294](https://github.com/mgradwohl/MoTrack/pull/294)). The "QDom
  writer" of the plan is B1's edit composition in `tracker_writer.cpp` and `tracker_rename.cpp`, which descends
  from it: it diffs a fresh `LegacyProject` import against the live one. QDom remains only in tests that decode
  XML snippets (`tests/osp_layout.hpp`, `tests/tracker_writer_test.cpp`), which link `Qt6::Xml`.

## 1. Track list and selection

### Rows

`model::trackRows(const TrackerDocument&)` (Qt-free, `motrack_model`) returns one `TrackRow` per live element of
the panel's `tracks`: its `ObjectId`, family, exact class, label (`loadedName`, or the Unknown object's label),
whether it is typed or preserved (Unknown), and whether one of its reference slots dangles because the target was
deleted. Typed and Unknown tracks are both rows (open question 1), in the order of open question 2, grouped as
Tracker's Select menu groups them: user tracks, then what `TrackerPanel.getUserTracks` leaves out (the axes,
calibration and measuring tools, perspective tracks and child data tracks). Removed objects and the singletons
(panel, clip, video, clip control, coordinate system) are not rows; C1 and C6 add entries for the coordinate
system and the clip that select those objects in the same inspector.

`TrackListModel` (a `QAbstractListModel` in `motrack_workspace`) holds the rows and their `DocumentStamp`.
`refresh(document, stamp)` does nothing for an unchanged stamp; otherwise it diffs the rows by `ObjectId` and
emits removals, insertions, moves and `dataChanged`, never a reset, so views keep their scroll position and
assistive technology does not reread the list. Roles: label, family icon, an accessible text naming the family
("Vector: Vector 1"), a tooltip with the class and any read-only reason, `ObjectIdRole`, `FamilyRole` and
`EditableRole`. A row index never outlives the event handler that read it. The list dock and the toolbar's track
box share the model and one `QItemSelectionModel`. A wave-2 family needs no list code: its objects become typed rows.

### Selection

Each `MainWindow` owns a `TrackSelection`, `std::optional<ObjectId> current`, on the GUI thread. It is view state:
never a command, never in history or the modified state, not saved in B3 (open question 4). The view's current
index is derived from `current` after every refresh, and set from a row only when the user picks one.

| Event | Selection afterwards |
| --- | --- |
| Open, editor recreated after Hide or Revert | The track the saved `selectedtrack` names when it binds to one row, as Tracker restores it (open question 4); else the first typed user track; else none. |
| Create (`Insert`) | The created track. |
| Delete the selected track (`Remove`) | None, as in Tracker, so a later Shift-click cannot mark a neighbour by accident. |
| Rename, field edit, deleting another track, reordered rows | Unchanged; the view scrolls to keep the row visible. |
| Undo or redo of an entry that addresses exactly one track | That track if it is live, so the inspector shows what changed (open question 3); none if the entry removed the selected track. |
| Undo or redo of any other entry (clip, calibration, units, a `Batch` over several tracks) | Unchanged; none if it removed the selected track. |
| Selecting another tab | Each editor keeps its own selection, as each Tracker tab does; nothing crosses documents. |

The undo rule needs the identities a history entry addresses: B3.2 adds `DocumentHistory::affectedObjects(entry)`
if B2.2 does not expose them. `ObjectId`s are per document, so anything that hands a track to another view (C7's
copy and paste, plots in other windows) carries the pair (`DocumentStamp::runtime`, `ObjectId`).

### Commands and stale results

The Tracks menu and the list's keys (F2 renames, Delete deletes) act on `current`; a row's context menu acts on
its row, as Tracker's Edit ▸ Delete submenu can delete any track. Each is one history entry: New point mass and
New vector (`Insert`), Rename (`SetField` on the label), Delete track (`Remove`), Delete mark on this frame
(`SetField` of the frame element to absent). The command, not the editor, refuses an empty name or one already in
use (open question 10).

Every deferred action captures what it acts on instead of re-reading the selection when it completes:

- **Editors** capture the object, field, element key and `before` value when they are filled. Their commit is a
  `SetField` with that `before`, which B2.2 refuses when the state has changed, whatever caused the change.
- **AutoTracker** keeps the `ObjectId` and the registry job token, and applies each match as the family's mark
  edit with `before = absent`. A removed track or a frame marked meanwhile is refused and stops the run with a
  message, instead of marking whichever track now has the stored index. Selecting another track still stops it.
- **Modal flows that remain** (calibration stick, axes, clip settings) keep `m_RequestGeneration` for the displayed
  frame and add the captured `before` values to their commands.

## 2. Descriptor-driven inspector

### Layout and hosts

The inspector is a dock in each `MainWindow`, beside the track list, and shows the selected track. Its sections
are **Track** (name, mass and other per-track values), **Appearance** (colour, footprint, visibility, trail;
empty until C2 types them), **Frame *n*** (frame-keyed values at the displayed original frame) and **Details**
(read-only fields, collapsed). An Unknown track shows its class, label and reference slots. Editors come from one
factory, so I3's compact trackbar and floating number panels (upstream [U5](upstream-delta.md)) can host the same
editors for the same fields. A row's context menu is Tracker's track menu, built from the same presentation
entries: boolean fields as checkable actions, other fields as items that focus their editor, then Delete.

### Descriptor metadata

B2.1's `FieldDescriptor` gives the id, OSP property and type, kind, `commandEdits`, facet, label flag and loader
default. B3.1 adds the Qt-free semantics that both commands and editors need:

| Added to `FieldDescriptor` | Content |
| --- | --- |
| `quantity` | `Pixels`, `Length`, `Mass`, `Time`, `Angle`, `Frequency`, `Frame` (an original frame number), `Count` or `Dimensionless`, with the stored unit where it is fixed (clip times in milliseconds). Length, mass and time display the panel's labels. |
| `domain` | Finite numbers with inclusive or exclusive bounds (mass at least `MINIMUM_POINT_MASS`), integer ranges, the name and unit codecs, choice ids for enumerations, a colour in the form the field saves (an RGB integer such as C1's `grid_RGB`, or a colour object), or a reference slot with its shape, `maxSlots` and `CandidateFilter`. |
| `components` | For frame-keyed fields, the element's members (mark `x`, `y`; vector `xtail` … `ycomponent`; key `xorigin` … `yscale`), each with a quantity and its coordinate space (image pixels, y down). |
| `keys` | `Sparse` (each frame stands alone: marks, vector samples) or `HeldUntilNextKey` (coordinate keys; C1's stick and C3's tape ends), and the boolean field that fixes the value for every frame, if any (`fixedorigin`). |
| `validate` | `validateField(family, field, value)`: B2.2's domain check, extracted so that `SetField` and the editors call the same function. It returns a `FieldRefusal` code, never English text. |

Presentation lives in the workspace (`src/inspector/field_presentation.*`), keyed by family and field id: the
label (`QT_TRANSLATE_NOOP` with the family as context), tooltip, section, order, editor override (multi-line text
for `description`) and number format. A completeness test fails for a typed field that has neither a presentation
entry nor an explicit `hidden` entry (open question 5).

### From descriptor to editor

| Descriptor | Editor | Commits |
| --- | --- | --- |
| Boolean | `QCheckBox` | On toggle. |
| Integer, frame or count | Number field, integer validator; frames show the clip step and time beside them | Enter or focus-out. |
| Double with a quantity | Number field: `QLineEdit` with a validator and a unit suffix label; no spin box, which would clamp out-of-range input silently and create an entry per arrow step | Enter or focus-out. |
| Label (`label = true`) | `QLineEdit` | Enter or focus-out; Escape restores the document's value. The command renames. |
| Text | `QPlainTextEdit` | Focus-out or Ctrl+Enter. |
| Colour | Swatch button opening `QColorDialog` | When the dialog is accepted. |
| Enumeration | `QComboBox` of translated choices, storing the choice id | On activation. |
| Single reference | `QComboBox` of the candidates the slot's filter and `cycleIfAdded` allow, plus None | On activation. |
| Multiple references | Checkable list of candidates | Each toggle. |
| Frame-keyed element | The **Frame *n*** section: the components at the displayed frame, a key-frame indicator, previous and next key, and Delete at this frame | Each component commits the whole element. |
| Records (parameters, functions, filters, constants) | Extension point, not built in B3 (open question 7) | Same rules. |

Editors carry the object name `inspector.<family>.<field>[.<component>]`, which tests use to find them.

### Validation, errors and commands

Input passes three checks. Parsing (workspace) reads the display locale, then the C locale, and trims a suffix of
the field's own unit as Tracker's `NumberField` does ([U19](upstream-delta.md)); locale-aware input is part of
[#66](https://github.com/mgradwohl/MoTrack/issues/66). `validateField` runs on every change, so the inspector
never offers what the command refuses for its domain. B2.2 then validates the command: the stale `before`, the
family's invariants (unique names, C2's track lock, the timing-provider guard, cycles) and the document's. A
failure shows an icon and a translated message under the editor, sets its accessible description and repeats
the message in the status bar, with no pop-up. The text stays until it is corrected or Escape restores the
document's value. Today's messages ("Enter a finite, positive mass.") become `FieldRefusal` translations.

One commit is one history entry. A value equal to `before` issues nothing, as the dialogs do now; typing never
commits and commits are never merged. An edit that changes several fields is a family recipe returning one
`Batch`, such as C1's fixed-flag toggle or C2's edit of several selected steps. A focus-out commit goes to the
captured object even when the focus left because another track was selected; closing the view drops an invalid
uncommitted value. While an editor holds uncommitted text, Ctrl+Z undoes its text; otherwise it is document undo.

### Per-frame and per-track fields

Scalar fields are per track. Frame-keyed fields appear in **Frame *n*** for the displayed original frame, which
is the frame number Tracker loads, never a clip step; the section refreshes its values on every frame change
without rebuilding widgets. A `Sparse` field shows the element at *n* or that there is none. A
`HeldUntilNextKey` field shows the value in effect, the key it comes from, a toggle that adds a key at *n* (the
effective value) or removes it, and the fixing flag when declared. B3 shows marks and vector samples read-only,
in image pixels and, where the frame is calibrated, in world units (open question 6). Delete at this frame works
for every frame-keyed field. PointMass `keyFrames` is not a separate editor: it changes in the same `Batch` as the
mark it records.

### Read-only and preserved fields

- Typed fields that no command edits appear under **Details**, read-only but focusable, with a tooltip saying so.
- A field the file does not save shows the loader default marked "(default)" and stays absent on export until
  edited; showing a value never creates an edit.
- A saved value the loader cannot read shows "Unreadable in the file"; replacing an editable one is one edit.
- An Unknown track shows "Preserved from the Tracker file; MoTrack does not edit it yet", its class and label,
  and each reference slot of its reference-only descriptors with the target's label, marked when the target was
  deleted (which blocks standard export). Each target can be selected from there.

### Localization and accessibility

Labels, section titles, choices and refusals go through `tr`. `tools/translations.py` seeds new entries from
Tracker's translations of identical English text, so labels reuse Tracker's wording where it fits; the rest are
machine-translated and left `unfinished`, as the 2026-11-21 and 2026-11-22 decisions require. Unit labels are
document data, shown verbatim; "px" and composed units use the plot's translated patterns. Numbers use
`dataDisplayLocale()`, and number editors keep left-to-right digits while right-to-left forms mirror.

Every editor has a buddy label with a mnemonic, an accessible name that includes the unit ("Mass (kg)") and an
accessible description for errors and read-only reasons. Tab order follows the presentation order: track list,
then Track, Appearance, Frame *n* and Details. Read-only fields stay in the tab order so screen readers reach
them; a disabled editor says why. Nothing uses colour alone.

## 3. Tracker workflows and command map

Tracker's behavior below was read from the `SwingJS` sources at
[`34b1b503`](https://github.com/OpenSourcePhysics/tracker/tree/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/)
(`TrackerPanel`, `TTrackBar`, `TrackControl`, `TTrack`, `PointMass`, `TapeMeasure`, `TMenuBar`, `Undo`).

**Kept.** One selected track per tab, held as an object; the saved `selectedtrack` restored on open; a created
track selected; a deleted selected track leaving none selected; a renamed track staying selected; tracks listed in
the panel's order with the axes and tools after the user tracks; the track menu (Name…, Color…, Footprints,
Visible, Locked, Notes…, Delete) on each track; the mass field with its unit, made positive and at least 1E-30;
a name refused when it is empty or in use; number fields that commit on Enter or focus-out; one undo step per
committed edit.

**Deliberate differences**, for the user guide:

- A docked track list and inspector replace the track bar's fields, the floating TrackControl and the modal Set
  Name dialog (the 2026-11-22 layout decision). Plots keep following the selected track until I1 gives plot and
  table views their own track choosers, as `TrackChooserTView` does.
- Every property edit can be undone. Tracker does not undo visibility, lock, mass, notes, Automark or track
  creation, and merges several position edits of one selected step into one entry.
- Undoing a deletion restores the same track at its place with its references; Tracker reloads it from XML as a
  new object ([#287](https://github.com/mgradwohl/MoTrack/issues/287)).
- A blank name is refused with a message; Tracker ignores it but still records an empty undo step. A mass typed
  with another unit's suffix is refused; Tracker asks whether to change the mass unit, which C1's Units dialog
  offers in MoTrack ([#342](https://github.com/mgradwohl/MoTrack/issues/342)).
- When the file names no selected track, MoTrack selects the first user track, as it does now; Tracker selects
  none.

| Tracker command and location | MoTrack after B3 |
| --- | --- |
| Track ▸ New ▸ Point Mass, Vector; TrackControl's New button | Tracks ▸ New point mass, New vector; the New button of the track list; the toolbar |
| Track ▸ Select; the track bar's track button | The track list; the toolbar's track box |
| Toolbar "Show or hide the track control" | View ▸ Tracks (the dock's toggle) |
| Track menu ▸ Name… (Set Name dialog) | The inspector's Name; Tracks ▸ Rename track…, F2 and the row's menu focus it |
| Track bar mass field "m" | The inspector's Mass; Tracks ▸ Set mass… focuses it |
| Track menu ▸ Color…, Footprints, Visible, Locked, Notes… | The inspector and the row's menu, once C2 types these fields |
| Track menu ▸ Delete; Edit ▸ Delete ▸ *track* | Tracks ▸ Delete track; the Delete key in the list; the row's menu |
| Edit ▸ Delete ▸ Selected Step; Delete Selected Step | Tracks ▸ Delete mark on this frame (the displayed frame); step selection is C2 |
| Edit ▸ Delete ▸ All Tracks | Not in B3 (I3's menu inventory) |
| Edit ▸ Undo, Redo, named after the edit | Edit ▸ Undo, Redo, named after the entry ("Undo Mass of Point Mass 1") |
| Track bar point fields (step, t, x, y, r, θ) | The inspector's Frame *n*, read-only until C2 |
| Clicking a step on the video selects its track | With step selection in C2 (R06, [U8](upstream-delta.md)'s hit tolerance) |

**Code-reading observations**, not reproduced, with no matching MoTrack or upstream issue on 2026-11-22: Tracker's
static name dialog keeps the first track that opened it, so a later rename can act on a deleted track or check
names in another tab; menus, undo and the Notes dialog look tracks up by name; step undo entries go stale once a
deletion is undone. Identities rule these out in B3. B3.3 reproduces the name-dialog case against the pinned JAR
and records it as AGENTS.md requires before the guide cites it; field commits converted with the current frame
instead of the step's go to C2, and the tape key-frame order in `commitMagField` to C3.

## 4. Removing `LegacyProject` and the QDom writer

Uses that B2.4a already converts (sessions, groups, Revert and saved baselines, paired save, archive media
resolution, image packaging, `WorkspaceWindow`'s media check) are not repeated here. What remains after B2.5:

| Use | Files | Replacement | Slice |
| --- | --- | --- | --- |
| Index-based track list, actions, selection and AutoTracker; counter capacity for New point mass and New vector | `main_window.*` (`m_TrackPicker`, `selectedVector`, `refreshEditViews`, rename, mass, delete, mark, repeat, `AutoTrackSession`), `track_identity.*` | `TrackListModel`, `TrackSelection`, commands by `ObjectId`, `TrackerDocument::hasCounterCapacity` | B3.2 |
| Rename and mass dialogs | `main_window.cpp` | Inspector editors | B3.3 |
| Marks table, canvas marks and arrows, axes overlay, transient measurements, calibration and clip dialogs | `main_window.cpp`, `video_canvas.*`, `image_sequence.*` (`Clip`, `CalibrationKey`) | Typed reads and two derived Qt-free views in `motrack_model`: clip frames and timing (from the clip, clip control, video and media extension) and the effective calibration (from `CoordinatesState`) | B3.4 |
| Measurements, CSV and plots by track index | `measurements.*` (`measurementPlot`, `measurementsCsv`, the legacy `supportsConstantClipTimingEdits`) | The same functions over the derived views and a mark series by `ObjectId`; O4 still compares them until C2 | B3.4 |
| Admission checks and the reference index | `tracker_writer.cpp` (table columns, coordinates, coordinate updates, shifted samples, vectors, mass frames), `tracker_rename.cpp` (membership, selection fallback), `tracker_references.*` | The same checks over the typed document; the reference index reads bindings and slots from it | B3.4 |
| Edit scenarios and snapshots of the corpus probe, including the reorder case | `tests/corpus_probe.cpp`, `tools/corpus.py` | B2.2's `--apply` command files and typed reads; the reorder case reverses `trackOrder` by reassembling the document, since no command reorders tracks | B3.4 |
| Test helpers that edit through `TrackEdits` | `tests/media_vectors.hpp`, `tests/movie_authoring.hpp`, shell, archive, paired-save and writer tests | A small command helper by `ObjectId` | B3.4 |
| B1's edit composition and the required comparison | `tracker_writer.cpp` (`renderTrackerProjectImpl`'s diff, the `patch*` functions over `LegacyProject`, `remapIds`, `sortTracks`), `tracker_rename.cpp`, `tools/corpus.py`, CI | Deleted; B2.3's composition is the only one | B3.5 |
| Legacy view and façade | `legacy_view.*` (`legacyProject`, `LegacyViewCache`, `trackerSourceView`), `track_edits.*` | Deleted; `DocumentStamp` moves to the document owners, whose caches keep using it | B3.5 |
| Reading native versions 1–6 | `workspace_document.cpp` JSON readers, `typedDocumentFromReduced` | A private reduced-model struct in the native reader with exactly the v1–6 schema, converted directly; the golden native fixtures must open to the same documents | B3.5 |
| B1's import policy over `LegacyProject` | `legacy_project.cpp` (`importTracker`, `parseLegacyProject`, `parseTrackerProject`, `readTrackerProject`, `validateTrackerSource`), `tracker_import.hpp` | `TrackerDocument::importBaseline`; the bounded parse `parseTrackerXml`, which tabsets use, moves to a Tracker XML adapter | B3.5 |
| Shared small types | `legacy_project.hpp` (`ProjectError`, `MAX_TRACKER_XML_BYTES`, `TrackerReading`, `TrackId`, `TrackKind`, `TrackSourceBinding`, `TrackerSource`) | `project_error.hpp`, the XML adapter and the native codec; `TrackId` becomes `ObjectId`; writer code reads the document's baseline instead of a `TrackerSource` | B3.5 |
| Tests of the deleted code | `legacy_project_test.cpp`, `track_edits_test.cpp`, `legacy_view_parity_test.cpp`, B2.2's `history_parity_test.cpp`, `measurements_test.cpp`, `motion_reference_test.cpp` | Scenarios kept against the typed document with their expected values recorded independently, not by comparison with deleted code | B3.4, B3.5 |
| QDom in tests | `tests/osp_layout.hpp`, `tests/tracker_writer_test.cpp`, `Qt6::Xml` in CMake | Snippets built with `motrack_osp`; `Qt6::Xml` leaves the build (open question 8) | B3.5 |

The order keeps one source of truth at each step: the UI stops using indices (B3.2, B3.3); every reader moves to
typed state while the legacy view and B1's comparison still run beside it (B3.4); then both are deleted (B3.5),
after the comparison passes on all three platforms on the merged B3.4. Each slice reports the remaining
`LegacyProject`, `TrackId` and `TrackEdits` uses, B3.5 ends at zero, and AGENTS.md's note on them goes too.

## 5. Threading

| Work | Thread | Cancellation and staleness |
| --- | --- | --- |
| Rows, inspector filling, validation, commands, undo and redo | GUI | Synchronous; refreshes are keyed by `DocumentStamp`, so an unchanged stamp does nothing. |
| The Frame *n* section | GUI | Reads one element per field (`FrameSeries::find`) for the displayed frame, not a requested one, so a decode in flight never shows another frame's values. |
| AutoTracker matching | Analysis pool, as now | The registry job token, then the captured `ObjectId` and `before = absent`; a refused command stops the run. |
| Derived values that need evaluation (wave 2 data columns, lengths, model steps) | Workers on snapshots | Keyed by runtime identity, node and effective graph revision and dropped unless `isCurrent`; the editor shows "Calculating…" meanwhile. |

No widget, list model or inspector code runs off the GUI thread; export and saving stay on the save worker; no
worker holds a pointer into a live document or these models; a result for another runtime identity is dropped.

## 6. Testing

Model tests (`motrack_tests`, Qt-free) cover `trackRows` for every fixture and corpus file and `validateField`
against valid and invalid samples for every descriptor, with the same refusal as B2.2's `SetField`.

Widget tests run offscreen in a new `TrackUi.*` group of `motrack_workspace_tests` with its own CTest entry and
60-second limit. They use temporary directories and wait with `QTest::qWaitFor` for observable state (the
current row, the inspector's title, the history revision, an error label), never for a sleep or a frame count.

- `QAbstractItemModelTester` watches `TrackListModel` through random create, rename, delete, undo and redo.
- Selection: every row of the table above, including deleting A from its row menu while C is selected, undo
  and redo of that, renaming C, reopening with another row order and opening a file whose `selectedtrack` names
  B. Two tabs keep independent selections, and undo in one leaves the other alone.
- Stale results: an AutoTracker match that completes after its track was removed by undo applies nothing and
  stops with its message; a commit whose `before` no longer holds shows the refusal and changes nothing.
- The generic inspector test iterates families and fields from the descriptors and presentation table. For each
  editable field it sets a valid sample, commits with the keyboard, waits for one history entry and the value,
  undoes and checks the old value; an invalid sample must show the error with an accessible description and
  leave history unchanged; read-only fields refuse input; every editor has a buddy, a non-empty accessible name
  and a label in the English catalog; a German-locale run enters a decimal comma.

A family gets this coverage by registering an `InspectorFixture` (a small synthetic document with one object of
the family and valid and invalid samples per editable field) in `tests/inspector_fixtures.hpp`; the completeness
test fails when a presented family has no fixture or a field no samples. Dragging, wizards and record tables still
need their own tests. B3.4 runs each corpus edit scenario through commands and through the old path and requires
equal typed documents before B3.5 deletes the old path.

## 7. Implementation slices

Each slice depends on merged slices only and updates the R04, R06, R21 and R25 rows and this status line.

**B3.1 — Descriptor semantics and track rows** (after B2.2; parallel with B2.3, B2.4a, B2.4b and B2.5)
- Content: quantities, domains, components, key policies, `validateField` and the `FieldRefusal` codes that
  B2.2's validation then returns; `trackRows`. No application change.
- Files: `src/model/descriptors.*`, new `src/model/track_rows.*`, `src/model/commands.*` (refusal codes only),
  `tests/model/descriptor_semantics_test.cpp`, `tests/model/track_rows_test.cpp`, `CMakeLists.txt`. B2.3 also
  adds to `descriptors.*`; separate members and functions keep that conflict textual.
- Exit: every typed field has semantics; `validateField` agrees with `SetField` on every sample; coverage holds.

**B3.2 — ID-based track list and selection** (after B2.4a and B3.1; parallel with B2.4b and B2.5)
- Content: `TrackListModel`, the list dock sharing its selection with the toolbar box, `TrackSelection` and its
  rules, track commands by `ObjectId` without the façade, AutoTracker by `ObjectId`, Unknown rows, the order of
  open question 2. The marks table, plot and canvas may still read the legacy view.
- Files: new `src/track_list_model.*`; `src/main_window.*`, `src/bound_workspace.cpp`, `CMakeLists.txt`,
  `translations/*.ts`, `README.md`; new `tests/track_ui_test.cpp`; `tests/workspace_test.cpp`. No conflict with
  B2.4b (`tracker_writer.*`) or B2.5 (native files); `main_window.*` is the hot file.
- Exit: the workspace tests pass with only the track box's order and empty state updated; the selection and
  stale-result tests pass; `MainWindow` turns no row index into a track outside the view glue.

**B3.3 — Descriptor-driven inspector** (after B3.2; parallel with B2.5)
- Content: the inspector dock, editor factory and row menus; presentation entries for point masses, vectors and
  Unknown tracks; errors, commits, Frame *n* and Details; Undo and Redo named after their entry; Rename track…
  and Set mass… focusing their editors; translations, accessibility and `docs/tracker-commands.md`.
- Files: new `src/inspector/inspector_panel.*`, `src/inspector/field_editors.*`,
  `src/inspector/field_presentation.*`, `tests/inspector_test.cpp`, `tests/inspector_fixtures.hpp`;
  `src/main_window.*`, `CMakeLists.txt`, `translations/*.ts`, `README.md`.
- Exit: the generic inspector test passes for every presented field, one history entry per commit, and the
  dialog tests are converted.

**B3.4 — Typed reads everywhere else** (after B2.4b, B2.5 and B3.2; parallel with B3.3, whose
`main_window.cpp` changes land first)
- Content: the derived clip and calibration views, measurements and plots by `ObjectId`, canvas and table reads,
  admission checks and the reference index on typed state, corpus probe and test helpers on commands.
- Files: new `src/model/clip_view.*`, `src/model/calibration_view.*`; the users in §4's B3.4 rows,
  `tests/corpus_probe.cpp`, `tools/corpus.py` and the test helpers.
- Exit: only the legacy view's parity tests and B1's comparison read `LegacyProject`; the 237 cases, the
  comparison and O4 pass on all three platforms; old and new corpus edit paths give equal documents.

**B3.5 — Delete `LegacyProject`, the legacy view, `TrackEdits` and B1's composition** (after B3.3 and B3.4)
- Content and files: the deletions and moves of §4 (`src/legacy_project.*`, `src/legacy_view.*`,
  `src/track_edits.*`, `src/track_identity.*`, `src/tracker_import.hpp` and their tests), the reduced-model
  reader, new `src/project_error.hpp` and the XML adapter, `Qt6::Xml` removed; CMake, CI, `tools/corpus.py`,
  `docs/tracker-document.md`, `docs/native-format.md`, the migration plan and `AGENTS.md`.
- Exit: no `LegacyProject`, `TrackId` or `TrackEdits` symbol remains; `Qt6::Xml` is not linked; every native
  fixture opens to its recorded document; the wave-1 gate passes on Windows, macOS and Linux.

## 8. Open questions

1. **Should preserved (Unknown) tracks be rows?** Recommended: yes, read-only, with their class and a preserved
   marker. Tracker lists every track, and the plan asks MoTrack to show what it preserves but does not edit.
2. **Which order do rows follow?** Recommended: the export order for typed and Unknown tracks alike (the panel's
   `tracks` in file order, then created point masses, then created vectors, each in creation order), so the list
   matches Tracker's after a round trip. It replaces the reduced model's point masses then vectors, and
   `trackOrder` then holds every live track, which changes B2.1's validation. Decide before B2.5 stores
   `trackOrder` in v7; otherwise B3.2 migrates the stored order.
3. **Should undo and redo move the selection?** Recommended: yes, to the one live track an entry addresses, so
   the inspector shows what changed; other entries keep it. Tracker's panel keeps its selection on undo, but its
   views select a restored track.
4. **Is the selection saved, and what is selected on open?** Recommended: B3 saves nothing; export keeps B1's
   audited `selectedtrack` cleanup, and persistence belongs to C7's saved views
   ([#348](https://github.com/mgradwohl/MoTrack/issues/348)). On open, B3 selects the track the saved
   `selectedtrack` names when it binds to one row, following Tracker without writing anything.
5. **Where does field metadata live?** Recommended: what validation and science need in the Qt-free descriptor;
   labels, sections and widget choices in the workspace's presentation table, guarded by the completeness test,
   so `motrack_model` holds no translatable UI text.
6. **Does B3 edit per-frame values numerically?** Recommended: no. Tracker edits marks in world coordinates, which
   needs C1's coordinate system and C4's reference frames; C1 and C2
   ([#342](https://github.com/mgradwohl/MoTrack/issues/342), [#343](https://github.com/mgradwohl/MoTrack/issues/343))
   enable entry through B3's element editor.
7. **Does B3 build the records editor** for C5's parameters and functions, C6's filter stack and C8's constants?
   Recommended: no. B3 defines the contract (the same commit, validation, accessibility and fixture rules); the
   first family that needs it builds one generic table.
8. **Does `Qt6::Xml` leave the build?** Recommended: yes, in B3.5. Its only users decode test snippets, which
   `motrack_osp` can build directly, and keeping it invites new QDom code.
9. **Where does the command map live?** Recommended: a user-facing `docs/tracker-commands.md`, linked from the
   README, which B3.3 starts with §3's track commands and I3 completes against A2's menu inventory.
10. **Is a name unique within its family or within the document?** Recommended: within the document, as
    `TrackerPanel.setTrackName` checks every track. MoTrack checks only the family today, so a point mass and a
    vector can share a name that Tracker refuses and name-based references cannot tell apart. Files that already
    hold duplicates still open; only new names are checked.
