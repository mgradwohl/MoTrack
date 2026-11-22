# TrackerDocument, commands and native version 7 (slice B2)

This is the design and implementation plan for slice B2 of wave 1 in the
[full-port plan](migration-plan.md#implementation-order-and-exit-gates): "`TrackerDocument` behind
`WorkspaceDocument::project`, ID-addressed commands, native v7 with v1–6 readers; `LegacyProject` read-only."
**None of it is implemented yet.** The sections below describe the intended behavior and name the existing
code each part replaces. B3 then moves the track list, selection and inspector to identities and deletes
`LegacyProject` and the legacy writer; B4 makes O3–O8 required checks. Wave 2 families build on this model and
on the [dependency graph](dependency-graph.md) (C0), a library that no loader or command uses yet.

The wave-1 exit gate holds after every slice: the 237 edited actual-Tracker cases, all native migrations,
paired save and recovery, and the workspace tests pass. B2 intends no user-visible change except the
persistent export note of [#317](https://github.com/mgradwohl/MoTrack/issues/317).

## Requirements

| Requirement | What B2 advances |
| --- | --- |
| R02 | Every typed object and every element of the panel's `tracks`, typed or not, has a persistent identity bound to its source node. Unknown content stays byte-preserved, reference slots of untyped classes bind to identities, and native v7 keeps all of it across reopen, including stale associations (#317). |
| R03 | Standard export is composed from typed state by field descriptors, separately from the Tracker-behavior checks; O8 also covers bytes outside nodes ([#316](https://github.com/mgradwohl/MoTrack/issues/316)); every corpus case adds a native v7 reopen before export. |
| R04 | Each document has its own ID-addressed history and save points; worker snapshots share immutable state and never borrow a live document. |
| R26 | Loading, migration, composition and v7 encoding run off the GUI thread with the existing cancellation and stale-result rules; paired saves write v7 companions. |
| R07, R08, R10, R19 | Relationships only: deleting and restoring a vector-sum member, center-of-mass member, attachment target, booster or two-body member keeps every reference bound or explicitly dangling. Nothing is evaluated. |

## Starting point after B1

- `LegacyProject` ([legacy_project.hpp](../src/legacy_project.hpp)) is the editable model: point masses,
  independent vectors, clip, calibration and unit labels. It is built from B1's typed loads
  ([tracker_loaders.hpp](../src/tracker_loaders.hpp)), which keep each object's `osp::NodeId`; the loads are then
  discarded.
- `TrackEdits` ([track_edits.hpp](../src/track_edits.hpp)) records one variant per kind of edit and resolves GUI
  list positions to a `TrackId` when an edit is recorded.
- The standard writer ([tracker_writer.cpp](../src/tracker_writer.cpp)) imports the preserved XML again, diffs it
  against the live `LegacyProject` and composes [`SourceEdits`](../src/source_edits.hpp). Each patch function mixes
  composing edits with checking Tracker behavior.
- Native v5 and v6 store the original XML, the reduced model as JSON and, since v4, bindings as ordinals in
  the panel's `tracks` collection ([native format](native-format.md)).
- Sessions, paired saving and Revert treat the model as an opaque value compared with `==`
  ([workspace guide](workspace-sessions.md#saved-models-and-per-experiment-revert)).
- Media indexing and archive resolution write `clip.sourceFrameCount`, `stepCount` and `mediaReferences`
  directly, outside history (`WorkspaceSession::applyMovieIndex`, `workspace_input.cpp`, `MainWindow`,
  `TrackerArchive::resolveMedia`).

## 1. Type model

### Document

`model::TrackerDocument` is a copyable value in a new target, `motrack_model`, that links `motrack_osp` and
`motrack_dependencies`. B1's typed loaders move into it unchanged. It holds:

| Part | Content |
| --- | --- |
| Baseline | `std::shared_ptr<const osp::Source>` of the imported Tracker XML, or none for media-only documents and native v1. It is immutable and shared by every copy. |
| Associations | `Known`, `Unknown` (native v2 and v3) or `Stale` (#317): whether typed objects are bound to baseline nodes. |
| Objects | Ordered slots, each with an `ObjectId`, class, binding (a baseline `NodeId` or none), state (`Live` or `Removed`) and a `std::shared_ptr<const T>` typed value that is copied on write. |
| Counter | `nextObjectId`, the never-rewinding allocator that replaces `nextTrackId` with the same values. |
| Media extension | The two clip values Tracker XML does not hold in this form: the current media reference spellings and the discovered source frame count (today `Clip::mediaReferences` and `Clip::sourceFrameCount`). Decoded frame times, orientation, alignment, archive ownership and runtime media locations stay in `WorkspaceDocument`. |
| Graph | A `std::shared_ptr<const DependencyGraph>`, derived, copied on write and never serialized. |
| Revision | A monotonic count of every mutation, including updates outside history. Not serialized. |

Equality, which saved baselines and Revert use, compares the baseline bytes, associations, objects, counter
and extension, never the graph or the revision.

### Object families in B2

B2 types only the classes B1 loads. Everything else in the baseline is preserved, not modeled.

| Family | Tracker class | Editable fields | Read-only typed fields |
| --- | --- | --- | --- |
| Panel | `TrackerPanel` | `length_unit`, `mass_unit`, `time_unit` | `semantic_version`, `description`; reference fields `referenceframe`, `selectedtrack` |
| Clip | `VideoClip` | `startframe`, `stepsize`, `stepcount`, `starttime` | `video_framecount`, `frameshift` |
| Video | `ImageVideo`, `XuggleVideo` and other video classes | none; current spellings live in the media extension | class, `path`, `paths`, `delta_t` |
| Clip control | `StepperClipControl`, `VideoClipControl` | `delta_t` | `rate`, `frame`, `looping` |
| Coordinates | `ImageCoordSystem` | `framedata` keys by original frame | fixed flags, `locked` |
| Point mass | `PointMass` | `name`, `mass`, `framedata` marks by original frame, with `keyFrames` | none |
| Vector | `Vector` | `name`, `framedata` samples by original frame | none |
| Unknown | any other `tracks` element | none | class, label (TTrack's `name` when readable), reference slots |

An object that B1's import policy cannot present loads as an Unknown object of its class, so the legacy view
shows what it shows today. Examples are a vector whose samples fail to load and a coordinate system with
unusable keys or under a `referenceframe`. An edit that the reduced model allows on such a document, such as
calibrating, inserts a native-only typed object that standard export refuses, as it does now.

**Reference-only descriptors.** Untyped classes that store track names get descriptors for those slots only,
taken from the audit behind the [source reference index](../src/tracker_references.hpp): `CenterOfMass.masses`,
`VectorSum.vectors`, the tools' `attachments`, `CircleFitter.step_attachment`, model `booster` and
`DynamicSystem.particles`. Their owners stay Unknown and byte-preserved. Their slots bind to identities through
C0's `NameIndex` and `CandidateFilter` and become `MemberOf`, `AttachedTo` and `BoosterOf` edges, added with
`CyclePolicy::Record`; the panel's `referenceframe` is a `FrameOf` edge. Selections, plot and table owners and
plot guests are view references, not edges, as the
[graph design](dependency-graph.md#edge-kinds-and-tracker-fields) states.

### Field descriptors

Each family has one static descriptor table. A field descriptor gives:

- a stable field id, used by commands, native v7 and dumps, and the OSP property name and type;
- the Java accessor semantics, Tracker's loader default ([#214](https://github.com/mgradwohl/MoTrack/issues/214))
  and whether the value was saved, so an unsaved default stays absent on export unless it is edited;
- whether the field is editable, its domain check and its graph facet (`Definition` for a mass, `Geometry`
  for marks and keys);
- its JSON codec, its O3 saver-tree form and its patch writer.

Frame-keyed fields (`framedata`) map Tracker's loaded frame number to a value. They are read with
`PropertyView::indexedElements()`, so the last duplicate index wins as in Java
([#314](https://github.com/mgradwohl/MoTrack/issues/314), [#315](https://github.com/mgradwohl/MoTrack/issues/315)).
Frame elements keep their own nodes, so unknown properties on retained frames and typed-null padding survive edits.

### Identities

`model::ObjectId` ([object_id.hpp](../src/model/object_id.hpp)) identifies every typed singleton and every
element of the panel's `tracks`, typed or Unknown. Marks, vector samples and calibration keys are not objects;
commands address them by object, field and original frame. Views, functions and filters receive identities
when C7, C8 and C6 type them.

Allocation is deterministic, so a fresh import and a migrated native file agree. Editable point masses and
vectors take the values B1 assigns (1, 2, … in source order). Unknown tracks follow in source order, then the
singletons: panel, clip, video, clip control and coordinates. Migration keeps the stored track IDs and
allocates the rest from the stored counter in the same order. The counter never rewinds, and an exhausted
counter disables creation, as now. Until B3, `TrackId` remains the legacy view's alias for the value.

### Source bindings and preserved content

A bound object holds the `NodeId` of its object node in the baseline; `Source::identity()` guards against
mixing sources. Unknown objects, unknown properties of typed objects and untyped panel properties are never
decoded into the typed state and never enter an edit set. `osp::render` therefore copies their bytes, and
O8 verifies them on every render. A typed field that returns to its baseline value produces no edit and keeps
its original text, as B1's writer does. Removing an object keeps its slot and binding in the `Removed` state:
undo restores the same node, and export removes exactly that element. Created objects have no binding and
export appends them. Standard export never replaces the baseline; the document keeps rendering against the
original import, as today.

### Ownership and lifetime

The live document belongs to its `DocumentSession` or archive-group member and changes only on the GUI
thread. Workers receive copies. A copy shares the baseline, the typed values and the graph until one side
writes, so a snapshot costs one copy of the slot table. A `NodeId` is valid while its `Source` lives, and every
holder keeps the source alive through the shared pointer. Archive payloads keep their existing shared ownership.

### Behind `WorkspaceDocument::project`

B2.4 changes the type of `WorkspaceDocument::project` from `LegacyProject` to `model::TrackerDocument`. The
field name stays, so session, group, paired-save and recovery code keep their structure. `trackerSource` and
`trackerBindingsReset` move into the document as its baseline and associations; writer code that still takes a
`TrackerSource` receives a view. Updates that are not edits become explicit document operations:
`adoptMediaExtent` for indexing and `resolveArchiveMedia` for archive members. They advance the document
revision but not the history revision, so they do not mark the document modified, as now. Saved baselines
become `std::shared_ptr<const TrackerDocument>`, and Revert keeps its rules: the counter's high-water mark and
a discovered movie extent survive restoration.

### What `LegacyProject` keeps until B3

`legacyView(const WorkspaceDocument&)` returns a `const LegacyProject&` built from the typed state with B1's
import policy (sorting, refusals, and `unsupportedTrackTypes` from the Unknown objects), cached by document
revision. Its consumers stay as they are in B2: the index-based track list, marks table and plots in
`MainWindow`, `measurements.cpp`, image-sequence opening, AutoTracker, the corpus probe, and the
Tracker-behavior checks in `tracker_writer.cpp`, `tracker_rename.cpp` and `tracker_references.cpp`. Nothing
writes through the view. `TrackEdits` keeps its index-based methods as a façade over the commands below. B3
replaces these consumers with ID-based ones and deletes the view, the façade, `LegacyProject` and B1's edit
composition.

## 2. ID-addressed commands

### Command set

| Command | Addresses | Graph | Inverse |
| --- | --- | --- | --- |
| `SetField` | Object, field, optional element key (an original frame), expected `before` and new `after`; an absent value removes the element | `markChanged` on the field's facet; `renameObject` for a name | `SetField` with the values exchanged |
| `Insert` | A complete object with a fresh ID and its position in `tracks` or its singleton slot | `addObject` and the object's edges, with `CyclePolicy::Reject` | `Remove`, keeping the object |
| `Remove` | Object | `removeObject`, storing the returned `RemovedObject`; references to the object dangle | `restoreObject` with `CyclePolicy::Record`, reinserted at the stored position with the same binding |
| `Batch` | Ordered commands, recorded as one history entry | Each member in order | The members' inverses in reverse order |

Today's edits map onto these: creation is `Insert`; rename, mass, unit labels, marks and vector samples are
`SetField`; calibration (keys and length label together) and clip range or timing are each a `Batch`; deletion
is `Remove`. `DocumentHistory` keeps `TrackEdits`' per-output save heads, `isModified`, `invalidateSaved` and a
history revision that advances on every edit, undo, redo and reset; `SaveState` checkpoints keep using it.

### Validation and atomicity

A command is validated completely before anything changes:

- the target is live and of the descriptor's family, the field is editable and the element key is in range;
- `before` equals the current value, so a command built from stale state is refused;
- the value passes the descriptor's domain check: finite numbers, Tracker's frame bounds, the unit and name
  codecs, and strings that OSP can represent ([#194](https://github.com/mgradwohl/MoTrack/issues/194));
- family invariants hold for the candidate object, such as constant clip timing and the timing-provider guard
  `supportsConstantClipTimingEdits`, and document invariants hold: live IDs are unique and below the counter;
- for `Insert` and new reference edges, `cycleIfAdded` finds no cycle.

The commit cannot stop half way. New object values are built first and the history slot is reserved; the
commit then swaps pointers. Graph mutations can throw only `std::bad_alloc`, after which the graph is rebuilt
from the committed document, because it is derived. A `Batch` validates each member against the state that the
previous members produce and commits once. A refused command leaves the document, the history, both revisions
and the save heads unchanged. Undo and redo check the same preconditions, the current value against the
entry's `after` or `before`, and refuse without moving the head, as `TrackEdits` does now.

### Undo, redo and dependencies

Undo and redo apply inverses with `CyclePolicy::Record`, so they restore an earlier state even when it holds a
cycle read from a file. Redoing a creation restores the stored object and its ID instead of allocating a new
one. `Remove` keeps the binding and the `RemovedObject`; undoing it brings back the same identity, its own edges
and every reference that dangled, and export writes the original node again. A history belongs to one live
document and is cleared when the document is replaced; native files store no history.

### Deleting referenced objects

Removing an object that other objects reference stays allowed in the native document, as it is today. The
references remain in the graph as dangling, and standard export refuses while any of them dangles, because
`renderName` returns `Dangling`; the refusal names each owner, field and removed name. A later
family command can instead make an explicit, reversible edit in the same `Batch`, such as removing a
center-of-mass member or clearing an attachment. View references keep B1's audited cleanup: selection
fallback and removal of plot and table owners. This rules out the reference defects recorded for Tracker:

| Issues | Tracker behavior | Why it cannot occur |
| --- | --- | --- |
| [#251](https://github.com/mgradwohl/MoTrack/issues/251), [#286](https://github.com/mgradwohl/MoTrack/issues/286) | Tools and models other than the selected one keep a deleted target, and a resave writes its name. | Every reference to the identity dangles and blocks export, whichever view is open. |
| [#252](https://github.com/mgradwohl/MoTrack/issues/252), [#288](https://github.com/mgradwohl/MoTrack/issues/288) | Attachments and two-body members resolve all or nothing. | Each slot binds on its own at load and is written back verbatim while faithful. |
| [#287](https://github.com/mgradwohl/MoTrack/issues/287) | Undoing a deletion reloads the XML as a new object, losing memberships. | Undo restores the same identity, which binds every dangling reference again. |
| [#289](https://github.com/mgradwohl/MoTrack/issues/289)–[#291](https://github.com/mgradwohl/MoTrack/issues/291) | Cycles in files or through indirect choices recurse. | Loaders record cycles and plans report them; new edits that close one are refused. |

### From state to `SourceEdits`

Commands never touch XML. Standard export composes its edits from the difference between the typed state
and the baseline, so undoing back to the imported state exports the original bytes:

| Difference | Composition |
| --- | --- |
| A scalar field differs from its baseline value | The descriptor's typed setter on the property, or an insertion in Tracker's saver order when the file did not save it |
| A frame-keyed element is added, changed or removed | B1's frame-array patcher: retained frame objects are edited in place, new `[n]` elements inserted, removed ones deleted, and `keyFrames` updated |
| A bound object is `Removed` | Its `tracks` element is removed after the name and view-reference patches, so ordinal paths stay valid |
| A live object has no binding | Appended with the family's saver (PointMass: `mass`, `name`, `color`, `footprint`, `visible`, `trail`, then `framedata` and `keyFrames`) and checked by O7 |
| A name changed or a referenced object was removed | `renderName` for every reference to that identity; any status other than `Faithful` refuses |

The patch functions in `tracker_writer.cpp` currently combine edits and checks. B2.3 separates them: the
descriptors compose, and the checks that encode Tracker's behavior become admission checks on the composed
output and the legacy view, run before anything is written. These are the numeric and timing validation, frame
budgets, snapping, coordinate inversion, the [#63](https://github.com/mgradwohl/MoTrack/issues/63) and
[#64](https://github.com/mgradwohl/MoTrack/issues/64) guards, the audited schemas and the name codec.
`SourceEdits::render` then verifies O8, which #316 extends to the prolog, the epilog and the bytes between
unedited siblings.

## 3. Native version 7

### Envelope

Version 7 keeps v5's containers: plain JSON, or a ZIP with exactly `project.json` and `tracker/source.trz`. Its
metadata replaces the reduced model:

| Key | Content |
| --- | --- |
| `format`, `version` | `"MoTrack"`, 7 |
| `requires` | Feature names that a reader must implement; empty in B2 |
| `nextObjectId` | A decimal string |
| `baseline` | `null`, `{"xmlBase64": …}`, or `{"archiveMember": …}` when the bytes are that member of `tracker/source.trz`, so archive groups stop storing each member twice |
| `associations` | `known`, `unknown` or `stale`; `stale` keeps the earlier bindings verbatim in `staleBindings` |
| `objects` | The identity sidecar: one entry per identity with its ID, class, baseline path or none, and `live` or `removed` |
| `state` | Every editable field of every live typed object, in full, by descriptor field id, and the order of `tracks` |
| `media` | The media extension and the `WorkspaceDocument` media fields that v5 stores today |
| `extensions` | Optional named MoTrack data, written back unchanged by readers that do not know it |

A baseline path lists the child ordinals from the root to the object, each with its property name; it
generalizes v4's `sourceIndex`. On open, each path must resolve with the current reader to an object of the
recorded class, each addressable object must have exactly one entry, IDs must be unique and below the counter,
and removed entries must still resolve. Editable values are stored in full rather than as differences, so if
a future reader sees the baseline differently, the associations become stale but no measurement is lost.
Once import applies `frameshift` ([#313](https://github.com/mgradwohl/MoTrack/issues/313)), frame-keyed fields
hold Tracker's loaded frame numbers and the baseline keeps the file's. The writer decodes its output before
replacing a file, as now. The graph and the history are not stored. On open the loader retires the removed
identities (`retireIdentity`), and a reference to one binds through the baseline's original names, so it stays
dangling.

Archive groups put the same body for each member in a version-7 group envelope (`kind: "archive-group"`) with
the existing UUIDs and archive digest.

### Reading versions 1–6

Every version opens into a `TrackerDocument`, and the reduced JSON is never written again. Converting the
reduced model is a function of B2.1, used by the native reader from B2.4.

| Version | Baseline | Associations | Identities |
| --- | --- | --- | --- |
| 1 | None | None: native-only objects from JSON | Fresh |
| 2 | XML | `unknown`: native-only objects; the baseline is preserved but not indexed | Fresh |
| 3 | XML | `unknown` | Stored track IDs kept |
| 4 | XML | `known`: bound objects take their JSON values, unbound JSON tracks become created objects and bound IDs missing from the JSON become removed objects; `stale` when the bindings no longer match the current reader | Stored IDs kept; the rest allocated after the counter |
| 5 | XML or an archive member | As version 4 | As version 4 |
| 6 | A group of version-5 bodies | As version 4, for each member | As version 4 |

Loader defaults (#214) and the time-unit recovery apply as they do now. Documents without known associations
keep native-only editing and refuse standard export with the re-import note.

### Stale associations (#317)

Today a native resave writes `bindings: null`, and the note that export is unavailable exists only in the
session that dropped the bindings. In v7 the earlier bindings stay verbatim in `staleBindings` with
`associations: "stale"` and are ignored at run time. The note is derived from the saved state, a baseline
without known associations, so it appears on every open. Only importing the original Tracker file again, which
replaces the document, clears them.

### Forward compatibility

- A reader refuses a higher `version`, or a `requires` entry it does not implement, with a message naming the
  newer MoTrack, and changes nothing. Builds before B2.5 refuse v7, as builds before v5 refuse v5.
- An `extensions` entry that a reader does not know is written back unchanged. Extension data that holds object
  IDs or depends on typed state must be listed in `requires`, because a writer unaware of it cannot keep it valid.
- A field id or class in `state` that the reader does not know is refused, never dropped; a newer build that
  types another class lists it in `requires`.
- Standalone and group readers refuse each other's envelopes, as now.

## 4. Threading

| Work | Thread | Cancellation and staleness |
| --- | --- | --- |
| Parsing, typed loading, graph building, native decoding and migration | The existing open workers (`prepareWorkspaceInput` and the single-window open) | The existing open flags, checked between objects and archive members; the GUI thread adopts or discards the prepared input |
| Commands, undo, redo, non-edit updates and the legacy view | GUI thread | Synchronous; worker results such as an index apply only through an accepted job token |
| Composition, admission checks, rendering, O8 and v7 encoding | Save worker, on an owned snapshot | The existing save flags; `SaveState` checkpoints and observation order reject stale or replaced completions |
| Evaluation by the wave-2 families | Workers, on snapshots | Results keyed by node and effective graph revision, dropped unless `isCurrent` |

No worker holds a pointer into a live document, and only the GUI thread updates widgets.

## 5. Oracles and tests

| Slice | Oracle | Checks |
| --- | --- | --- |
| B2.1 | O3 for typed classes | `motrack_corpus_probe --state` dumps each typed object's descriptor saver tree from `TrackerDocument`; the [gap baseline](oracle-dumps.md#gap-baseline) is recorded again and each closed gap reviewed. The legacy view of every corpus and fixture file equals B1's import. |
| B2.2 | O6, MoTrack side | A JSON form of commands and `motrack_corpus_probe --apply ops.json`. Seeded random command sequences check that undoing everything returns an equal document and that the incrementally maintained graph equals a rebuilt one. The Java applier belongs to B4. |
| B2.3 | O7, O8 | Descriptor savers for created objects; #316's checks, with a seam that feeds doctored output; byte equality between the descriptor composition and B1's on every writer test and corpus export. |
| B2.4 | O5, existing routes | The 237 cases through the descriptor composition, with B1's composition kept as a test-only shadow. |
| B2.5 | O5, native route | Every corpus round trip also saves v7, reopens it and exports again; that export equals the export before closing. |

O4 does not change in B2: data columns still come from `measurements.cpp` over the legacy view until C2.

**Native migrations.** Small committed fixtures cover each version: v1; v2 and v3 with a source; v4 with known
and with stale (pre-B1) bindings; v5 plain and archive-backed; and a v6 group. Each is written by the build
that introduced its version where that build still compiles, otherwise by hand from the documented schema,
and its provenance is recorded. Each opens to an expected document dump, saves as v7 and reopens equal. Where
its associations are known, its export equals a recorded B1 export byte for byte; refusals (v1–v3 and stale)
are checked together with their note. The fixtures land with B2.1, so they pin today's reading before the
switch.

**No silent loss** rests on these checks: legacy-view parity; O8 on every render; equality between the two
compositions; equal typed state, baseline bytes and sidecar after every v7 round trip; sidecar completeness;
the gap ratchet; and a preservation census that requires every baseline node to be bound to a typed field or
to lie inside an Unknown object or an untyped panel property, and never to be in an edit set unless a command
changed it.

## 6. Implementation slices

Order: B2.1, then B2.2 and B2.3 in parallel, then B2.4, then B2.5. Each later slice depends on merged slices
only, so no stack is more than one PR deep. Each slice updates the R02–R04 rows of the migration plan and the
status line of this document.

**B2.1 — Typed document** (first)
- Content: the `motrack_model` target; `TrackerDocument`, families, descriptors and reference-only descriptors;
  identity allocation, bindings and graph population; conversion from the reduced model for native v1–v6; the
  cached `legacyView`. No application change.
- Files: new `src/model/tracker_document.*`, `src/model/families.*` and `src/model/descriptors.*`;
  `src/tracker_loaders.*` moved into the target; new `src/legacy_view.*` in `motrack_project`; `CMakeLists.txt`;
  `tests/model/tracker_document_test.cpp`, `tests/model/legacy_view_parity_test.cpp`, the native fixtures under
  `tests/fixtures/native/` with provenance in `tests/fixtures/README.md`; `tests/corpus_probe.cpp`,
  `tests/corpus/state-gaps.json`; `docs/oracle-dumps.md`; a legacy-view benchmark.
- Exit: parity for every corpus file, fixture and native fixture; the O3 dump comes from the typed document;
  the 90% coverage gate holds.

**B2.2 — Commands and history** (after B2.1, parallel with B2.3)
- Content: the four commands, validation, `DocumentHistory` and graph updates. Nothing is wired to the UI.
- Files: `src/model/commands.*`, `src/model/document_history.*`; `tests/model/commands_test.cpp`,
  `tests/model/history_parity_test.cpp` (replays the `track_edits_test.cpp` scenarios through commands and
  compares the legacy view with `TrackEdits`), `tests/model/command_sequence_test.cpp`;
  `tests/corpus_probe.cpp` for `--apply`.
- Exit: parity with `TrackEdits` in every scenario, including refusals and the counter; deleting and restoring
  members, attachment targets and boosters in corpus files keeps their references.

**B2.3 — Descriptor composition** (after B2.1, parallel with B2.2)
- Content: composing `SourceEdits` from typed state; admission checks split from the patch functions; #316.
  Production export still uses B1's composition.
- Files: new `src/tracker_compose.*`; `src/tracker_writer.cpp` and `src/tracker_rename.cpp`, split without a
  change in behavior; `src/source_edits.*`; `tests/tracker_compose_test.cpp`, `tests/source_edits_test.cpp`;
  `tools/corpus.py` for the shadow comparison.
- Exit: byte equality with B1's composition on every writer test and corpus export; every doctored output
  refused.

**B2.4 — Switch-over** (after B2.2 and B2.3)
- Content: `WorkspaceDocument::project` becomes `TrackerDocument`; the `TrackEdits` façade; sessions, groups
  and Revert on typed baselines; non-edit updates as document operations; export through B2.3, with B1's
  composition as a test-only shadow. Native files are still v5 and v6, written from the legacy view.
- Files: `src/workspace_document.*`, `src/document_session.*`, `src/archive_group_session.*`,
  `src/workspace_session.*`, `src/workspace_input.cpp`, `src/main_window.*`, `src/bound_workspace.cpp`,
  `src/track_edits.*`, `src/track_identity.*`, `src/tracker_writer.*`, `src/tracker_archive.*`,
  `src/tracker_image_package.cpp`, `src/paired_save.cpp`, `src/measurements.*`. Existing tests change only
  where they build a `LegacyProject` directly.
- Exit: the full wave-1 gate on Windows, macOS and Linux CI with no user-visible change; the two-document
  isolation tests pass.

**B2.5 — Native version 7** (after B2.4; its codec needs only B2.1 and can be written alongside)
- Content: the v7 reader and writer for standalone documents and groups; #317.
- Files: new `src/native_document.*`; `src/workspace_document.cpp`, `src/workspace_document_codec.hpp`,
  `src/archive_group.cpp`; `tests/workspace_document_test.cpp`, `tests/archive_group_test.cpp`;
  `tools/corpus.py` for the native route; `docs/native-format.md`.
- Exit: every native fixture passes; the native O5 route passes for every corpus case; the paired-save journal
  tests pass with v7 companions.

### Open pull requests

- **[#169](https://github.com/mgradwohl/MoTrack/pull/169), tabsets**, being ported onto `motrack_osp`, changes
  `workspace_input.*`, `workspace_context.*`, `workspace_window.*`, `tests/corpus_probe.cpp`, `tools/corpus.py`,
  `tests/test_corpus.py`, `CMakeLists.txt` and the migration plan. B2.1 to B2.3 conflict with it only in text
  (CMake, probe, corpus tool). B2.4 also conflicts in meaning in `workspace_input.cpp`, where a tabset's member
  imports would have to use the new media operations. Landing #169 before B2.4 is simpler; otherwise the later
  PR adapts. A tabset remains a workspace source like an archive group, not a `TrackerDocument`.
- **[#259](https://github.com/mgradwohl/MoTrack/pull/259), opening from the OS**, changes `workspace_context.*`,
  `workspace_services.*`, `workspace_window.*`, `main.cpp`, CMake, CI and documentation. It overlaps B2 only in
  `CMakeLists.txt` and status text, so the merge order does not matter.
- Every slice edits the R02 and R03 rows, so those edits are serialized with other open PRs that touch them.

## 7. Open questions

1. **Does v7 store the re-rendered current XML, as the [architecture table](migration-plan.md#target-layers)
   says?** Recommended: no. Store the import baseline and the full editable typed state, as above. A re-rendered
   XML would still need a second representation for state without a Tracker XML form: native-only and stale
   objects, and native replacements of hidden objects such as a calibration under a reference frame. It would
   also drop the original names through which unknown content still refers to removed objects, and an edited
   value restored later would no longer get its original text back. The current XML is a deterministic render
   of baseline and state, so not storing it loses nothing. B2.5 updates that table row.
2. **Where do #313–#315 land?** Recommended: in one small loader-fidelity PR before B2.1. #313 and #314 are
   scientific defects that should not wait, and B2.1's parity baseline should already include their fixes.
3. **Should removing a referenced object be refused?** Recommended: no. Allow it in the native document with
   the dangling references reported and export refused, as today, so B2 changes nothing visible; the families
   add explicit cleanup edits.
4. **Should untyped classes get graph edges in B2?** Recommended: yes, through reference-only descriptors.
   Deleting and undoing need them now, and the reference index already audits these fields.
5. **How is the legacy view maintained?** Recommended: rebuilt lazily once per document revision. B2.1's
   benchmark on the largest corpus experiment decides whether incremental updates are needed.
6. **May `motrack_model` use Qt?** The unit and name codecs classify code points with `QChar`. Recommended:
   public headers use only standard types, and the target links Qt Core privately for that classification,
   which AGENTS.md allows. A separate Unicode table could disagree with the classification in use today.
7. **When does export switch to the descriptor composition?** Recommended: in B2.4, keeping B1's composition as
   a test-only shadow that B3 deletes.
8. **Is an AutoTracker run one `Batch`?** Recommended: not in B2. Each mark stays one undo step, and C6 decides
   with its interface.
