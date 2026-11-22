# MoTrack agent guidance

This is the shared guidance for Codex, Claude Code, Copilot, and other agents working on MoTrack.
`CLAUDE.md` imports it and `.github/copilot-instructions.md` points here. Keep shared rules here rather
than maintaining competing copies. The user's instructions and authorization take precedence.

## Read the right source

- [README.md](README.md): current user workflows, supported features, and setup entry points.
- [CONTRIBUTING.md](CONTRIBUTING.md): C++ conventions, dependencies, build commands, coverage, and PR process.
- [Migration plan](docs/migration-plan.md): architecture, Tracker compatibility, and unfinished milestones.
- [Tracker issue triage](docs/upstream-issues.md): upstream reports, native filings, feature decisions,
  and regressions to address in the corresponding port phase.
- [Review checklist](docs/code-review.md): apply to each proposed diff, including your own.

Keep each document scoped to its purpose. Update the canonical source when behavior changes and link
to it elsewhere. Do not describe planned functionality or unavailable tooling as already implemented.

## Working on a change

- Inspect the checkout, current branch, existing PRs, and relevant code before editing. Preserve other
  work. Reuse a suitable checkout; use a separate branch for an independent change.
- Keep PRs focused on one behavior or problem. Fix build failures, data loss, and incorrect scientific
  results before adding unrelated features. Use the PR and issue templates in `.github/`.
- The maintainer allowed parallel work on 2026-11-21: several independent PRs on disjoint components
  may be open at once, each developed in its own branch/worktree with its own review. Dependent stacks
  stay at most one PR deep. Resolve findings and merge eligible PRs promptly; after a parent merges,
  retarget the child and clean up the verified merged branch. Do not accumulate long stacks of green
  PRs. Keep PRs that change the same code sequential; shared registration lines (CMake source lists, CI
  steps) and separate status rows may be rebased as each PR merges.
- Run formatting, static analysis, and relevant tests before opening a code PR. Follow CONTRIBUTING
  for the actual tools; MoTrack does not currently require TaskSmack's pre-commit hooks or scripts.
- Distinguish locally verified results from CI results. Report the tested commit and platform. A Windows
  result alone is not evidence of macOS or Linux parity. Never weaken the 90% combined C++ coverage gate
  or skip failing checks to get a change through.
- Apply the review checklist and resolve findings with evidence. Report actionable defects and their
  impact; avoid formatter-enforced style nits and speculative claims.
- The maintainer authorized autonomous MoTrack PR merges on 2026-11-06 until the full port is complete:
  merge when required CI is green on the current revision and code review passes, with blocking findings
  and discussions resolved. Apply this standing authorization without asking again for each eligible PR.
  Respect repository protections; never use an administrator bypass or rewrite shared branch history.
  This authorization does not grant new release-publication or signing authority.

## MoTrack-specific constraints

- Use C++23, Clang, CMake, and Qt 6 on Windows, macOS, and Linux. Preserve the dependency pins in the
  setup tools unless changing them is part of the task.
- Ship a native application without Java or Swift runtime requirements. Java belongs only to the
  development/reference tests; it must not be needed for Tracker import, export, or normal application use.
  Native Qt, FFmpeg, libarchive, C/C++ runtimes, and operating-system libraries remain valid dependencies.
- Keep scientific analysis and the project data model independent of Qt Widgets, FFmpeg, and OS APIs.
  Translate at adapter boundaries; keep drawing and selection in the workspace.
