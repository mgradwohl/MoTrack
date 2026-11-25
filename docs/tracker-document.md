# TrackerDocument, commands and native version 7 (slice B2)

This is the design and implementation plan for slice B2 of wave 1 in the
[full-port plan](migration-plan.md#implementation-order-and-exit-gates): "`TrackerDocument` behind
`WorkspaceDocument::project`, ID-addressed commands, native v7 with v1–6 readers; `LegacyProject` read-only."
**Status:** B2.1 is implemented ([PR #337](https://github.com/mgradwohl/MoTrack/pull/337)): `motrack_model` holds the
typed document, its families, descriptors, identities, bindings and graph, and `motrack_project` the cached legacy
view and the conversion from the reduced model. B2.2 adds the ID-addressed commands and `DocumentHistory`
([commands.hpp](../src/model/commands.hpp), [document_history.hpp](../src/model/document_history.hpp)), at parity with
`TrackEdits`. B2.3 is implemented
([PR #355](https://github.com/mgradwohl/MoTrack/pull/355), [`tracker_compose.hpp`](../src/tracker_compose.hpp)): the descriptor composition makes B1's decision, with B1's bytes,
on every writer test, corpus case and fixture, and O8 covers the bytes outside nodes (#316); production export still
uses B1's composition. Nothing in the application uses the commands or the descriptor composition yet. B2.4 and B2.5
are not implemented. The sections below describe the intended behavior and name the existing code each part replaces. B3
then moves the track list, selection and inspector to identities and deletes
`LegacyProject` and the legacy writer; B4 makes O3–O8 required checks. Wave 2 families build on this model and
on the [dependency graph](dependency-graph.md) (C0), which the typed document builds and the commands maintain.

The wave-1 exit gate holds after every slice: the 237 edited actual-Tracker cases, all native migrations,
paired save and recovery, and the workspace tests pass. B2 intends no user-visible change except the
persistent export note of [#317](https://github.com/mgradwohl/MoTrack/issues/317) and opening, rather than
refusing, native files whose preserved source the current reader cannot read.

## Requirements

| Requirement | What B2 advances |
| --- | --- |
| R02 | Every typed object and every element of the panel's `tracks`, typed or not, has a persistent identity bound to its source node. Unknown content stays byte-preserved, reference slots of untyped classes bind to identities, and native v7 keeps all of it across reopen and reader changes, including stale associations (#317). |
| R03 | Standard export is composed from typed state by field descriptors, separately from the Tracker-behavior checks; O8 also covers bytes outside nodes ([#316](https://github.com/mgradwohl/MoTrack/issues/316)); every corpus case adds a native v7 reopen before export. |
| R04 | Each document has its own ID-addressed history, save points and never-rewinding revisions; worker snapshots share immutable state and never borrow a live document. |
| R26 | Loading, migration, composition and v7 encoding run off the GUI thread with the existing cancellation and stale-result rules; paired saves write v7 companions. |
| R07, R08, R10, R19 | Relationships only: deleting and restoring a vector-sum member, center-of-mass member, attachment target, booster or two-body member keeps every reference bound or explicitly dangling. Nothing is evaluated. |

## Starting point after B1

- `LegacyProject` ([legacy_project.hpp](../src/legacy_project.hpp)) is the editable model, built from B1's typed
  loads ([tracker_loaders.hpp](../src/model/tracker_loaders.hpp)), which keep each object's `osp::NodeId` and are then
  discarded. `TrackEdits` ([track_edits.hpp](../src/track_edits.hpp)) resolves list positions to a `TrackId`
  when an edit is recorded.
- The standard writer ([tracker_writer.cpp](../src/tracker_writer.cpp)) diffs a fresh import of the preserved XML
  against the live model and composes [`SourceEdits`](../src/model/source_edits.hpp); its patch functions mix edits with
  Tracker-behavior checks, and it refuses unless importing its output gives exactly the intended project. Renames
  and deletions also rewrite reference slots of untyped owners and saved views ([tracker_rename.cpp](../src/tracker_rename.cpp)).
- Native v5 and v6 store the original XML, the reduced model as JSON and, since v4, bindings as ordinals in
  `tracks` ([native format](native-format.md)). Sessions, paired saving and Revert treat the model as an opaque
  value compared with `==` ([workspace guide](workspace-sessions.md#saved-models-and-per-experiment-revert)).
- Media indexing and archive resolution write `clip.sourceFrameCount`, `stepCount` and `mediaReferences`
  outside history (`WorkspaceSession::applyMovieIndex`, `workspace_input.cpp`, `MainWindow`,
  `TrackerArchive::resolveMedia`).

## 1. Type model

### Document

`model::TrackerDocument` is a copyable value in a new target, `motrack_model`. It holds:

| Part | Content |
| --- | --- |
| Baseline | `std::shared_ptr<const osp::Source>` of the imported Tracker XML, or none. It is immutable and shared by every copy. |
| Associations | `None` (no baseline), `Known`, `Unknown` (native v2 and v3) or `Stale` (#317, a refused baseline or a reader conflict): whether typed objects are bound to baseline nodes. |
| Objects | Ordered slots, each with an `ObjectId`, class, binding (a baseline `NodeId` or none), state (`Live` or `Removed`) and a `std::shared_ptr<const T>` typed value that is copied on write. Frame-keyed fields are stored in shared chunks, so an edit copies one chunk rather than a whole track. |
| Counter | `nextObjectId`, the never-rewinding allocator for typed tracks that replaces `nextTrackId` with the same values, and the countdown low-water mark for all other identities. |
| Media extension | The two clip values Tracker XML does not hold in this form: the current media reference spellings and the discovered source frame count (today `Clip::mediaReferences` and `Clip::sourceFrameCount`). Decoded frame times, orientation, alignment, archive ownership and runtime media locations stay in `WorkspaceDocument`. |
| Graph | A `std::shared_ptr<const DependencyGraph>`, derived, copied on write and never serialized. |

Equality, which saved baselines and Revert use, compares the baseline (pointer first, then bytes),
associations, objects, counter and extension, never the graph.

**Layering.** `motrack_model` links `motrack_osp` and `motrack_dependencies` and holds the typed loaders (moved
from `motrack_project` unchanged), `SourceEdits` (already Qt-free, moved so that descriptors can patch without a
dependency cycle), the descriptors, commands and history, the unit and name codecs, and the timing-provider
guard `supportsConstantClipTimingEdits`. Its public headers use only standard types; Qt Core is linked privately
for the codecs' `QChar` classification. `motrack_project` keeps the native JSON codec, the legacy view, the
admission checks and the media adapters.

### Object families in B2

B2 types only the classes B1 loads. Everything else in the baseline is preserved, not modeled. Every typed field
is stored in native v7; "other" fields below are those no command edits.

| Family | Tracker class | Fields commands edit | Other typed fields |
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
calibrating, inserts a native-only typed object that standard export refuses, as it does now; its fixed flags
are typed fields like any other. In a document without a usable baseline (v1–v3, stale), Unknown objects are
class-only placeholders taken from the stored unsupported-class list, so `unsupportedTrackTypes` and the
timing-provider guard keep their values.

**Reference-only descriptors.** Untyped classes that store track names get descriptors for those slots only,
taken from the audit behind the [source reference index](../src/tracker_references.hpp): `CenterOfMass.masses`,
`VectorSum.vectors`, the tools' `attachments`, `CircleFitter.step_attachment`, model `booster` and
`DynamicSystem.particles`. Their owners stay Unknown. Each slot, like every other baseline reference, binds
through C0's `NameIndex` and `CandidateFilter` by its original name against the baseline's original labels, so
later renames and removals never change what it binds to. Slots become `MemberOf`, `AttachedTo` and `BoosterOf`
edges, added with `CyclePolicy::Record`; the panel's `referenceframe` is a `FrameOf` edge. Selections, plot and
table owners and plot guests are view references, not edges, as the
[graph design](dependency-graph.md#edge-kinds-and-tracker-fields) states.

### Field descriptors

Each family has one static descriptor table. A field descriptor gives:

- a stable field id, used by commands, native v7 and dumps, and the OSP property name and type;
- the Java accessor semantics, Tracker's loader default ([#214](https://github.com/mgradwohl/MoTrack/issues/214))
  and whether the value was saved, so an unsaved default stays absent on export unless it is edited;
- whether a command edits the field, its domain check and its graph facet (`Definition` for a mass,
  `Geometry` for marks and keys);
- its value as a Qt-free `FieldValue`, which the native layer maps to JSON, its O3 saver-tree form and its
  patch writer.

Frame-keyed fields (`framedata`) map Tracker's loaded frame number to a value. They are read with
`PropertyView::indexedElements()`, so the last duplicate index wins as in Java
([#314](https://github.com/mgradwohl/MoTrack/issues/314), [#315](https://github.com/mgradwohl/MoTrack/issues/315)).
Frame elements keep their own nodes, so unknown properties on retained frames and typed-null padding survive edits.

### Identities

`model::ObjectId` ([object_id.hpp](../src/model/object_id.hpp)) identifies every typed singleton and every
element of the panel's `tracks`, typed or Unknown. Marks, vector samples and calibration keys are not objects;
commands address them by object, field and original frame. Views, functions and filters receive identities
when C7, C8 and C6 type them. Identities come from two directions, so track identities stay exactly what B1
assigns:

- **Typed tracks** take the counter: point masses and vectors get 1, 2, … in source order at import, creation
  takes the next value, the counter never rewinds and exhaustion disables creation, as now.
- **Unknown tracks and all other objects** (the singletons and an inserted coordinate system) count down from
  2^64 − 1 in source order, skipping every value a counter identity in the document uses, live, removed or
  recorded in a binding. The lowest value handed out is the countdown low-water mark, which never rises.
  Creation from the counter stops below it, so the counter's capacity shrinks by their number and opening never
  fails for lack of identities.

An identity is classified by the range it was allocated from, which the sidecar records, never by its current
class. A class that a later build types, or a vector that only a newer reader loads, keeps its countdown
identity and stays valid. The rule that the counter exceeds every live ID applies to counter identities only.
The counter, the parity tests and native v5/v6 output therefore concern counter identities only. Until B2.5,
countdown identities are derived again on each open and nothing persistent refers to them; they change only if
a file's counter identities reach the top of the range, which only crafted files do. From B2.5 the sidecar
stores them and the envelope stores the low-water mark, so an inserted coordinate system never reuses a value
after undo or reopen. Until B3, `TrackId` remains the legacy view's alias for the value.

### Source bindings and preserved content

A bound object holds the `NodeId` of its object node in the baseline; `Source::identity()` guards against
mixing sources. A typed field that returns to its baseline value produces no edit and keeps its original text,
as B1's writer does. Removing an object keeps its slot and binding in the `Removed` state: undo restores the
same node, and export removes exactly that element. Created objects have no binding and export appends them.
Standard export never replaces the baseline; the document keeps rendering against the original import.

Untyped content enters an edit set in only two audited ways, both B1's: a reference slot of an untyped owner
(center-of-mass member, attachment, booster, system particle) that `renderName` rewrites when its target is
renamed, and the saved-view cleanup (packed `selected_track_views`, view selections, plot and table owners and
guests) when a target is renamed or removed. Everything else is copied verbatim by `osp::render` and checked by
O8: other properties of Unknown objects, unknown properties of typed objects, untyped panel properties, and the
prolog, epilog and gap bytes.

### Ownership, revisions and caches

The live document belongs to its `DocumentSession` or archive-group member and changes only on the GUI
thread. Workers receive copies; a copy shares the baseline, typed values and graph until one side writes. A
`NodeId` is valid while its `Source` lives, and every holder keeps the source alive through the shared
pointer. Archive payloads keep their existing shared ownership.

Revisions belong to the live owner, not to the value, so copying or restoring a value cannot move them
backward. The owner's `DocumentHistory` keeps the runtime identity and two monotonic counters: the history
revision, as `TrackEdits` has today, and a document revision that also counts updates outside history and every
assignment (replacement, Revert). On assignment, a new C0 operation always marks every node of the incoming
graph changed at max(incoming graph revision + 1, outgoing live graph revision + 1), so `isCurrent(node, R)`
rejects anything computed before. The floor comes from the graphs, not the document revision, because a command
advances the graph's revision several times. Assignment never skips this step, even when the incoming graph's
revision is already high, because a node's effective revision can still be low; only an explicit floor call
ignores a floor at or below the graph's own revision. Caches, including the legacy view and later evaluation results, are keyed by (runtime identity,
document revision), and snapshots carry that stamp. Revert keeps the runtime identity, so that key protects
against a rewind only because the document revision always advances on assignment. The allocators never rewind
either: an assignment or an update takes the higher counter and the lower countdown low-water mark of the outgoing
and incoming values (`installedValue`), so a reverted value keeps the counter's high-water mark and no identity a
discarded or recorded edit holds is handed out again. A history belongs to the live document it is first given and
refuses any other.

### Behind `WorkspaceDocument::project`

B2.4a changes the type of `WorkspaceDocument::project` from `LegacyProject` to `model::TrackerDocument`. The
field name stays, so session, group, paired-save and recovery code keep their structure. `trackerSource` and
`trackerBindingsReset` move into the document as its baseline and associations; writer code that still takes a
`TrackerSource` receives a view. Updates that are not edits become explicit document operations:
`adoptMediaExtent` for indexing and `resolveArchiveMedia` for archive members. They advance the document
revision but not the history revision, so they do not mark the document modified, as now. Saved baselines
become `std::shared_ptr<const TrackerDocument>`, and Revert keeps its rules: the counter's high-water mark and
a discovered movie extent survive restoration.

### What `LegacyProject` keeps until B3

`legacyView(const DocumentSession&)` (and its group-member form) returns a `std::shared_ptr<const LegacyProject>`
built from the typed state with B1's import policy: sorting, refusals, and `unsupportedTrackTypes` from the
Unknown objects. The cache belongs to the live owner, is keyed as above and is never shared between copies; the
first view is built on the open worker. Its consumers stay as they are in B2: the index-based track list, marks
table and plots in `MainWindow`, `measurements.cpp`, image-sequence opening, AutoTracker, the corpus probe, and
the Tracker-behavior checks in `tracker_writer.cpp`, `tracker_rename.cpp` and `tracker_references.cpp`. Nothing
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
is `Remove`. A mark is a `SetField` of `framedata`, batched with one of `keyFrames` when B1's key rule changes them:
for a bound point mass the baseline's keys without its removed marks and with every mark that differs from the
baseline's, for a created one every mark (`setMark`). `DocumentHistory` keeps `TrackEdits`' per-output save heads,
`isModified` and `invalidateSaved`; `SaveState` checkpoints keep using the history revision. A command that advances
the counter leaves every saved output stale, as creation does today; the countdown low-water mark is not saved before
native v7.

### Validation and atomicity

A command is validated completely before anything changes:

- the target is live and of the descriptor's family, a command edits the field, and the element key is in range;
- `before` equals the current value, so a command built from stale state is refused;
- the value passes the descriptor's domain check: finite numbers, Tracker's frame bounds, the unit and name
  codecs, and strings that OSP can represent ([#194](https://github.com/mgradwohl/MoTrack/issues/194));
- family invariants hold for the candidate object, such as constant clip timing and the timing-provider guard,
  and document invariants hold: counter identities lie below the counter, countdown identities at or above the
  low-water mark, and no ID is used twice;
- for `Insert` and new reference edges, `cycleIfAdded` finds no cycle.

The new object values, chunks and graph are built on copies first and the history slot is reserved; the
commit is then a sequence of `noexcept` pointer swaps, so it cannot stop half way. A `Batch` validates each
member against the state that the previous members produce and commits once. A refused command leaves the
document, the history, both revisions and the save heads unchanged. Undo and redo check the same
preconditions, the current value against the entry's `after` or `before`, and refuse without moving the head,
as `TrackEdits` does now. The domain checks apply to new values only, since an undo restores whatever the object
held, an unreadable baseline value included. The clip's invariants apply in every direction, except that an undo
that restores exactly the timeline its command started from is accepted, so a legacy timing that is not constant
returns as it was (`TrackEdits`' rule).

Deliberate differences from `TrackEdits`, which the B2.4a façade keeps or reconciles: a command must change something,
so a calibration that changes nothing records no undo entry, where `TrackEdits` records one; and new names and units
pass the codecs, so creating a track with a name standard export cannot store (empty, surrounding white space, the
packed view separators) is refused, where `TrackEdits` accepts any name on creation and export refuses it later. A new
object saves only the fields commands edit (and a coordinate system's fixed flags); a key list returns to an unsaved
or unreadable value only when that is the baseline's own reading.

### Undo, redo and dependencies

Undo and redo apply inverses with `CyclePolicy::Record`, so they restore an earlier state even when it holds a
cycle read from a file. Redoing a creation restores the stored object and its ID instead of allocating a new
one. A removed object that is bound keeps its slot in the `Removed` state; an unbound one (a created track, an
inserted coordinate system) leaves the document while its history entry keeps it, its identity kept from reuse by the
allocators, and returns to the place its entry recorded. `Remove` keeps the binding and the `RemovedObject`; undoing
it brings back the same identity, its own edges and every reference that dangled, and export writes the original
node again. A history belongs to one live
document and is cleared when the document is replaced; native files store no history.

### Deleting referenced objects

Removing an object that other objects reference stays allowed in the native document, as it is today. The
references remain in the graph as dangling, and standard export refuses while any of them dangles, because
`renderName` returns `Dangling`; the refusal names each owner, field and removed name. A later family command
can instead make an explicit, reversible edit in the same `Batch`, such as removing a center-of-mass member or
clearing an attachment. View references keep B1's audited cleanup: selection fallback and removal of plot and
table owners. This rules out the reference defects recorded for Tracker:

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
| Media references differ from the baseline's for the destination | The media adapters' path patches (`patchLinkedImagePaths`, `patchLinkedMoviePath` with decoded frame times), from the media extension, the `WorkspaceDocument` media fields and the destination |
| A bound object is `Removed` | Its `tracks` element is removed after the name and view-reference patches, so ordinal paths stay valid |
| A live object has no binding | Appended with the family's saver (PointMass: `mass`, `name`, `color`, `footprint`, `visible`, `trail`, then `framedata` and `keyFrames`) and checked by O7 |
| Any label changed, or an object was created or removed | `renderName` for every reference in the document: typed reference fields, slots of untyped owners and view references; any status other than `Faithful` refuses |

The last row is deliberately broad. Creating a track named like an unresolved saved name, such as "C" while an
Unknown center of mass holds "C", makes Tracker bind that name on reload; `renderName` reports `Rebinds` and
export refuses, which today only B1's unique-name schema guard achieves. Export order is B1's: bound objects in
baseline order, then created point masses, then created vectors, each in ascending identity. The track list
follows the export order, and no separate presentation order is stored; B3.5b switches both to ascending identity
across families ([track UI design](track-ui.md#rows)).

The patch functions in `tracker_writer.cpp` currently combine edits and checks. B2.3 separates them: the
descriptors compose, and the checks that encode Tracker's behavior become admission checks on the composed
output and the legacy view, run before anything is written. These are the numeric and timing validation, frame
budgets, snapping, coordinate inversion, the [#63](https://github.com/mgradwohl/MoTrack/issues/63) and
[#64](https://github.com/mgradwohl/MoTrack/issues/64) guards, the audited schemas and the name codec. The last
admission check stays in production, as B1's reimport comparison does today: the typed load of the output,
with identities mapped through the export order, must equal the intended typed state of every object. O8 covers
only the edit set; this check covers what O8 cannot, an intended edit that was never composed.

`SourceEdits::render` then verifies O8. #316 extends it to the prolog, the epilog and the gaps between siblings
under one rule: a removal deletes only the removed element's own lines, an insertion adds only its own lines
after the preceding sibling or the container's opening line, and every other gap stays byte-identical. B2.3
first confirms that `osp::render` follows this rule, or states the rule it does follow, and O8 then enforces it.
`osp::render` follows it with one precision, which O8 enforces: a removal deletes the element's lines with the line
break of its last line; an insertion at index *i* goes just before the first line of original sibling *i*, so after
the whole gap that precedes it, or just before the closing line when appending; a renamed element is rewritten in
place with the file's line ending; and a self-closing container that receives elements is reopened. O8 also keeps
an edited container's opening and closing lines and the text around an edited value.

As built in B2.3, a scalar the file did not save is appended to its owner, as B1 appends it, rather than inserted in
Tracker's saver order, because the exit criterion requires B1's bytes; insertion in saver order can follow the switch.
The clip range (`startframe`, `stepsize`, `stepcount`) and the clip timing (`starttime` and the clip control's
`delta_t`) are each composed as one batch, as B1 writes them: when one field differs, every field of the batch is
written, an unsaved one with Tracker's loader default. The reimport check compares every typed field except the
panel's `selectedtrack`, a view reference that the audited cleanup rewrites. `renderName` covers the references the
graph holds (typed reference fields and slots of untyped owners); view references (selections, plot and table owners,
plot guests) are not graph edges, so the last row of the table above covers them through B1's audited cleanup and
its reference checks, not through `renderName`. Mark edits must keep the key frames with the marks they change: every
changed or new mark is a key frame and no removed one is, which B1 computes and the descriptor composition checks.

## 3. Native version 7

### Envelope

Version 7 keeps v5's containers: plain JSON, or a ZIP with exactly `project.json` and `tracker/source.trz`. Its
metadata replaces the reduced model:

| Key | Content |
| --- | --- |
| `format`, `version` | `"MoTrack"`, 7 |
| `requires` | Feature names that a reader must implement; empty in B2 |
| `readerRevision` | The loader revision of the writer. MoTrack advances it whenever a loader change can read an unchanged baseline differently. A version-5 body, standalone or in a v6 group, has the same number as its `trackerReading` ([native format](native-format.md#tracker-reading-revisions): absent is 0, #330 writes 2); v1–v4 count as revision 0 |
| `nextObjectId`, `countdownLowWater` | The counter and the countdown low-water mark, as decimal strings |
| `baseline` | `null`, `{"xmlBase64": …}`, or `{"archiveMember": …}` when the bytes are that member of `tracker/source.trz`, so archive groups stop storing each member twice. The 32 MiB XML limit applies to both forms; `archiveMember` requires the container payload and must name one of its project members |
| `associations` | `none`, `known`, `unknown` or `stale`; `stale` keeps earlier bindings verbatim in `staleBindings` |
| `objects` | The identity sidecar: one entry per identity with its ID, allocation range (`counter` or `countdown`), class, baseline path or none, and `live` or `removed` |
| `state` | Every typed field of every live typed object, in full, by field id, and for each edited field the writer's reading of the baseline as `base` (below). Also the class list of placeholders. No presentation order: the track list follows the export order |
| `media` | The media extension and the `WorkspaceDocument` media fields that v5 stores today |
| `extensions` | Optional named MoTrack data, written back unchanged by readers that do not know it |

A baseline path lists the child ordinals from the root to the object, each with its property name; it
generalizes v4's `sourceIndex`. On open, each path must resolve with the current reader to an object of the
recorded class, each addressable object must have exactly one entry, IDs must be unique and lie in their
recorded range (counter identities below the counter, countdown identities at or above the low-water mark), and
removed entries must still resolve; otherwise the associations are stale. Values are stored in full,
so stale associations never lose a measurement. Once import applies `frameshift`
([#313](https://github.com/mgradwohl/MoTrack/issues/313)), frame-keyed fields hold Tracker's loaded frame numbers
and the baseline keeps the file's. The writer decodes its output before replacing a file, as now. The graph and
history are not stored; on open the loader retires removed identities (`retireIdentity`), and a reference to one
binds through the baseline's original names, so it stays dangling.

**Edited fields.** A scalar field is edited when its value differs from the writer's reading of the baseline;
`base` is that reading. A frame-keyed field is edited when any element differs, and its `base` lists every
differing element with the writer's reading: a changed element with its old value, an inserted one as `absent`,
and a deleted one as a tombstone `{frame, absent, base: reading}`, because a deleted element has no entry in
`state` that could carry it.

**Reader changes.** With the writer's `readerRevision`, `state` applies as written. With another reader, newer or
older (the rules are symmetric):

- a field without `base` was unedited and takes this reader's value, so loader fixes reach it;
- an edited field keeps its stored values, tombstones included, when every entry of its `base` equals this
  reader's reading and every element not listed in `base` equals this reader's reading too; any other difference
  is a conflict, so the whole field keeps its stored values and all its elements stay in one frame space, the
  associations become stale and the note is shown (otherwise an edit at frame 5 would keep an old misreading of
  unedited frame 3, and export would write it silently);
- a bound live typed object without a `state` entry, for example a class that a newer build types, takes the
  current reading;
- a baseline that the reader refuses opens the document native-only from `state`, with the bytes kept opaque,
  associations stale and export refused, instead of failing to open.

Archive groups put the same body for each member in a version-7 group envelope (`kind: "archive-group"`) with
the existing UUIDs and archive digest.

### Reading versions 1–6

Every version opens into a `TrackerDocument`, and the reduced JSON is never written again. Converting the
reduced model is a function of B2.1, used by the native reader from B2.4a.

| Version | Baseline | Associations | Identities |
| --- | --- | --- | --- |
| 1 | None | `none`: native-only objects from JSON | Fresh |
| 2 | XML | `unknown`: native-only objects; the baseline is preserved but not indexed | Fresh |
| 3 | XML | `unknown` | Stored track IDs kept |
| 4 | XML | `known`: bound objects take their JSON values, unbound JSON tracks become created objects and bound IDs missing from the JSON become removed objects; `stale` when the bindings no longer match the current reader | Stored track IDs kept as counter identities; others from the countdown |
| 5 | XML or an archive member | As version 4 | As version 4 |
| 6 | A group of version-5 bodies | As version 4, for each member | As version 4 |

The reduced JSON of v4–v6 follows the reader revision its `trackerReading` names, and every file written before
#330 follows revision 0, the reader before #313/#314. Applying it as edits would turn that reader's misreadings into
edits, so the current v5 native reader migrates it on open
([native format](native-format.md#tracker-reading-revisions)). B2.1's conversion consumes that reader's output and
takes the body's (migrated) `trackerReading` as its `readerRevision`. It must not transform the values again: a
migrated body already holds the current reading, and an outdated one keeps its earlier values on purpose.

- **`frameshift` (#313, revision 1):** Tracker's `adjustFrameData` moves track `framedata` only. An array of Java
  length 1 is unchanged, an array no longer than the shift loads empty, and otherwise saved index *n* loads at frame
  *n* − `frameshift`. Coordinate keys never shift, single or multi-key, because `TrackerPanel` loads `coords` without
  the adjuster; a negative shift moves only the clip start.
- **Duplicate keys (#314, revision 1):** a calibration that revision 0 dropped takes the baseline's reading.
- **Track slots (part of #315, revision 2):** track arrays take Java's slots, so the last of duplicate indices wins, a
  null one included.
- **Migration rule:** all or nothing per document. A body migrates only when every stored value these fixes cover
  equals its revision's reading of the baseline, every track is bound and none was created or deleted, the
  shift drops no stored sample and moves none off its clip step, and the calibration (where #314 applies) is absent in
  both. Otherwise no stored value changes and none becomes native-only. The associations are dropped, export
  refuses, the note persists, and `trackerReading` keeps the earlier revision; a calibration created in MoTrack
  for such a source is handled the same way, not kept as an edit. Export of changed or new samples of a shifted
  source is refused until [#328](https://github.com/mgradwohl/MoTrack/issues/328) decides how frames map back to
  saved indices, including for objects created in MoTrack.
- **Empty `mass_unit` (#315):** planned as revision 3. A stored `kg` where the file saved an empty label is the old
  default, not an edit, and becomes empty.
- **Inputs Java cannot load (#315):** still read as before #330. Coordinate arrays whose indices Java cannot place
  import without calibration, and such track arrays keep file order. B2 proposes refusing the baseline and opening the
  document native-only from its JSON with associations stale.
Loader defaults (#214) and the time-unit recovery apply as they do now. Documents without known associations
keep native-only editing and refuse standard export with the re-import note.

### Stale associations (#317)

Today a native resave writes `bindings: null`, and the note that export is unavailable exists only in the
session that dropped the bindings. In v7 the earlier bindings stay verbatim in `staleBindings` with
`associations: "stale"` and are ignored at run time. The note is derived from the saved state (a baseline
without known associations), so it appears on every open. Only importing the original Tracker file again, which
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
| Parsing, typed loading, graph building, native decoding, migration and the first legacy view | The existing open workers (`prepareWorkspaceInput` and the single-window open) | The existing open flags, checked between objects and archive members; the GUI thread adopts or discards the prepared input |
| Commands, undo, redo, non-edit updates and later legacy views | GUI thread | Synchronous; worker results such as an index apply only through an accepted job token |
| Composition, admission checks, rendering, O8 and v7 encoding | Save worker, on an owned snapshot | The existing save flags; `SaveState` checkpoints and observation order reject stale or replaced completions |
| Evaluation by the wave-2 families | Workers, on snapshots | Results keyed by runtime identity, node and effective graph revision, dropped unless `isCurrent` |

No worker holds a pointer into a live document, and only the GUI thread updates widgets.

## 5. Oracles and tests

| Slice | Oracle | Checks |
| --- | --- | --- |
| B2.1 | O3 for typed classes | `motrack_corpus_probe --state` dumps each typed object's descriptor saver tree from `TrackerDocument`; the [gap baseline](oracle-dumps.md#gap-baseline) is recorded again and each closed gap reviewed. The legacy view of every corpus and fixture file equals B1's import. |
| B2.2 | O6, MoTrack side | A JSON form of commands and `motrack_corpus_probe --apply ops.json`. Seeded random command sequences check that undoing everything returns an equal document, that revisions only grow, and that the incrementally maintained graph equals a rebuilt one. The Java applier belongs to B4. |
| B2.3 | O7, O8 | Descriptor savers for created objects; #316's checks, with a seam that feeds doctored output. On every writer test and corpus export the descriptor composition makes B1's accept or refuse decision and, where both accept, equal bytes. |
| B2.4b | O5, existing routes | The 237 cases through the descriptor composition, with the comparison against B1's composition a required CI test. |
| B2.5 | O5, native route | Every corpus round trip also saves v7, reopens it and exports again; that export equals the export before closing. A test-only loader variant simulates newer and older readers: unedited fields follow them; a field with only deletions keeps them; a field with one conflicting element, or with an edit at one frame while the reader reads an unedited frame differently, keeps all its stored values and makes the document stale. A class typed only by the variant keeps its countdown identity and opens with known associations. |

O4 does not change in B2: data columns still come from `measurements.cpp` over the legacy view until C2.

**Native migrations.** Small committed fixtures cover each version: v1; v2 and v3 with a source; v4 with known
and with stale (pre-B1) bindings; v4 with `frameshift`, duplicate keys, an empty `mass_unit` and a baseline the
current reader refuses; v5 plain and archive-backed; and a v6 group. Each is written by the build that
introduced its version where that build still compiles, otherwise by hand from the documented schema, and its
provenance is recorded. Each opens to an expected document dump, saves as v7 and reopens equal. Where its
associations are known, its export equals a recorded B1 export byte for byte; refusals (v1–v3, stale) are
checked together with their note. The fixtures land with B2.1, so they pin today's reading before the switch.

**No silent loss** rests on these checks: legacy-view parity; O8 on every render; the reimport check; the
decision and byte comparisons with B1; equal typed state, baseline bytes and sidecar after every v7 round trip;
sidecar completeness; the gap ratchet; and a preservation census. The census assigns every baseline node and
every byte between nodes to one class: typed field, reference slot of an untyped owner, node of the audited
view cleanup, unknown property of a typed object, content of an Unknown object or untyped panel property, or
prolog, epilog and gap bytes. Only the first three may enter an edit set, and only when a command changed the
corresponding state.

## 6. Implementation slices

Order: the loader-fidelity prerequisite, B2.1, then B2.2 and B2.3 in parallel, then B2.4a, then B2.4b and B2.5 in
parallel. Each slice depends on merged slices only, so no stack is more than one PR deep. Each slice updates the
R02–R04 rows of the migration plan and the status line of this document.

**Prerequisite — loader fidelity** ([#330](https://github.com/mgradwohl/MoTrack/pull/330), merged)
- Fixes #313, #314 and #315's track slots, and adds the v≤6 migrations of §3 to the current native reader as
  `trackerReading` revisions 1 and 2. B2.1 landed before the rest of #315, so #315's empty `mass_unit` (revision 3)
  and its refusals now gate B2.4a: until then B1's reading maps an empty saved mass or time label to its default,
  and `typedDocumentFromReduced` refuses a native file that stores an empty `massUnit` or `timeUnit`, which would be
  an open failure once B2.4a reads native files through it.

**B2.1 — Typed document** (first)
- Content: `motrack_model`; `TrackerDocument`, families, descriptors and reference-only descriptors; identities,
  bindings and graph population; conversion from the reduced model; the cached `legacyView`. No application change.
- Files: new `src/model/tracker_document.*`, `src/model/families.*`, `src/model/descriptors.*`; moved into the
  target: `src/tracker_loaders.*`, `src/source_edits.*`, the unit and name codecs (from `legacy_project.cpp` and
  `tracker_rename.cpp`) and `supportsConstantClipTimingEdits` (from `measurements.cpp`); new `src/legacy_view.*`
  in `motrack_project`; `CMakeLists.txt`; `tests/model/tracker_document_test.cpp`,
  `tests/model/legacy_view_parity_test.cpp`, the native fixtures under `tests/fixtures/native/` with provenance
  in `tests/fixtures/README.md`; `tests/corpus_probe.cpp`, `tests/corpus/state-gaps.json`; `docs/oracle-dumps.md`;
  a benchmark of an AutoTracker run on the longest corpus track.
- Exit: parity for every corpus file, fixture and native fixture; the O3 dump comes from the typed document;
  the 90% coverage gate holds.

**B2.2 — Commands and history** (after B2.1, parallel with B2.3)
- Content: the four commands, validation, `DocumentHistory` with both revisions, graph updates on copies and
  C0's revision floor. Nothing is wired to the UI.
- Files: `src/model/commands.*`, `src/model/document_history.*`, `src/model/dependencies/dependency_graph.*`
  and `docs/dependency-graph.md` (revision floor); `tests/model/commands_test.cpp`,
  `tests/model/history_parity_test.cpp` (replays the `track_edits_test.cpp` scenarios through commands and
  compares the legacy view with `TrackEdits`), `tests/model/command_sequence_test.cpp`; `tests/corpus_probe.cpp`
  for `--apply`.
- Exit: parity with `TrackEdits` in every scenario, including refusals, track identities and the counter;
  assignment never lowers a revision; deleting and restoring members, attachment targets and boosters in corpus
  files keeps their references.

**B2.3 — Descriptor composition** (after B2.1, parallel with B2.2)
- Content: composing `SourceEdits` from typed state; admission checks, including the reimport check, split from
  the patch functions; the broad `renderName` rule; #316. Production export still uses B1's composition.
- Files: new `src/tracker_compose.*`; `src/tracker_writer.cpp` and `src/tracker_rename.cpp`, split without a
  change in behavior; `src/model/source_edits.*`; `tests/tracker_compose_test.cpp`, `tests/source_edits_test.cpp`;
  `tools/corpus.py` for the comparison with B1.
- Exit: the same accept or refuse decision as B1, and equal bytes where both accept, on every writer test and
  corpus export; every doctored output refused.

**B2.4a — Type switch** (after B2.2 and B2.3)
- Content: `WorkspaceDocument::project` becomes `TrackerDocument`; the `TrackEdits` façade; sessions, groups and
  Revert on typed baselines with owner-held revisions; non-edit updates as document operations. Export still
  runs B1's writer over the legacy view, and native files stay v5 and v6. `DocumentParts::trackOrder` and its
  validation are removed: the track list follows the export order ([track UI design](track-ui.md#rows)), which
  B2.2's commands keep `trackOrder` equal to until then, and the v1–6 conversion compares each family's tracks by
  identity.
- Files: `src/workspace_document.*`, `src/document_session.*`, `src/archive_group_session.*`,
  `src/workspace_session.*`, `src/workspace_input.cpp`, `src/main_window.*`, `src/bound_workspace.cpp`,
  `src/track_edits.*`, `src/track_identity.*`, `src/tracker_archive.*`, `src/tracker_image_package.cpp`,
  `src/paired_save.cpp`, `src/measurements.*`, `src/model/tracker_document.*`, `src/model/commands.*`,
  `src/legacy_view.cpp`; `docs/workspace-sessions.md`. Existing tests change only where they build a
  `LegacyProject` directly, and where `trackOrder` is dumped or checked: `tests/model/document_dump.hpp`, the
  `tests/fixtures/native/*.document.json` dumps and `tests/model/legacy_view_parity_test.cpp`.
- Exit: the full wave-1 gate on Windows, macOS and Linux CI with no user-visible change; the two-document
  isolation tests pass.

**B2.4b — Export through the descriptors** (after B2.4a, parallel with B2.5)
- Content: standard export uses B2.3's composition; B1's composition remains only as the comparison test, which
  becomes a required CI check until B3 deletes it.
- Files: `src/tracker_writer.*`, `src/tracker_compose.*`, `tools/corpus.py`, `.github/workflows/ci.yml`;
  `docs/native-format.md` (writer sections).
- Exit: the 237 cases and the required comparison pass on all three platforms.

**B2.5 — Native version 7** (after B2.4a, parallel with B2.4b; its codec needs only B2.1)
- Content: the v7 reader and writer for standalone documents and groups, `readerRevision` and `base` handling,
  allocation ranges and the countdown low-water mark, #317. It never reads or writes a presentation order of
  `tracks`, which B2.4a has removed from `DocumentParts`.
- Files: new `src/native_document.*`; `src/workspace_document.cpp`, `src/workspace_document_codec.hpp`,
  `src/archive_group.cpp`; `tests/workspace_document_test.cpp`, `tests/archive_group_test.cpp`; `tools/corpus.py`
  for the native route; `docs/native-format.md`, `docs/workspace-sessions.md`.
- Exit: every native fixture passes; the native O5 route passes for every corpus case; the paired-save journal
  tests pass with v7 companions.

### Open pull requests

- **[#169](https://github.com/mgradwohl/MoTrack/pull/169), tabsets**, being ported onto `motrack_osp`, changes
  `workspace_input.*`, `workspace_context.*`, `workspace_window.*`, `tests/corpus_probe.cpp`, `tools/corpus.py`,
  `tests/test_corpus.py`, `CMakeLists.txt`, `README.md`, `docs/workspace-sessions.md`, the migration plan and the
  translation catalogs. B2.1 to B2.3 conflict with it only in text (CMake, probe, corpus tool). B2.4a also
  conflicts in meaning in `workspace_input.cpp`, where a tabset's member imports would have to use the new media
  operations, and in `docs/workspace-sessions.md`. Landing #169 before B2.4a is simpler; otherwise the later PR
  adapts. A tabset remains a workspace source like an archive group, not a `TrackerDocument`.
- **[#259](https://github.com/mgradwohl/MoTrack/pull/259), opening from the OS**, changes `workspace_context.*`,
  `workspace_services.*`, `workspace_window.*`, `main.cpp`, CMake, CI and documentation. It overlaps B2 only in
  `CMakeLists.txt`, the CI workflow (B2.4b) and status text, so the merge order does not matter.
- Every slice edits the R02 and R03 rows, so those edits are serialized with other open PRs that touch them.

## 7. Open questions

1. **Does v7 store the re-rendered current XML, as the [architecture table](migration-plan.md#target-layers)
   says?** Recommended: no. Store the import baseline and the full typed state, as above. A native save must
   succeed while standard export is refused (dangling references, stale associations, native-only objects), and
   re-rendered XML cannot represent that state. It would also drop the original names through which unknown
   content still refers to removed objects, and a value edited and later restored would not get its original text
   back. The current XML is a deterministic render of baseline and state. The table row is marked proposed;
   B2.5 updates it.
2. **Where do #313–#315 land?** Decided: #330 landed #313, #314 and #315's track slots with the v≤6 migrations
   before B2.1, so B2.1's parity baseline includes the fixes and no migration turns an old misreading into an edit.
   B2.1 then landed before #315's remaining parts (the empty `mass_unit` as revision 3 and the refusals), which now
   gate B2.4a.
3. **Should removing a referenced object be refused?** Recommended: no. Allow it in the native document with
   export refused while references dangle, as today; the families add explicit cleanup edits.
4. **Should untyped classes get graph edges in B2?** Recommended: yes, through reference-only descriptors that
   bind every baseline reference by its original name. Deleting and undoing need them now.
5. **How is the legacy view maintained?** Recommended: rebuilt lazily once per document revision, with chunked
   frame fields so copying stays proportional to the edit. If B2.1's AutoTracker benchmark shows the rebuild
   dominating, the owner patches its unshared view in place for frame-keyed edits. Measured in B2.1
   (`motrack_autotracker_benchmark`, `benchmark` preset, Windows): on the longest corpus track (BallToss, 11 marks)
   a rebuild takes 1.5 µs against 216 µs for the template match alone, and 5 µs at 1,000 marks; it equals the match
   at 10,000 marks (229 µs) and is ten times it at 100,000 (2.1 ms). A typed mark edit copies one chunk and costs
   1–29 µs. The rebuild therefore does not dominate ordinary runs; it does in runs of tens of thousands of marks,
   so B2.4a should patch the owner's unshared view in place for frame-keyed edits rather than let them grow
   quadratically.
6. **May `motrack_model` use Qt?** Recommended: Qt Core, linked privately for the codecs' `QChar` classification
   only, which AGENTS.md allows; a separate Unicode table could disagree with today's classification.
7. **When does export switch to the descriptor composition?** Recommended: in B2.4b, keeping B1's composition as
   a required comparison until B3 deletes it.
8. **Is an AutoTracker run one `Batch`?** Recommended: not in B2. Each mark stays one undo step, and C6 decides
   with its interface.
