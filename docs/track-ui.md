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
save and recovery, and the workspace tests pass. B3's user-visible changes are the track list panel, the
inspector, read-only rows for tracks MoTrack preserves but does not edit, and the selection rules below. Export
bytes change only once, in B3.5, as §1 describes.

## Requirements

| Requirement | What B3 advances |
| --- | --- |
| R04 | Each experiment view keeps its own selection by identity, independent of other tabs, and edits reach the document's own history as commands. |
| R06 | Track selection by identity through undo, redo, rename, deletion and restoration; the per-frame inspector section is the hook for numeric positions, which C2 enables. |
| R21 | A track list dock and an inspector replace the per-property dialogs and show the fields of Tracker's trackbar in a modernized layout; the toolbar's track box stays, driven by the same selection. The compact trackbar default is I3. |
| R25 | Translated labels and choices, locale-aware numbers, labelled and keyboard-reachable editors, errors shown without pop-ups. |
| R02, R03 | Deleting `LegacyProject` and B1's edit composition changes no export decision, and every native file opens to the same document. |

## Starting point after B2

- `MainWindow` lists tracks in `m_TrackPicker`, a `QComboBox` in the Project toolbar: point masses, then vectors
  with a "(vector)" suffix, read from the legacy view. Every track action reads `currentIndex()` and splits it into
  a point-mass or vector index (`selectedVector`). `refreshEditViews(int)` rebuilds the combo box after each edit,
  undo and redo and keeps the old index: with A, B and C, deleting A, selecting C and undoing selects B. AutoTracker
  keeps the index in `AutoTrackSession::track`. A standalone window replaces its whole document in `applyProject`.
- Rename track… and Set mass… open modal `QInputDialog`s guarded by `m_RequestGeneration`; calibration, axes and
  clip settings have their own dialogs. The New point mass and New vector name generators avoid names of their
  own family only.
