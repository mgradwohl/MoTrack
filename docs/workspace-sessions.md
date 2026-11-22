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
Cross-tab import and saved layouts are not implemented by this shell; tabsets are described below.

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
  up to 256 files.
- **File checks off the GUI thread.** Before opening, `checkRequestedFile` runs on the services' single
  request worker, because an offline share or sleeping NAS can block any file query: existence, folders
  and file-system identity (`std::filesystem::equivalent`, so case variants and aliases match where the
  file system treats them as one file). A repeat of an earlier request in the batch is skipped, as in
  Tracker. A source is identified by the file it was opened from and by its native and standard save
  destinations, so a file saved with Save As or a paired save also names it. Such a file is selected, and
  a hidden view shown (after any modal prompt), instead of opening a second copy whose later save would
  replace the first ([#255](https://github.com/mgradwohl/MoTrack/issues/255)). A check blocked by an
  unresponsive share cannot be interrupted: closing the window stops the batch and ignores the check's
  late answer, but its worker waits for the file system.
- **Problems and notices.** Missing files, folders, web links, failed opens and unreadable archive member
  projects (an incomplete import) are problems. Skipped unsafe archive members and copies a standard save
  omits are notices: the file opened. Both are listed under the file's name in one window-modal summary
  after the batch, without a nested event loop, because the batch's next open replaces the status
  message. Canceling a requested
  open stops its remaining files. Batches wait while a save, revert or close prompt runs its nested
  event loop. Closing the window stops queued files before the close prompts run, and again before the
  window closes, so a request that arrived during the prompts opens only if the close is canceled.
  A hidden window shows no summary.
- **Paths.** Arguments are not split or canonicalized. Relative paths use the launch directory, local
  `file:` URLs become paths, and Windows `\\?\` and `\\?\UNC\` prefixes are removed because Qt adds its
  own prefix to long paths. Other URLs are reported, never fetched, and their user information is omitted.
  `--` ends option parsing, unknown options are reported on standard error and the legacy macOS `-psn_`
  argument is ignored. On macOS, Cocoa's `-NS…`/`-Apple…` user defaults are skipped with their value,
  so `-AppleLanguages (de)` still selects the interface language.
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
- **Smoke tests.** `--smoke-test FILE…` prints `opened<TAB>path` for each tab after the batch, problems as
  `MoTrack: …` and notices as `MoTrack notice: …` on standard error. It exits with 1 when any problem was
  reported (a file did not open, or an archive member project was unreadable) and with 0 otherwise,
  notices included. With no files it waits for an operating-system open request.

Staged builds declare the file types: the macOS bundle's `cmake/MoTrackInfo.plist.in` (shared with the camera usage string), and Linux
installs a desktop entry with `Exec=… %F` and a shared-mime-info package. The desktop entry quotes the
binary path chosen at install time following the Desktop Entry Specification. An install path containing
`%` is refused: the specification spells it `%%`, but GLib looks up the Exec program before expanding
`%%` and then refuses to load the entry. The bundle's `LSMinimumSystemVersion` is the deployment
target, or the SDK version Clang then targets. Installers that register
the types, including Windows ProgIDs, are deferred with R27.

Tests: `DesktopLaunch.*` covers parsing, routing, request order within the input bound, duplicates and
case variants, selecting an already-open or hidden file, checks held by a blocked worker, requests during
a canceled or completed close, a stale check after the window closed, selecting a project by its Save As
file, archive problems and notices kept for the summary and the launch exit code, missing/folder/failed/remote problems, the 256-file bound, cancel and
close, drops on the workspace and
an experiment view through Qt's propagation, refused drags, file-open events before and after a window,
child launches with spaces, Unicode, relative and long (Windows `\\?\`) paths, and a second process opening
a file that the first is editing. CI's staged check validates the plist and desktop files, asks
LaunchServices and GIO which application opens each type, and opens a file through `open -a` and GLib's
desktop-entry expansion, including an install prefix with quotes, `$` and backticks, and checks that a
prefix containing `%` is refused.

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

## Tracker tabsets

A tabset is a `.trk` whose root is Tracker's `TFrame` instead of a `TrackerPanel`. Its `tabs` property is a
`String[][]` of {absolute, relative} paths, one per Tracker tab; it owns no experiment data. `tracker_tabset`
reads and writes it through [`motrack_osp`](osp-xmlcontrol.md#tracker-import-and-export), like every Tracker import
and export since B1. `tracker::loadTabset` reads `tabs` and `basepath` with Tracker's accessors and MoTrack's import
policy refuses malformed entries on top of it. Files therefore read as Tracker reads them: entities and element-like
text stay literal, a document type is inert and a blank line ends the `tabs` array. A saved `basepath` string replaces
the tabset's folder as the first base, as `XMLControlElement.getString` returns it (Tracker never writes one).
Unreadable XML is reported with Tracker import's diagnostic, including its Windows-1252 hint. Only the root line
decides whether a `.trk` is a tabset, so experiments are still parsed once.

**Opening.** `prepareWorkspaceInput` detects the root off the GUI thread and returns an expansion instead of a
source; adoption refuses it. Each entry resolves in Tracker's order: relative to the tabset's folder, relative to
the process working directory (OSP's user directory), then the absolute path. A relative slot that is itself
absolute uses only the absolute path, as `java.io.File(parent, child)` would. Nested tabsets expand in place,
depth-first, to depth 8 and 256 entries in total. Each physical file is listed once: a tabset already being
expanded is a cycle, any other repeat is a duplicate. Only a `TFrame` root is parsed in full; listed experiments
are parsed once, when they open. The window then opens the resolved files one at a time through the ordinary
two-input bound, so tabs keep the listed order and each file becomes an independent source with its own history,
jobs and save state. A missing file offers Yes/No to locate it; a located file that is itself a tabset opens its
files in place. Duplicates are opened once without a warning; cycles, unreadable nested tabsets, files that fail
to open and bound overflows are summarized afterward; a file that fails shows the same diagnostic a single
project would. A listed `.motrack` whose stale links to its Tracker source were reset on open
([native recovery](native-format.md#original-object-associations)) opens with B1's note in the summary and on its
view. Canceling one open skips the remaining files. One tabset opens at a time: another that arrives, even during
the first one's prompts, waits its turn. Closing the window drops queued tabset work before its prompts. As in
Tracker, the first listed experiment file is selected afterward, not a leading video. `tabsetFinished` signals that
the selection is final.

The files open through the window's one ordered queue (`OrderedOpens`). When the queue's current file turns out to
be a tabset, `expandInPlace` puts its files next, ahead of the rest, without further prompts. Other ordered opens,
such as files the operating system asks to open ([#259](https://github.com/mgradwohl/MoTrack/pull/259)), should feed
this queue rather than run a second one that competes for the two input slots; a tabset among them then expands in
place.

**Saving.** **Save tabset as…** lists sources in tab order, followed by hidden-only sources. A source with a
standard Tracker file is referenced by that file. If it has unsaved changes, Yes saves them first (a failed or
canceled save writes no tabset, as in Tracker), No lists the file as it was last saved (Tracker lists every tab that
has a file) with a note, and Cancel stops. An unchanged raw single movie or image is referenced by its own path.
MoTrack-only sources, image sequences and changed raw media are reported as not included, because Tracker could not
reopen them from one path; so is a native file whose Tracker links were reset, because its Tracker export is refused.
The dialog offers the tabset opened last. As Tracker's `fixXML` does, a name not ending in `.trk` gets `.trk` in
place of its extension (`set.trz` and `Lab 3.5` become `set.trk` and `Lab 3.trk`); an existing file of that new name
is not replaced unless it was chosen. The destination may not be an open source's experiment file, companion or raw
media file, and an existing file that is not a tabset is never replaced. **Save tabset as…** is unavailable while any
file is opening, since it may turn out to be a tabset, and it checks again after each of its prompts and saves, so a
set is never saved with only some of its files. If a referenced source changes while prompts are open, nothing is
written. Reading the existing tabset, resolving its entries and writing run on the save worker while the window
keeps painting, and the file is staged and committed atomically.

A new tabset is written as `XMLControlElement.toString` writes it (LF line breaks, no final one). Saving over an
existing tabset edits it instead, with the same O8 check as every standard export: each entry that still names its
listed file in Tracker's search order keeps its bytes, together with the repeats Tracker writes for each further tab
of an archive. An entry is rewritten only when its relative path no longer finds the file from the base, for
example after the tabset moved alone. Entries whose files are not open keep their place: a file that failed to open,
was not found, was not opened because an open was canceled or the window closed, or names an archive member (#325)
stays listed, and the summary says so. Only entries of files the user closed are removed, unless a tabset opened
since then lists the file again; a repeat that does not follow its file's entry is removed too, since each file opens
once. The summary names every removed entry. A nested tabset whose files are open stays one entry. Saving an
unchanged set therefore leaves the file byte for byte, except entries whose relative path no longer finds their file,
which are repaired. Otherwise only the changed paths, removed entries and new entries differ; new entries follow the
kept ones, and other properties and line breaks stay. Entries that Tracker cannot read because of their index order are rewritten in order whenever
anything changes. A tabset MoTrack cannot read is not replaced.

**Deliberate differences from Tracker 6.3.5.**

- OSP's relative-path search uses `String.startsWith`, so it treats `/data/a` as an ancestor of `/data/ab`;
  MoTrack requires a path-component boundary. Both keep OSP's five-level climb, the Windows drive root and the
  absolute fallback, so Tracker resolves MoTrack's output. OSP has no UNC case and can climb above a share;
  MoTrack treats `//server/share` like a drive root.
- Tracker writes a `.trz` once per experiment tab, and reopening such a tabset duplicates those tabs; MoTrack
  opens each file once. Identity folds case only on Windows, so distinct files on case-sensitive volumes stay distinct.
- Tracker silently lists a saved file even when its tab has newer edits; MoTrack asks first, and also lists it as
  last saved when told not to save. Tracker offers to save changed or new media as a project; MoTrack reports them as
  not included until saved as Tracker projects.
- Tracker never opens a file located for a missing entry, and fails to load a tabset whose only experiment was
  located; MoTrack opens it where the entry is listed
  ([#333](https://github.com/mgradwohl/MoTrack/issues/333), reproduced in the Tracker 6.3.5 GUI).
- Tracker saves only open tabs; saving over an existing tabset, MoTrack keeps the entries of files that did not open
  and drops only those of files the user closed.
- Tracker does not check whether the tabset would overwrite an experiment; MoTrack refuses.
- Tracker offers to load a listed numbered image as its whole sequence; MoTrack opens that single image. An
  animated image opens as a movie so its frames are kept.
- Tracker requires ascending `[n]` indices; MoTrack also reads other orders. It keeps such a file as it was read when
  nothing changes and writes changed entries in ascending order.
- Tracker opens an archive's experiments after every file listed after the archive (its loader queues them last),
  so a resave loses the listed order ([#326](https://github.com/mgradwohl/MoTrack/issues/326)); MoTrack opens them
  where the archive is listed.
- Entries that name an experiment inside an archive (`archive.trz!/member.trk`), which Tracker's resource loader
  opens and a published tabset uses, are reported as not found
  ([#325](https://github.com/mgradwohl/MoTrack/issues/325)).

**Evidence.** Unit tests cover the exact OSP layout, reading as Tracker's line reader does, parser refusals, in-place
edits with exact expected bytes (CRLF, CDATA, removed and inserted paths, appended entries, out-of-order indices),
entry reuse, `basepath`, relative paths on each platform, resolution order, nesting/duplicate/cycle/depth/size bounds,
cancellation, atomic writes and Unicode paths. Desktop tests cover ordered opening of a movie, an experiment and an
archive with selection and independent histories; locating and skipping missing files, including a located tabset; two
tabsets arriving together; a listed file that fails, with B1's diagnostic; a native file with reset Tracker links;
canceling partway; destroying the window during a prompt; Yes/No/Cancel for unsaved changes; saving an unchanged set
back byte for byte and adding a tab; keeping entries of files that failed to open or were not found and dropping one
the user closed; listing a file as last saved; saving refused while any file opens, including one started during a
save prompt; Tracker's `.trk` name; refusal to replace open project files, an unopened experiment or an unreadable
tabset; unsupported sources; and saving then reopening after the whole folder moves. In Linux CI the pinned Tracker
opens MoTrack's tabset through its normal loader after the folder is moved (so only relative paths resolve), must load
all three experiments in order with their independently expected masses and samples, and writes its own tabset.
MoTrack must resolve that to the same two files after everything moves again and save the same files over it without
changing a byte. MoTrack then edits its own tabset in place (the archive first, another experiment added), and Tracker
must reopen every tab of the edited set, in Tracker's own order, with its data.

## Remaining integration

The registry/routing foundation merged in [#160](https://github.com/mgradwohl/MoTrack/pull/160);
services, editors, input preparation, the shell and Revert followed in #161–#165. The remaining phase-4
order is in the [full-port plan](migration-plan.md#phase-4-remaining-work): persisted views and
cross-tab import. Tracker tabsets are above.
Source records and visible views must not be conflated at any close step.
