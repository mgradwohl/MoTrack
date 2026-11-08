# Standard and native paired saving

Status: internal phase-3 paired writer, explicit recovery adapter and per-output session checkpoints
are implemented. Earlier three-platform and actual Tracker acceptance is recorded below; later review
fixes require current-revision CI. Desktop standard Save and explicit recovery controls are implemented
in the current slice; cross-platform acceptance of the desktop controller remains pending.
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
5. Replace the standard file atomically, then the native companion atomically. Recheck both expected
   destination contents and every present stage before each replacement, including the already
   committed standard file. Report each completed write separately.
6. Remove the recovery record and owned staging files only after both outputs match the intended
   bytes. A cleanup failure must not misreport successfully written content as missing.

Two filesystem replacements are not one atomic operation. Preparation failures leave both original
outputs intact. Once the standard replacement succeeds, a later failure or cancellation leaves that
valid standard file in place, retains the previous native file, and preserves the prepared companion
plus recovery record. Do not silently mark the pair saved or roll back a newer externally edited file.
Final validation clears an output's current flag if it changed or cannot be read; cancellation or
cleanup failure preserves independently confirmed successful output status.

## Recovery and ownership

Recovery validates the record format, filename relationships, size bounds, staged contents and
physical-directory containment before writing. Each destination must still match either its recorded
original state or its intended new state. A third-party change, alias, missing/corrupt staging file or
ambiguous record refuses further replacement and preserves available files for explicit resolution.
A fully completed pair with a leftover record only needs safe cleanup. A missing destination is an
explicit original state, not a wildcard authorizing overwrite.

The initial API is an internal adapter operating on an immutable snapshot in a worker. It reports
saved, canceled, failed-before-commit and recovery-required outcomes, with individual output status.
For a known transaction, an absent journal observed under its lock is reported separately as
recovery-not-needed; this does not claim either destination is current.
Cancellation is checked before preparation and at bounded copy/commit boundaries.
Cancellation reported by nested archive/native adapters is classified consistently before commitment. Progress callbacks
must never touch widgets directly. Session identity/revision checks remain necessary when applying
completion: an old or replaced document cannot become clean because its worker finished.

Process-interruption recovery is an acceptance target. This contract does not promise durability
against arbitrary power loss, hostile local filesystem races or hardware failure. The initial pair
uses sibling files; different-directory companions and general multi-file/media transactions need
separate contracts. Recovery records from imported archives are data, never automatically executed.

## Adapter and compatibility evidence

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

## Internal implementation and verification

`writePairedDocuments` and `recoverPairedDocuments` live in `paired_save.hpp`. They operate on an
owned snapshot without widgets or Java. The result reports a diagnostic, each output path and whether
each output is current; `RecoveryRequired` retains the journal path for explicit follow-up. An existing
journal prevents another pair from replacing its outputs. The same physical-path rebasing helper is
used by native desktop Save and by the native companion.

Stages use reserved sibling names and release their temporary-file handles before the atomic
writers replace them, including on Windows. A scope guard removes uncommitted stages; ownership
passes to the journal only after its atomic write succeeds. Recovery permits cleanup after one stage
has already been removed, but refuses to delete altered staging content. Bounds are 64 KiB for the
record, 32 MiB for standard XML, the archive reader's existing 544 MiB for TRZ, and the native format's
bounded metadata plus archive payload. Fingerprints use streaming SHA-256 with 64 KiB blocks.

Public-entry-point regressions cover standalone and two-tab archive pairs, native reopen, original
archive removal, existing-output preservation, cancellation at all five progress boundaries,
external edits, missing/corrupt stages, malformed/escaping records, concurrent cooperative writers,
physical-directory aliases and output/stage/record file aliases. A bounded child probe exits without
destructors after the first replacement; the parent process must recover through the stale lock and
remove only the transaction's files. This exercises process interruption, not power-loss durability.

