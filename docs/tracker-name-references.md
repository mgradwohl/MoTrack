# Tracker track names and source references

This records the partial phase-3 source-reference implementation and rename architecture.
The internal standard writer now supports conservative point-mass and independent-vector renames, with
selected/unselected protractor cases and a selected-vector case. Native names have undo/redo and persistent identities. Broader rename/export
eligibility and desktop standard Save remain unfinished.
The [full-port plan](migration-plan.md) remains the requirements/status authority.

The first reference-only slice records independent protractor selected-track and plot/table targets
in the corpus manifest. Its actual-Tracker probe compares live selections and the normal packed-string
serializer before saving, after saving and after reopening. The PR's CI result establishes whether
this baseline passes. A second Java-authored reference covers center-of-mass constituent names,
resolved member objects, total mass and independent weighted positions at sparse frames. The saved-view
reference now records the protractor table's column/format owner and ordered columns, and the dynamic-sine
plot's owner, axis variables and linked-axis flag. These independent original-file expectations are
compared with serializers of live loaded views at all three stages. A synthetic three-point-mass
case adds ordered, nonempty comparison guests on two plots, checked through the live view serializers
and independent point-mass image/world baselines. A separate Java/OSP-authored source-structure case adds two nonempty table dataset owner rows;
it has no media and is not an application-session acceptance case. Media-backed dataset behavior,
protractor/circle attachment behavior, broader dynamics and edited dependency cases remain. A separate
tape-attachment fixture requires two attached endpoints and each partly attached tape to follow point
masses and retain independently calculated lengths through normal Java load/save/reopen. The vector-sum reference
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
| [CircleFitter](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/CircleFitter.java) | `step_attachment` | A separate track name for fitting to that track's steps; distinct from the point-attachment slots and their frame-range settings. Source indexing only; application behavior remains unverified. |
| [CenterOfMass](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/CenterOfMass.java) | `masses` | Collection of constituent point-mass names. The audited loader scans matching `PointMass` drawables and can add multiple same-named masses; duplicate-name behavior needs application cases. |
| [VectorSum](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/VectorSum.java) | `vectors` | Collection of constituent vector names resolved with `getTrackByName(Vector.class, name)`. The reference case has two unique member names, but the pinned loader does not reset `initialized`; live resolution never runs and resave loses the names. This case is a known compatibility failure. Duplicates and nested sums need separate cases. |
| [DynamicSystem](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/DynamicSystem.java) | `particles` | Ordered array of up to two component particle names, resolved during normal initialization/drawing. |
| [DynamicParticle](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/DynamicParticle.java) | `booster` | Name of a point mass resolved through `getTrackByName(PointMass.class, boosterName)` during normal drawing. The model-dependencies case requires the booster to replace deliberately stale initial coordinates. |

## Partial native index

The read-only [native index](../src/tracker_references.hpp) inventories original source track ordinals,
Java classes and names, retaining unsupported track nodes. Known persistent IDs come only from
validated original bindings; native rename/reorder/deletion does not change them. Old documents with
unknown bindings retain source-node resolution without inventing native IDs.

