# Workspace ownership and routing

`WorkspaceSession` is the owner-thread registry between prepared project inputs and future desktop
tabs. It uses Qt Core and the project adapters; it does not depend on Widgets, FFmpeg, Java or Swift.
It owns independent `DocumentSession` records and `ArchiveGroupSession` owners. It does not yet
replace the desktop window's single-document state or implement visible tabs.

## Documents, sources and views

Each source and document receives a fresh runtime UUID. Those IDs differ from a native archive's
persistent group/member IDs. Opening the same path twice creates independent owners; matching names,
digests or persisted IDs do not merge histories. One source owns either one standalone document or
one archive group. A group retains every adopted member, including members with no visible view.

Inputs must already be loaded and validated. Adoption transfers `unique_ptr` ownership after
validation and allocation; failure leaves the caller's owner intact. Existing project addresses,
histories, save identities and recovery records survive. Callers must finish old external jobs and
saves before transfer. Appending another member requires the group's exact archive owner, and follows
the existing group transfer rules. A newly loaded `.trz` can explicitly establish a clean group
baseline; it is not treated as an edit simply because no native companion exists yet. Promoting
edited live records remains distinct and preserves their dirty membership/history state.

Views borrow data/history through a runtime document ID. Tab order and active selection contain IDs,
not document copies. Showing, moving or hiding a view preserves its record and undo history. Hiding
the active view selects its adjacent remaining tab; asynchronous adoption can keep the current
selection. Hiding an archive member never excludes its edits from a later group save.

`hideDocument` is a visibility operation, not the final user-facing close/discard policy. Releasing a
source requires all its views to be hidden and destroyed, no pending save/recovery operation, and
either a clean source or explicit confirmation to discard the whole source. It cancels only that
source's jobs and removes its owned records. It never deletes source files or recovery journals.
Discarding one member's edits while retaining the group still needs an actual saved model snapshot;
a history checkpoint or `replaceMember` alone cannot implement that policy.

## Background results and resource limits

Frame, media-index and AutoTracker jobs have separate per-document cancellation identities. Starting
a replacement of one kind cancels only that document's previous job of that kind. Acceptance requires
the current document/kind/flag identity and a noncancelled flag. Finishing an old request cannot cancel
its successor. Explicit document cancellation, source release and workspace destruction invalidate the
appropriate tokens; flags survive with workers. Selecting, moving or hiding another tab does not stop
AutoTracker. Callers must cancel jobs made incompatible by an edit or media change, and validate any
scientific revision requirements before applying results.

These are ownership and result-routing rules, not a worker scheduler. Desktop integration still needs
shared bounded pools and an aggregate decoder/cache budget. Creating an existing `MainWindow` for
every tab would multiply decoder threads, cache memory and save pools, so that is not the planned tab
implementation. Keep local builds at two workers and local tests serial while developing these layers.

## Saving and recovery

`beginSave(document)` captures an owned snapshot and checkpoint of its source. For a group member,
that always includes the complete retained group, including hidden members. The worker adapters
`writeWorkspaceNative` and `writeWorkspacePair` dispatch to the existing locked document/group writers.
Standalone native snapshots refresh and rebase their media from the retained experiment path; group
snapshots retain their original archive-relative bindings.

At most two save/recovery operations may be pending across the registry. A busy request returns before
copying models. This bounds queued snapshots; it does not establish a fixed memory ceiling for a large
individual experiment. Capture requests only when ready to dispatch. A caller that fails before
dispatch, catches a worker exception or cancels work must still deliver a failed/cancelled completion.
Cancellation alone does not free an operation slot or permit source release: a write may have committed
before observing cancellation. Every returned result must reach the owner thread.

Completions route by runtime source, operation and cancellation identity, never by active tab, path or
persistent member ID. Duplicate callbacks and callbacks from a destroyed workspace are rejected.
The existing per-output `SaveState` and group membership/member checkpoints decide whether a result
is saved, stale, partial or superseded. Native-only saving preserves the established standalone rules;
once paired mode is established, a native-only write cannot clear outstanding Tracker changes.

`beginRecovery` captures the currently observed journal path and transaction without copying all
models. Recovery uses the same two-operation bound and explicit transaction check as ordinary saves.
Completing recovery clears only the appropriate record and leaves current live edits dirty until a
fresh save succeeds. Cancelling recovery retains the unresolved journal and reports a partial save;
it does not downgrade that obligation to an ordinary cancelled write. A request owns everything its
worker needs even if the workspace is destroyed;
destruction cancels its flag instead of leaving borrowed model pointers in the worker.

## Evidence and remaining integration

Twelve public-API regressions exercise independent histories/selection, duplicate paths,
stable owners, refused transfers, per-document jobs, hidden members in both edit/save orders, group
append during an older save, native media relocation, two-operation backpressure, cancelled writes,
physical write/callback ordering, cancellation after commit, cancelled/partial recovery, native reopen
and owner destruction. The four
existing image/MOV group corpus modes now also use workspace routing, hide a member, retain another
member's tracking token, save from the hidden member and recover before final actual-Tracker checks.
Earlier independent-companion, native relocation/source-removal, XML/ZIP/media and byte-equality
oracles remain; fixture data and the case inventory are unchanged.

All 10 Windows CTest groups pass serially in 177.35 seconds with **96.11%** production-line coverage
(9819/10216), including all 301 project tests. All C++ formatting, static analysis for the four changed
translation units, four extended native corpus modes, both reimports, byte equality and 68 relative
file links pass. Three previously investigated LLVM function-mismatch warnings remain visible;
no production coverage records are missing and the 90% gate is unchanged. Current-head CI/review and
actual Tracker acceptance of this routing change remain pending. This foundation does not close
[MoTrack #69](https://github.com/mgradwohl/MoTrack/issues/69), whose upstream
[Tracker #18](https://github.com/OpenSourcePhysics/tracker/issues/18) discussion was refreshed on
2026-11-09. The historical report concerns actual playback/tracking controls. Multi-tab desktop
acceptance, shared worker/cache limits, per-view media/playhead state, explicit close/discard UI,
cross-tab import and [Tracker tabsets #150](https://github.com/mgradwohl/MoTrack/issues/150) remain in
the [full-port order](migration-plan.md#implementation-order-and-exit-gates).

The remaining integration order is:

1. Add workspace-owned bounded execution services and a shared decoder/cache budget. Frame requests
   carry document and media-generation identity; serving another document must not invalidate a
   running experiment's AutoTracker request. Workers retain immutable source ownership until finished.
2. Bind reusable experiment views to registry records and those services. Keep each view's playhead,
   image, selections, playback/tracking controls and derived media index with its document; route
   saves through the source owner. Activate actions for the selected view without replacing records.
3. Add tab interactions and asynchronous opening of complete prepared archive groups. Exercise actual
   view switching/closure while tracking, distinct timing/units, undo, source removal and save/reopen.
   Save All deduplicates source IDs and respects the pending-operation bound.
4. Implement explicit close/discard decisions and a saved-model restoration policy, then tabsets and
   cross-tab import. Retained source records and visible views must not be conflated at any close step.
