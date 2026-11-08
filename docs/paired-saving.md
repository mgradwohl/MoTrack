# Standard and native paired saving

Status: proposed implementation contract for the next phase-3 slice. The current desktop still
saves only native documents; this document does not claim paired saving or recovery is available.
The [full-port plan](migration-plan.md#implementation-order-and-exit-gates) and
[format guide](native-format.md) remain the canonical status and supported-schema references.

## Contract and order

A requested standard `.trk` or `.trz` may have a sibling `.motrack` companion with the same stem.
Both represent one owned document snapshot. Existing audited writers continue to enforce their
schema, source-preservation, media and scientific checks; pairing cannot expand supported exports.
The standard file remains usable by Tracker without reading the companion.

1. Resolve the existing physical destination directory; reject output-file aliases, colliding
   destinations, media/source-payload overwrites and an unresolved prior save for the same stem.
2. Acquire a cooperative per-stem lock. Capture bounded content fingerprints of existing outputs.
3. Prepare both complete outputs in private temporary files in the destination directory. Keeping
   their parent equal to the final parent preserves relative linked-media paths. Rebase a native
   snapshot through the same adapter used by ordinary native Save. Validate both staged outputs.
4. Persist a bounded recovery record before replacing either destination. It records only sibling
   filenames, the two original fingerprints (or absence), staged filenames and new fingerprints.
   A record never supplies unrestricted external destination paths.
5. Replace the standard file atomically, then the native companion atomically. Recheck expected
   destination contents before each replacement. Report each completed write separately.
6. Remove the recovery record and owned staging files only after both outputs match the intended
   bytes. A cleanup failure must not misreport successfully written content as missing.

Two filesystem replacements are not one atomic operation. Preparation failures leave both original
outputs intact. Once the standard replacement succeeds, a later failure or cancellation leaves that
valid standard file in place, retains the previous native file, and preserves the prepared companion
plus recovery record. Do not silently mark the pair saved or roll back a newer externally edited file.

## Recovery and ownership

Recovery validates the record format, filename relationships, size bounds, staged contents and
physical-directory containment before writing. Each destination must still match either its recorded
original state or its intended new state. A third-party change, alias, missing/corrupt staging file or
ambiguous record refuses further replacement and preserves available files for explicit resolution.
A fully completed pair with a leftover record only needs safe cleanup. A missing destination is an
explicit original state, not a wildcard authorizing overwrite.

The initial API is an internal adapter operating on an immutable snapshot in a worker. It reports
saved, canceled, failed-before-commit and recovery-required outcomes, with individual output status.
Cancellation is checked before preparation and at bounded copy/commit boundaries. Progress callbacks
must never touch widgets directly. Session identity/revision checks remain necessary when applying
completion: an old or replaced document cannot become clean because its worker finished.

Process-interruption recovery is an acceptance target. This contract does not promise durability
against arbitrary power loss, hostile local filesystem races or hardware failure. The initial pair
uses sibling files; different-directory companions and general multi-file/media transactions need
separate contracts. Recovery records from imported archives are data, never automatically executed.

## Required evidence before desktop integration

- Standalone and existing-archive pairs reopen independently; edits, unknown source XML, all archive
  members and media associations remain correct after native close/reopen and source-input removal.
- Existing destination files survive preflight, unsupported-export and staging failures.
- Cancellation before commit changes neither output. Interruption/cancellation after the standard
  commit leaves a valid recoverable partial pair; a later recovery completes exactly that snapshot.
- Conflicting external edits, corrupt records/stages, aliases, path escapes and concurrent saves are
  refused without overwriting unrelated files. Cleanup removes only verified transaction artifacts.
- Undo/redo, stale completion and document replacement retain correct per-output save state. Desktop
  actions and translated recovery messaging follow the tested internal API, not an assumed atomic pair.
- Actual Tracker load/save/fresh reopen and native reimport pass for emitted standard files on the
  pinned corpus. Windows/macOS/Linux checks and the 90% combined production coverage gate remain required.
