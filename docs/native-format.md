# Native document compatibility

MoTrack saves a bounded JSON document with `format: "MoTrack"`. The current writer emits version 3;
the reader accepts versions 1, 2 and 3 and rejects unknown versions. Existing measurement, calibration,
clip, media-reference and movie-time fields retain their version-1 meanings.

## Track identities

Version 3 stores an `id` on every editable point mass and independent vector and a document-wide
`nextTrackId` counter. These are canonical positive decimal strings, preserving the full unsigned
64-bit range without JSON floating-point rounding. IDs are unique across both track types, scoped to
one document, and independent of names and list positions. Zero is reserved for uninitialized records.
Import assigns IDs; creation allocates a fresh ID; rename, mark edits, deletion/undo and creation/redo
retain the original identity. The allocation counter never rewinds, including after undo or deletion,
so newly created tracks cannot reuse an earlier identity. An exhausted counter disables creation.

Native reads and writes reject zero/duplicate IDs and a counter not greater than every live ID. Writes
validate before replacing the existing file. Versions 1 and 2 receive fresh identities during migration;
subsequent version-3 saves retain them and the counter, including after the highest-ID track is deleted.
This migration does not infer associations with original Tracker XML from names or current list order.
Source-object bindings and commands addressed by IDs remain separate implementation work.

## Tracker import source

Versions 2 and 3 require `trackerSource`, either `null` for a document without preserved source or an object
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
limit remains 128 MiB. The reader checks the encoded length before decoding, requires strict canonical
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

## Version-1 migration and remaining work

Version 1 did not retain source XML. Reading it produces no source envelope; saving it emits version 3
with `trackerSource: null`. Migration preserves its editable data but cannot recover content previously
discarded. Earlier MoTrack builds that only accept versions 1 or 2 cannot open version-3 documents.

For a `.trz` import, this first preservation slice retains the selected tab's XML. Media still depends
on the referenced original archive, and other archive members/tabs are not embedded. Associations between
edited records and original XML objects, complete archive preservation, per-document
session ownership, and standard-file writers remain unfinished. An unknown object retained in this
envelope is not an implemented track type.

See the [full-port implementation order](migration-plan.md#implementation-order-and-exit-gates) and
[round-trip acceptance requirements](migration-plan.md#tracker-round-trip-compatibility). Tests exercise
[native validation and migration](../tests/workspace_document_test.cpp) and
[normal desktop import/edit/save/reopen](../tests/workspace_test.cpp).
