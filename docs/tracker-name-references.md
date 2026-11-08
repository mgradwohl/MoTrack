# Tracker track names and source references

This records the partial phase-3 source-reference implementation and proposed rename architecture.
Standard-file rename export is not implemented.
Native names already have undo/redo and persistent identities; standard export still rejects their
changes. The [full-port plan](migration-plan.md) remains the requirements/status authority.

The first reference-only slice records independent protractor selected-track and plot/table targets
in the corpus manifest. Its actual-Tracker probe compares live selections and the normal packed-string
serializer before saving, after saving and after reopening. The PR's CI result establishes whether
this baseline passes. A second Java-authored reference covers center-of-mass constituent names,
resolved member objects, total mass and independent weighted positions at sparse frames. The saved-view
reference now records the protractor table's column/format owner and ordered columns, and the dynamic-sine
plot's owner, axis variables and linked-axis flag. These independent original-file expectations are
compared with serializers of live loaded views at all three stages. A synthetic three-point-mass
case adds ordered, nonempty comparison guests on two plots, checked through the live view serializers
and independent point-mass image/world baselines. Nonempty dataset indices, attachments, dynamic
systems and edited dependency cases remain. The vector-sum reference
retains named members, independently expected image/world components and missing-member frames, but
exposes a pinned 6.3.5 defect: the loader leaves pending names uninitialized, so the live sum has no members
or samples and resave loses the names. The suite classifies this exact failure separately; it is not round-trip
acceptance. Native dependent-track editing and a safe standard-file strategy remain.


The saved-view baseline visits each customized table through Tracker's normal selection/refresh path,
waits for populated column names, and restores its original selection before taking snapshots. Reports
explicitly label this preparation. An immediate unvisited protractor resave on pinned Tracker 6.3.5
was observed to write null column slots and lose the angle column on reopen. The runner does not
accept that loss as the expected result or change the original-file expectations. This visited-view
case does **not** establish fidelity for an immediate unvisited Tracker resave. Hidden customized
tables and that upstream save behavior still need separate acceptance/handling before full parity.

## Why a name is more than one XML property

The real protractor corpus stores `mass B` in its track object, the root `selectedtrack`, a table's
`selected_track`, and the packed `selected_track_views` string. Table column/format owners also use
track names. Replacing only a track's `name` can leave saved selections or dependent objects detached;
global text replacement can corrupt unrelated descriptions and column labels.

The following inventory comes from upstream source candidate
`020692a3d23781763dd8a7a19d73bf004c3faefe`, with those protractor fields corroborated by the pinned
corpus bytes. The candidate-to-binary mapping is not fully verified. Actual checksum-pinned Tracker
6.3.5 load/save/reopen tests must establish each new writer's acceptance. This table is not yet a
complete inventory of every name/reference field.

| Owner | Serialized field | Reference representation |
| --- | --- | --- |
| [TrackerPanel](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/TrackerPanel.java) | `selectedtrack`, `referenceframe` | A selected track or moving-reference-frame track name. |
| TrackerPanel | `selected_track_views` | Packed per-view names, with comma/semicolon separators and null markers. Decode its structure before editing; do not replace substrings. |
| [TableTView](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/TableTView.java) | `selected_track` | Track name for the selected table. |
| TableTView | `track_columns`, `dataset_indices` | Each row's first element identifies its owning track; subsequent elements are column names or dataset settings. |
| TableTView | `column_formats` | Each group is selected by its first row's first element; format rows also carry names, column keys and patterns. Preserve column keys/patterns. |
| [PlotTView](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/PlotTView.java) and [PlotTrackView](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/PlotTrackView.java) | `selected_track`; nested `track_views` and `track` | Selected plot and per-track plot settings identify their owner by name. |
| [TrackPlottingPanel](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/TrackPlottingPanel.java) | `guests` | Ordered comparison-track names on each plot. The `plot-guests` case loads two plots with guest lists C/B and B; normal live serialization, save and fresh-process reopen must retain both lists and the underlying point-mass data. |
| [TTrack](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/TTrack.java) | `attachments` | Names in attachment slots, including empty slots, resolve to track objects after load. |
| [CenterOfMass](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/CenterOfMass.java) | `masses` | Collection of constituent point-mass names. The audited loader scans matching `PointMass` drawables and can add multiple same-named masses; duplicate-name behavior needs application cases. |
| [VectorSum](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/VectorSum.java) | `vectors` | Collection of constituent vector names resolved with `getTrackByName(Vector.class, name)`. The reference case has two unique member names, but the pinned loader does not reset `initialized`; live resolution never runs and resave loses the names. This case is a known compatibility failure. Duplicates and nested sums need separate cases. |
| [DynamicSystem](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/DynamicSystem.java) | `particles` | Array of component particle names. |
| [DynamicParticle](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/DynamicParticle.java) | `booster` | Name of a point mass resolved through `getTrackByName(PointMass.class, boosterName)` during normal drawing. Its model-initialization behavior needs an actual-application case. |

## Partial native index

The read-only [native index](../src/tracker_references.hpp) inventories original source track ordinals,
Java classes and names, retaining unsupported track nodes. Known persistent IDs come only from
validated original bindings; native rename/reorder/deletion does not change them. Old documents with
unknown bindings retain source-node resolution without inventing native IDs.

