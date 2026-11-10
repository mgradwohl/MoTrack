# Workspace ownership and routing

`WorkspaceSession` is the owner-thread registry between prepared project inputs and future desktop
tabs. It uses Qt Core and the project adapters; it does not depend on Widgets, FFmpeg, Java or Swift.
It owns independent `DocumentSession` records and `ArchiveGroupSession` owners. Retained experiment
editors are in local validation. The ordinary application entry point still uses its existing
single-document window; the tabbed workspace shell is the next integration step.

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

These registry tokens define ownership and result routing. The next `WorkspaceServices` layer is in
local validation and connects the existing desktop to bounded execution services. A window creates
an owner by default; multiple views in one workspace must receive the same shared owner. The registry
and desktop document bindings are still separate until the next integration slice. Keep local builds
at two workers and local tests serial while developing these layers.

## Shared execution services

| Resource | Bound per shared service owner | Ownership and scheduling |
| --- | --- | --- |
| Frame execution | One persistent worker thread | One active decode, one replaceable pending frame per document, fair document order. |
| Frame decoders | Two least-recently-used readers | Path, orientation, media generation and archive owner must match before reuse. Eviction closes the reader before releasing its archive. |
| Frame cache | 32 MiB of cached image pixels across those readers | Keys include document, media generation, original frame ordinal and optional presentation timestamp. Smaller budgets are supported; oversized frames are displayed without caching. |
| Import, indexing and template matching | Two workers in one analysis pool | Existing per-document cancellation and stale-result checks remain. Each running indexer can additionally own its own decoder. |
| Saving, packaging and recovery | One save worker | Separate from frame/analysis work so saving does not stop tracking. Registry binding must additionally enforce its two-pending-operation snapshot limit. |

The cache limit is not a total application-memory claim: displayed images, a returned frame, model
snapshots, source ownership and FFmpeg's internal buffers have separate lifetimes. Thread and frame
cache limits belong to the shared owner, not to the number of tabs. Worker pools are borrowed by
desktop callers; production code must not change their limits. Existing scheduling regressions now
pause/wait on these actual pools instead of the process-global pool, preserving their assertions and
timeouts.

Frame requests carry owned cancellation/source references plus document and media-generation IDs.
Submitting a newer frame cancels only the same document's active/pending frame; other documents keep
their place. Closing a view cancels its queued frames and coalesces asynchronous decoder cleanup.
Requests and decoders retain archive ownership until their handles close, so releasing extracted
media no longer needs a blocking GUI-thread decoder call. Path/orientation/source changes invalidate
that document's cache before lookup. The existing timestamp seek and presentation-order fallback
remain the decoding algorithm.

Worker results return through the service's GUI-thread watcher. A result callback may destroy its
last owner; the scheduler checks the QObject lifetime before dispatching more work. The last service
owner cancels pending frames, closes decoder handles on their worker thread and drains its pools.
Each window's parent-owned QObject lease keeps shared services alive through Qt's destruction
notification before that final drain. The new regression reproduced the incorrect ordering before
this correction and passes afterward. Closing one view of a shared workspace
must not join or cancel another view's analysis work.