Two additional corpus modes interrupt a pair after its standard commit, recover it, and reopen the
native companion before passing the recovered standard file through the existing independent
XML/ZIP/media oracle and actual Tracker load/save/fresh-process reopen/native-reimport gate.
Both application modes passed at `32c21d6` (the downstream checkpoint branch), Linux job
128747171486 in [run 42485770063](https://github.com/mgradwohl/MoTrack/actions/runs/42485770063).
The same run passed Windows/macOS/Linux builds, both setup checks and combined coverage.
Subsequent fixes still require current-revision CI and review.

Local Windows validation on 2026-11-08: all six CTest suites passed, including process-loss recovery;
combined production coverage was 95.32% (7632/8007 lines). Formatting and clang-tidy passed, as did
76 corpus-harness tests, native corpus checks and both recovered-export XML/media/ZIP oracles.
These local results are separate from the later three-platform/application evidence above.


Long operations disable Qt's age-based lock expiry with `setStaleLockTime(0)` while retaining
its dead-process detection. This follows [Qt's long-lived lock guidance](https://doc.qt.io/qt-6/qlockfile.html#details).
A regression copies a live owner's lock identity to an aged claim without an OS advisory lock,
so timeout-based theft cannot be masked by native locking support. Both new writes and recovery
must refuse that claim; the separate terminated-child regression still proves dead-owner recovery.

After the destination-status review fixes in `7d04cd4`, all six Windows suites and formatting/clang-tidy
pass; coverage is 95.34% (7679/8054). Both output statuses are refreshed before reporting conflicts or
read failures, so an early conflict cannot hide or falsely retain a companion commit. The inherited
source-digest and standalone-media corrections require the next combined current-revision CI run.



## Per-output session checkpoints

The edit history keeps separate native and Tracker save points. Undo/redo can return either output
to its saved state independently; a new branch of edits invalidates save points in discarded history.
Consumed track IDs remain persistent changes even after undoing creation, so both save points are
invalidated. A failed write changes neither save point; a stale completion cannot clear newer edits.

`DocumentSession::completePairedSave` applies only confirmed per-output results and retains pending
recovery records for every interrupted destination. Saving another pair cannot discard an earlier
record or report the session clean while it remains. Recovery removes only its own record and cannot
redirect the current save target to an older destination. Once a pair is required, a native-only save
cannot clear outstanding Tracker edits.
A partial pair remains modified even when both replacements finished but recovery cleanup failed.
Replacement of the document invalidates all earlier completion tokens.

`completePairedRecovery` deliberately does not mark the live document saved: the record can refer
to an older snapshot or predate this session. Successful recovery clears the pending record and
requires a fresh paired save to establish both output checkpoints for the current live snapshot.
This preserves new edits made after interruption. The desktop controller uses these session queries and completion methods for standard Save and
explicit recovery, including the ordinary Save and close/discard flow.

The multiple-destination recovery correction passes all six Windows suites with 95.46% combined
production line coverage (7780/8150). Checkpoint formatting/static checks pass. Its regression performs
two interrupted saves, saves a third destination, recovers both earlier records and finally saves the
current pair. Later inherited corrections still require combined current-revision CI; desktop integration
remains separate.

Prepared-file fingerprints are fixed immediately after each validated write, before notifying progress
observers. A changed stage is rejected before journaling or destination replacement. Linked media is
compared against both output paths by filesystem identity, including aliases and hard links. Distinct
case or Unicode spellings remain usable when the filesystem distinguishes them; a media file with a
project-like filename extension still cannot be overwritten. When identity is unavailable, the fallback
compares exact absolute paths rather than unconditionally folding their case.

The combined review-corrected stack passed all six Windows suites with 95.54% line coverage
(7803/8167); this includes immutable archive export, unsupported ZIP layouts imported before
validation, multiple recovery destinations, case-folded media collisions and prepared-stage changes.
Current-revision cross-platform CI remains required before merging.

## Desktop save and recovery controls

File → Save Tracker project chooses the standard destination and confirms replacement of an unrelated
existing companion. The current adapter always writes a same-stem `.motrack` alongside it. Ordinary
Save uses the pair after that choice; Save MoTrack copy as keeps a separate native-only path available.
The paired preference is currently session-local: reopening starts with the default native save behavior
until a standard destination is chosen again. Persisting this choice is separate from interchange data.

A one-worker save pool receives an owned snapshot and cancellation token. A modal progress window
keeps the event loop responsive and supports cancellation, while ordinary document input is blocked.
Programmatic edits still advance the revision; prepared project/movie opens wait until save bookkeeping
finishes. Canceled open requests are rejected, and replaced documents never accept old completions.
Window destruction cancels the worker without permitting it to access widgets.

A pending record is recovered only after explicit confirmation. Recovery is followed by a fresh write
of the current snapshot. Conflicts keep the record and live edits available; a native-only save cannot
clear outstanding standard edits. Status distinguishes failure before commit, cancellation, partial output,
recovery cleanup, stale snapshots and successful paired saving. New UI strings are marked unfinished
in the three non-English catalogs and fall back to English until translated.

Desktop regressions exercise edited linked/archive pairs and ordinary Save, recovery of older data with
newer edits, conflicting recovery, cancellation and overlapping save/close requests, stale and replaced
completions, queued/canceled project and movie opens, unsupported exports, companion replacement
confirmation, and window destruction during a queued save. Current Windows validation and required
cross-platform CI are reported with the desktop PR. The established actual-Tracker paired corpus gates
continue validating the same adapter; these UI tests do not independently establish broader export parity.

Local Windows desktop validation: all thirteen paired-workspace tests and all six CTest suites pass.
Combined production line coverage is 95.59% (8044/8415), above the unchanged 90% gate.
The 76 Python corpus tests and native corpus checks also pass.
Formatting and clang-tidy pass for the controller and desktop tests. Linux/macOS/current-head CI
and code review remain required before this desktop slice can be merged.

Every journal contains a persistent transaction UUID, returned by both paired writes and recovery.
A new transaction at the same pathname receives a new identity. Session completion checks use this
identity so a delayed result cannot clear a newer pending transaction. Missing/invalid identities are
refused before recovery modifies either output. This is part of the initial, unmerged journal format.

The session retains every known pending transaction identity, including multiple successive identities
at one journal pathname when worker completions arrive out of order. A recovery checkpoint selects
the first pending identity at each path; applying its result removes only that identity, leaving newer
ones pending. Each core result carries a process-local observation sequence assigned under its journal
lock. A confirmed absent or recovered journal supersedes only older observations at that path, including
unreadable records whose UUID was unavailable. Delayed callbacks cannot resurrect those records, while
a genuinely newer unreadable or restored journal remains pending. These sequences are never persisted
or compared across processes. A successful save elsewhere cannot hide pending entries.

Recovery checkpoints capture the selected pending path-to-transaction identities. Applying a delayed success
requires the recorded transaction still to match both the checkpoint and result. A newer transaction
at the same path therefore remains pending. Recovery also preserves an existing native Save As
target, including one selected while recovery was running; only an unset native target is adopted.

Save dialogs capture their originating session before entering a nested event loop. A prepared open
that replaces the document cancels the old chooser, companion confirmation or recovery confirmation.
Recovery confirmation also copies the destination and checks the pending transaction identities.
Desktop regressions accept those dialogs after asynchronous replacement and verify both projects,
companions and recovery records remain unchanged. The progress dialog is parent-owned; its own
connection contexts bound cancellation callbacks, and the window lifetime guard prevents access
after destruction.

The recovery worker receives the transaction identity captured for confirmation and validates it
under the same per-stem lock used to commit. If another instance replaced the journal, recovery
changes no files, reports the newly observed identity, and requires another explicit confirmation.
If that instance already removed the known journal, the session clears only that obsolete pending
entry and a fresh paired write is still required. Delayed blocked retries cannot restore a cleared
identity or overwrite a newer pending transaction. Direct accessor arguments are copied before list
mutation, so clearing the first pending entry cannot invalidate the path used for identity cleanup.

Native and paired media-overwrite checks compare actual filesystem identity. Distinct case or Unicode
spellings remain usable where the filesystem distinguishes them; true aliases stay protected. The
Tracker filename chooser is destroyed before companion/recovery/progress dialogs begin, including
when the window is destroyed during an action-triggered save.

Each output also has a session invalidation generation. A write captured before stale disk content or
recovery invalidates that output cannot later mark it clean just because its edit revision matches.
A fresh write must follow that invalidation. The desktop recovery-and-save sequence must apply recovery
before capturing the fresh write checkpoint, or explicitly carry forward only that operation’s recovery
invalidation while retaining the original snapshot revision. Real-file regressions cover writes committed
in one order and reported in another, unknown journals completed externally, and newer/restored records.

Failed recovery attempts also retain their lock-observed journal state, including an unreadable UUID
or a restored record with the same UUID. A delayed earlier success cannot clear that newer failure;
the pending journal continues to keep the session modified even after a successful save elsewhere.
This holds in either callback delivery order: a newer failed observation restores the pending entry
even if the older successful callback already removed it.

A recovery request may carry a known UUID or an explicitly unidentified expectation from an unreadable
journal. The adapter checks that expectation under the journal lock. If the journal now has a valid
UUID, an unidentified request returns its identity without replacing either output; the caller must
obtain fresh confirmation. An absent journal can be reconciled without writing. Only an adapter call
that omits the expectation entirely requests unrestricted explicit recovery.


Filename, companion, recovery and save/discard prompts are parent-owned heap dialogs guarded across
their nested event loops. Closing the owning window during any prompt cannot delete a stack widget
or resume a save through a destroyed controller. Prepared project/movie opens also check lifetime
before updating status after a discarded prompt. These cases have direct menu/open/close regressions.
