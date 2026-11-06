# MoTrack review checklist

Apply the relevant checks to each proposed change. Describe a concrete failure scenario, location,
and consequence for a finding; distinguish blocking defects from suggestions. Inspect the diff and
its callers rather than relying on a feature summary. Skip style nits enforced by the formatter.

This checklist adapts the shared-review approach in
[TaskSmack](https://github.com/mgradwohl/tasksmack/blob/623e63a524c52de8b0f7ddb842c4d8d901f56594/.github/skills/code-review/SKILL.md)
to MoTrack. It is the only review checklist; agent entry points link here.

## Architecture and behavior

- Keep project records, edits, and numerical analysis usable without widgets or media decoders. Keep
  legacy XML, archive handling, native persistence, and media decoding at explicit boundaries.
- Do edits update all affected views and dirty/save state? Verify undo and redo after interleaved track
  creation, removal, and sample changes. Do not let reordered indices edit a different track.
- Check that cancellation and unsupported data leave an existing document usable. Report unsupported
  content instead of silently discarding it or presenting incomplete imports as full compatibility.
- Check Windows, macOS, and Linux behavior. State unsupported platform capabilities and follow-up work
  explicitly; do not claim parity from a single-platform test.

## Scientific and media correctness

- Identify image pixels versus calibrated coordinates, angle conventions, units, and axis direction.
  Check changing calibration keys, nonuniform scale, rotation/reflection, and missing calibration.
- Distinguish decoded frame ordinals, clip steps, source timestamps, and saved clip time offsets. Test
  skipped frames, negative time origins, variable frame rates, backward seeks, and missing samples where
  relevant. Rendering cadence must not change measured time or numerical results.
- Require finite values, safe bounds/conversions, and defined behavior for empty or degenerate inputs.
  Use explicit tolerances for scientific comparisons; preserve uncertainty/missing values.
- Compare changed scientific algorithms or legacy interpretations with a pinned reference and record
  intentional differences. Newly supported `.trk`/`.trz` writes require validation in the Java app,
  including edited values, untouched content, media, and dependent objects. Apply the
  [round-trip acceptance criteria](migration-plan.md#tracker-round-trip-compatibility), including a
  native save/reopen and partial failures when writing the companion and standard files together.
- Cache keys must cover every input that affects the result, including document/decoder generation,
  frame identity, orientation, calibration, and requested units. Bound cache and archive resource use.

## Ownership, exceptions, and asynchronous work

- Use RAII and value semantics. Raw `new` for a Qt parent-owned widget is valid; verify parent ownership
  and borrowed lifetimes instead of mechanically replacing it with a second owner.
- Guard optional values at the point of use. Copy stable values before calls that may change owner state.
  Check integer narrowing, exception boundaries, and history consistency if an allocation fails.
- Keep blocking decoding/indexing/tracking work off the GUI thread. Check callbacks after a project
  switch, frame seek, cancellation, or window destruction; reject stale results by request identity.
- Workers must not touch widgets. Avoid shared mutable data and locks held across I/O that the GUI also
  needs. Shutdown must have a defined lifetime and cancellation path without detached work using freed data.

## Tests, UI, and documentation

- Test through normal public entry points. Include failure/cancel paths, undo/redo, and save/reopen for
  editable features. Keep fixture media small, deterministic, and documented.
- UI tests use isolated temporary files and wait for the state they assert. Keyboard shortcut tests must
  give the intended window focus. Do not depend on another test's saved files, layout, or event timing.
- Verify missing media, high-DPI/zoom coordinate mapping, disabled actions, translated labels, and
  keyboard interaction when affected. Keep file paths Unicode-safe at platform boundaries.
- Run the actual MoTrack format, static-analysis, and test commands. Explain any narrower validation;
  docs-only changes need link/content checks, not a new C++ rebuild. CI still follows its configured rules.
- Preserve the combined 90% handwritten production-C++ line coverage requirement. Exclusions must not
  hide untested production targets. Investigate real coverage mismatches rather than suppressing them.
- Update the relevant canonical guide and translations. PRs state what changed, why, validation results,
  platform coverage, and remaining compatibility limits.

## Dependencies, automation, and release changes

- Keep dependency revisions, checksums, enabled features, and licenses traceable. Explain new downloads
  and setup tools. A build-time helper is not automatically an application runtime requirement.
- Review workflow permissions and untrusted input handling; do not interpolate PR text into shell code.
  Pin third-party actions to reviewed commit SHAs when touching those declarations.
- Keep signing credentials and sensitive data out of logs and artifacts. Distinguish a local staged build
  from a signed release. Do not claim signing, SBOM, release, or backport automation before it exists.
- Honor the user's authorization and repository protections for publishing, merging, and release actions.
