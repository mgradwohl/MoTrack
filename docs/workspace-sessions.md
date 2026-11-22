# Workspace ownership and routing

`WorkspaceSession` is the owner-thread registry between prepared project inputs and desktop
tabs. It uses Qt Core and the project adapters; it does not depend on Widgets, FFmpeg, Java or Swift.
It owns independent `DocumentSession` records and `ArchiveGroupSession` owners. The ordinary
application entry point is the tabbed `WorkspaceWindow` described below.

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
Discarding one member's edits while retaining the group uses the actual saved model described below;
a history checkpoint or `replaceMember` alone cannot implement that policy.

## Background results and resource limits

Frame, media-index and AutoTracker jobs have separate per-document cancellation identities. Starting
a replacement of one kind cancels only that document's previous job of that kind. Acceptance requires
the current document/kind/flag identity and a noncancelled flag. Finishing an old request cannot cancel
its successor. Explicit document cancellation, source release and workspace destruction invalidate the
appropriate tokens; flags survive with workers. Selecting, moving or hiding another tab does not stop
AutoTracker. Callers must cancel jobs made incompatible by an edit or media change, and validate any
scientific revision requirements before applying results.

These registry tokens define ownership and result routing. `WorkspaceServices` supplies bounded
execution, and `WorkspaceContext` binds it to retained experiment editors. A window creates an owner
by default; every view in one workspace receives that shared owner. Keep the CTest groups serial
locally; their 60-second limits assume it.

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
notification before that final drain. Closing one view of a shared workspace must not join or cancel
another view's analysis work.