- After B2.4a, edits pass through `TrackEdits`' index-based façade over B2.2's commands and the consumers in
  [What LegacyProject keeps until B3](tracker-document.md#what-legacyproject-keeps-until-b3) read `legacyView`.
  After B2.4b export composes from typed state, with B1's composition as a required comparison; after B2.5
  native files are version 7, and versions 1–6 open through the reduced model. B2.4a removes B2.1's
  `DocumentParts::trackOrder`; until then B2.2's commands keep it equal to the export order, so B2.5 never reads or
  writes it.
- Production code has had no QDom since B1 ([#294](https://github.com/mgradwohl/MoTrack/pull/294)). The "QDom
  writer" of the plan is B1's edit composition in `tracker_writer.cpp` and `tracker_rename.cpp`, which descends
  from it: it diffs a fresh `LegacyProject` import against the live one. QDom remains only in test snippet
  decoding and the prerequisite probe (§4).

## 1. Track list and selection

### Rows

`model::trackRows(const TrackerDocument&)` (Qt-free, `motrack_model`) returns one `TrackRow` per live element of
the panel's `tracks`: its `ObjectId`, family, exact class, label (`loadedName`, or the Unknown object's label), its
kind (below), and whether one of its reference slots dangles because the target was deleted. Removed objects and
the singletons (panel, clip, video, clip control, coordinate system) are not rows; C1 and C6 add entries that
select the coordinate system and the clip in the same inspector. Child `ParticleDataTrack`s are not saved in
`tracks` and are never rows.

**Order.** Rows follow the export order: the panel's `tracks` in file order, then created point masses, then
created vectors (B1's order), each by ascending identity. That is the order Tracker shows after a round trip.
Tracker itself appends a created track in creation order (`TActions.addTrack` → `TrackerPanel.addTrack` →
`addDrawable`), so during a session the lists differ when point masses and vectors are created alternately. B3.5
switches export and rows together to ascending identity across families, which is creation order, as a documented
change of the bytes exported for documents that create both kinds. No separate presentation order exists, so rows
have one source: B2.4a removes `DocumentParts::trackOrder` and its validation, and its v1–6 conversion compares
each family's tracks by identity, so a stored list order that differs from the export order is not a conversion
failure. Native v7 stores no presentation order either ([B2 design](tracker-document.md#envelope)).

**Groups and kinds.** Rows are grouped as Tracker's Select menu groups them: user tracks, then the tracks that
`TrackerPanel.getUserTracks` leaves out (the axes, calibration and measuring tools and perspective tracks), using
a Qt-free class table in `track_rows.cpp`. In B3 only point masses and vectors are typed; every other row is
preserved and can only be selected and inspected. The marks table and CSV export list every point mass whatever is
selected, so they do not depend on the row kind:

| Action | Typed point mass or vector | Preserved track or tool |
| --- | --- | --- |
| Select, inspect, open's `selectedtrack` | Yes | Yes |
| Rename (F2, menu), inspector edits, Delete track | Yes | No; the inspector is read-only |
| Shift-click marking, Repeat previous mark, Delete mark on this frame | Yes | No; the status bar says MoTrack does not edit this kind of track yet |
| AutoTracker and plot | Point masses, as now | No; the plot says it has no data for the track |

`TrackListModel` (a `QAbstractListModel` in `motrack_workspace`) holds the rows with their runtime identity and
`DocumentStamp`. `refresh(document, stamp)` does nothing for an unchanged stamp; otherwise it diffs the rows by
`ObjectId` and emits removals, insertions, moves and `dataChanged`, never a reset, so views keep their scroll
position and assistive technology does not reread the list. Roles: label, family icon, an accessible text naming
the family ("Vector: Vector 1"), a tooltip with the class and any read-only reason, `ObjectIdRole`, `FamilyRole`
and `KindRole`. A row index never outlives the event handler that read it. A wave-2 family needs no list code: its
objects become typed rows, and its slice moves its classes in the enablement table.

### Selection

Each `MainWindow` owns a `TrackSelection` on the GUI thread: the document's runtime identity and its assignment
count, and `std::optional<ObjectId> current`. The assignment count advances on every assignment of the document
value (replacement in `applyProject`, Revert, a later in-place reimport that restarts identities at 1) and never on
commands or non-edit updates; B3.2 adds it to the owner if B2.4a does not expose one. When either part changes,
the model resets and the Open rule applies, so an `ObjectId` is never looked up in another document. The runtime
identity, assignment count and `ObjectId` together are a track's key. Selection is
view state: never a command, never in history or the modified state, not saved in B3. The list view and the
toolbar box are both driven from `TrackSelection`, not through a shared `QItemSelectionModel`, which a
`QComboBox` does not reliably share.

| Event | Selection afterwards |
| --- | --- |
| Open, a new assignment, editor recreated after Hide or Revert | The first row, in list order, whose original saved name equals the saved `selectedtrack`, as `TrackerPanel.getTrack(name)` returns the first match; else the first typed user track; else none. |
| Create (`Insert`) | The created track. |
| Delete the selected track (`Remove`) | None, as in Tracker, so a later Shift-click cannot mark a neighbour by accident. |
| Rename, field edit, deleting another track | Unchanged; the view scrolls to keep the row visible. |
| Undo or redo that restores a track (undoing a deletion, redoing a creation) | The restored track. |
| Any other undo or redo | Unchanged, and the status bar names the track the entry changed; none if it removed the selected track. |
| Selecting another tab | Each editor keeps its own selection, as each Tracker tab does. |

The undo rules need the identities a history entry addresses: B3.2 adds `DocumentHistory::affectedObjects(entry)`
if B2.2 does not expose them. Anything that hands a track to another view (C7's copy and paste, plots in other
windows) carries the runtime identity with the `ObjectId`.

### Commands and stale results

The Tracks menu and the list's keys (F2 renames, Delete deletes) act on `current`; a row's context menu acts on
its row. Tracker's Edit ▸ Delete submenu similarly lists the user tracks and disables locked ones (`TMenuBar`,
`MENU_EDIT`). Each command is one history entry: New point mass and New vector (`Insert`), Rename (`SetField` on
the label), Delete track (`Remove`), Delete mark on this frame (`SetField` of the frame element to absent). The
command refuses an empty name or one already in use anywhere in the document (open question 10). The New name
generators therefore check every live row's label, exactly as the command compares it, so New never fails silently.

Every deferred action captures the full key of what it acts on instead of re-reading the selection when it
completes, and is dropped with a status message when the key no longer matches. A captured `before` alone is not
enough: after a replacement, `ObjectId` 3 of the new document may be another track with the same default mass.

- **Editors** capture the key, field, element key and `before` value when they are filled. A refresh never
  overwrites uncommitted text, and the commit is a `SetField` with the captured `before`, which B2.2 refuses
  when the state has changed meanwhile. A reset (a new runtime identity or assignment) discards uncommitted text
  with a status message, so a later focus-out cannot commit into the new document.
- **Row menus** capture the row's key when they open; **modal flows** (calibration stick, axes, clip settings)
  capture the key and their `before` values before `exec` and recheck them after it, with `m_RequestGeneration`
  still guarding the displayed frame; **plot requests** carry the key of the plotted track.
- **AutoTracker** keeps the key and the registry job token, and applies each match as the family's mark edit with
  `before = absent`. A changed key, a removed track or a frame marked meanwhile stops the run with a message,
  instead of marking whichever track now has the stored index. Selecting another track still stops it.

## 2. Descriptor-driven inspector

### Layout and hosts

The inspector is a dock in each `MainWindow`, beside the track list, and shows the selected track. Its sections
are **Track** (name, mass and other per-track values), **Appearance** (colour, footprint, visibility, trail;
empty until C2 types them), **Frame *n*** (frame-keyed values at the displayed original frame) and **Details**
(read-only fields, collapsed). A preserved track shows its class, label and reference slots. Editors come from one
factory, so I3's compact trackbar and floating number panels (upstream [U5](upstream-delta.md)) can host the same
editors. A row's context menu is Tracker's track menu, built from the same presentation entries: boolean fields as
checkable actions, other fields as items that focus their editor, then Delete.

### Descriptor metadata

B2.1's `FieldDescriptor` gives the id, OSP property and type, kind, `commandEdits`, facet, label flag and loader
default. B3.1 adds the Qt-free semantics that commands and editors share; wave-2 slices add the variants their
fields need under the same names:

| Added to `FieldDescriptor` | Content |
| --- | --- |
| `quantity` | Unit powers of length, mass and time (as the analysis kernels' `UnitPowers`), so velocity, a scale in pixels per length or a frequency is one value; plus `Pixels`, `Angle`, `Frame` (an original frame number), `Count` and `Dimensionless`. The stored unit is given where it is fixed (clip times in milliseconds). Length, mass and time display the panel's labels; angles follow the panel's `radians` setting (C1, [U10](upstream-delta.md)). |
| `domain` | Finite numbers with inclusive or exclusive bounds (mass at least `MINIMUM_POINT_MASS`), integer ranges, the name and unit codecs, choice ids, a choice with parameters per choice (footprints and their sizes), or a colour in the form the field saves (an RGB integer such as `grid_RGB`, or a colour object) with its alpha policy: kept, fixed or a separate field such as `grid_alpha`. |
| `slots` | For reference fields: single, positional (one slot per end or vertex, null meaning free, as the tools' `attachments`) or an ordered list with `maxSlots` (`DynamicSystem.particles`, at most two), each with its `CandidateFilter`. |
| `components` | For frame-keyed fields, the element's members (mark `x`, `y`; vector `xtail` … `ycomponent`; key `xorigin` … `yscale`), each with a quantity and coordinate space (image pixels, y down), or a variable count of points (CircleFitter's marked points). The keyed array may be any property, not only `framedata` (`worldlengths`, `world_coordinates`, RGB region `positions` and `shapes`). |
| `keys` | `Sparse` (each frame stands alone: marks, vector samples) or `HeldUntilNextKey` (coordinate keys, stick and tape ends), with the flags that fix the value for every frame and the components each fixes: `fixedorigin`, `fixedangle` and `fixedscale` on coordinates, `fixedtape` and `fixedlength` on tapes, `fixed` and `fixed_shape` on RGB regions. |
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
| Number with a quantity | Number field: `QLineEdit` with a validator and a unit suffix label; no spin box, which would clamp input silently and create an entry per arrow step | Enter or focus-out. |
| Label (`label = true`) | `QLineEdit` | Enter or focus-out; Escape restores the document's value. The command renames. |
| Text | `QPlainTextEdit` | Focus-out or Ctrl+Enter. |
| Colour | Swatch button opening `QColorDialog`, with alpha only when the policy keeps it | When the dialog is accepted. |
| Choice, choice with parameters | `QComboBox` of translated choices storing the choice id, followed by the chosen choice's parameter editors | On activation; each parameter like its own kind. |
| Single reference | `QComboBox` of the candidates the filter and `cycleIfAdded` allow, plus None | On activation. |
| Positional references | One single-reference chooser per slot, labelled by its position (end 1, end 2, vertex), None meaning free | Each slot. |
| Ordered references | A list of chosen targets with add, remove, move up and move down, bounded by `maxSlots` | Each change. |
| Frame-keyed element | The **Frame *n*** section: the components at the displayed frame, a key-frame indicator, previous and next key, the fixing flags, and Delete at this frame | Each component commits the whole element. |
| Variable-length element, records (parameters, functions, filters, constants) | Extension points, not built in B3 (open question 7) | Same rules. |

Editors carry the object name `inspector.<family>.<field>[.<component>]`, which tests use to find them.

### Validation, errors and commands

Input passes three checks. Parsing (workspace) reads the display locale, then the C locale, and trims a suffix of
the field's own unit as Tracker's `NumberField` does ([U19](upstream-delta.md)); locale-aware input is part of
[#66](https://github.com/mgradwohl/MoTrack/issues/66). `validateField` runs on every change, so the inspector
never offers what the command refuses for its domain. B2.2 then validates the command: the stale `before`, the
family's invariants (unique names, C2's track lock, the timing-provider guard, cycles) and the document's. A
failure shows an icon and a translated message under the editor, sets its accessible description and repeats the
message in the status bar, with no pop-up. The text stays until it is corrected or Escape restores the document's
value. Today's messages ("Enter a finite, positive mass.") become `FieldRefusal` translations.

One commit is one history entry. A value equal to `before` issues nothing, as the dialogs do now; typing never
commits and commits are never merged. An edit that changes several fields is a family recipe returning one
`Batch`, such as C1's fixed-flag toggle or C2's edit of several selected steps. A focus-out commit goes to the
captured object even when the focus left because another track was selected; closing the view drops an invalid
uncommitted value. An editable `QLineEdit` takes the Undo and Redo shortcuts through `ShortcutOverride`, so an
event filter on the inspector's editors lets the editor have them only while it holds uncommitted text; otherwise
the window's Undo and Redo act on the document.

### Per-frame and per-track fields

Scalar fields are per track. Frame-keyed fields appear in **Frame *n*** for the displayed original frame, which
is the frame number Tracker loads, never a clip step; the section refreshes its values on every frame change
without rebuilding widgets. A `Sparse` field shows the element at *n* or that there is none. A
`HeldUntilNextKey` field shows the value in effect, the key it comes from, a toggle that adds a key at *n* (the
effective value) or removes it, and its fixing flags. B3.3 shows marks and vector samples read-only in image
pixels; world units follow in B3.4c with the calibration view (open question 6). Delete at this frame works for
every frame-keyed field a command edits. PointMass `keyFrames` is not a separate editor: it changes in the same
`Batch` as the mark it records.

### Read-only and preserved fields

- Typed fields that no command edits appear under **Details**, read-only but focusable, with a tooltip saying so.
- A field the file does not save shows the loader default marked "(default)" and stays absent on export until
  edited; showing a value never creates an edit.
- A saved value the loader cannot read shows "Unreadable in the file"; replacing an editable one is one edit.
- A preserved track shows "Preserved from the Tracker file; MoTrack does not edit it yet", its class and label,
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
(`TrackerPanel`, `TTrackBar`, `TrackControl`, `TTrack`, `PointMass`, `TapeMeasure`, `TMenuBar`, `TActions`, `Undo`).

**Kept.** One selected track per tab, held as an object; the saved `selectedtrack` restored on open by its first
matching name; a created track selected; a deleted selected track leaving none selected; a renamed track staying
selected; user tracks listed before the axes and tools; the track menu on each track; a name refused when it is
in use by any track; number fields that commit on Enter or focus-out.

**Deliberate differences**, for the user guide:

- A docked track list and inspector replace the track bar's fields, the floating TrackControl and the modal Set
  Name dialog (the 2026-11-22 layout decision); the toolbar's track box stays. Plots follow the selected track
  until I1 gives plot and table views their own track choosers, as `TrackChooserTView` does.
- Every property edit is one undo entry. Tracker does not undo visibility, lock, mass, notes, Automark or track
  creation, and merges several position edits of one selected step into one entry.
- Undoing a deletion restores the same track at its place with its references; Tracker reloads it from XML as a
  new object and appends it at the end of the list ([#287](https://github.com/mgradwohl/MoTrack/issues/287)).
- An undo or redo that restores a track selects it; Tracker's panel keeps its selection (its views select the
  restored track). Other undo and redo keep the selection and name the changed track in the status bar.
- A zero, negative or smaller mass than 1E-30 is refused; Tracker's mass field raises any typed value below its
  `NumberField` minimum of 1E-30, negative ones included, to 1E-30 (`setMass` takes the absolute value only of
  loaded or programmatic values). A mass typed with another unit's suffix is refused; Tracker asks whether to
  change the mass unit, which C1's Units dialog offers in MoTrack ([#342](https://github.com/mgradwohl/MoTrack/issues/342)).
- A blank name is refused with a message; Tracker ignores it but still records an empty undo step.
- Created tracks are listed in B1's export order until B3.5 and in creation order after it; Tracker lists them in
  creation order.
- When the file names no selected track, MoTrack selects the first typed user track, as it does now; Tracker
  selects none.

| Tracker command and location | MoTrack after B3 |
| --- | --- |
| Track ▸ New ▸ Point Mass, Vector; TrackControl's New button | Tracks ▸ New point mass, New vector; the New button of the track list; the toolbar |
| Track ▸ New ▸ other kinds, Clone, Measuring Tools, Calibration Tools | The wave-2 slice that types each kind |
| Track ▸ Select; the track bar's Select button | The track list; the toolbar's track box |
| The track menu, from the track bar's track button, a TrackControl button or a right-clicked step | The row's context menu |
| Toolbar "Show or hide the track control" | View ▸ Tracks (the dock's toggle) |
| Name… (Set Name dialog) | The inspector's Name; Tracks ▸ Rename track…, F2 and the row's menu focus it |
| Track bar mass field "m" | The inspector's Mass; Tracks ▸ Set mass… focuses it |
| Color…, Footprints, Visible, Locked, Notes…; Autostep, Automark, Clear Steps | The inspector and the row's menu, once C2 types these fields |
| Define… | The Data Builder (C8, I5) |
| Delete; Edit ▸ Delete ▸ *track* | Tracks ▸ Delete track; the Delete key in the list; the row's menu |
| Delete Selected Step; Edit ▸ Delete ▸ Selected Step | Tracks ▸ Delete mark on this frame (the displayed frame); step selection is C2 |
| Edit ▸ Delete ▸ All Tracks | Not in B3 (I3's menu inventory) |
| Edit ▸ Undo, Redo, named after the edit | Edit ▸ Undo, Redo, named after the entry ("Undo Mass of Point Mass 1") |
| Track bar point fields (step, t, x, y, r, θ) | The inspector's Frame *n*, read-only until C2 |
| Autotracker… | Tracks ▸ AutoTracker, basic until C6 |
| Clicking a step on the video selects its track | With step selection in C2 (R06, [U8](upstream-delta.md)'s hit tolerance) |

**Code-reading observations**, not reproduced, with no matching MoTrack or upstream issue on 2026-11-22: Tracker's
static name dialog keeps the first track that opened it, so a later rename can act on a deleted track or check
names in another tab; menus, undo and the Notes dialog look tracks up by name; step undo entries go stale once a
deletion is undone. Identities rule these out in B3. B3.3 reproduces the name-dialog case against the pinned JAR
and records it as AGENTS.md requires before the guide cites it; field commits converted with the current frame
instead of the step's go to C2, and the tape key-frame order in `commitMagField` to C3.

## 4. Removing `LegacyProject` and the QDom writer

Uses that B2.4a already converts (sessions, groups, Revert and saved baselines, paired save, archive media
resolution, image packaging, `WorkspaceWindow`'s media check, `DocumentParts::trackOrder`) are not repeated here.
What remains after B2.5:

| Use | Files | Replacement | Slice |
| --- | --- | --- | --- |
| Index-based track list, actions, selection and AutoTracker; counter capacity; the name generators | `main_window.*` (`m_TrackPicker`, `selectedVector`, `refreshEditViews`, rename, mass, delete, mark, repeat, `AutoTrackSession`), `track_identity.*` | `TrackListModel`, `TrackSelection`, commands by `ObjectId`, `TrackerDocument::hasCounterCapacity`, document-wide default names | B3.2 |
| Rename and mass dialogs | `main_window.cpp` | Inspector editors | B3.3 |
| Measurements, CSV and plots by track index; clip and calibration value types | `measurements.*` (`measurementPlot`, `measurementsCsv`, the legacy `supportsConstantClipTimingEdits`), `image_sequence.*` (`Clip`) | Two derived Qt-free views in `motrack_model` (clip frames and timing; the effective calibration from `CoordinatesState`) and a mark series by `ObjectId`; O4 still compares them until C2 | B3.4a |
| Admission checks and the reference index | `tracker_writer.cpp` (table columns, coordinates, coordinate updates, shifted samples, vectors, mass frames), `tracker_rename.cpp` (membership, selection fallback), `tracker_references.*` | The same checks over the typed document; the reference index reads bindings and slots from it | B3.4b |
| Admission of each `.trz` member replacement | `tracker_archive_writer.cpp` (`parseLegacyProject(replacement.xml)`) | `TrackerDocument::importBaseline` of each member, with the same refusal | B3.4b |
| Marks table, canvas marks and arrows, axes overlay, transient measurements, calibration and clip dialogs; world units in the inspector | `main_window.cpp`, `video_canvas.*` (`CalibrationKey`) | Typed reads and B3.4a's views | B3.4c |
| Corpus probe edit scenarios and snapshots; test helpers that edit through `TrackEdits` | `tests/corpus_probe.cpp`, `tools/corpus.py`, `tests/media_vectors.hpp`, `tests/movie_authoring.hpp` | B2.2's `--apply` command files and typed reads. The case that reverses the reduced model's list after creating a point mass has nothing to reverse once rows follow export order; it keeps its create, undo, redo, delete and undo steps and checks the export order | B3.4d |
| Reading native versions 1–6 | `workspace_document.cpp` JSON readers, `typedDocumentFromReduced` and its run-time check that the converted document's legacy view equals the stored model, `keepsRecordedAssociations` | A private reduced-model struct with exactly the v1–6 schema, converted directly, with the same projection-and-compare at run time: golden fixtures cannot cover users' files. `keepsRecordedAssociations` moves with it | B3.5a |
| B1's edit composition and the required comparison | `tracker_writer.cpp` (`renderTrackerProjectImpl`'s diff, the `patch*` functions over `LegacyProject`, `remapIds`, `sortTracks`), `tracker_rename.cpp`, `tools/corpus.py`, CI | Deleted; B2.3's composition is the only one, now in ascending identity across families | B3.5b |
| Legacy view and façade | `legacy_view.*` (`legacyProject`, `LegacyViewCache`, `trackerSourceView`, `importTypedTracker`), `track_edits.*` | Deleted; `importTypedTracker` (size bound, diagnostics, `importBaseline`) moves to the Tracker XML adapter; `DocumentStamp` moves to the document owners | B3.5b |
| B1's import policy over `LegacyProject` | `legacy_project.cpp` (`importTracker`, `parseLegacyProject`, `parseTrackerProject`, `readTrackerProject`, `validateTrackerSource`), `tracker_import.hpp` | `TrackerDocument::importBaseline`; the bounded `parseTrackerXml`, which tabsets use, moves to the XML adapter | B3.5b |
| Types and constants of `legacy_project.hpp` | `Mark`, `PointMass`, `VectorSample`, `VectorTrack`, `Clip`, `Calibration`, `CalibrationKey`, `MIN_POINT_MASS`, `ProjectError`, `MAX_TRACKER_XML_BYTES`, `TrackerReading`, `TrackId`, `TrackKind`, `TrackSourceBinding`, `TrackerSource`; included by `utf8_thread_locale.hpp`, `tracker_tabset.cpp`, `video_canvas.hpp`, `image_sequence.hpp` | The model's families and B3.4a's views; `project_error.hpp`; the XML adapter; the reduced reader; `ObjectId`; writer code reads the document's baseline | B3.5b |
| Tests of the deleted code and of `TrackId` | `legacy_project_test`, `track_edits_test`, `legacy_view_parity_test`, B2.2's `history_parity_test`, `measurements_test`, `motion_reference_test`, `name_binding_test`, `locale_formats_test`, `non_ascii_round_trip_test`, `tracker_references_test`, `workspace_document_test`, `archive_group_test`, `archive_group_save_test`, `tracker_archive_test`, the image and movie package tests, `workspace_test`, `paired_workspace_test` | Scenarios kept against the typed document, with expected values recorded independently rather than by comparison with deleted code | B3.4, B3.5 |
| QDom | `tests/osp_layout.hpp`, `tests/tracker_writer_test.cpp`, `tools/prereqs/probe.cpp` (`QDomDocument`), `Qt6::Xml` in `CMakeLists.txt` and `tools/prereqs/CMakeLists.txt`; the stale QDom note in `tests/corpus/README.md` | Snippets built with `motrack_osp`; `Qt6::Xml` leaves the build and the prerequisite check (open question 8) | B3.5b |

The order keeps one source of truth at each step: the UI stops using indices (B3.2, B3.3); every reader moves to
typed state while the legacy view and B1's comparison still run beside it (B3.4a–d); the v1–6 reader stops using
`LegacyProject` (B3.5a); then the rest is deleted (B3.5b), after the comparison passes on all three platforms on
the merged B3.4 PRs. Each PR reports the remaining `LegacyProject`, `TrackId` and `TrackEdits` uses; B3.5b ends at
zero and removes AGENTS.md's note on them.

## 5. Threading

| Work | Thread | Cancellation and staleness |
| --- | --- | --- |
| Rows, inspector filling, validation, commands, undo and redo | GUI | Synchronous; refreshes are keyed by runtime identity, assignment count and `DocumentStamp`. |
| The Frame *n* section | GUI | Reads one element per field (`FrameSeries::find`) for the displayed frame, not a requested one, so a decode in flight never shows another frame's values. |
| AutoTracker matching | Analysis pool, as now | The registry job token, then the captured runtime identity, `ObjectId` and `before = absent`; a refused command stops the run. |
| Derived values that need evaluation (wave 2 data columns, lengths, model steps) | Workers on snapshots | Keyed by runtime identity, node and effective graph revision and dropped unless `isCurrent`; the editor shows "Calculating…" meanwhile. |

No widget, list model or inspector code runs off the GUI thread; export and saving stay on the save worker; no
worker holds a pointer into a live document or these models; a result for another runtime identity is dropped.

## 6. Testing

Model tests (`motrack_tests`, Qt-free) cover `trackRows` (order, groups and kinds for every fixture and corpus
file) and `validateField` against valid and invalid samples for every descriptor, with the same refusal as B2.2's
`SetField`.

Widget tests run offscreen in a new `TrackUi.*` group of `motrack_workspace_tests` with its own CTest entry and
60-second limit. They use temporary directories and wait with `QTest::qWaitFor` for observable state (the
current row, the inspector's title, the history revision, an error label), never for a sleep or a frame count.

- `QAbstractItemModelTester` watches `TrackListModel` through random create, rename, delete, undo and redo.
- Selection: every row of the selection table, including deleting A from its row menu while C is selected, undo
  and redo of that, renaming C, a file whose `selectedtrack` names two tracks, and replacing the document in a
  standalone window with identities that restart at 1. Two tabs keep independent selections.
- Kinds: with a preserved track selected, every action of the enablement table is disabled or reports why, and
  nothing reaches history; New finds a free default name when a preserved track or the other family holds it.
- Stale results: an AutoTracker match that completes after its track was removed by undo applies nothing and
  stops with its message; a commit whose `before` no longer holds shows the refusal and changes nothing; a refresh
  during typing keeps the typed text; replacing the document while an editor holds text discards it, and neither
  its focus-out, an open row menu nor a pending modal flow edits the new document; Ctrl+Z in an unmodified editor
  undoes the document.
- The generic inspector test iterates families and fields from the descriptors and presentation table. For each
  editable field it sets a valid sample, commits with the keyboard, waits for one history entry and the value,
  undoes and checks the old value; an invalid sample must show the error with an accessible description and
  leave history unchanged; read-only fields refuse input; every editor has a buddy, a non-empty accessible name
  and a label in the English catalog; a German-locale run enters a decimal comma.

A family gets this coverage by registering an `InspectorFixture` (a small synthetic document with one object of
the family and valid and invalid samples per editable field) in `tests/inspector_fixtures.hpp`; the completeness
test fails when a presented family has no fixture or a field no samples. Dragging, wizards and record tables still
need their own tests. B3.4d runs each corpus edit scenario through commands and through the old path and requires
equal typed documents before B3.5b deletes the old path.

## 7. Implementation slices

Each PR depends on merged PRs only and updates the R04, R06, R21 and R25 rows and this status line.

**B3.1 — Descriptor semantics and track rows** (after B2.2; parallel with B2.3, B2.4a, B2.4b and B2.5)
- Content: quantities, domains, slots, components, key policies, `validateField` and the `FieldRefusal` codes
  that B2.2's validation then returns; `trackRows` with its class table. No application change.
- Files: `src/model/descriptors.*`, new `src/model/track_rows.*`, `src/model/commands.*` (refusal codes only),
  `tests/model/descriptor_semantics_test.cpp`, `tests/model/track_rows_test.cpp`, `CMakeLists.txt`. B2.3 also
  adds to `descriptors.*`; separate members and functions keep that conflict textual.
- Exit: every typed field has semantics; `validateField` agrees with `SetField` on every sample; coverage holds.

**B3.2 — ID-based track list and selection** (after B2.4a and B3.1; parallel with B2.4b and B2.5)
- Content: `TrackListModel` and the list dock, `TrackSelection` with its keys and rules, the enablement table,
  track commands by `ObjectId` without the façade, document-wide default names, keyed captured actions and
  AutoTracker, rows in export order. The marks table, plot and canvas may still read the legacy view.
- Files: new `src/track_list_model.*`; `src/main_window.*`, `src/bound_workspace.cpp`, `src/document_session.*`
  (assignment count, if B2.4a has none), `CMakeLists.txt`, `translations/*.ts`, `README.md`; new
  `tests/track_ui_test.cpp`; `tests/workspace_test.cpp`; `main_window.*` is the hot file.
- Exit: the workspace tests pass with only the track box's order and empty state updated; the selection, kind and
  stale-result tests pass; `MainWindow` turns no row index into a track outside the view glue.

**B3.3 — Descriptor-driven inspector** (after B3.2; parallel with B2.5)
- Content: the inspector dock, editor factory and row menus; presentation entries for point masses, vectors and
  preserved tracks; errors, commits, the Undo filter, Frame *n* in pixels and Details; Undo and Redo named after
  their entry; Rename track… and Set mass… focusing their editors; translations, accessibility and
  `docs/tracker-commands.md`.
- Files: new `src/inspector/inspector_panel.*`, `src/inspector/field_editors.*`,
  `src/inspector/field_presentation.*`, `tests/inspector_test.cpp`, `tests/inspector_fixtures.hpp`;
  `src/main_window.*`, `CMakeLists.txt`, `translations/*.ts`, `README.md`.
- Exit: the generic inspector test passes for every presented field, one history entry per commit, and the
  dialog tests are converted.

**B3.4 — Typed reads everywhere else**, four PRs (after B2.4b, B2.5 and B3.2):
- **a** the clip and calibration views and measurements, CSV and plots by `ObjectId` (`src/model/clip_view.*`,
  `src/model/calibration_view.*`, `src/measurements.*`, `src/image_sequence.*`); parallel with B3.3.
- **b** admission checks, the reference index and `.trz` member admission on typed state (`tracker_writer.cpp`,
  `tracker_rename.cpp`, `tracker_references.*`, `tracker_archive_writer.cpp`); parallel with B3.3 and **a**.
- **c** canvas, marks table, dialogs and the inspector's world units (`main_window.cpp`, `video_canvas.*`), after
  B3.3 and **a**.
- **d** the corpus probe and test helpers on commands (`tests/corpus_probe.cpp`, `tools/corpus.py`, the helpers
  and their tests); parallel with the others.
- Exit, for the last of them: only the legacy view's parity tests, B1's comparison and the v1–6 reader read
  `LegacyProject`; the 237 cases, the comparison and O4 pass on all three platforms; old and new corpus edit
  paths give equal documents.

**B3.5 — Remove the legacy model**, two PRs (after B3.3 and B3.4):
- **a** the private reduced-model reader for native versions 1–6 with its run-time projection-and-compare
  (`src/workspace_document*`, `src/legacy_view.*` for the moved conversion, `tests/workspace_document_test.cpp`).
  Exit: every native fixture opens to its recorded document without `LegacyProject`.
- **b** the deletions and moves of §4 (`src/legacy_project.*`, `src/legacy_view.*`, `src/track_edits.*`,
  `src/track_identity.*`, `src/tracker_import.hpp` and their tests, new `src/project_error.hpp` and the XML
  adapter), export and rows in ascending identity with the byte change in `docs/native-format.md`, `Qt6::Xml`
  removed; CMake, CI, `tools/corpus.py`, `tools/prereqs/`, `docs/tracker-document.md`, the migration plan and
  `AGENTS.md`. Exit: no `LegacyProject`, `TrackId` or `TrackEdits` symbol remains; `Qt6::Xml` is not linked;
  the wave-1 gate passes on Windows, macOS and Linux.

## 8. Open questions

The review of [PR #351](https://github.com/mgradwohl/MoTrack/pull/351) adopted these answers.

1. **Preserved (Unknown) tracks are rows**, read-only, grouped by the Qt-free class table and limited by the
   enablement table of §1. Tracker lists every track, and the plan asks MoTrack to show what it preserves.
2. **Rows follow the export order**, with no separately stored presentation order: B2.4a removes
   `DocumentParts::trackOrder`, and B1's created-track order stays until B3.5b, which switches export and rows to
   ascending identity as a documented byte change (§1).
3. **Undo and redo select a track only when they restore it**; otherwise the selection stays and the status bar
   names the changed track. Tracker's panel keeps its selection on undo; the difference is listed in §3.
4. **The selection is not saved in B3**; export keeps B1's audited `selectedtrack` cleanup, and persistence belongs
   to C7's saved views ([#348](https://github.com/mgradwohl/MoTrack/issues/348)). On open, B3 selects the first
   row whose original saved name equals `selectedtrack`, as `TrackerPanel.getTrack(name)` returns the first match.
5. **Field semantics live in the Qt-free descriptor**, and labels, sections and widget choices in the workspace's
   presentation table, guarded by the completeness test, so `motrack_model` holds no translatable UI text.
6. **B3 does not edit per-frame values numerically.** Tracker edits marks in world coordinates, which needs C1's
   coordinate system and C4's reference frames; C1 and C2 ([#342](https://github.com/mgradwohl/MoTrack/issues/342),
   [#343](https://github.com/mgradwohl/MoTrack/issues/343)) enable entry through B3's element editor.
7. **B3 defines the records and variable-length element contracts only** (the same commit, validation,
   accessibility and fixture rules) for C3's circle points, C5's parameters and functions, C6's filter stack and
   C8's constants; the first family that needs each builds one generic editor.
8. **`Qt6::Xml` leaves the build in B3.5b**, including the prerequisite probe (`tools/prereqs/probe.cpp` and its
   CMake file). Its other users decode test snippets, which `motrack_osp` can build directly.
9. **The command map lives in `docs/tracker-commands.md`**, linked from the README, which B3.3 starts with §3's
   track commands and I3 completes against A2's menu inventory.
10. **A name is unique within the document.** `TrackerPanel.setTrackName` compares names exactly (case-sensitive,
    untrimmed) against every track, the axes and tools included; MoTrack checks only the family today, so a point
    mass and a vector can share a name that Tracker refuses and name-based references cannot tell apart. The
    command and the New name generators apply the rule; files that already hold duplicates still open.
