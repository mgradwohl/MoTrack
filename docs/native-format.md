# Native document compatibility

Desktop sessions retain resolved local media locations separately from serialized references when opening a project. Native Save As and linked-image standard export can therefore rebase those locations even after the original project and its empty directory are removed. These runtime bindings are rebuilt on open and are not serialized; changing a media source must rebuild them. Native saving rejects an unresolved local rebase before replacing the destination.

Successful native saves also adopt the worker snapshot's refreshed physical locations. Restoring media
through a temporary alias and later removing that alias therefore preserves subsequent saves and
undo/redo. The active image reader adopts the same locations without resetting frame navigation.
Failed writes and replaced-document completions do not adopt those runtime bindings. A temporarily
missing image with an unrecognized suffix retains its known reader; later saves retry image detection
even when an earlier open could not create a reader. A successful save redraws the current image and
restores clip controls without moving the playhead; single-image recovery needs no navigation event.
This redraw preserves AutoTracker timers and pending matches and does not start a duplicate match.
An empty location captured from an unresolved media alias is retried against the source path after
repair; nonempty physical bindings retain their original base when the source directory is removed.

When both a native destination and a linked media file are absent in the same physical directory,
save preflight compares their original basenames inside one private temporary child directory.
It uses the inherited filesystem naming policy without creating either requested file or globally
folding names. Keeping the basenames unchanged avoids exceeding the filename limit with a UUID prefix.
The probe and directory are removed before writing or on failure; existing-file identity checks remain
in place. If verification cannot be completed, saving stops before replacing the destination.
[Windows inherits the parent case-sensitivity flag for new directories](https://devblogs.microsoft.com/commandline/improved-per-directory-case-sensitivity-support-in-wsl/#case-sensitivity-inheritance);
[Linux ext4 likewise inherits its casefold flag](https://github.com/torvalds/linux/blob/master/fs/ext4/ext4.h).

On Windows, existing-file identity queries that fail with ordinary paths retry using canonical
[extended-length paths](https://learn.microsoft.com/windows/win32/fileio/maximum-file-path-limitation).
This preserves overwrite protection when Qt can open a long media pathname that the standard-library
query cannot otherwise inspect. Canonicalization precedes the prefix so dot/parent path handling is
not changed by Win32's extended-path rules. Long existing-media aliases and long missing-media names
are covered alongside case and Unicode variants; both failure and success leave no probe artifacts.

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
when the two histories have the same revision. Failed writes preserve the old checkpoint. Native and
paired workers share a completion order; an older callback cannot replace a newer destination, media
bindings or saved checkpoint. The latest completed write of an outdated revision keeps its destination
but invalidates the checkpoint until the latest state is saved.
Native session saves use `writeNativeDocument`, which holds the shared paired-destination lock from
replacement through completion stamping. The low-level `writeWorkspaceDocument` storage API returns
no session token. This prevents a delayed stamp from making an older on-disk snapshot appear current.

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
timing, moving reference frames, fixed-flag changes and referenced-track deletions. Range exports require a known `VideoClip` without
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
original relationships. Original data dependencies must remain valid. The source must use the audited
mass/vector/axes/unattached-protractor/view schema with unique names and resolved links; an unresolved
name could otherwise become a new dependency merely by adding a track. New names obey the rename codec.

New objects explicitly start visible, with trails, unlocked, a red Diamond footprint, mass and manual
marks. These are documented interchange defaults until native per-track appearance is implemented;
no unknown original track style is cloned. Sparse new marks retain original frame numbers, bounded at
500000 and one million aggregate dense frame slots across all emitted masses. Every edited export
counts retained arrays and null placeholders as well as new marks, so standard reimport and later
marking cannot reset the budget. Recognized inherited mass arrays on retained objects such as
`CenterOfMass` also count, even though they are not natively editable. Creation and rename validate
the final schema after all metadata/sample patches, preventing an export that would exceed the next
identity edit's entry/depth limits. A present but unusable source calibration refuses creation; absent
coordinates retain the existing media-free behavior. The source schema audit permits
the same frame range specifically for point-mass `framedata`, so later creation after standard reimport
does not impose a smaller index limit. Generic array indices, aggregate entry counts and nesting limits
remain bounded separately. The native source XML stays
unchanged. Deleting data-referenced tracks, dependent creation, media-backed creation, desktop
standard saving and paired-output transactions remain separate work. The corpus has independent empty
and marked creation cases, including actual Tracker appearance, keys and scientific coordinates.
Mark-only edits apply the same coordinate checks after standard reimport and native save/reopen.
Creating an empty mass first cannot bypass affine-precision or present-but-unusable-calibration checks
when marking that existing mass later. Mark-only exports check the full retained/edited mass sample set.

New independent vectors use the same audited identity/schema/name handling, with explicit visible,
unlocked magenta BoldArrow defaults and no point-mass key list. Creation requires a source containing
only independent vectors and coordinate axes. Empty and sparse/zero-vector cases have independent
application oracles. All existing snapping, endpoint reconstruction, calibrated-coordinate and dense
allocation checks still apply, including other frames' visible tips. Adding an empty vector does not
authorize dropping data-referenced tracks, changing dependent links or weakening the measured-vector guards.
The source audit also admits the writer's original-frame index range for vector `framedata`; a native
regression exports and reimports a mark at frame 10000, then creates and exports another vector. Other
array indices and aggregate schema/allocation limits remain unchanged.
Membership comparisons use sorted IDs; surviving source-order IDs are remapped explicitly before final reparse.

Unreferenced original point masses and independent vectors may also be removed from an audited source.
The source envelope and its original bindings remain intact in `.motrack`; each export removes only
bound objects absent from the live document. A dataset-owner or dependent reference to any removed
identity refuses export, even if a new track reuses the old name. Stored data ownership and dependent
links are never silently redirected. Unknown schemas, missing/ambiguous references and dependent-track sources
remain excluded from membership edits.

Deletion compacts IDs assigned during standard reimport, so the writer rebuilds their native mapping
in surviving XML order before appending new objects. Equal-count delete/create operations compare
identities, not sizes. Existing-name updates happen before node deletion to preserve original source
paths. An unreferenced name can be reused by a surviving renamed track after that audit. Original
objects and their content remain unchanged except for explicitly supported edits; only collection
whitespace is normalized for OSP. Plot-owner/guest cleanup is specified below; table-column/format ownership is specified below. Dataset-owner and dependent-link cleanup still require separate policies and application acceptance.

Tracks referenced only by panel, plot and table selections can be deleted with explicit cleanup.
The panel selection is cleared. Each affected plot/table selects the first remaining viewable track
in source order, using its current renamed name; axes are excluded, retained protractors are eligible,
and newly appended tracks follow original survivors. With no eligible track, affected selections are
cleared. Existing empty packed slots and unaffected selections remain unchanged. This follows the
pinned 6.3.5 `TrackerPanel.removeTrack`, `TrackChooserTView.refresh` and panel serializer bytecode.
Live-name patches run before selection cleanup and source-node removal, preserving original ordinal
paths. The unchanged native source envelope allows undo to restore the original relationships.
Independent application cases require removal of selected mass B (fallback mass A) and both masses
(fallback protractor A), retaining the custom protractor table columns/formats and calibrated content.

Deleted custom plot owners remove their entire `PlotTrackView`; deleted comparison guests remove
only their ordered string cells, compacting surviving indices and omitting an empty guest property.
Remaining axes, lines/points settings, plot order and numeric guest-index order are retained, even
when source array cells appear out of order in XML. Source reference nodes
are collected before any removal, so deletion cannot shift a later collection ordinal onto a different
view. Live-name changes precede this cleanup; reusing a deleted name never transfers its plot data.
Only modified arrays/collections have whitespace normalized for OSP, with a newline before every
surviving child and after the last child. These separators are required by OSP's line-oriented reader
even though general XML parsers accept children on the container's line. Seven native deletion subsets
cover renamed survivors, native close/reopen, repeated export and replacement-name isolation.
Three independent corpus cases remove the owner, a guest present on both plots, or the first guest
in an ordered pair. Actual Tracker application acceptance remains required before merging this slice.

Custom table ownership is cleaned up by original identity, independently of view selection. A deleted
column owner removes its entire `String[]` row from `track_columns`; a deleted format owner removes
its whole `String[][]` group from `column_formats`. Every row in a format group must already name the
same unambiguous owner. Multiple reference rows therefore identify one removal target, deduplicated
by original path before mutation. Surviving row/group indices are compacted, empty properties omitted,
and OSP property line separators restored. Remaining columns, formats and owner order are retained.
If cleanup leaves a custom table with no properties, its saved view slot and any empty enclosing
view arrays are omitted, matching Tracker's saver. Retained view indices are roles and are not
renumbered. An empty `TableTView` would pass a missing selected name to the pinned loader's
null-unsafe `getTrackByName`; removing the empty custom state preserves the selected table-pane type.
This also covers selection-only tables and already empty column/format arrays when the last
viewable track is removed. Native cases retain unrelated plot slots in the same and another chooser
and verify their role indices. Selection fallback still preserves a table when a viewable track remains.
Original source bindings and undo history remain intact; replacement tracks cannot inherit deleted
owners' settings. Dataset-index owners and scientific dependencies remain excluded.

Four independent application cases remove the first, selected or last owner, or all owners from
`table-default-format-owners.trk`. This separate fixture uses three explicit-frame column sets and
only Tracker's default frame/step/time formats. Seven native subsets additionally exercise renamed
survivors, name reuse and native save/reopen. All four cases passed actual Tracker resave,
fresh reopen and native reimport at `7586d95`, Linux job 128722151613 in
[run 42477554192](https://github.com/mgradwohl/MoTrack/actions/runs/42477554192), merged as PR #118.
The all-owner case includes the corrected empty-table loader failure above. The reference
harness expands panes through public divider actions, visits custom tables and restores their
selection; this does not establish immediate unvisited-resave fidelity.

Four earlier variants remain checksum-pinned failures under [MoTrack #64](https://github.com/mgradwohl/MoTrack/issues/64).
The original subset/default-column cases were loaded with a pane that Swing considered showing but
Tracker considered collapsed. Expanding it populated columns in the explicit-frame legacy case,
but formats, order and selection were lost. Adding modern per-track format records and packed
selections retained the selected owner but still lost custom patterns and order in run 42468681114.
Its original strict session oracle remains in `table-owners-rejected-session.json`, exercised by
Python corruption tests; its losses are not accepted as the new expected result.

Changed exports conservatively refuse the observed `t,x`, `t,x,y`, `t,y,x` and `t,y,x,frame` rows,
and custom point-mass numeric patterns in either legacy tables or per-track records. Only default
frame/step `0`, time `0.000`, or an empty table override are accepted. Dense, unique per-track
variable/pattern grammar remains audited before use, including mass-only edits. This does not claim
broader column combinations are verified. All failures leave an existing destination untouched.

Native saving and unchanged byte-preserving rendering retain every failing variant. Removing all
affected owners removes their unsafe rows; deleting one cannot waive checks on survivors. Numeric
format editing, custom order, broader pattern grammars and full view parity remain unfinished.

### Linked-media standard output

The internal [writeTrackerDocument adapter](../src/tracker_writer.hpp) receives an immutable document
snapshot, its current media project location and the chosen standard destination. Current native
media references are relative to the current document, while preserved source XML retains its
original spellings. The adapter resolves current paths and prepares destination-relative references
without inferring the original directory or modifying the source envelope. A removed original XML
file is therefore unnecessary once the native document has been reopened.

The first supported media schema is a standalone, unfiltered ImageVideo containing independent point
masses and coordinate axes. Clip start, step size and step count can change within the original image
inventory, including a single-frame clip. Source frame count/order, interval and time origin must be
retained; numeric timing edits, vectors, movie state, archives and unaudited fields are refused. A native
decoded-frame count is accepted only when it equals the complete image list and is treated as derived
metadata. An optional saved video_framecount must also match. Source path and paths fields must agree.

Range edits patch only VideoClip's startframe, stepsize and stepcount. Original frame numbers, every
image reference and all measurements remain, including marks and images outside the selected range.
No media is cut, reordered or encoded. Native and desktop paired-save tests cover undo/redo, native
reopen and source removal. Independent corpus cases check strided and single-frame linked clips,
an edited existing image archive and a new portable image package; actual Tracker application
load/resave/fresh-reopen acceptance for these new range cases remains pending.

Every linked file must be readable before output is prepared. The adapter rebases known path fields,
uses OSP-safe CDATA, validates the complete edited project by reparsing, and writes through QSaveFile.
The reader and exporter share physical project/media path resolution: existing directory and source-file
aliases are resolved before computing relative links, including POSIX symlink components followed by `..`.
The destination's parent must exist; a symbolic-link output file is refused. Generated links pass the
same local-reference checks as inputs, so a parent folder containing `!/` cannot create an unreadable
archive-style reference. Reopening each generated reference must resolve to the same original image.
Native desktop Save uses the same physical-directory rebasing and verifies each generated reference
before writing. Its atomic native writer follows project-file aliases, including a link whose target
has not been created yet; the saved references are relative to that target's physical directory. The media
overwrite guard uses the resolved target's filename as well as its directory, so a dangling alias cannot
replace a retained missing image or falsely reject a distinct image with the alias's basename. Media
references also follow dangling file aliases, including aliases restored after open; cyclic aliases are
refused. Canonicalization errors, including cyclic directory aliases, return an unresolved path rather
than accepting an unchecked spelling. Retained absolute media locations refresh their physical identity
before overwrite checks.
Cyclic aliases and links that would be interpreted as archive references fail without changing an
existing destination or the session's saved state. POSIX desktop regressions reopen through both the
alias and physical path and compare decoded image pixels; CI runs these on Linux and macOS. Standalone
movie imports retain the opened absolute media path so a differently named file alias rebases to
the real movie. Single-image detection also accepts decoded image headers when the physical file
has an unrecognized suffix; native save/reopen must not reinterpret that image as a movie.

It rejects an output that aliases linked media and leaves an existing destination intact on preflight
or rendering failure. This links existing files; it neither copies media nor marks a session saved.
The filesystem preflight establishes availability, not general codec validation or protection from
later external media changes. Long-running save work belongs on the existing worker boundary.

Native regressions cover relocation, Unicode/ampersand paths, history, native close/reopen, source
removal, repeat export/reimport and failed writes. The independent corpus adds a mass and mark edit
after relocating all four images; its full-XML and byte oracles permit only the requested edits/paths,
and the actual Tracker application must preserve all decoded pixels/times and calibrated measurements
through save and fresh reopen. This application acceptance remains required before merge.

Movie engines, filters and archive members need their own audited path/timing contracts. Archive
packaging will preserve untouched members and feed the paired-save transaction/recovery layer;
standard Save UI follows that layer. This internal adapter is not full R03 completion.

### Selected-member Tracker archive output

The internal archive document adapter exports supported edits to one original `.trk` member after
native close/reopen, including after the original `.trz` is removed. It verifies that the source
XML still matches that member and that current media references resolve to the original embedded
files. Each referenced extracted file must also match the SHA-256 digest captured while reading
its original archive member; missing or same-path replaced files are rejected before destination
replacement. Selected XML and images must also resolve within the physical extraction root captured
at import; identical-byte external file/parent-directory aliases cannot redirect exported links.
Prepared XML is resolved back through the original archive inventory and must name the same media
members, so even an in-root rename/alias cannot produce links to files absent from the output ZIP.
Digest checks use bounded blocks, honor cancellation and avoid rereading duplicate
references. Native save/reopen recreates these digests from the owned archive bytes. An immutable
copy prepares member-relative ImageVideo paths; external/replaced media,
movies, filters and numeric timing edits remain refused. Supported image clip range/stride edits
retain the complete original media inventory and measurements. The source envelope and owned
original archive bytes are unchanged. The [paired-save adapter](paired-saving.md) and desktop standard
Save use this writer; their current-revision CI and review gates remain separate from archive acceptance.

The writer supports bounded, single-disk classic ZIP archives with stored or deflated members. It
uses the [ZIP format specification](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT)
(local headers, descriptors, central records and end record) to replace only the selected payload
with stored UTF-8 XML and a recomputed CRC32. Untouched compressed local records remain byte-identical;
central records retain their metadata with adjusted offsets. Entry/archive comments, member order,
Unicode names and supporting files survive. The edited member retains timestamps, attributes and
supported timestamp/Unix-identity/Unicode-name extra fields. Unknown extra fields on that edited
member are refused; untouched members' extra fields remain opaque.

The macOS reference archive exposed libarchive's UTF-8-to-C-locale filename conversion failure
(run 42473238462, job 128714134485). On POSIX systems the reader now scopes a UTF-8 character
locale to the archive call, using `newlocale`/`uselocale` and restoring the caller's thread locale
on success or failure. It never changes the global locale or accepts lossy conversion warnings.
Windows retains the existing wide-path API. Native regression coverage opens the Unicode-member
fixture under an explicit C thread locale and verifies successful extraction and locale restoration.
See the [Apple thread-locale contract](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/uselocale.3.html).
The POSIX regression requires macOS/Linux CI; local Windows checks cannot establish that result.

The limits are 544 MiB compressed, 512 MiB extracted, 4096 members, 8 MiB central metadata and
32 MiB selected XML. Copying uses 64 KiB blocks and cancellation checks. ZIP64, encryption, split
archives, signatures, ambiguous/noncontiguous layouts and unaudited encodings are refused. A private
staged archive is reopened with the existing validated reader and its selected XML/inventory checked
before an atomic destination replacement. Cancellation, source mismatch, missing media, corrupt ZIP
metadata or a failed destination leaves the existing destination intact. Output cannot replace the
owned source snapshot or extracted files, including their physical spelling through a symbolic-link
temporary-directory ancestor. Archive output-file aliases (including dangling links) are refused.

The compressed source also has an immutable SHA-256 digest captured during the original bounded
snapshot. Standard export copies it into a private working file while hashing the exact copied bytes,
requires the import digest, and performs all ZIP edits from that verified copy. Native bundle saving
hashes the actual bytes fed to its ZIP writer and checks the same digest before committing. Replacing
the writable owned source with a different valid archive, even with identical project XML and member
names, therefore cannot silently change embedded pixels or supporting content in either saved format.
A regression proves both existing destinations survive such a replacement and saving resumes only
after restoring the original bytes.

Native tests cover first/middle/last member replacement, other tabs, opaque binary/support files,
Unicode names, Java deflation/descriptors/comments, native reopen, history, repeated export and
failure/cancellation. The corpus independently checks edited XML, every untouched payload/compressed
record and central metadata. The baseline and edited archive passed actual Tracker `.trz` loading,
resave, fresh-process reopen and native reimport at `fec0e7a`, Linux job 128733094094 in
[run 42481133737](https://github.com/mgradwohl/MoTrack/actions/runs/42481133737), including every
independent pixel/time/geometry expectation. The destination/member-alias guards also passed at `6e35268`, Linux job 128744838260 in
[run 42484998884](https://github.com/mgradwohl/MoTrack/actions/runs/42484998884). The immutable-source
digest and later guards passed all six checks at `875c3fa` in
[run 42534429062](https://github.com/mgradwohl/MoTrack/actions/runs/42534429062), including actual Tracker
archive acceptance and 95.26% combined C++ coverage, before PR #121 merged. Broader ZIP/media support
remains unfinished. Portable image packaging and paired recovery have their own contracts below.

The additional two-experiment corpus case verifies that selected-member editing does not damage a
second experiment sharing the same images. Its independent archive/XML oracle preserves that
experiment byte-for-byte. The reference opens the actual archive once, matches loaded tabs to the
explicit member inventory, saves every tab through Tracker, and reopens each saved standard file
in its own process. Each tab has separate scientific/media expectations, including a different mass
and track name. Manifest validation and negative oracles reject omitted/duplicate tabs and matching
but incorrect results across save/reopen. Both `tracker_archive_tabs_resave` and
`motrack_archive_tabs_roundtrip` passed at `505c40c5`, Linux job 128744837376 in
[run 42484997811](https://github.com/mgradwohl/MoTrack/actions/runs/42484997811), and at reviewed head
`4a7ccb0` in [run 42534428777](https://github.com/mgradwohl/MoTrack/actions/runs/42534428777) before
PR #124 merged with all six checks green. This scoped application
evidence does not provide native multi-tab workspace editing or expand the supported archive/media schema.



### Paired standard and native output (internal adapter)

The [paired-saving contract](paired-saving.md) defines sibling `.trk`/`.trz` plus `.motrack` output,
separate atomic replacements, bounded journaling, explicit conflict-safe recovery and per-output
results. The adapter stages and validates both files, commits standard first, and retains recovery
data after interruption. It reuses the supported standard writers and does not broaden their schemas.
Native desktop Save now shares its physical media-path rebasing helper with this adapter. Desktop
standard actions, per-output history checkpoints and explicit recovery prompts now use the adapter.
The [desktop contract](paired-saving.md#desktop-save-and-recovery-controls) documents cancellation,
conflicts, session-local paired-save selection and current acceptance limits.

Both recovered linked-image and archived-image modes passed actual Tracker load/save/fresh reopen
and native reimport at `940a7bc`, Linux job 128904840707 in
[run 42537466953](https://github.com/mgradwohl/MoTrack/actions/runs/42537466953), with all six checks
green and 95.48% combined coverage before the adapter merged. The desktop controller passed all six
checks at `d9651f9` in [run 42534998848](https://github.com/mgradwohl/MoTrack/actions/runs/42534998848),
with 95.72% combined coverage. Subsequent fixes retain their own current-head CI and review gates.


## Creating a portable image archive

The internal `writeTrackerImagePackage` adapter exports an audited linked-image experiment to a new
`.trz`. It retains the supported writer's schema/edit restrictions, copies each referenced image byte
for byte, patches only the known ImageVideo references to generated archive member names, and preserves
other source XML. The active document, undo state, native source envelope and original media remain unchanged.
It is an export copy, not a new session save checkpoint or an automatically paired native save.
An edited clip range/stride retains all source images and marks in the package, including those
outside the clip. Single-frame packaging has independent native/XML/media coverage; its new actual
Tracker application acceptance case remains pending.

Preparation uses a private temporary directory, at most 4095 images plus one experiment, 512 MiB total
extracted content and 544 MiB ZIP bytes, with 64 KiB copy buffers and cancellation checks. The generated
ZIP uses DEFLATE with streaming data descriptors and has no trailing block padding. Java’s streaming
ZIP reader rejects descriptors on stored entries, so uncompressed streaming entries are not compatible.
The normal archive reader validates the package, then the image-package adapter decodes every staged
PNG, JPEG or BMP before the destination is atomically replaced. Each header must describe positive
dimensions of at most 32 Mi pixels; one image is decoded at a time and Qt's process-wide allocation
limit remains unchanged. Other formats, corrupt pixels, oversized headers, output-file aliases,
linked-media overwrites, missing images, movies, existing archive inputs, unsupported edits and
nonportable image extensions are refused. The source path appears in image failure diagnostics.
Image bytes are retained without transcoding; native tests cover PNG/JPEG/BMP, while the actual Tracker
application acceptance fixture consists of four PNGs. Broader image-format acceptance remains pending.

Native tests cover native close/reopen, removal of the original files, relocation, embedded native
archive reopen and subsequent standard editing. Independent corpus checks cover exact inventory,
unchanged image hashes and full XML preservation. Actual Tracker load/resave/fresh-reopen acceptance
passed the `packaged-images` mode at `f8ea95f` in Linux job 128802309844 of
[run 42503907010](https://github.com/mgradwohl/MoTrack/actions/runs/42503907010), with all six platform/coverage
checks green. Later revisions still require their own CI and review. Missing or unreadable input diagnostics
include the resolved source path while preserving an existing output.
Paired native snapshot adoption, movies and arbitrary supporting-resource collection remain unfinished.

Local Windows validation for this slice passes all six CTest suites, formatting, clang-tidy,
77 Python corpus tests and the native corpus checks, with 95.65% production line coverage
(8173/8545). Current-revision Windows/macOS/Linux CI and code review remain required before merge.

The corrupt-image overwrite was reproduced before adding staged decoding. The correction passes all
eight local Windows CTest groups in 171.33 seconds, with 95.62% production C++ line coverage (8642/9038),
all tracked formatting and both changed translation-unit static analyses. Regression inputs include
non-image PNG data, a truncated PNG and a tiny BMP declaring oversized dimensions; each preserves
existing destination bytes and source bindings. PNG/JPEG/BMP success cases retain exact image bytes.
The project-only import benchmark still links Qt Core/Xml without Qt Gui. Three existing LLVM
function-profile mismatch warnings remain visible. Current-head CI and review remain required.

### Desktop portable-image export

`File → Export portable image archive…` chooses a `.trz` destination and runs the adapter on the
session's bounded save worker with an immutable document snapshot. The filename chooser is guarded
against document replacement and window destruction. Progress and cancellation remain on the GUI thread;
closing or destroying the window cannot leave a worker borrowing its widgets. Prepared project opens
wait until export bookkeeping has finished, and overlapping save/export requests are rejected.

The action is available for imported linked-image documents. The writer remains the authoritative schema
check, so an enabled action does not promise that every imported setting or edit is exportable. Missing
source diagnostics identify the resolved file. A successful export preserves the source paths, native and
Tracker checkpoints, title, dirty state and undo/redo history. Edits made programmatically during export
remain unsaved; the status identifies that the exported snapshot predates them. Ordinary Save continues
to use the previously selected native or paired destinations.

Controller regressions cover clean/dirty export, undo/redo, ordinary Save after export, unsupported and
missing inputs, cancellation, overlapping saves, queued project opens, stale filename dialogs and window
destruction during both filename selection and worker progress. The four-PNG actual-Tracker acceptance
above verifies the underlying format; new desktop-controller revisions require their own platform checks.

The locked export wrapper lives in the image-package adapter and calls the project library's synchronous
destination-lock operation. This keeps the dependency one-way: project persistence never calls image
decoders. Linux's static linker exposed the previous wrapper placement as an unresolved package symbol;
the correction passes the ten desktop-export cases, eight package cases and native replacement/stamp-lock
regression locally on Windows, plus formatting and both changed source-file static analyses. Fresh Linux,
macOS and Windows CI remains required; local linking alone cannot establish Linux acceptance.

### Linked constant-rate movie edits

The document writer accepts the explicitly audited `XuggleVideo`/`VideoClipControl` schema only through
its movie adapter. Existing point-mass values and image-coordinate marks, plus the linked media path,
may change. Original names, membership, calibration, unit labels, clip timing/range and other parsed
fields must remain unchanged. Filters, vectors, other track types except axes, display-orientation
transforms and archived movies remain refused. The archive entry point rejects movie documents before
clearing archive metadata for its internal snapshot; this also applies after indexing and native reopen.
This opt-in schema does not enable movie renames or
membership edits through the media-free writer.

Default point-mass number formats use the same movie-aware schema audit and remain in the exported
XML after native reopen. Custom formats remain refused under the existing #64 fidelity guard.

A completed native timestamp index must start at zero and agree with the preserved positive Tracker
frame interval within one microsecond for every decoded frame, up to the existing two-million-frame
index limit. Clip endpoints and marks must fit that index. The native runtime frame bound must match
the complete index or the clip endpoint inferred on import; it is removed from the export snapshot,
not written as a standard-file edit. Existing saved `video_framecount`, when present, must match the
index. Decoded timestamps stay in seconds; preserved XML clip time origin and interval use milliseconds.
Native movie analysis remains aligned to the imported clip's time origin.

Movie links use the same physical-directory, retained-location and atomic-write checks as images.
Preflight verifies current readability and identity, not general codec compatibility or protection
from subsequent external media replacement. The original movie bytes are never rewritten. Missing
media, an incomplete/nonconstant index or unsupported edits preserve an existing destination.

Local tests edit, undo/redo, native save/reopen, remove the original project/directory, relocate media,
export, reimport and edit again. The desktop test uses the real background indexer and paired save,
then reopens the native companion and saves another mass edit. The corpus probe also uses the real
native decoder; independent XML and movie-byte oracles allow only the requested mass, mark and path.
The edited linked-movie case passed actual Tracker load/save/fresh reopen and native reimport at
`b9ca22d593f961a5a1e0b996d1a573f00f34a04d`, Linux job 128866258456 in
[run 42524910795](https://github.com/mgradwohl/MoTrack/actions/runs/42524910795), with all six CI checks
green and 95.83% combined C++ coverage. Subsequent format/review fixes require their own current-head
CI; this scoped acceptance does not establish full movie interchange or resolve #65.