It reads root `selectedtrack`, `referenceframe`, exactly four plot/table pairs in
`selected_track_views`, nested table/plot `selected_track`, table column/format/dataset owners and
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
Table `dataset_indices` rows retain their ordered owner slots, including repeated owners. Only cell 0
is a reference; cell 1 is a setting. The conservative codec requires dense two-string rows and canonical
nonnegative decimal Java-int settings (0 through 2147483647), as emitted by the audited serializer.
Other encodings (including signed, leading-zero or whitespace spellings that Java might parse) are
diagnosed; they do not prevent importing or retaining the original XML. It does not validate the
available dataset range or target's table capabilities. Missing/ambiguous names remain explicit, and
malformed rows emit no partial dataset-owner list. These rows share the nested-entry/candidate budgets.
The index includes serialized rows even without `track_columns`; the Java loader only applies them
inside its non-null `track_columns` branch. Source references do not establish active table state.
The `table-datasets` fixture uses real LineProfile serializers and the pinned OSP String[][] codec,
with an independently transcribed native-index oracle. It has no media; actual application resave,
line-profile measurements and dataset editing/export still require a media-backed reference case.
Attachment arrays on exact `TapeMeasure`, `Protractor` and `CircleFitter` source classes retain every
ordered slot. Canonical OSP null objects (`type="object"` with literal `null` text) become `NoTarget`
with an empty `originalName`; a string containing `null` remains an ordinary name. OSP omits interior
null entries, so gaps up to the last encoded index also become `NoTarget` and consume the shared entry
budget. Trailing null extent omitted by a normal Java save is not invented by the source index.
Canonical null encodings and well-formed bounded strings are required before emitting any slot.
Empty arrays and absent fields are allowed. The index describes serialized slots, including unusual
slot counts; it does not claim that every slot maps to an active tool point. Generic panel name lookup
retains ambiguous/missing targets and unsupported source nodes. Unknown owner classes are not assumed
to implement TTrack attachment semantics. CircleFitter's separate `step_attachment` scalar is indexed
independently; its frame settings are data. All slots share the entry/candidate budgets.
The actual-application attachment case covers three read-only tapes and marked point masses on three
frames, including leading implicit and trailing explicit null slots. The application harness pads only
its expected tape slot shape to two endpoints when Java omits trailing null extent; it does not alter
the loaded objects or the independent measurement expectations. Normal deferred attachment refresh is observed through live
membership, attached points and matching endpoints; the harness never repairs the loaded objects.
Protractor/circle application cases, absent target steps, cycles, unknown classes and edited attachment
round trips remain. These are preserved source relationships, not native attachment evaluation.
Exact `DynamicParticle`, `DynamicParticlePolar` and `DynamicSystem` owners now index their optional
`booster` field. Exact `PointMass` targets resolve; the known wrong base `Vector` type is excluded,
while other same-named classes remain conservatively `Unclassified`. Systems also index optional
`particles` arrays with zero to two dense, non-null string slots. Cartesian and polar dynamic particles
are known eligible member classes; base point masses/vectors are excluded, and other classes (including
nested systems) remain unclassified. Repeated member slots remain separate; missing/duplicate targets,
malformed arrays and shared resource bounds are explicit. This does not certify a valid simulation.
The `model-dependencies` fixture authors a static two-particle system and one named point-mass booster
through the pinned serializers and OSP controls. Independent initial positions and mass-weighted system
positions must survive normal application loading, saving and fresh-process reopening. Live serializers
expose resolved relationships; the harness does not repair loaded objects. Polar dynamics, moving
boosters, missing target frames, interacting forces, cycles and edited model exports remain separate cases.
Constituents resolve only unique exact `PointMass`/`Vector` candidates; the other base type is excluded.
Other same-named classes (including nested dependent tracks) yield `Unclassified`, retaining their possible
source targets. Duplicate eligible names stay `Ambiguous`; this index does not simulate either loader's
duplicate-name behavior. Repeated serialized member slots remain distinct references.
Resolution also distinguishes missing targets, packed `null` tokens and typed null attachment slots.
An ordinary string property containing `null` remains a name. Malformed known
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

This is not a complete dependency graph or an export eligibility check.
No malformed paths does not mean a rename is safe.
The `referenceframe` scalar is indexed, but moving-coordinate behavior still lacks application cases.

## Implemented rename subset and remaining architecture

The internal writer uses original source identities to patch point-mass/vector names, root/packed view
selections, nested table/plot selections, table column/format owners and modern plot owners/guests. Native reorder, repeated renames,
undo/redo and reopen do not change which original object is patched. Other words in descriptions,
column labels and format patterns remain data. No global text replacement is used.