Tests: `WorkspaceServices.*` cover cache/decoder bounds, path/orientation changes, timestamp seeking,
cancellation, source lifetime, queue fairness, isolated cleanup, owner deletion from a result callback
and two windows sharing services while one continues AutoTracker. Merged in
[#161](https://github.com/mgradwohl/MoTrack/pull/161).

## Retained experiment editors

`WorkspaceContext` connects the registry, shared execution services and experiment editors on the
GUI thread. The bound `MainWindow` constructor takes this owner and a runtime document ID. It borrows
that record's project and history rather than moving or copying them into its old standalone session.
Only one experiment editor may claim a record at a time; later plot/table views will share its editor
model. Duplicate or unknown claims are refused. The existing standalone constructor remains available for focused editor tests. The application
workspace adopts prepared inputs and creates bound editors on first selection. Opening/replacing sources is disabled within a bound editor because it belongs to that shell.

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

Tests: the bound-editor cases cover independent playheads/history/destinations, hidden-member group
saves, duplicate/invalid view claims, indexed movie identity, pending-operation bounds, destroyed
save views, stale writes, interrupted recovery, and AutoTracker continuing through saving, selection
and another view's destruction, including restart after cancellation. Merged in
[#162](https://github.com/mgradwohl/MoTrack/pull/162).

## Preparing complete inputs

`prepareWorkspaceInput` reads a local experiment into an idle, owned `DocumentSession` or
`ArchiveGroupSession` before changing the live registry. Standalone `.trk` inputs retain their source
XML and physical media locations. Standard `.trz` inputs prepare **every** experiment against one
immutable archive owner; one malformed member rejects the whole input. The existing archive limits
and cancellation checks apply. Records are accumulated before one complete membership validation;
incremental append would repeatedly revalidate every preceding member. Preparation never needs a
widget or a Java runtime.

The separate `motrack_input` adapter probes/indexes available movies before adoption, including archive
members with no editor. This keeps Qt Gui and FFmpeg outside the document model. It uses the existing
bounded, cancellable movie indexer, preserves clip timing/range and fills missing derived frame
metadata. Existing native indexes remain intact; older unindexed native members are initialized too.
An invalid available movie rejects preparation before adoption, while missing media remains missing.
Without this step an unopened movie would still look like an image document to whole-source saving.

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

Tests: the input cases cover standalone/native opening, Unicode paths, complete image/MOV archives
after original-file removal, native group order, older companions, malformed-member refusal, atomic
adoption, cancellation, backpressure, destruction while queued, and saving a mixed archive without
ever creating a movie editor. The four archive-group corpus modes import through this dispatcher.
Merged in [#163](https://github.com/mgradwohl/MoTrack/pull/163).

## Desktop workspace shell

`WorkspaceWindow` is the default application window and owns one shared context. Local projects, movies
and image sequences enter through the same two-input worker bound, with individual progress/cancel
controls and failure diagnostics. Raw media has `DocumentSourceKind::Media`: its original movie/image
is never a native-save target. Image rates are finite and nonnegative; zero means unknown timing.
Movies use the existing bounded display-orientation indexer; imported Tracker coordinates are unchanged.
The prepared absolute presentation timestamps remain in immutable, runtime-only document state,
shared by recreated views. Opening a prepared movie tab therefore does not scan it a second time.
The adapter checks physical path, file size, modification time and orientation before reuse; native
files retain their existing relative timing format and acquire a runtime index on first view creation.

Every imported archive member gets a lightweight tab. Its complete editor is created on first
selection and retained across tab switches, preserving playback, tracking, selection, playhead and
undo state without constructing thousands of editors during a large archive open. The active editor
supplies the workspace menus and keyboard actions. Reordering updates registry order; selecting or
reordering another experiment never cancels the running document's jobs.

The tab × and Ctrl+W deliberately mean **Hide experiment view**, not discard: destroying its editor
cancels that document's work while retaining its data and history. **Experiments** reopens a hidden
record. Its recreated editor starts at the first clip step, as documented above. **Close project…**
resolves Save/Discard/Cancel for the entire source, including every hidden archive member. The separate
Revert command below restores one member. Persisted view layouts remain unfinished.

Ordinary Save on a standard Tracker source writes the standard output and companion together. Save All
and save-before-close use the same companion-replacement confirmation as Save As when establishing a
pair over an existing independent `.motrack`; Cancel preserves both files. Established pairs save
automatically. Save All
iterates distinct source identities, writes only modified sources, includes hidden member edits and
stops at the first failure/cancel. Before an aggregate/close save, known destinations are checked
against other modified sources, including the standard file's companion and physical file aliases.
Two independently edited copies sharing an output are refused until saved to distinct destinations.
This prevents Save All from overwriting one copy and marking both saved. Hidden sources can use a temporary parent-owned editor for the
existing save/recovery prompts without reopening a visible tab. Native-only copies keep the existing
paired-save obligations; raw media or native-only inputs choose a native destination as before.

Closing the window cancels pending inputs and stops tracking/playback, refuses active saves and collects
all source decisions before releasing any records. A later Cancel preserves earlier sources, although
already-approved saves may have written files. A complete revision preflight prevents a later prompt
from discarding newer edits in an earlier source. Destruction during open/save/close modal loops
retains worker ownership and clears only the appropriate view/job state. The shell destroys its
editors and disconnects callbacks before its C++ member maps and registry leases are destroyed.
Pending frame/index requests survive close decisions, so an aborted close leaves frame controls
usable; actual editor destruction/source release cancels the remaining work.

Tests: `WorkspaceShell.*` covers tab selection/reordering and focused Undo/Redo, lazy members, hidden
edits with Save All, whole-source and whole-window Save/Discard/Cancel, failed saves, raw media,
input cancellation, destruction during modal operations, shared-destination refusal, companion
replacement prompts, and AutoTracker continuing through another tab's switch, reorder and close.
Merged in [#164](https://github.com/mgradwohl/MoTrack/pull/164), which closed native tab-isolation
issue [#69](https://github.com/mgradwohl/MoTrack/issues/69); upstream Tracker #18 remains open.
Tracker TFrame tabsets ([#150](https://github.com/mgradwohl/MoTrack/issues/150)), cross-tab import
and saved layouts are not implemented by this shell.

## Saved models and per-experiment revert

Each document retains immutable native and Tracker `LegacyProject` baselines, sharing one initial
snapshot when opened. Completed workspace saves supply their actual captured models; completion
observation order chooses the baseline independently for each output, even if newer live edits make
the callback stale. Superseded/replaced callbacks cannot replace a newer baseline. A stale legacy
caller without a captured model leaves that baseline unknown instead of inventing one from live edits.
Group completion matches members by stable ID and runtime session identity with linear lookup maps,
so appended/replaced records never inherit a previous member's saved model.

`restoreSavedDocument` requires the editor destroyed/hidden and no pending source operation. Paired
sources require known, identical native/Tracker baselines and no recovery journal. Recovery publishes
files without a known live snapshot, so a physically newer save must establish the baseline afterward.
A separate monotonic recovery-observation barrier rejects earlier write snapshots even if their callbacks
arrive later. Both observed recovery failures and lock-confirmed journal absence advance it without
retargeting save destinations. A write dispatched before recovery but physically published afterward
can supply a known model while its history checkpoint remains stale. Restoration
copies before mutation, clears only the selected history, preserves the track-ID allocation high-water
mark and the full extent already discovered for unchanged movie media, while restoring the saved clip
selection. It retains the document/source/member identities, media bindings and archive ownership. It
cancels only that document's jobs; other member histories, jobs and unsaved group membership survive.

**Revert experiment…** confirms the discard, stops the selected experiment's tracking/playback, then
rechecks its runtime identity, revision and pending-save state after the modal loop. Cancel, newer
edits, a save started during confirmation or owner destruction preserves the live model. Successful
revert recreates a visible editor at its original tab position and first clip step; restoring a hidden
record leaves it hidden. The command clears undo/redo history as stated in its confirmation. It restores
the last state observed by this session rather than reloading files changed by another application.

Tests: restore cases cover branch-discarded save points and non-reused IDs; stale, failed, replaced
and snapshot-less completions; standalone/group paired writes and native reopen; split-pair and
interrupted-save refusal; appended/replaced members; desktop Cancel/Discard, hidden records and
modal destruction; preserved movie extent; and an older save callback arriving after recovery.
Merged in [#165](https://github.com/mgradwohl/MoTrack/pull/165).

## Opening from the desktop

Command-line arguments, drops and macOS `QFileOpenEvent`s all call `WorkspaceWindow::openFiles`, which
uses the same context entry points as the menus. `launchFileKind` routes by suffix on the owner thread:
`.trk`/`.trz`/`.motrack` in any case are projects, suffixes Qt can read are one-image sequences with
unknown timing, and every other file is a movie whose indexer reports unsupported formats.

- **Order and the input bound.** One requested file prepares at a time, so tabs appear in request order
  while a menu open can still use the other slot. When menu opens hold both slots, the batch waits
  for a completion instead of reporting a busy workspace. Requests arriving during a batch join it,
  up to 256 files; a canonical identity already started in the batch is skipped, as in Tracker.
- **Problems.** Missing files, folders, web links and failed opens are collected and shown together
  in one window-modal summary after the batch, without a nested event loop. Canceling a requested
  open stops its remaining files. Closing the window stops queued files before the close prompts run,
  and a hidden window shows no summary.
- **Paths.** Arguments are not split or canonicalized. Relative paths use the launch directory, local
  `file:` URLs become paths, and Windows `\\?\` and `\\?\UNC\` prefixes are removed because Qt adds its
  own prefix to long paths. Other URLs are reported, never fetched, and their user information is omitted.
  `--` ends option parsing, unknown options are reported on standard error and the legacy macOS `-psn_`
  argument is ignored.
- **Drops.** A drag needs at least one local file and a copy or link action. The window always reports a
  copy (or link), never a move, because a file manager deletes the source after a move. A dashed
  overlay in the current palette shows how many files will open and links will be skipped.
  Experiment views do not take file drags: their `QMainWindow` base accepts only dock-widget drags,
  so Qt offers the drag to the workspace window.
- **macOS open requests.** `FileOpenRequests` is installed as an application event filter right after
  `QApplication` is created. It keeps requests in order until a window is attached, including those
  delivered while launching, and again after that window is destroyed.
- **Instances.** There is no single-instance mode, as in Tracker. Each process has independent records
  and unique archive extraction directories. Saves keep the destination lock and atomic replacement,
  so two processes never interleave a file, but a later save replaces an earlier one without warning
  ([#255](https://github.com/mgradwohl/MoTrack/issues/255)).
- **Smoke tests.** `--smoke-test FILE…` prints `opened<TAB>path` for each tab after the batch, and exits
  with 0 only if every file opened. With no files it waits for an operating-system open request.

Staged builds declare the file types: the macOS bundle uses `desktop/macos/Info.plist.in`, and Linux
installs a desktop entry with `Exec=… %F` and a shared-mime-info package. The desktop entry quotes the
binary path chosen at install time following the Desktop Entry Specification. Installers that register
the types, including Windows ProgIDs, are deferred with R27.

Tests: `DesktopLaunch.*` covers parsing, routing, request order within the input bound, duplicates,
missing/folder/failed/remote problems, the 256-file bound, cancel and close, drops on the workspace and
an experiment view through Qt's propagation, refused drags, file-open events before and after a window,
child launches with spaces, Unicode, relative and long (Windows `\\?\`) paths, and a second process opening
a file that the first is editing. CI's staged check validates the plist and desktop files, asks
LaunchServices and GIO which application opens each type, and opens a file through `open -a` and GLib's
desktop-entry expansion, including a prefix that needs every Exec escape.

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

## Remaining integration

The registry/routing foundation merged in [#160](https://github.com/mgradwohl/MoTrack/pull/160);
services, editors, input preparation, the shell and Revert followed in #161–#165. The remaining phase-4
order is in the [full-port plan](migration-plan.md#phase-4-remaining-work): persisted views,
then Tracker tabsets ([#150](https://github.com/mgradwohl/MoTrack/issues/150)) and cross-tab import.
Source records and visible views must not be conflated at any close step.
