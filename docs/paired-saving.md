# Standard and native paired saving

Status: the internal paired writer, explicit recovery adapter, per-output session checkpoints and
desktop paired Save/recovery controls are merged. Final session revision 58540b7 passed all six
checks and review in [PR #126](https://github.com/mgradwohl/MoTrack/pull/126),
[run 42546971061](https://github.com/mgradwohl/MoTrack/actions/runs/42546971061). Final desktop
revision 4c15c1e passed the same gates in [PR #127](https://github.com/mgradwohl/MoTrack/pull/127),
[run 42544952030](https://github.com/mgradwohl/MoTrack/actions/runs/42544952030).
Both recovered linked-image and archive-image exports passed actual Tracker load/save/fresh reopen
and native reimport. This includes the long-filename, copied-journal and completion-order corrections
described below. Earlier local validation records are historical; subsequent changes require new
current-revision checks and review.
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

Native writes, paired writes and recovery share a sibling `stem~` lock. Keeping the complete stem
lets the filesystem determine case and Unicode aliases; the one-character suffix leaves room for
Qt's `.rmlock` stale-owner cleanup suffix. Ordinary journals retain `.stem.motrack-save.json`.
If that name exceeds 248 UTF-8 bytes, the journal uses `stem!`, leaving room for Qt's seven-character
atomic-write suffix on filesystems with 255-byte filename components. These internal names do not
change either project format. Both journal forms block new writes, and recovery refuses ambiguous
simultaneous records. Their process-local identities remain separate even though they share a lock.
Older lock names are also acquired when they leave the same cleanup margin; an existing oversized
legacy claim is preserved and reported for attention instead of being ignored or removed unsafely.
Lock creation errors are distinguished from a live competing writer.
The Tracker XML reader uses Qt's Unicode and long-path file adapter too, so standard outputs beyond
Windows' ordinary path-length limit can be reopened. It retains the 32 MiB pre-read bound and rejects
read errors or a changed length instead of accepting a truncated input.

Regression cases exercise 240-byte ASCII and Unicode stems through native, paired and desktop Save,
interruption, dead-writer recovery and filesystem-specific case aliases. Existing journal and live-lock
compatibility remain covered. Local Windows validation passes all eight CTest groups in 163.49 seconds,
with **95.55% (8479/8874)** production line coverage. Formatting and static analysis pass for all seven
changed/integrated C++ translation units; three existing LLVM function-mismatch warnings remain visible.
The filename correction is included in the reviewed #126/#127 revisions and passing CI above.

After the destination-status review fixes in `7d04cd4`, all six Windows suites and formatting/clang-tidy
pass; coverage is 95.34% (7679/8054). Both output statuses are refreshed before reporting conflicts or
read failures, so an early conflict cannot hide or falsely retain a companion commit. The inherited
source-digest and standalone-media corrections subsequently passed the `6ae83b6` run recorded above.



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
current pair. The later corrections and desktop integration are covered by the merged evidence above.

Prepared-file fingerprints are fixed immediately after each validated write, before notifying progress
observers. A changed stage is rejected before journaling or destination replacement. Linked media is
compared against both output paths by filesystem identity, including aliases and hard links. Distinct
case or Unicode spellings remain usable when the filesystem distinguishes them; a media file with a
project-like filename extension still cannot be overwritten. When identity is unavailable, the fallback
compares exact absolute paths rather than unconditionally folding their case.

The combined review-corrected stack passed all six Windows suites with 95.54% line coverage
(7803/8167); this includes immutable archive export, unsupported ZIP layouts imported before
validation, multiple recovery destinations, case-folded media collisions and prepared-stage changes.
The merged revisions and cross-platform acceptance are recorded above.

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
Formatting and clang-tidy pass for the controller and desktop tests. The final controller revision
subsequently passed all six platform/coverage checks and review in #127, as recorded above.

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

Native-only saves share the owned cancellation token used by paired saves. Window destruction cancels
a queued save before it starts; native storage checks cancellation while copying embedded archives and
immediately before atomic replacement. Destruction tests preserve an existing destination through both
the direct and menu save entry points, with worker release ordered after cancellation.

Completed paired writes carry the snapshot's refreshed media bindings back to their live session.
Only a result that actually updated an output may adopt them; blocked retries and replaced-document
completions leave the current bindings unchanged. Recovery alone has no live-snapshot bindings to adopt.

Media bindings use the same process-local observation order: an older completed write cannot replace
locations from a later write, even if callbacks arrive in reverse order. Confirmed journal absence
reconciles pending records by observation order without requiring a transaction UUID; successful
recovery writes still require the expected UUID. Regressions remove the original alias and older media
before saving again, and reconcile externally completed journals through the default recovery API.

Storage progress callbacks expose archive chunks and the final pre-commit boundary on the writing
thread. Deterministic cancellation regressions stop during source-archive streaming and immediately
before plain/ZIP commit, preserving every original destination byte. Successful paired saves also
refresh the active image reader, preserving navigation after a temporary media alias is removed.

Successful paired writes retain their lock-observed journal pathname even after deleting the record.
Session completion reconciles older observations at that path, so an unreported external recovery
followed by a fresh successful write cannot leave a deleted journal pending. Newer observations at
the same path and unresolved records at other destinations remain pending in either callback order.

Desktop native saves hold the destination's sibling paired-save lock through replacement. An active
writer or an unresolved journal blocks replacement of that companion, including a file alias to it;
saving a native copy at another destination remains available. The worker checks the filesystem even
when this session has not yet observed the journal. Normal paired saving requires explicit recovery
before that companion can change, preserving its recorded fingerprints and staged recovery data.

The filename and confirmation helpers guard their parent-owned dialog borrows with synchronous
GUI-thread destruction notifications. A local QObject disconnects those callbacks before their flags
leave scope. This retains the existing parent-destruction behavior while avoiding a Linux Clang 18
weak-pointer cleanup diagnostic in the portable-export chooser; the existing destruction regressions
cover both helpers through native, paired and image-package menu actions.

Native-save conflicts carry structured reasons to the desktop: pending recovery explains how to use
Save Tracker project or choose another destination, while an active sibling save explains retry/copy
options. Both messages use the translation catalogs; failed saves keep existing files and dirty state.

Desktop portable-image archive export shares that destination lock and refuses unresolved recovery
journals before packaging. The lock spans preparation and atomic replacement, including cancellation.
Exports elsewhere remain available and do not change companion files or document save checkpoints.

Prepared project/movie opens use the same synchronous destruction guard while asking whether to save
the previous document. A dead window cannot receive the prepared result; the local connection context
disconnects before the guard flag leaves scope. No Qt weak-pointer control block spans these nested calls.

Journal observations also retain a process-local filesystem-slot identity. While the destination lock
exists, the worker compares it with live prior lock spellings using actual filesystem identity; it
never assumes a platform-wide case policy or folds Unicode names. A weak registry retains no abandoned
session/result ownership. Sessions keep the identities while their observation watermarks are live,
so delayed callbacks and externally recovered/deleted outputs cannot reintroduce a case-variant record.
These identities and observations are never serialized and are not needed across process restarts.
Recovery accepts the recorded sibling spelling only when its journal and lock resolve to the same
files as the requested spelling, after the existing regular-file/alias checks. A journal copied or
hard-linked under a different stem cannot authorize writes to an unlocked sibling pair.
Case-variant and Unicode regressions run both callback orders and with both outputs removed, and retain
distinct destinations on volumes where the names differ.

Completed transaction watermarks are scoped to the journal's filesystem identity as well as its UUID.
Copying an interrupted save and its journal to another directory therefore creates an independently
pending location, even when the copied UUID is unchanged. Recovering the original cannot suppress a
delayed observation of the copy. Each output destination also retains its observation order, so an
older successful callback cannot replace a newer selected destination or its saved checkpoint. Older
pending journals are still registered and reconciled independently of destination selection.

Recovery completion also checks the result's observed filesystem-slot identity against the requested
pending location before changing any session state. A successful, failed or absent result for the
original cannot clear or invalidate a copied journal's entry, even when their UUIDs match. This uses
the retained observation identity because successful recovery may already have deleted the journal.
Two successful paired writes of the live revision can complete in either order without a destination
switch alone invalidating their Tracker checkpoint. Stale revisions, recovery invalidation and newer
on-disk observations retain their independent guards.

Native-only writes and paired writes share this process-local completion order. The `writeNativeDocument`
adapter holds the same physical per-stem destination lock across native replacement and observation
stamping, and refuses pending recovery journals. It returns a `NativeSaveResult` containing its stamp
and original-reference media bindings. Low-level `writeWorkspaceDocument` performs storage only and
returns no completion token; desktop workers and real-file session callers use the locked adapter.
The cancellation flag reaches native serialization and storage: it is checked before serialization,
at archive-copy chunk boundaries and immediately before atomic replacement. Mid-copy and pre-commit
cancellation preserve the previous plain or archive-backed native file. A bounded storage observer
lets regressions cancel at those boundaries without relying on timing or large stress fixtures.
Its worker observer reports `NativeSaved` between replacement and stamping. A regression attempts
both an older native write and an older paired write in that interval: both are refused, the current
file remains on disk and only its matching completion can make the session clean. A later old write,
after lock release, receives a later stamp and leaves the session dirty. An exception from the
post-commit progress observer cannot change the successful storage outcome: the adapter still returns
the stamped completion so a written snapshot cannot become invisible to session bookkeeping. In all cases,
the worker must retain that result through callback delivery, rather than constructing a new stamp
on the GUI thread. A superseded native callback leaves the selected destination, saved checkpoint and
bindings untouched. A newer standard-only partial save can supply the latest media bindings while
an older native write still supplies the native destination. Pending journals remain independent of
these choices. Regression cases reverse both API and callback orders, remove the older media and save
again, and cover the interrupted standard-only case.

A lock-confirmed absent journal reconciles all older observations at that location, including when
the recovery checkpoint captured an unreadable UUID that a later known observation superseded. Newer
observations remain pending. Successful recovery writes still require the checkpoint's transaction to
match; confirmed absence does not authorize a write or mark the live document clean.

At `f6f5de7`, the location-scoped completion, mixed native/paired destination order and superseded-unidentified-checkpoint
regressions pass on Windows with all eight CTest groups (including all 28 session and 23 paired-save cases).
Combined local production line coverage is 95.49% (8108/8491); formatting and all six changed C++
translation-unit static analyses pass. Persistence and session cases now have separate CTest groups
from the UI, retaining every case and each 60-second limit. Current-revision CI/review remain required.

The combined desktop/package/movie integration at `1df6f03` passes all nine Windows CTest groups
with 95.70% production line coverage (8734/9126), including the mixed-order cases through the locked
native writer. All tracked C++ formatting and four affected integration translation-unit static
analyses pass. The four complementary workspace groups preserve every case and the existing
60-second limit per group. The local collector reports four function-profile mismatch warnings;
the coverage gate passes without suppressing them. Each PR still requires its own current-head
cross-platform CI and passing review.

The replacement/stamp lock regression, native cancellation/recovery guards and post-commit observer
failure cases pass locally on Windows at `9d08bd85`. The complete eight-group suite passes in
174.96 seconds, with 95.40% production line coverage (8174/8568); formatting and all five changed
C++ translation units pass static analysis. Three existing LLVM function-mismatch warnings remain
visible. The new interval regression deterministically attempts competing writes from the post-commit
observer before the outer result is stamped. It also checks that a later obsolete write invalidates
the session's saved state. Final head `c9f0d30` passes all six required checks in
[run 42540556261](https://github.com/mgradwohl/MoTrack/actions/runs/42540556261), with **95.47%
(8204/8593)** combined coverage and both recovered paired-image modes passing actual Tracker
load/save/fresh reopen and native reimport. Subsequent fixes retain their own current-head CI and review gates.

The copied-journal callback and older-first successful-save defects were reproduced before their
corrections. All 30 session cases and the two deterministic storage-cancellation cases then passed.
Local Windows validation of these review fixes passes all eight CTest groups in 141.23 seconds,
with **95.40% (8190/8585)** production line coverage, formatting and all six changed C++ translation
units passing static analysis. Three existing LLVM function-mismatch warnings remain visible.
The later fixes, including long-name handling, subsequently passed all six checks and review at
58540b7 in #126; the merged evidence is recorded at the top of this guide.

The filename-limit correction is included in the session prerequisite as well as the desktop slice.
Its three C++ files are identical to the already statically analyzed desktop correction. Independent
Windows validation of the prerequisite passes all eight CTest groups in 127.91 seconds with **95.41%
(8239/8635)** production line coverage, including long ASCII/Unicode names, stale-owner recovery,
legacy lock preservation and both journal forms. All tracked C++ formatting passes; three existing
LLVM function-mismatch warnings remain visible. This local result does not replace current-head CI
and code review.

### Shared completion state

`SaveState` separates destination selection, runtime identity, ordered completion observations and
recovery records from a particular document or undo history. The owner supplies the current revision;
each completion returns independent native/Tracker save-point actions and whether its media bindings
may be adopted. Every returned action is applied immediately on the owner thread, including actions
from partial or stale writes. A summary outcome alone is not permission to mark both outputs saved.

`DocumentSession` retains its public API, document, history and dirty-state rules. It delegates the
bookkeeping and applies the returned actions to its existing `TrackEdits` save points. Replacement
prepares fresh state before installing the document; old completion tokens retain their old runtime
identity. Save-state ownership can be transferred, but cannot be copied into an independent session.

This extraction prepares shared archive-group coordination without copying the journal/observation
algorithm. It does not yet implement group membership checkpoints, a workspace controller or tabs.
The group controller must additionally capture every member identity/revision, retain closed members
and track membership changes independently of individual histories. Native/standard formats, worker
results and the explicit recovery contract remain unchanged. The existing session and desktop suites
exercise this refactoring through their unchanged public entry points.

Local Windows validation passes all 10 CTest groups serially in 153.80 seconds, with **95.93%**
production-line coverage (9317/9712), all C++ formatting and both changed-source static analyses.
All 30 existing session cases remain unchanged; they cover independent/replaced sessions, per-document
undo/redo, partial writes, stale and reordered callbacks, media adoption and journal-slot recovery.
The three updated guides have 43 valid relative file links. Merged #158 at `8f2161d` passed all six
checks and review in [CI 42741967508](https://github.com/mgradwohl/MoTrack/actions/runs/42741967508),
with 95.99% combined coverage (9346/9736) and all 237 unique edited actual-Tracker cases. The three
known Tracker defects remained explicitly unaccepted; its local and remote branch is cleaned up.
