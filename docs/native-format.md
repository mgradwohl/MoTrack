# Native document compatibility

MoTrack saves bounded JSON metadata with `format: "MoTrack"`. The current writer emits version 5;
the reader accepts versions 1 through 5 and rejects unknown versions. Archive-backed documents use
the native ZIP container described below; other documents remain plain JSON. Existing measurement, calibration,
clip, media-reference and movie-time fields retain their version-1 meanings.

## Unit labels

The optional `timeUnit` string records the experiment's analysis-time label, independently of clip
start/duration and decoded movie timestamps. A label edit does not convert numerical times, rates,
momentum or energy. Plot labels and the timed CSV header use that label (the default remains `time_s`).

When older native metadata omits `timeUnit`, the reader recovers it from the preserved Tracker XML,
if present, or uses `s`. Later explicit native edits take precedence over the original source value.
The standard writer patches `time_unit` without changing the clip or measurement data. The bounded
label codec refuses unsupported labels before replacing a destination. This adds an optional metadata
field to version 5; it does not add a persistent undo log or a desktop units editor.

## Track identities

Versions 3 through 5 store an `id` on every editable point mass and independent vector and a document-wide
`nextTrackId` counter. These are canonical positive decimal strings, preserving the full unsigned
64-bit range without JSON floating-point rounding. IDs are unique across both track types, scoped to
one document, and independent of names and list positions. Zero is reserved for uninitialized records.
Import assigns IDs; creation allocates a fresh ID; rename, mark edits, deletion/undo and creation/redo
retain the original identity. The allocation counter never rewinds, including after undo or deletion,
so newly created tracks cannot reuse an earlier identity. Undoing creation therefore keeps the document
modified until the advanced counter is saved, and closing still offers Save. An exhausted counter disables creation.

Native reads and writes reject zero/duplicate IDs and a counter not greater than every live ID. Writes
validate before replacing the existing file. Versions 1 and 2 receive fresh identities during migration;
subsequent saves retain them and the counter, including after the highest-ID track is deleted.
This migration does not infer associations with original Tracker XML from names or current list order.

Edit commands resolve the selected list position to an ID when recorded. Undo/redo look up that ID,
including when removing a created track or reapplying a deletion; saved list positions are used only
to restore insertion order. Missing, duplicate, wrong-type or colliding identities reject the operation
without advancing history or the revision. A history belongs to one live document and is cleared on
replacement. Native files retain project data and IDs, not a serialized undo log.

`DocumentSession` owns the complete editable `WorkspaceDocument`, its `TrackEdits`, the source path
and the current native save target. It has no widget or decoder dependency. Each replacement gets a
fresh identity object; save checkpoints retain that identity and the history revision. A completion
from another session or a replaced document cannot change the current target or mark it clean, even
when the two histories have the same revision. Failed writes preserve the old checkpoint. A successful
stale write keeps its destination but invalidates the checkpoint until the latest state is saved.

Workers receive an owned document snapshot, including shared archive ownership, rather than access
to the live session. Replacing the session clears only its own history and data; a pending snapshot
can finish without retaining a widget or losing its extracted media. Session state is GUI-thread owned.
The desktop still has one active session: decoder/playback, open/save orchestration and view state
remain in `MainWindow` until the multi-document controller work. The extraction does not itself add tabs.

## Tracker import source

Versions 2 through 5 require `trackerSource`, either `null` for a document without preserved source or an object
with `xmlBase64`. This is canonical Base64 of the **original XML bytes**, not Unicode text reserialized
from a DOM. The original encoding declaration, whitespace, comments, unknown objects, properties and
references survive exactly. The source is the immutable import baseline; the editable project fields
hold subsequent MoTrack changes. Do not export this baseline as if it contained those edits.

`readTrackerProject` reads and parses a single file snapshot. `importTrackerDocument` carries both
the reduced editable model and `TrackerSource` into the workspace. Saving and reopening a native
document retains both independently, including after the original standalone `.trk` is removed.
Opening a different project replaces the source; opening standalone media clears it only after the
new media successfully opens. Failed and canceled opens leave the existing document intact.

Source XML is limited to 32 MiB, including when read from the native envelope. The native document
metadata limit remains 128 MiB. The reader checks the encoded length before decoding, requires strict canonical
Base64, and validates decoded XML with the same bounded Tracker parser used for import. Document
types are rejected. The writer validates source data before opening its atomic replacement file, so
an invalid or oversized envelope cannot overwrite an existing valid native document.

Desktop saves validate and serialize an owned document snapshot on a single save worker. The GUI
continues painting and processing worker completions while user input is deferred. Prepared project
and movie opens wait until saving finishes, then recheck cancellation before applying their results.
Overlapping saves and closing during a save are rejected. A revision check prevents a background edit or replacement
document from being marked saved by an older snapshot; the caller must save the latest state again.
A successful stale save invalidates the old saved checkpoint, so undo cannot falsely label the old
file contents clean. It keeps the successfully written destination for the next save. A failed write
or a save completing after a document replacement does not change the active document's checkpoint.