All nine new local service tests pass, covering cache/decoder bounds, physical path and
orientation changes, timestamp
seeking, refused/cancelled frames, source lifetime, queue coalescing/fairness, isolated cleanup,
owner deletion from a result callback, destruction notification before worker drain and two actual
windows sharing services while one continues AutoTracker after the other closes. Formatting and all
seven affected translation-unit static analyses pass without new suppressions. All 10 Windows CTest
groups pass serially in 182.49 seconds with **96.30%** production-line coverage (9955/10338).
The first CI revision passed all three platform builds and all 237 edited Tracker cases, but Clang 18
flagged Qt weak-pointer cleanup and callback-owned test cleanup. The follow-up uses an explicit
destruction observer and parent-owned test cleanup, preserving all nine cases and adding no
suppression. PR #161 merged at reviewed head `537e617` with all six checks passing in
[CI 42758565994](https://github.com/mgradwohl/MoTrack/actions/runs/42758565994), **96.35% combined
coverage (9984/10362)** and all 237 unchanged edited Tracker cases. Three known reference defects
remain unaccepted. Review 5836935274 recommends approval with no threads; the completed branch was
verified contained in main and removed locally/remotely.
This is not yet a tabbed desktop or complete acceptance of #69.

## Retained experiment editors

`WorkspaceContext` connects the registry, shared execution services and experiment editors on the
GUI thread. The bound `MainWindow` constructor takes this owner and a runtime document ID. It borrows
that record's project and history rather than moving or copying them into its old standalone session.
Only one experiment editor may claim a record at a time; later plot/table views will share its editor
model. Duplicate or unknown claims are refused. The existing standalone constructor remains the
application's compatibility path until the workspace shell adopts prepared inputs and creates bound
editors. Opening/replacing sources is disabled within a bound editor because it belongs to that shell.

Each existing editor keeps its own playhead, displayed image, selection, plot, playback controls,
tracking timers and media index. Selecting another record does not reset these controls. Destroying
an editor cancels only its document's jobs and hides its record; the retained model, undo history and
source membership survive. Recreating an editor restores its model/history and starts at the first
clip step. Persistent view/playhead restoration remains part of the subsequent layout work.

Frame and movie-index requests use registry tokens. AutoTracker and playback keep separate tokens
for each complete run, including time spent waiting for a movie frame or a patch match. Explicit
registry cancellation posts a context notification independently of worker results, so suppressed
frame delivery cannot leave a checked Stop/Pause control without work. The owner-thread observer
must defer notifications; it cannot synchronously mutate registry state or views during cancellation.
Each editor stops only its own currently canceled run. A queued notice from an older run cannot stop
a newly restarted run. Pressing Play after a canceled frame re-requests that frame before scheduling
playback. Normal completion does not emit cancellation notifications. Index completion applies only
to the current document/index identity, retaining edit history and source bindings. The registry
marks an identified movie before indexing so an unindexed movie cannot be offered as an image
package. Finite, zero-origin, strictly increasing index data must match the reported frame count and
native reopen invariants; failed or obsolete results do not replace live metadata. Source limits only expand when the complete saved clip fits the index.

Native and paired saves from any member capture the entire retained source through `beginSave`.
Recovery completes through the source owner before taking a fresh current-edit snapshot. Native-only
saving preserves established Tracker obligations. Save and recovery callbacks retain the workspace
through their nested GUI event loops and always deliver their results even after the initiating view
is destroyed. This releases operation slots while retaining any partial-save journal. Portable
exports also reserve a slot before copying a model, but release it without adopting a save path or
advancing checkpoints. Busy sources/workspaces and cancellation keep their existing data.

Group dirty indications and refreshed physical media bindings reach sibling views through queued
source notifications, without changing their selections, playheads or tracking jobs. Queued delivery
lets a workspace observer create or destroy views after the initiating editor operation returns.
The existing QObject service lease continues to keep pools alive through window destruction
notifications, including when a window owns the final workspace reference.

Twelve new local regressions cover independent playheads/history/destinations; hidden-member edits,
native/Tracker group saves and restored undo history; duplicate/invalid view claims; indexed movie
identity and refused data; pending-operation bounds and export copies; destroyed native/paired-save
views; stale writes; interrupted/cancelled group recovery; and AutoTracker continuing through saving,
selection and another view's destruction with distinct timing/units; and restarting after a pending
match is canceled through either document or source cancellation; resetting tracking/playback while
a movie frame is pending, without receiving a result; and restarting before an older cancellation
notice is delivered. Both timestamp admission and
tracking-control regressions reproduced the review findings before their fixes. Rejected index
values leave existing metadata unchanged; valid indexed groups save and reopen natively. All 10
Windows CTest groups pass serially in 179.39 seconds with **96.27%** production-line coverage (10338/10739). Formatting and all
five affected translation-unit static analyses pass without new suppressions. Existing plot/measurement
labels and save-progress strings remain unchanged. All three translation catalogs contain
the new pending-save message, and 35 relative links across the three changed guides resolve. The
three previously investigated LLVM wrapper warnings remain visible, with no missing production
records and the unchanged 90% gate. Cross-platform CI, actual Tracker acceptance and review remain
required. These editors do not add visible tabs or a source-discard policy: a bound view's close
operation hides it and retains edits. The future shell must make save/discard decisions
explicit before releasing sources. #69 and #150 remain open.

## Preparing complete inputs

`prepareWorkspaceInput` reads a local experiment into an idle, owned `DocumentSession` or
`ArchiveGroupSession` before changing the live registry. Standalone `.trk` inputs retain their source
XML and physical media locations. Standard `.trz` inputs prepare **every** experiment against one
immutable archive owner; one malformed member rejects the whole input. The existing archive limits
and cancellation checks apply. Preparation never needs a widget or a Java runtime.

Native dispatch reads the container once and selects standalone versions 1–5 or the version-6 group
envelope. A native group keeps precisely its saved membership, order, IDs and edits; preserved but
unopened payload members do not become invented live records. Older external-archive companions
retain a snapshot of the referenced archive and clear that external reference. Standalone archive
companions remain standalone, including old files without a known member association. Promotion is
a separate explicit operation. This does not implement Tracker `TFrame` tabsets (#150).

`adoptWorkspaceInput` transfers one completely prepared source on the registry's owner thread.
Refusal retains incoming ownership and does not alter existing sources. Independently opening the
same path creates independent live records. `KeepCurrent` preserves an existing selection; an empty
workspace selects the first new experiment.

`WorkspaceContext::openProject` runs preparation on the shared analysis pool. At most two inputs may
be pending; a busy request returns before starting another worker. Each request has its own ID and
cancellation flag. Cancellation keeps its slot until completion and never cancels another source's
tracking, playback or open request. Completion removes the pending slot before notifying observers,
which may immediately open another source or destroy the context. Workers own their results and
flags, never a borrow of the registry. Context destruction cancels outstanding inputs before shared
services drain; completed results can no longer adopt into the destroyed registry.

Eleven public-entry-point tests cover standalone/native opening and physical media, Unicode paths,
complete image/MOV archives after original-file removal, native group order/subsets, older embedded
and external archive companions, malformed-member refusal, atomic adoption, independent cancellation,
two-input backpressure, failed-open retry, destruction while queued and from completion observers,
and editing/saving both members through bound editors. The four existing image/MOV archive-group
corpus modes now reopen through this dispatcher before whole-source save/recovery and the unchanged
independent XML/ZIP/media/reimport checks. All four local modes pass, including byte-identical exports
in either order. All 10 Windows CTest groups pass serially in 184.78 seconds with **96.24%**
production-line coverage (10491/10901). All C++ formatting, five affected translation-unit static
analyses and 36 relative guide links pass. The three previously investigated LLVM wrapper warnings
remain visible; no production records are missing and the 90% gate is unchanged. Actual Tracker
acceptance, cross-platform checks and external review remain the current-head CI/review gates.

The default desktop still opens a standalone editor. The next shell integration must connect these
prepared inputs to visible tabs, user-facing cancellation/error controls and explicit close decisions.

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
no production coverage records are missing and the 90% gate is unchanged. PR #160 merged at reviewed
head `a8a7148` after all six checks in
[CI 42753586627](https://github.com/mgradwohl/MoTrack/actions/runs/42753586627), with **96.18% combined
coverage (9849/10240)**. All 237 edited Tracker cases, including the four extended group modes,
passed; three known Tracker defects remain explicitly unaccepted. The merged branch was removed
locally and remotely after containment checks. This foundation does not close
[MoTrack #69](https://github.com/mgradwohl/MoTrack/issues/69), whose upstream
[Tracker #18](https://github.com/OpenSourcePhysics/tracker/issues/18) discussion was refreshed on
2026-11-09. The historical report concerns actual playback/tracking controls. Multi-tab desktop
acceptance, registry-bound views using shared worker/cache limits, per-view media/playhead state, explicit close/discard UI,
cross-tab import and [Tracker tabsets #150](https://github.com/mgradwohl/MoTrack/issues/150) remain in
the [full-port order](migration-plan.md#implementation-order-and-exit-gates).

The remaining integration order is:

1. Shared execution/cache services are merged in #161. Frame requests carry
   document and media-generation identity; serving another document must not invalidate a running
   experiment's AutoTracker request. Workers retain immutable source ownership until finished.
2. Finish validation of the retained experiment editors above, then activate their actions from the
   workspace shell without replacing records. Retain each editor while switching tabs.
3. Connect the bounded asynchronous input preparation above to visible tab interactions. Exercise actual
   view switching/closure while tracking, distinct timing/units, undo, source removal and save/reopen.
   Save All deduplicates source IDs and respects the pending-operation bound.
4. Implement explicit close/discard decisions and a saved-model restoration policy, then tabsets and
   cross-tab import. Retained source records and visible views must not be conflated at any close step.
