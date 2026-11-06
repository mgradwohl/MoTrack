# Native document compatibility

MoTrack saves bounded JSON metadata with `format: "MoTrack"`. The current writer emits version 5;
the reader accepts versions 1 through 5 and rejects unknown versions. Archive-backed documents use
the native ZIP container described below; other documents remain plain JSON. Existing measurement, calibration,
clip, media-reference and movie-time fields retain their version-1 meanings.

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

See the [full-port implementation order](migration-plan.md#implementation-order-and-exit-gates) and
[round-trip acceptance requirements](migration-plan.md#tracker-round-trip-compatibility). Tests exercise
[native validation and migration](../tests/workspace_document_test.cpp) and
[normal desktop import/edit/save/reopen](../tests/workspace_test.cpp).