## Original object associations

Version 4 adds `trackerSource.bindings`: `null` when associations are unknown, or an array when they
were recorded at import. An empty array means the source has no editable point masses or independent
vectors. Each record contains a decimal-string `trackId`, integer `sourceIndex`, and `kind` (`pointMass`
or `vector`). The index is the zero-based ordinal among **all direct property children** of the original
`tracks` property, including unknown objects and empty entries. It is not a track name, current list
position, or the numeric text inside an OSP property name such as `[10]`.

Every originally editable object has exactly one binding. Bindings remain in the immutable source
envelope after deletion; a future writer can distinguish a deleted imported object from a newly created
track. Rename, reorder, undo/redo, native reopen and removal of the standalone source file leave these
associations intact. Unknown objects remain byte-preserved in XML and are not given editable bindings.

Validation reparses the baseline and checks complete, unique object positions, matching original/live
types, unique IDs, and IDs below the saved allocation counter. A deleted object's ID must remain reserved.
Malformed associations fail before an existing native file is replaced. Versions 2/3 retain their XML
when migrated but get `bindings: null`; version 3 retains its existing IDs, while versions 1/2 receive
new ones. Matching names or order cannot prove historical identity after earlier edits, so migration
does not invent associations. Future additions to binding kinds require an explicit schema migration.

## Complete original archive payload

Version 5 adds a native ZIP container for archive-backed documents, with exactly two regular entries:

- `project.json`: the editable native model and preserved XML/associations, limited to 128 MiB.
- `tracker/source.trz`: the exact original compressed archive bytes, limited to 544 MiB.

The writer stores both entries without another compression pass, streams the payload in 64 KiB blocks,
and atomically replaces the destination only after the container is complete. The reader rejects extra
or duplicate entries, links, encryption, malformed/truncated storage and declared/actual size violations.
The outer file is bounded to the two payload limits plus 8 MiB of ZIP overhead. The retained Tracker archive
still passes the existing safe-name, 4096-member and 512 MiB expanded-content checks before use.

On import, the archive adapter first copies the input to an owned, bounded snapshot and extracts from
that snapshot. The model, original XML and archive payload therefore come from the same copied bytes.
All tabs, embedded media and unknown members remain byte-for-byte preserved, including after the
original `.trz` is replaced or removed. An archive member named `source.trz` cannot collide with the
snapshot because extraction and source storage use separate temporary directories. Shared ownership
keeps both alive while an open/save worker or document uses them; the final owner removes the temporary files.
Copying and extraction honor cancellation. Native reopen creates its own independent archive owner.

Version-5 metadata requires `embeddedArchive` to agree with the actual container. `archiveMember` is
the selected original `.trk` path or `null` when that association is unknown. A known member must exist,
and any preserved source XML must exactly match that original member. Native media references are
already relative to the archive root, even when the original selected tab lives in a nested directory.
The edited model remains separate from the unchanged original payload; the payload is not an edited export.

Versions 1–4 remain readable as plain JSON. Their external `archive` reference is opened by the desktop
loader and embedded on the next successful native save, provided the referenced archive is available.
Older files did not record the selected archive member, so migration keeps it unknown rather than
guessing from names or XML. Embedded and external archive sources cannot coexist in one document.
Standalone media outside an archive still uses external references; version 5 does not copy all local
movies or image sequences into the native document.

## Version-1 migration and remaining work

Version 1 did not retain source XML. Reading it produces no source envelope; saving it emits version 5
with `trackerSource: null`. Migration preserves its editable data but cannot recover content previously
discarded. Earlier MoTrack builds that only accept versions 1 through 4 cannot open version-5 documents.

Only the selected archive tab is editable today. Multi-document controllers, multi-tab editing,
and standard-file writers remain unfinished. An unknown object retained in this
envelope is not an implemented track type.

