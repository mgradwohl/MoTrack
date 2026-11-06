# MoTrack agent guidance

This is the shared guidance for Codex, Claude Code, Copilot, and other agents working on MoTrack.
`CLAUDE.md` imports it and `.github/copilot-instructions.md` points here. Keep shared rules here rather
than maintaining competing copies. The user's instructions and authorization take precedence.

## Read the right source

- [README.md](README.md): current user workflows, supported features, and setup entry points.
- [CONTRIBUTING.md](CONTRIBUTING.md): C++ conventions, dependencies, build commands, coverage, and PR process.
- [Migration plan](docs/migration-plan.md): architecture, Tracker compatibility, and unfinished milestones.
- [Review checklist](docs/code-review.md): apply to each proposed diff, including your own.

Keep each document scoped to its purpose. Update the canonical source when behavior changes and link
to it elsewhere. Do not describe planned functionality or unavailable tooling as already implemented.

## Working on a change

- Inspect the checkout, current branch, existing PRs, and relevant code before editing. Preserve other
  work. Reuse a suitable checkout; use a separate branch for an independent change.
- Keep PRs focused on one behavior or problem. Fix build failures, data loss, and incorrect scientific
  results before adding unrelated features. Use the PR and issue templates in `.github/`.
- Run formatting, static analysis, and relevant tests before opening a code PR. Follow CONTRIBUTING
  for the actual tools; MoTrack does not currently require TaskSmack's pre-commit hooks or scripts.
- Distinguish locally verified results from CI results. Report the tested commit and platform. A Windows
  result alone is not evidence of macOS or Linux parity. Never weaken the 90% combined C++ coverage gate
  or skip failing checks to get a change through.
- Apply the review checklist and resolve findings with evidence. Report actionable defects and their
  impact; avoid formatter-enforced style nits and speculative claims.
- Merge only within the user's authorization and repository protections, with required checks passing
  on the current revision and blocking review discussions addressed. Never use an administrator bypass
  or rewrite shared branch history. This guide does not grant new merge or release authority.

## MoTrack-specific constraints

- Use C++23, Clang, CMake, and Qt 6 on Windows, macOS, and Linux. Preserve the dependency pins in the
  setup tools unless changing them is part of the task.
- Keep scientific analysis and the project data model independent of Qt Widgets, FFmpeg, and OS APIs.
  Translate at adapter boundaries; keep drawing and selection in the workspace.
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