It reads root `selectedtrack`, `referenceframe`, exactly four plot/table pairs in
`selected_track_views`, nested table/plot `selected_track`, table column/format owners and
`PlotTrackView.track` owners, plus `CenterOfMass.masses` and `VectorSum.vectors` collections.
Plot guest arrays under `plot0`–`plot2` retain every ordered name slot. Modern `track_views`
containers take precedence over legacy direct `PlotTView.plotN` properties. Legacy slot indexing
describes serialized references, including slots after gaps that the Java loader might ignore;
it is not proof of active plotted data. The actual-application corpus currently covers the modern form.
Guest lookup uses the panel's names without constituent type filtering, matching the audited loader's
unrestricted lookup. Duplicate names remain ambiguous, unsupported targets retain source ordinals,
and a literal `null` string is a name. Resolution does not certify that a target can supply compatible
plot data. Malformed guest arrays emit no partial list; sparse/null/non-string entries are diagnosed
conservatively. Plot nodes and guest cells share the existing nested-entry and candidate budgets.
Constituents resolve only unique exact `PointMass`/`Vector` candidates; the other base type is excluded.
Other same-named classes (including nested dependent tracks) yield `Unclassified`, retaining their possible
source targets. Duplicate eligible names stay `Ambiguous`; this index does not simulate either loader's
duplicate-name behavior. Repeated serialized member slots remain distinct references.
Resolution also distinguishes missing targets and packed `null` markers. Only the packed codec treats `null` as an empty slot. Malformed known
fields have diagnostic paths; ordinary description text is ignored. Limits are 10000 source entries,
10000 total nested array/collection entries, 4096 UTF-16 units per string and 65536 units for the packed
value, within the existing XML byte cap. View indices are bounded to four choosers/four slots; string
arrays use bounded sparse maps. Table-format groups/rows must be dense because the Java loader
dereferences every row. Duplicate array labels, invalid types/classes, malformed cells and mixed
format owners produce diagnostic paths. Column names and format patterns are validated as data,
never resolved as track references. All references share a 100000 same-name candidate-visit budget, charged before filtering or copying;
duplicate names cannot multiply storage or scanning up to tracks times owners. Constituent collections
share the nested-entry budget with views, require known encodings and well-formed strings, and emit
no partial member list when malformed.
The original XML remains unchanged. Native tests cover ambiguity across supported/unknown types,
malformed fields, bounds, Unicode, rename/reorder/delete/undo and native save/reopen. The protractor
and dynamic-sine corpus checks compare the index with independently transcribed source ordinals and
the Java-verified selected mass, customized table owner and model plot owner. The center-of-mass and
vector-sum corpus cases additionally require the two serialized member names to resolve to source nodes
1/2 and retain their native IDs. The vector-sum source-index check does not negate the separate pinned
Java reload failure or permit export; it describes the original XML, not successfully loaded membership.
The internal writer rejects edits to any project containing a vector sum, even after all independent
member samples are removed. Native saving still retains the exact original source; an unchanged
standard-file copy is also preserved byte-for-byte and is not advertised as repaired Tracker behavior.

This is not a complete dependency graph or an export eligibility check. Dataset indices, attachments
and model boosters are not indexed yet; no malformed paths does not mean a rename is safe.
The `referenceframe` scalar is indexed, but moving-coordinate behavior still lacks application cases.

## Proposed rename architecture

Extend the partial source-reference index alongside the existing original-object bindings. Its source nodes
include the original collection position, Java class and original name. Editable nodes resolve to
the existing persistent `TrackId`; unsupported tracks retain source nodes without being presented as
implemented native track types. Names remain labels, not identity.

Each recognized reference records its owner/path, field kind, encoding and resolved source target.
Keep ambiguous and unresolved names explicit. Dependent fields need their loader's track-type and
duplicate-match semantics, not an unrestricted name lookup; unknown subclass relationships require
explicit diagnostics and acceptance cases. Decode packed strings and nested arrays with dedicated
bounded codecs; retain original XML for all untouched fields. Do not infer a semantic reference merely
because a display string contains a track name.

A rename transaction updates the target name and every supported reference to that source identity.
Validate unique destination names across all tracks, including unsupported tracks; check that each
affected encoding can represent the new name. Duplicate names, unresolved targets, malformed known
reference fields or unclassified content that could contain a reference keep the relevant rename or
deletion unsupported. Fail before replacing a standard file, retaining the editable native document.

Rebuild the index from preserved source XML after native reopen. The original source and bindings
remain the baseline, so several renames and undo/redo must not resolve against already-edited labels.
Older documents with unknown original bindings remain unsupported for these exports; do not guess an
association from a matching label. Do not introduce a second persistent identity system or serialize
transient undo history.

## Implementation order and acceptance

1. Complete the source-field audit and add actual-Tracker reference cases for named selections,
   table/plot owners, attachments and dependent-track constituents. Record scientific values as well
   as reference targets; a successful parse alone cannot prove the connection survived. Reference
   coverage and read-only indexing may advance field by field; each writer still requires all affected
   references and their application acceptance cases first.
2. Complete the read-only source-reference index with explicit ambiguity/unresolved results. Test
   duplicate names across types, unknown tracks, missing targets, malformed nested arrays and bounded
   packed-string parsing. Keep ordinary description/column-label text separate from references.
3. Enable selected and unselected point-mass/vector renames for the fully covered cases. Exercise
   Unicode, encoding delimiters/null markers, repeated renames, interleaved edits, undo/redo, source
   removal, native save/reopen, standard export, Tracker load/save/reopen and native reimport. Every
   affected selection/reference must retain its target and all unrelated data must remain intact.
4. Extend the same identity/reference mechanism to creation and deletion. Reject dangling dependent
   references or make an explicit reversible dependency edit; never silently drop a constituent.
5. Complete the remaining phase-3 media/archive packaging and paired-save recovery before exposing
   full standard Save. In phase 5, use the audited graph for native dependent-track evaluation with
   cycle detection and revision-based invalidation, following the main plan's intervening UI work.

The first rename case must include the protractor experiment's selected `mass B`, rather than only
an isolated track with no references. Additional cases must prove that identical words in an unrelated
description stay unchanged and that an unknown or ambiguous relationship cannot be silently rewritten.