Eligibility additionally requires the explicitly audited media-free schema in
[`tracker_rename.cpp`](../src/tracker_rename.cpp): axes, point masses, independent vectors, unattached protractors and table/modern plot
settings, together with their numeric geometry, color, clip and toolbar fields. Unrecognized properties,
attributes/classes, model/function data, attachment fields, reference frames, legacy/scaled plot fields and dataset
rows reject rename export, even when the partial reference index has no malformed paths. The original
source and native document remain available. Extend this allowlist only alongside application acceptance.

All source/final names must be unique, including unsupported protractors and axes. Original labels
must also satisfy the codec restrictions below: a track named `null` is ambiguous with no selection,
even when a different track is renamed. Unedited source copies remain byte-exact. Every indexed
relationship must resolve uniquely or explicitly mean no target. New labels must be valid round-tripping
UTF-8, nonempty/trimmed, at most 4096 UTF-16 units, without controls/format characters, the packed
delimiters comma/semicolon, the reserved `null` token or the CDATA terminator. The schema walk limits
depth to 32 and aggregate properties/array indices to 10000. Rejection happens before replacing a file.
These conservative restrictions apply to this exporter, not ordinary native naming.

The protractor cases rename selected mass B and, separately, unselected mass A to `élève β & <B>`,
after repeated native edits and undo/redo. Native save/reopen removes dependence on the input copy.
An independent complete-source comparison permits only the name/reference edits, including a temporary
note containing the old names. Tracker must retain selected plot/table objects, custom table definitions,
untouched measurements and the renamed label through load/save/fresh-process reopen and native reimport.
The existing visited-table preparation remains explicit; [MoTrack #64](https://github.com/mgradwohl/MoTrack/issues/64)
is not closed or established safe for immediate unvisited Tracker resaves by this change.

The independent-vector case adds explicit root/packed selections for vector B in a temporary source
copy, then performs the same Unicode rename, native reopen and application round trip. Vector frame-data
and typed null cells that retain the array extent remain untouched. The schema accepts canonical null
Vector/PointMass frame-data entries only; arbitrary missing named objects remain unsupported. All existing vector
export safeguards still apply, including fixed coordinates, supported arrow footprints, bounded frame
arrays and distances from origins/other vector heads that avoid Tracker's first-draw snapping.
This does not extend export to vector sums or mixed dependent scenes; [MoTrack #63](https://github.com/mgradwohl/MoTrack/issues/63)
remains an explicit external compatibility blocker.

Modern `PlotTView.track_views` collections now admit audited `PlotTrackView` owners and their
`plot0`–`plot2` panels with x/y variables, lines/points flags and ordered guest names. Reference paths
walk collection members by ordinal even though their XML labels repeat `item`. Two plot-guests modes
rename owner mass A and guest mass B separately. Every affected owner, selection and guest slot must
retain its original target; C/B and B guest ordering and all three masses' sparse scientific data are
independent expectations. Variable labels remain data even when equal to a track name.

Track-view owners must be unique within each view collection, and guests must be unique within each
panel. Java otherwise overwrites repeated owners or coalesces repeated guests on resave. The same guest
in separate panels remains supported. Invalid definitions reject export before replacing a file.
Legacy direct plots, fixed plot ranges/scaling, datasets and dependent-track edits remain outside this writer subset.

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

The first rename cases include the protractor experiment's selected `mass B` and unselected `mass A`.
They require identical words in an unrelated description to stay unchanged; native regressions reject
unknown/ambiguous relationships. Independent vectors now have a separate selected-vector case.
Next extend to each dependent
field with its own application evidence, then creation/deletion and archive/paired saving.

Plot/table selections and view owners/guests must be viewable targets. Coordinate axes are valid
main-panel selections but are ignored by TrackChooserTView; a rename export rejects such view
references even when the referenced name is unchanged. Atomic-rejection tests cover nested selection,
owner, guest and packed plot/table selections.