Edited projects containing `VectorSum` are rejected before replacing a standard-file destination,
including when all independent member samples have been removed. The pinned Tracker 6.3.5 reference
fails to initialize these member names and loses them on its own resave; see the
[explicit known-issue corpus case](../tests/corpus/README.md#vector-sum-reference). Native documents
still retain the exact original source. An unchanged standard-file copy remains byte-for-byte and does
not repair this upstream behavior.

The internal standard writer can patch point-mass mass values, mark positions/additions/deletions,
independent-vector tails/components/additions/deletions, clip range/constant timing, length/mass/time-unit labels,
audited track names, and supported fixed/partly fixed/fully varying coordinate-system keys in media-free projects after a native reopen.
Clip edits retain all measurements, including frames outside the clip.
Vector exports reject nonfinite endpoints and components that Tracker's endpoint subtraction would
change beyond the reference tolerance (absolute `1e-10` image pixels or relative `1e-12`).
Tracker also automatically snaps newly loaded arrows on their first draw. Changed files containing
vector samples currently require one fixed calibration key at frame 0, plain Arrow/BoldArrow footprints, no point-mass
or unsupported vector providers, and at most 4096 samples. Vector frame indices (including retained
null XML entries) cannot exceed 500000, and the sum of dense frame-array lengths across vectors cannot
exceed 500001 slots; this bounds Tracker's allocation for sparse high-frame samples. Tails within 8 image pixels of the origin or
any other sample's tip are rejected before output replacement; the comparison includes other frames
because trails can expose them. This conservative bound avoids silently introducing links or moving
geometry. Full dependent-vector/display semantics must replace these restrictions before full parity.
It requires known source associations, preserves unknown XML objects/properties and
normalizes changed output to a matching UTF-8 declaration. Unchanged output is byte-exact. It rejects
every other changed domain field before opening its atomic replacement file, including media-based
timing, moving reference frames, fixed-flag changes and original-track deletions. Range exports require a known `VideoClip` without
video, frame-shift or saved-video-frame-count properties. They reject missing/invalid counts, endpoints
beyond Tracker 6.3.5's media-free frame 500000 limit, and overflow in its temporary frame-count arithmetic.
Media references are rejected until relocation
and packaging are supported. Changed/new marks become manual keyframes; unchanged automatic marks
retain their original key status. Deleting the last manual key while leaving automatic marks is rejected,
because Tracker treats an empty key list as all remaining marks being manual. Unknown properties on
retained frame objects survive; deleting a mark removes its frame object. Rewritten frame arrays omit
structural blank lines because the OSP array reader stops at them. Empty key lists use Tracker's
empty array container, and adding a first mark after Java has resaved that container is reference-tested.
Explicitly preserved frame-array whitespace and comments that would stop OSP's reader cause rejection.
This path is exercised by the reference corpus.
Vector cases also clear every sample and add the first sample after Java resaves the empty vector.
The shared frame writer retains unknown properties on existing frames, validates finite positions/components,
and preserves original frame numbers; vector samples do not acquire point-mass keyframe lists.
It does not add a desktop export action or mark the native document saved. Standard export checkpoints,
additional edit types, media relocation, `.trz` packaging and paired-save recovery remain phase-3 work.

See the [full-port implementation order](migration-plan.md#implementation-order-and-exit-gates) and
[round-trip acceptance requirements](migration-plan.md#tracker-round-trip-compatibility). Tests exercise
[native validation and migration](../tests/workspace_document_test.cpp) and
[normal desktop import/edit/save/reopen](../tests/workspace_test.cpp).

Fixed calibration export requires one original and edited key at frame 0, all three fixed flags, an
unlocked `ImageCoordSystem`, finite values, nonzero scales, a finite nonzero scale product and finite
inverse-transform coefficients/translation. Validation follows Tracker's normalized trigonometry and
Java affine inversion arithmetic (including diagonal/shear special cases), rejects intermediate
overflow/underflow, and compares the resulting coefficients with the native inverse within the corpus
tolerance before opening an output file. Evaluated world coordinates must also agree at the origin,
the independent corpus probe, every point-mass mark and every vector endpoint; coefficient agreement
alone cannot detect cancellation near zero. Translation-free vector components must also agree, and
Tracker's squared-component magnitude calculation must remain finite. Component checks use Tracker's
reconstructed `(tail + component) - tail` values, so scale amplification cannot hide behind the image-pixel
tolerance. Changed vector files receive these checks even when the calibration itself is unchanged;
keyed and partly fixed vector-coordinate systems need their own reference cases before export. Angles stay in the native editor's -180 to 180 degree range.
Unknown fields and measured image positions are retained. Creating/removing a coordinate system,
changing fixed flags and moving reference frames require later reference cases.
The vector snapping guard also runs after calibration changes, preventing an origin move from silently
reattaching a previously independent vector when Tracker opens the export.

The same writer supports adding/editing keys in existing, unlocked partly fixed or fully varying
coordinate systems, starting at frame 0, in projects without vector tracks. The fixed flags must remain
unchanged. Fixed origin/angle use the last loaded key and fixed scale uses the first; validation uses
that effective calibration at each original frame. Each key's inverse,
origin and independent coordinate probe are validated; retained marks use the active key at their own
original frame number. Unchanged keys are validated too, preventing a different edit from hiding an
unstable transform. New/retained calibration frame indices, including null source placeholders, are
bounded at 500000 for OSP's dense arrays. Partly fixed and varying systems bound coordinate-setter work at 1000000 dense frame slots, including
the clip/marks. Each varying setter counts its remaining frame range; fixed-origin/angle setters count
the entire dense range because OSP applies those changes globally.
This rejects excessive work before opening a file, without running a stress test. Updates preserve unknown key fields and untouched keys.
The existing distance/axes commands own these changes with undo/redo and native save/reopen. Removing
keys, changing fixed flags, varying vector coordinates and moving reference frames remain unsupported;
failed validation leaves an existing destination unchanged.

Length- and mass-unit labels can be exported independently or with other supported edits. The current
calibration command owns the length label and scale together; a separate reversible command owns the
project-wide mass label. Changing only a label never converts numerical scale or mass values, matching
Tracker's unit setters. Both commands participate in saved/dirty history and undo/redo; existing native
files already store both labels, so no format version change is needed. Labels must be nonempty,
trimmed, valid UTF-8 and at most 256 UTF-16
code units; Unicode control/format characters and decimal digits are rejected by code point,
including C1 controls and supplementary digits. Supplementary letters remain valid labels.
String labels use OSP-compatible CDATA because its string reader does not decode XML entities. Edits
to a source unit containing comments or processing instructions are rejected: OSP requires the CDATA
terminator immediately before the closing property tag and can hang on an intervening comment.
The CDATA terminator in a label is also rejected before replacing a destination. Time labels follow the
same rules; the full units UI and unit defaults remain separate work. The corpus explicitly checks imported labels,
Java's independently recorded default for an omitted label, and labels after Java resave/native reimport.

Constant media-free clip timing can now be edited through history using the existing frame-duration
and start-time fields, both in milliseconds. No native format version change is required. Undo restores
the prior optional duration exactly. Standard export requires the audited StepperClipControl subset
and finite increasing times at original frames. Edits, undo/redo and standard export share a provider
guard: only point masses, independent vectors and retained axes/protractors are accepted. A refused
edit leaves the project, saved/dirty state and both history directions unchanged. Decoded media timing, model/data time sources and
desktop timing controls remain separate work. The corpus guide documents the independent negative-origin
and nonzero-first-frame/stride acceptance cases.

Time origins must also preserve adjacent intervals within relative `1e-9` in both milliseconds and
seconds, preventing large offsets from materially distorting consumers that subtract absolute times.
Native constant-time derivatives use original-frame differences and duration directly, so imported
large origins cannot invent acceleration. Range changes and timing history validate the complete
candidate span; undo can restore original absent or legacy timing/ranges when the corresponding
range/timebase is unchanged. Range-only export applies the same timing checks before replacing a file.

New independent point masses can be appended to an audited media-free source after native save/reopen.
Original bindings stay unchanged in the native envelope. Each export maps newly allocated IDs to fresh
source objects, so repeated exports do not duplicate tracks and reordered native lists cannot change
original relationships. Original bound tracks must remain present. The source must use the audited
mass/vector/axes/unattached-protractor/view schema with unique names and resolved links; an unresolved
name could otherwise become a new dependency merely by adding a track. New names obey the rename codec.

New objects explicitly start visible, with trails, unlocked, a red Diamond footprint, mass and manual
marks. These are documented interchange defaults until native per-track appearance is implemented;
no unknown original track style is cloned. Sparse new marks retain original frame numbers, bounded at
500000 and one million aggregate dense frame slots across all emitted masses. Every edited export
counts retained arrays and null placeholders as well as new marks, so standard reimport and later
marking cannot reset the budget. A present but unusable source calibration refuses creation; absent
coordinates retain the existing media-free behavior. The source schema audit permits
the same frame range specifically for point-mass `framedata`, so later creation after standard reimport
does not impose a smaller index limit. Generic array indices, aggregate entry counts and nesting limits
remain bounded separately. The native source XML stays
unchanged. Deleting original tracks, dependent creation, media-backed creation, desktop
standard saving and paired-output transactions remain separate work. The corpus has independent empty
and marked creation cases, including actual Tracker appearance, keys and scientific coordinates.

New independent vectors use the same audited identity/schema/name handling, with explicit visible,
unlocked magenta BoldArrow defaults and no point-mass key list. Creation requires a source containing
only independent vectors and coordinate axes. Empty and sparse/zero-vector cases have independent
application oracles. All existing snapping, endpoint reconstruction, calibrated-coordinate and dense
allocation checks still apply, including other frames' visible tips. Adding an empty vector does not
authorize dropping original tracks, changing dependent links or weakening the measured-vector guards.
The source audit also admits the writer's original-frame index range for vector `framedata`; a native
regression exports and reimports a mark at frame 10000, then creates and exports another vector. Other
array indices and aggregate schema/allocation limits remain unchanged.
Both point-mass and vector membership checks use linear sorted-ID inclusion before appending objects.