- Preserve the required [Tracker round-trip workflow](docs/migration-plan.md#tracker-round-trip-compatibility).
  A `.motrack` companion is allowed alongside standard files; native-only saving is not the compatibility
  target. Retain unknown source content and validate exported edits by reopening them in Java Tracker.
- Follow the [full-port implementation order](docs/migration-plan.md#implementation-order-and-exit-gates)
  and its [architecture](docs/migration-plan.md#architecture). The OSP XMLControl layer and typed
  `TrackerDocument` are planned (waves 0–1), not yet implemented. New model work targets them rather than
  growing `LegacyProject` or the QDom writers; until slice B3 retires those, fixes for data loss, crashes
  or incorrect science in the current code remain required, and workspace features may keep using them
  where the plan schedules their move (for example tabsets in B1).
  Update its requirement/status matrix with each completed slice. Keep status docs concise: the matrix
  row, user-facing docs and a PR link; detailed validation evidence belongs in the PR description.
  A working prototype, preserved unknown object, or green coverage result does not by itself establish
  full Tracker parity.
- The maintainer extended completion on 2026-11-08: after the full port, resolve the remaining MoTrack
  issues in prioritized, coherent PR groups; then review the entire codebase, file actionable findings
  and resolve those issues with the same process. Follow migration-plan waves 5–6 (formerly phases 13–14). Do not postpone
  current data-loss/scientific failures until that audit, or close unresolved work merely to empty the queue.
- Before a port slice, consult the Tracker issue triage and refresh relevant upstream discussions.
  Link accepted fixes/workflows to a MoTrack issue with the original report, reproduction status and
  acceptance criteria. Keep upstream and native resolution separate; close native issues with regression
  evidence or an explicit investigated disposition. Deferred proposals do not silently expand port scope.
- Preserve correct scientific behavior when the reference has a bug: keep independent expected values,
  document the intentional difference, and do not accept a lossy Tracker resave as compatibility evidence.
- Rationalize every bug you find, whether in MoTrack, Tracker or OSP, with existing issues before or with
  the fix: search the OpenSourcePhysics/tracker and OpenSourcePhysics/osp issues and MoTrack's issues;
  link a match or file a MoTrack issue (reproduction, upstream search result, impact on Tracker workflows,
  disposition and closing condition); add reference defects to
  [docs/upstream-issues.md](docs/upstream-issues.md#defects-discovered-by-the-reference-harness); and
  reference the issue from the PR. The maintainer approved filing confirmed reference defects upstream
  under their account (2026-11-21): verify against the current upstream source, file in the
  OpenSourcePhysics repository with permalinks, a reproduction and a suggested fix, and link both ways.
- Document coordinate spaces, units, original frame numbers, clip steps, and timestamps. Changes to
  scientific results need explicit reasoning and numerical reference tests against the pinned Tracker
  behavior. Do not silently substitute zero for missing or unsupported measurements.
- Keep long decoding, indexing, import, and tracking operations off the GUI thread. Make cancellation,
  stale-result rejection, and lifetime/ownership explicit. Only the GUI thread updates widgets.
- Preserve familiar Tracker workflows; record deliberate differences in the user-facing documentation.
  Send visible strings through Qt translation and update catalogs or mark new entries unfinished.
- Use small synthetic fixtures and offscreen smoke tests by default. Wait for observable UI state rather
  than assuming a fixed frame count or sleep is enough. Keep writes in temporary test directories.
- Bound expensive local builds and tests; respect concurrency settings. Stress tests and hardware/system
  configuration changes are separate work, not routine validation. Do not stop unrelated applications.

## Clean up as work completes

The maintainer requested ongoing local and remote housekeeping on 2026-11-06.

- After a feature branch is merged, verify its current local and remote tips are contained in `main`,
  then remove the completed branch locally and remotely. Preserve open PRs, unmerged commits, other
  contributors' work, release branches, and tags. If the remote tip changed, recheck before deletion.
- Fast-forward local `main` after merges when safe. Do not discard local changes or divergent commits.
- Remove obsolete task-owned preview copies and scratch outputs once their source and useful results
  are saved. Verify exact paths before recursive deletion and do not follow links into another tree.
- Retain the active build, pinned SDKs, dependency downloads/binary caches, and representative experiment
  samples while useful. Do not trade a small cleanup for a full toolchain rebuild on the next task.
- Finish with a clean, committed checkout or clearly describe any deliberately unfinished work. Report
  the PR/check status and any remaining compatibility gaps without claiming full Tracker parity.
