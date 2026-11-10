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

MoTrack saves bounded JSON metadata with `format: "MoTrack"`. The standalone document writer emits version 5;
the reader accepts versions 1 through 5 and rejects unknown versions. Archive-backed documents use
the native ZIP container described below; other documents remain plain JSON. Existing measurement, calibration,
clip, media-reference and movie-time fields retain their version-1 meanings.

An engaged frame duration must be finite and strictly positive before the shared encoder creates JSON.
Explicit absence remains valid and is different from an invalid numeric duration. NaN, either infinity,
zero and negative durations are rejected without replacing an existing standalone or group file.
The standalone regression reproduced silent duration loss/unreadable output before this guard;
the group decoder already rejected the invalid in-memory values. See [#157](https://github.com/mgradwohl/MoTrack/issues/157).

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

Native archive saves resolve the destination and reject files inside either owned temporary directory,
including existing attachments and new paths. Directory identity checks cover aliases and the actual
filesystem's case rules. An external save cannot become a temporary attachment that disappears when
the archive owner closes. The original source-file alias guard and payload digest check remain in place.
This protection applies to both the document-storage API and the session save API; it does not change
the version-5 file format. See [#153](https://github.com/mgradwohl/MoTrack/issues/153).

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
vector samples require a validated calibration beginning at frame 0, plain Arrow/BoldArrow footprints, no point-mass
or unsupported vector providers, and at most 4096 samples. Vector frame indices (including retained
null XML entries) cannot exceed 500000, and the sum of dense frame-array lengths across vectors cannot
exceed 500001 slots; this bounds Tracker's allocation for sparse high-frame samples. Tails within 8 image pixels of any effective origin or
any other sample's tip are rejected before output replacement; the comparison includes other frames
because trails can expose them. This conservative bound avoids silently introducing links or moving
geometry. Full dependent-vector/display semantics must replace these restrictions before full parity.
It requires known source associations, preserves unknown XML objects/properties and
normalizes changed output to a matching UTF-8 declaration. Unchanged output is byte-exact. It rejects
every other changed domain field before opening its atomic replacement file, including media-based
timing, moving reference frames, fixed-flag changes and referenced-track deletions. Range exports require a known `VideoClip` without
video, frame-shift or saved-video-frame-count properties. They reject missing/invalid counts, endpoints
beyond Tracker 6.3.5's media-free frame 500000 limit, and overflow in its temporary frame-count arithmetic.
The media-free rendering entry point rejects media references; the adapters described below handle
audited relocation and packaging. Changed/new marks become manual keyframes; unchanged automatic marks
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
Rendering by itself does not mark the native document saved. The session/desktop adapters described
below provide per-output checkpoints, paired saving, media relocation and audited `.trz` packaging;
broader edit/dependency schemas remain phase-3 work.

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
keyed and partly fixed vector cases use the same per-frame checks plus the all-origin snapping audit
described below. Angles stay in the native editor's -180 to 180 degree range.
Unknown fields and measured image positions are retained. Creating/removing a coordinate system,
changing fixed flags and moving reference frames require later reference cases.
The vector snapping guard also runs after calibration changes, preventing an origin move from silently
reattaching a previously independent vector when Tracker opens the export.

The same writer supports adding/editing keys in existing, unlocked partly fixed or fully varying
coordinate systems, starting at frame 0, including audited independent vectors. The fixed flags must remain
unchanged. Fixed origin/angle use the last loaded key and fixed scale uses the first; validation uses
that effective calibration at each original frame. Each key's inverse,
origin and independent coordinate probe are validated; retained marks use the active key at their own
original frame number. Unchanged keys are validated too, preventing a different edit from hiding an
unstable transform. New/retained calibration frame indices, including null source placeholders, are
bounded at 500000 for OSP's dense arrays. Partly fixed and varying systems bound coordinate-setter work at 1000000 dense frame slots, including
the clip, marks, vector samples and retained coordinate/vector XML padding. Every changed calibrated export applies this budget
to the final document, even when calibration is unchanged or all vectors are deleted. Removed samples no longer count,
but retained null XML padding still does. Each varying setter counts its remaining frame range; fixed-origin/angle setters count
the entire dense range because OSP applies those changes globally.
This rejects excessive work before opening a file, without running a stress test. Updates preserve unknown key fields and untouched keys.
The existing distance/axes commands own these changes with undo/redo and native save/reopen. Removing
keys, changing fixed flags and moving reference frames remain unsupported;
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
retained; numeric timing edits, movie state and unaudited fields are refused by this image adapter.
Independent vectors use the additional audit described below; archives use the separate archive adapter. A native
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

Movie engines, filters and archive members need their own audited path/timing contracts. The scoped
movie/archive adapters below feed the paired-save transaction and desktop Save workflow. This does
not establish full R03 completion.

### Selected-member Tracker archive output

The internal archive document adapter exports supported edits to one original `.trk` member after
native close/reopen, including after the original `.trz` is removed. It verifies that the source
XML still matches that member and that current media references resolve to the original embedded
files. Each referenced extracted file must also match the SHA-256 digest captured while reading
its original archive member; missing or same-path replaced files are rejected before destination
replacement. Selected XML and media must also resolve within the physical extraction root captured
at import; identical-byte external file/parent-directory aliases cannot redirect exported links.
Prepared XML is resolved back through the original archive inventory and must name the same media
members, so even an in-root rename/alias cannot produce links to files absent from the output ZIP.
Digest checks use bounded blocks, honor cancellation and avoid rereading duplicate
references. Native save/reopen recreates these digests from the owned archive bytes. An immutable
copy prepares member-relative ImageVideo or audited XuggleVideo paths; external/replaced media,
filters and numeric timing edits remain refused. Archived movies currently require a `.mov` member and the complete constant-rate native
index and retain the standalone movie adapter's edit restrictions. Supported clip range/stride edits
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

### Desktop portable archive export

`File → Export portable archive…` chooses a `.trz` destination and runs the adapter on the
session's bounded save worker with an immutable document snapshot. The filename chooser is guarded
against document replacement and window destruction. Progress and cancellation remain on the GUI thread;
closing or destroying the window cannot leave a worker borrowing its widgets. Prepared project opens
wait until export bookkeeping has finished, and overlapping save/export requests are rejected.

The shared action is available for imported linked-image documents and indexed MOV documents (see the movie package contract below). The writer remains the authoritative schema
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
regression locally on Windows, plus formatting and both changed source-file static analyses. Final
revision 7a73ea7 then passed all six checks and review in [PR #130](https://github.com/mgradwohl/MoTrack/pull/130),
[run 42549946662](https://github.com/mgradwohl/MoTrack/actions/runs/42549946662), including Linux build/setup,
Windows/macOS and actual Tracker package acceptance, with 95.72% combined C++ line coverage.

### Linked constant-rate movie edits

The document writer accepts the explicitly audited `XuggleVideo`/`VideoClipControl` schema only through
its movie adapter. Existing point-mass values and image-coordinate marks, clip start/step/count, existing
fixed, fully varying or partly fixed calibration, length-unit labels and the linked media path may change.
Audited independent point-mass names and membership can also change through the movie adapter.
Mass/time-unit labels, numeric clip timing and other parsed fields must remain unchanged. Independent
vectors now use the shared stability audit described below. Filters, other track types except axes, display-orientation
transforms remain refused. The selected-member archive writer uses the same audited movie adapter
after verifying its immutable source, media members and completed native index.
This opt-in schema does not enable movie renames or
membership edits through the media-free writer.

Movie calibration edits retain the original fixed-origin/angle/scale flags and every existing key.
All-fixed geometry has one frame-0 key; varying geometry starts at frame 0 and may add/edit keys within
the complete indexed movie, including frames outside the selected clip. The shared coordinate writer
validates finite/invertible values, bounded frame arrays and image/world precision before replacement.
Creating/removing calibration, removing existing keys, changing fixed flags, locked or unsafe transforms
and keys beyond the full movie remain refused. Known typed-null coordinate-frame padding is preserved
without admitting unrelated null objects or malformed values. Existing axes/distance commands retain
undo/redo and length labels together; native reopen, selected MOV archive saves, paired recovery and
portable MOV packaging use the same validated writer.

Four fixed [movie-calibration corpus modes](../tests/corpus/README.md#fixed-movie-calibration-saving)
independently check a moved origin, 90-degree rotation, nonuniform scale, centimetre labels and all
calculated positions, plus complete movie bytes/times and the untouched image tab. Native export/oracle/
reimport and desktop paired-save regressions passed in [PR #142](https://github.com/mgradwohl/MoTrack/pull/142),
reviewed at `9c864e0`. All six checks passed in [CI 42591194102](https://github.com/mgradwohl/MoTrack/actions/runs/42591194102)
with **95.83% combined C++ coverage (8938/9327)**, including actual Tracker load/save/fresh-process reopen.

The [varying movie cases](../tests/corpus/README.md#varying-and-partly-fixed-movie-calibration) add all seven
nonconstant fixed-flag combinations. Each independently checks existing and newly added keys, every
original-frame mark, complete movie bytes/times and the untouched archive tab through linked, archived,
recovered paired and portable saves. The 28 native exports and independent XML/media/reimport checks
passed actual Tracker load/save/fresh reopen and native reimport in [PR #143](https://github.com/mgradwohl/MoTrack/pull/143),
reviewed at `7e7ace4` with [CI 42602902771](https://github.com/mgradwohl/MoTrack/actions/runs/42602902771)
and **95.83% combined coverage (8940/9329)**.

Default point-mass number formats use the same movie-aware schema audit and remain in the exported
XML after native reopen. Custom formats remain refused under the existing #64 fidelity guard.

A completed native timestamp index must start at zero and agree with the preserved positive Tracker
frame interval within one microsecond for every decoded frame, up to the existing two-million-frame
index limit. Clip endpoints and marks must fit that index. The native runtime frame bound must match
the complete index or the clip endpoint inferred on import; it is removed from the export snapshot,
not written as a standard-file edit. Existing saved `video_framecount`, when present, must match the
index. Decoded timestamps stay in seconds; preserved XML clip time origin and interval use milliseconds.
Native movie analysis remains aligned to the imported clip's time origin. Range edits retain every
original-frame mark and the full movie index; no frames are renumbered or movie bytes rewritten. The
shared clip writer retains its 500000 last-frame bound and checks Tracker's temporary Java-int count.

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
green and 95.83% combined C++ coverage. The subsequent format/review fixes passed all six checks
and review at d9a24c6 in [PR #133](https://github.com/mgradwohl/MoTrack/pull/133),
[run 42549945960](https://github.com/mgradwohl/MoTrack/actions/runs/42549945960), including the edited
Tracker movie case and 95.71% combined coverage. This scoped acceptance does not establish full
movie interchange or resolve #65.


The movie-range extension adds strided and single-frame native export/reopen cases, desktop Clip
Settings/undo/redo/paired-save checks and two actual-Tracker acceptance modes. Independent whole-XML
expectations permit only the requested range, mass, mark and path; every mark outside the clip remains.
After Tracker saves and freshly reopens, the saved movie reference must resolve locally to the complete
pinned byte count/SHA-256, including excluded frames. Initial Windows validation passes all nine CTest
groups (124.80 seconds), 86 Python tests, both native export/oracle/reimport cases, formatting and
static analysis for all four changed C++ source files, with 95.64% line coverage (8838/9241). Three
existing LLVM function-profile mismatch warnings remain visible. This initial local evidence is
superseded by the merged acceptance below.


A standard movie import initially infers a provisional source-frame bound from its saved clip endpoint.
When the linked movie's full index completes and contains the saved clip, the desktop extends that
bound to the decoded source count without reducing a previously known bound. Otherwise a reopened
single-frame clip could not expand to retained frames beyond its endpoint.
A desktop regression reproduced that failure, then verified single-frame export, standard-file reopen,
index completion, expansion to all four frames, paired resave and display of the retained final frame.
Index completion does not mark the document modified or remove measurements.

If a replacement movie is shorter than the recorded clip, indexing retains the original bound rather
than producing an invalid native document. A regression records a five-frame clip and a mark on frame
4 with an available four-frame movie: native save/reopen retains the range, mark and available index.
An already trimmed native copy also retains its larger known bound, so indexing cannot invalidate an
earlier range that can be restored through undo.
Standard export still rejects this incomplete media/index match and preserves the existing destination.

After the reopen-bound correction, Linux static analysis required a checked image-sequence reference
outside the test's frame loop. With that guard, retained movie bounds and the merged journal-group split,
all ten Windows CTest groups pass in 129.56 seconds with 95.64% production line coverage (8844/9247).
Desktop regressions reproduced both the shortened expansion bound and an unreadable native save
after indexing shorter media, then passed with the corrections. All four related desktop cases, both affected source-file static
analyses and formatting pass. Three existing LLVM profile-mismatch warnings remain visible.
PR [#136](https://github.com/mgradwohl/MoTrack/pull/136) merged reviewed head `4cf2b0f` after all six checks
passed in [run 42561178958](https://github.com/mgradwohl/MoTrack/actions/runs/42561178958), including both
actual Tracker movie-range cases and **95.72% combined C++ line coverage (8874/9271)**.

### Existing movie archive output

An existing `.trz` containing an audited constant-rate MOV file can use the selected-member writer and
paired desktop Save workflow. The original archive, other experiments, compressed media and opaque
members retain their existing preservation contract. The selected XML may change only within the
movie adapter's mass/mark/clip-range and existing calibration limits. Native close/reopen and removal of the original input do
not break the owned archive or index. Both root and nested selected-member paths are covered.

The desktop regression edits mass and clip settings, uses undo/redo and ordinary paired Save, removes
the original input, reopens the native companion, saves again and opens the edited standard archive.
It displays the original final movie frame and checks every untouched member. Missing/changed extracted
movies, incomplete indexes and cancellation preserve the existing output.

The [mixed movie/image archive corpus](../tests/corpus/README.md#existing-movie-archive-round-trips)
adds full-range, strided, single-frame and interrupted/recovered paired cases. Each must pass actual
Tracker loading of both tabs, resaving every tab, fresh-process reopen, native reimport and independent
media-byte checks. Initial Windows validation at `bb373ea` passed all ten CTest groups in 127.00 seconds
with 95.74% line coverage (8852/9246), but [the first Linux reference run](https://github.com/mgradwohl/MoTrack/actions/runs/42565821290)
rejected the original unedited AVI archive. Tracker 6.3.5 omits AVI from its automatic movie-extraction
extensions, then tries to pass the archive URL to Xuggle. This is tracked separately in
[#140](https://github.com/mgradwohl/MoTrack/issues/140); that fixture stays pinned as `archived-avi`, with
native/headless import checks and an explicit application-acceptance exclusion. It is not counted as a
successful round trip. Standard archive output now refuses non-MOV movie members before replacing a
destination, including after native reopen. MoTrack can still open that AVI archive and save a native copy.

The positive reference now uses a genuine QuickTime raw RGB MOV generated from independently specified
pixels/timing. Native and pinned headless Xuggle decoders verify every pixel and timestamp. Both movie
and archive generators are byte-identical across UTC and Pacific/Kiritimati. Baseline archive loading
uses a fresh directory containing only the archive, so adjacent fixture media cannot stand in for extraction;
Tracker's extraction writes are also isolated from the committed fixtures. Corrected Windows validation
passes all ten CTest groups in **123.28 seconds** with **95.74% line coverage (8857/9251)**, all 89 Python
tests, all 30 native/headless Java import cases and all four native archive export/oracle/reimport modes.
Formatting and static analysis of all five changed C++ sources pass; three existing LLVM profile-mismatch
warnings remain visible. The following CI attempts supplied the application/three-platform evidence below.
The next Linux attempt (`4efb381`, run 42567671197) loaded the MOV archive but exposed missing cache
initialization in the bare-`TFrame` reference harness: with `ResourceLoader.ospCache` unset, extracted
movies are marked for deletion on JVM exit. The harness now uses Tracker's public `setOSPCache`
preference API under each subprocess's private home, records that policy and verifies it in all-tab
reports. A bounded headless resource experiment reproduced deletion without the setting and exact-byte
retention with it; the ordinary AVI extraction omission remains. This sets up the standard cache policy;
it does not copy media or change the application loader, saver, expected data or fresh-process gate.
The existing MOV archive slice merged in [PR #139](https://github.com/mgradwohl/MoTrack/pull/139) at reviewed
head `f166ba9`, with all six checks passing in [run 42568545597](https://github.com/mgradwohl/MoTrack/actions/runs/42568545597)
and **95.82% combined C++ line coverage (8887/9275)**. The isolated baseline and all four edited modes passed
actual Tracker loading, saving and fresh-process reopening of both tabs, full media hashes and native reimport.
Broader archived containers, filters, variable-rate timing and broader movie schemas remain unfinished.


## Creating a portable movie archive

`writeTrackerMoviePackage` creates a new `.trz` from an audited, fully indexed constant-rate linked MOV
experiment. It shares the bounded staging/ZIP adapter used for images, preserving the one-way boundary:
project persistence does not link media decoders. The package contains `experiment.trk` and
`media/movie.mov`; the complete source movie is copied byte for byte, including frames excluded by a
trimmed or strided clip. No transcode, frame renumbering or timestamp change is performed.

The standalone movie writer validates mass/mark/clip-range, existing calibration and audited point-mass
identity/membership edits, with unchanged timing against the retained source envelope. Staging clears runtime media bindings only in its
private snapshot, so generated members resolve inside the package. Before atomic replacement, the
normal archive reader reopens the ZIP, resolves its media, and FFmpeg decodes/indexes the staged movie.
Its complete frame count and relative timestamps must match the retained document index. Corrupt media,
changed timing, incomplete indexing, other containers (including AVI under #140), existing archives,
unsupported edits, file aliases and cancellation are refused without replacing an existing destination.
The existing 512 MiB total extracted-media and bounded archive/index/decoded-pixel limits apply.

`writeTrackerMoviePackageCopy` uses the same destination lock and pending-recovery refusal as image
export. The desktop's **Export portable archive…** command selects the movie adapter for indexed linked
MOV imports, runs it on the bounded save worker, and retains the session's current title, save destinations,
checkpoints, dirty state and undo/redo history. The copied archive has no automatic native companion;
opening it enables the existing paired Save workflow, including a native companion when requested.

Native tests cover source/native reopen and removal, relocation, complete movie bytes, clip edits,
subsequent embedded native saving and archive resave, stale timing, corrupt sources, unsupported schemas,
cancellation and shared save locks/recovery. Desktop cases exercise export after indexing, ordinary Save
and undo/redo after export, cancellation, refusal of AVI, media removal/relocation, frame seeking and
paired saving of the reopened package. Three independent corpus modes require actual Tracker acceptance:
`packaged-movie`, `packaged-movie-clip` and `packaged-movie-clip-single`. All three passed actual Tracker
load/save/fresh reopen in [PR #141](https://github.com/mgradwohl/MoTrack/pull/141), reviewed at `d0e1a66`;
[CI 42583275559](https://github.com/mgradwohl/MoTrack/actions/runs/42583275559) passed all six checks with
**95.83% combined C++ line coverage (8931/9320)**. The `packaged-movie-calibration` extension passed
in PR #142. All seven `packaged-movie-keyed-calibration` variants passed in PR #143. Broader movie formats and full export authoring remain unfinished.

Local Windows validation at `f18cecd` passes all ten CTest groups in **127.87 seconds** with
**95.75% C++ line coverage (8897/9292)**, 90 Python corpus tests and all three native package
export/oracle/reimport modes. All tracked C++ formatting and five changed-source static analyses pass.
The three existing LLVM function-profile mismatch warnings remain visible.

The subsequent review correction sends the shared image/MOV destination-refusal message through Qt
translation and adds unfinished entries to all three catalogs. Its rebuilt package tests (12 cases),
affected-source formatting/static analysis and catalog checks pass; package behavior is unchanged.

### Movie point-mass authoring

The movie writer now applies the existing source-binding, name-reference and membership audits using
its explicit movie schema. Selected/unselected renames, empty/marked creation, selected/unselected/all
mass deletion and same-name replacement retain stable native IDs through history and native reopen.
Deletion clears the original selected-track reference; a new track with the same name does not inherit
that deleted identity. Known source relationships still require unambiguous resolution, and unknown
fields, dangling links, duplicate/reserved names and out-of-movie marks refuse output atomically.
Broader dependencies, numeric timing edits and mass/time-label edits remain refused. Media-backed
independent vectors are described in the following section.

The two-mass MOV fixture and unchanged image experiment provide 28 linked/archive/paired-recovery/
portable-package reference modes. All pass local whole-XML/media/numeric oracles; the desktop paired-save
regression exercises create, rename, mark, delete, undo/redo and native reopen. All 28 authoring modes
passed actual Tracker application acceptance in [PR #144](https://github.com/mgradwohl/MoTrack/pull/144),
reviewed at `951d92a` with all six checks passing and 95.83% combined line coverage in
[CI 42610128356](https://github.com/mgradwohl/MoTrack/actions/runs/42610128356).


### Independent vectors on image and movie media

The image and constant-rate movie adapters now pass independent vectors through the shared
source-binding, identity, membership and geometry audits. Sample tail/component edits, additions,
removals, the first sample of an empty track, Unicode naming, empty/sampled creation, deletion and
same-name replacement use ordinary reversible commands. All samples must stay within the complete
image inventory or indexed movie, even when outside the selected clip. Native close/reopen and
reordered native lists retain IDs; standard reimport reconstructs bindings in emitted object order.

The snapping and numerical guards require ordinary Arrow/BoldArrow footprints, finite reconstructable
components, bounded dense arrays and no point-mass snap providers for sampled vectors. Every tail
must remain more than eight image pixels from every other potential vector tip and every effective
coordinate origin. Tracker's first draw can expose a trail sample at another playhead; checking only
the sample frame's origin is insufficient. Fixed origin uses the last loaded key. Varying-origin
sample/key comparisons are bounded to one million; the existing 4096-sample bound also limits tip
comparisons. Per-frame calibrated components, endpoint reconstruction and magnitude must remain safe.
Mixed/dependent vectors, vector sums and full display scaling/attachment semantics remain unfinished.
This extension does not change numeric media timing.

Four independent Java-authored fixtures cover linked image and MOV projects and corresponding
mixed-tab archives. Six scenarios run through linked TRK, existing TRZ, interrupted/recovered paired
TRZ and portable TRZ for each media kind (48 modes). Full XML, untouched compressed ZIP records,
all media bytes, independent image/calibrated components, selection, created-track appearance and
native reimport are checked. The desktop regression creates, renames and draws a vector, deletes it,
undoes/redoes, saves both files and opens the native companion again. All 48 modes passed actual
Tracker first-draw/load/save/fresh-JVM reopen in [PR #145](https://github.com/mgradwohl/MoTrack/pull/145),
reviewed at `23b3a10`, with all six checks and 95.83% combined C++ line coverage passing in
[CI 42620528465](https://github.com/mgradwohl/MoTrack/actions/runs/42620528465).

The following varying-coordinate extension retains the original fixed flags and existing keys,
supports editing/adding keys, and includes every vector sample when bounding coordinate updates.
Fourteen Java-authored image/MOV coordinate variants cover all seven combinations of fixed and
varying origin, angle and scale. Their linked/archive/package/recovered-pair cases combine coordinate
edits with sample replacement, deletion and addition (56 modes). The corresponding 28 baseline sessions
and 56 edited modes passed actual Tracker load/draw/save/fresh reopen in [PR #147](https://github.com/mgradwohl/MoTrack/pull/147), reviewed at `dc8fed8`, with all six checks and 95.86% coverage in [CI 42655960136](https://github.com/mgradwohl/MoTrack/actions/runs/42655960136). Native
and desktop tests cover history, source removal, native reopen, unchanged raw vectors on coordinate-only
edits, and another frame's origin within or exactly on the snapping boundary. Fixed-flag toggles,
key deletion and moving reference frames remain separate work.

These acceptance fixtures use a zero time origin. The original negative-origin fixtures exposed
[Tracker's missing negative-time vector analysis rows](https://github.com/mgradwohl/MoTrack/issues/146).
Their raw vectors and timing survive, but the live analysis omission is recorded as a non-accepted
reference defect with independent correct expectations. Saving does not shift time to conceal it;
negative-time native vector tables/plots/export and complete reference analysis parity remain open.

### Multiple edited members in an existing archive

The storage adapter now accepts a bounded nonempty set of document snapshots through
`writeTrackerArchiveDocuments`. Each must name a distinct existing member, retain the same immutable
original archive SHA-256, and pass the single-member writer's source, edit, calibration, media-byte
and movie-index checks. Separately reopened native companions can participate without sharing a
payload pointer. Their owned source and extraction directories are all protected from output writes;
every payload's source-file aliases are rejected, including a later owner's link to the output.

The low-level writer accepts borrowed XML views, retained by the caller for the synchronous call.
Rendered XML is accumulated with an incremental 512 MiB aggregate bound and a 32 MiB per-member
bound; final extracted size, including unchanged members, also stays within 512 MiB. Classic ZIP,
4096-entry, 8 MiB directory and 544 MiB compressed-container limits remain unchanged. Every member
must occur exactly once. Physical local-record order and central-directory order are preserved
independently, including when they differ. Untouched compressed records/descriptors and comments
remain exact; only necessary central offsets change. All edited members are re-read from the staged
archive before atomic destination replacement.

This API always renders against the original archive. Its caller must supply the complete set of
edits to retain, including previously saved members. It does not update session checkpoints, retain
closed-tab edits by itself, or define a shared native workspace format. Archive-group persistence,
save coordination and recovery precede the tab UI, as specified in the migration plan.
Merged [PR #152](https://github.com/mgradwohl/MoTrack/pull/152) passes all six checks with 95.88% combined
coverage at `e73ea6f`, including all four actual Tracker grouped modes. Both experiments load, save,
freshly reopen and reimport with independent expectations. No new UI strings were introduced.

## Shared archive-group snapshots

The new `ArchiveGroup` persistence API uses a version-6 envelope with `kind: "archive-group"`.
Standalone document writers remain at version 5; standalone readers accept versions 1–5 and reject
group envelopes instead of selecting the first experiment. Desktop dispatch for groups is not yet wired.

A group stores a canonical non-null UUID, the SHA-256 of its original archive, and ordered member
records containing their own canonical UUID, exact original archive member key and complete version-5
document metadata. The container still has exactly `project.json` and `tracker/source.trz`: every
record shares one immutable archive owner. Unknown experiments, media and opaque members remain in
that original payload. Member records are independent of visible tabs; undo histories and live
session identities are not serialized.

Promotion adopts a known-member native document's owner and current edits. Appending a document
requires that exact owner, not only an equal digest. Independently opened copies remain distinct
logical groups, even when their persistent IDs and source hashes match. A live controller must use
separate runtime identities when coordinating saves; persisted UUIDs are not completion tokens.

Reads and writes reject empty or duplicate members, duplicate/nil/malformed IDs, foreign member keys,
inconsistent original-source bindings and missing/mismatched archive payloads. Writes authenticate
each extracted source against its import-time hash, validate every encoded document by decoding it,
and count compact metadata incrementally before appending it to the group array. Aggregate JSON stays
within 128 MiB; the existing per-XML/archive/container limits and owned-directory/alias protections
remain. Cancellation and errors preserve an existing destination through the shared atomic storage API.

The new local regressions cover native relocation, original-input removal, two edited experiments,
an untouched third experiment, opaque bytes, promotion/ownership isolation, undo/redo, malformed
metadata and refused/cancelled writes. The corpus additionally compares independent-companion export
with shared-group export byte-for-byte before actual Tracker acceptance. Merged #155 at `a672849`
passed all six checks, review and 95.94% combined coverage in
[CI 42737686781](https://github.com/mgradwohl/MoTrack/actions/runs/42737686781). All 237 edited cases
passed, including the four group modes. Live group checkpoints and desktop tab lifecycle remain
later layers; this low-level snapshot API does not mark live documents saved.

## Coordinated archive-group file writes

The next worker-adapter layer adds `writePairedArchiveGroup`: it stages all retained edits into one
standard `.trz` and one same-stem version-6 `.motrack`, reopens and compares every native member, then
commits through the existing paired-save engine. Standalone and group saving share destination locks,
bounded fingerprints, stage validation, observation ordering and the unchanged version-1 recovery
journal. Groups reject `.trk` output, foreign archive owners and unsupported member edits before
replacing either destination. Native-only `writeNativeArchiveGroup` uses the same lock and refuses
pending paired recovery, including a transaction initiated by a standalone save.

Interruption before the journal is published leaves both destinations unchanged and removes staging.
After publication, the existing explicit `recoverPairedDocuments` can finish the complete pair without
the original group, input archive or any live document session. It verifies the expected transaction,
every stage and each existing output before further replacement. Changed destinations or a replaced
journal are preserved for investigation; opening a project never starts recovery automatically.

Five public-API regressions cover source removal, all five interruption boundaries, invalid member
snapshots, native/standalone/group lock contention, post-commit observer failure and transaction/output
conflicts. The four grouped image/MOV corpus modes additionally recover an interrupted pair after
destroying the prior group owners and deleting its previous native file; recovered identities, models
and shared ownership must agree, and the standard output must remain byte-identical to both preceding
export routes. Merged #156 at `a8e6e83` passed all six checks, review and 95.98% combined C++ line
coverage (9280/9669) in [CI 42738323793](https://github.com/mgradwohl/MoTrack/actions/runs/42738323793).
All 237 unique edited Tracker cases passed, including the four paired-group recovery modes; the
three known Tracker defects remain explicitly unaccepted. Local Windows testing also passed all
10 CTest groups and four extended native corpus routes before CI.

These worker results describe written files. Live group membership revisions and per-session native/
standard checkpoints, stale completion handling and desktop close/discard behavior still require the
next controller layer; successful recovery alone does not establish that current live edits are saved.
