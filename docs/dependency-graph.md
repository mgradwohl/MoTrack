# Dependency graph

The dependency graph is the evaluation core that every dependent Tracker object will use: centers of
mass, vector sums, reference frames, attachments, data and model functions, boosters and two-body
systems, and filtered clones. It is slice C0 of wave 2 in the [full-port plan](migration-plan.md)
(tracked in [MoTrack #172](https://github.com/mgradwohl/MoTrack/issues/172)). The plan's
architecture asks for explicit references and a checked graph that rejects cycles and dangling
references, recomputes only affected results and invalidates by document revision. Names are labels,
not identity, as the [source name and reference design](tracker-name-references.md) explains.

This slice is a Qt-free library, `motrack_dependencies`
([dependency_graph.hpp](../src/model/dependencies/dependency_graph.hpp),
[name_binding.hpp](../src/model/dependencies/name_binding.hpp)). The typed document (B2.1) builds a
graph from its objects and reference slots, and its ID-addressed commands (B2.2) maintain it; no view or
evaluator uses it yet. It changes no file format and no application behavior. The families (C1 to C8)
will connect to it as described below. Implementing this core is not native evaluation of any dependent
track and is not evidence of Tracker parity.

## Model

- **Objects** have a persistent `ObjectId`, an optional label and an exact Tracker class name. `ObjectId`
  wraps the same 64-bit values as `TrackId`, with zero reserved, and is the identity that B2 generalizes
  to every addressable object. The label is the current Tracker name. It may be empty or duplicated, and
  it is absent for an object without a readable name.
- **Nodes** are the three facets of each live object:
  - `Definition`: inputs that do not depend on coordinates, such as mass, model parameters and initial
    values, and function expressions. A Definition may read only other Definitions; the graph refuses any
    other dependency, so no later reader of an input inherits a frame dependency.
  - `Geometry`: image-space geometry in pixels, such as marks, attached end points, model, center-of-mass
    and sum positions, and coordinate transforms. It implicitly depends on its own Definition.
  - `Data`: world-space values in calibrated units, such as data columns and evaluated functions. It
    implicitly depends on its own Geometry.

  Reading a target's Geometry therefore also reads its Definition. This is conservative: a mass edit also
  invalidates the object's own positions and, through any frame on it, all world data, even where no
  position changes. With fewer facets, ordinary workflows
  would look like cycles. Tracker's
  [ReferenceFrame](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/ReferenceFrame.java#L180-L210)
  follows its origin's image positions, while every track's world data, including the origin's, is
  measured in that frame. A center of mass
  [weights image positions by the members' masses](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/CenterOfMass.java#L391-L409),
  so editing a member's mass must move the center and any frame on it.
- **Dependencies** belong to an owner node and point to the target facet they read. Each has a kind, a
  slot (the ordinal within the owner's field, unique per owner and kind), a bound target identity or an
  unresolved saved name, and a missing-target policy. Repeated serialized members keep separate slots.
- **References** are live, dangling (bound to an identity that is not live) or unbound (the saved name
  did not resolve uniquely at load). Dangling and unbound references are kept in the graph, returned by
  every plan and never dropped. `MissingTarget::Blocks` prevents evaluating the owner, so no partial
  result is computed silently. `MissingTarget::FallsBack` is for owners with a defined value of their
  own, such as an unattached tool point or a model's own initial state. These owners remain evaluable,
  and the reference is still reported.
- **Identities are never reused.** An identity that was removed, or retired with `retireIdentity`, can
  return only through `restoreObject`, which is how undo works. A dangling reference therefore cannot
  silently bind to a new object.
- **Cycles.** New edits use `CyclePolicy::Reject`. An edit that would close a cycle is refused, and the
  error contains the complete path. `cycleIfAdded` performs the same check without changing the graph,
  so an inspector can offer only acceptable targets. Loaders and undo/redo use `CyclePolicy::Record`,
  which preserves a cyclic reference found in a file or restores an earlier state. Plans report one
  shortest path per strongly connected component and block its nodes and their dependents.
  `describeCycle` formats a path for logs.
- **Plans.** `plan()` lists the evaluable nodes, each after all of its dependencies. Ties are broken by
  ascending node key, so the order does not depend on insertion history. A plan also returns the
  blocked nodes, the cycles, and the unresolved references. Each blocked node carries its reason and the
  responsible explicit dependency, when one exists. Every algorithm is iterative and takes
  O((nodes + edges) log nodes) time. The tests use a chain 10,000 nodes deep.
- **Revisions.** `revision()` counts every successful mutation, including renames. It never rewinds,
  even across undo, and a refused operation leaves it unchanged. Each node has an effective revision,
  the latest revision at which the node, its dependency list or anything it depends on transitively
  changed. A result computed at revision R stays valid while `isCurrent(node, R)` holds. `planSince(R)`
  lists exactly the nodes invalidated after R, in evaluation order. A rename does not invalidate any
  node, because names do not affect values. `planSince` rebuilds the topology on every call, so an
  evaluator running at drag rate should plan once per committed revision, not once per mouse event.
- **Revision floor.** Revisions are compared only within one graph and its copies, but a document owner
  replaces its value (opening, Revert) with one whose graph counted separately, possibly lower.
  `markAllChanged(floor)` marks every live node changed at a new revision, the next one or `floor` when that
  is later. Assignment calls it on a copy of the incoming graph with the outgoing live graph's revision + 1,
  so the new revision is max(incoming + 1, outgoing + 1) and `isCurrent(node, R)` fails for every R computed
  against either value. The step is never skipped: a floor at or below the graph's own revision is ignored
  and the revision still advances by one.
- **Threads.** The graph is a value. A copy is an independent snapshot that workers may read
  concurrently. Only the owning thread mutates the document's graph. A mutation can throw only
  std::bad_alloc. The graph is derived state, so after such a failure it is rebuilt from the document.

## Names at load and save

`NameIndex` performs exact, case-sensitive lookup within one scope, such as the tracks of one panel. It
includes every named object in that scope, even unsupported ones. Objects without a readable name are
not indexed, while an empty name is an ordinary name; the source index makes the same distinction.
`CandidateFilter` expresses a field's loader type rule:

- An excluded class is skipped.
- A class is eligible when the eligible list is empty (a panel-wide lookup) or when the list contains it.
- Any other class is unclassified.

Lookup has the same outcomes as `TrackerReferenceResolution` in the read-only
[source reference index](../src/tracker_references.hpp):

| Outcome | Meaning |
| --- | --- |
| Unique | Exactly one eligible candidate and no unclassified one. |
| Ambiguous | More than one eligible candidate. |
| Missing | No eligible candidate. |
| NoTarget | An explicitly empty slot. It creates no dependency. |
| Unclassified | A candidate's class eligibility has not been verified, for example a nested center of mass. |

A test feeds the same Tracker XML to both indexes and requires equal outcomes and candidates for
selections, the reference frame, center-of-mass and sum members, boosters, system particles,
attachments and circle-fitter step attachments. The test covers unnamed tracks, empty names and typed
null slots. Same-name candidate visits share a budget of 100,000, as in the source index.

This parity is with the conservative source index, not with every Tracker loader. Tracker accepts any
PointMass subclass as a center-of-mass member or booster, but the filters leave a center of mass of
models, or a model boosted by another model, Unclassified. The C4 and C5 families widen those filters
as they add application cases.

At save, `renderName` writes a bound target's current label or an unresolved reference's saved name.
It then checks what Tracker would bind if the file were reloaded:

| Status | Meaning | Writer action |
| --- | --- | --- |
| Faithful | A reload binds the same target, or produces the same unresolved outcome and the same set of candidates, in any order ([#350](https://github.com/mgradwohl/MoTrack/issues/350)). | Write the name. |
| Ambiguous | Another eligible object now has the same label. | Refuse, or make an explicit edit. |
| Unclassified | An object of an unverified class has the same label and could capture the reference. | Refuse, or make an explicit edit. |
| Rebinds | A reload would bind a different object, for example an unresolved name that has become unique. | Refuse, or make an explicit edit. |
| Lost | A reload would find no eligible target where the reference had a target or candidates. | Refuse, or make an explicit edit. |
| Dangling | The target was removed. | Refuse, or make an explicit edit. |
| Unnamed | The target is live but has no name to write. | Refuse, or make an explicit edit. |

Writers write the name only when the status is Faithful and never patch it by text replacement.

## Edge kinds and Tracker fields

The table records the reference behavior of upstream `SwingJS` at
[`34b1b50`](https://github.com/OpenSourcePhysics/tracker/tree/34b1b503f6cb55bee031d64ea916bf4f3262504d),
read as source. Except where the corpus already covers a field (see the
[name design](tracker-name-references.md)), these findings have not been reproduced in the pinned 6.3.5
application. At run time Tracker references hold objects, so a rename is followed automatically and only
the saved name changes.

| Kind | Tracker field | Tracker resolution and cleanup | MoTrack edge and policy |
| --- | --- | --- | --- |
| `MemberOf` | CenterOfMass `masses` | Resolved after load by [scanning point masses](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/CenterOfMass.java#L109-L128). Every same-named mass is added, and the center itself is not excluded. Missing names are dropped silently. A deleted member is [removed by a listener](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/CenterOfMass.java#L302-L310). | Center Geometry to each member's Geometry, which includes the member's mass; the center's total mass reads the members' Definitions. Blocks. |
| `MemberOf` | VectorSum `vectors` | `getTrackByName(Vector)` first match. The [loader](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/VectorSum.java#L497-L509) never clears `initialized`, so the names are never resolved ([MoTrack #63](https://github.com/mgradwohl/MoTrack/issues/63)). | Sum Geometry to vector Geometry; Blocks. |
| `MemberOf` | DynamicSystem `particles` (up to two) | [Scans dynamic particles](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/DynamicSystem.java#L185-L208) on each draw until all names resolve. | The system computes its members' positions. The system's Geometry reads each member's Definition (initial values, mass) and the force functions (`FunctionOf`), and each member's Geometry reads the system's Geometry (`ReferencesDataOf`). Blocks. |
| `AttachedTo` | TTrack `attachments` (tape, protractor, circle fitter), CircleFitter `step_attachment` | First match of any class by name. [Resolution is all or nothing](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/TTrack.java#L2072-L2107) and is retried on every draw. The [attachment dialog](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/AttachmentDialog.java#L473-L491) offers point masses and RGB regions as targets. | Tool Geometry to target Geometry, one slot per end, for point-mass and RGBRegion targets alike; FallsBack to the stored point. |
| `FrameOf` | TrackerPanel `referenceframe` | [`setReferenceFrame`](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/TrackerPanel.java#L1391-L1421): first point-mass match, and a missing name silently gives the default frame. Deleting the origin reverts to the default frame. The [reference-frame menu](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/TMenuBar.java#L2497-L2508) offers models. A model that is the origin [computes in the default coordinates](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/ParticleModel.java#L1014-L1019). | The default coordinates and the ReferenceFrame are separate objects. Frame Geometry reads the default coordinates' Geometry and the origin's Geometry, and every track's Data reads the active frame's Geometry. A model's Geometry reads the default coordinates while it is the origin, and the active frame otherwise; the family rewires this edge when the origin changes. A frame directly on a model is therefore acyclic. A frame on a center of mass that contains a non-origin model is a real cycle. |
| `BoosterOf` | DynamicParticle, DynamicParticlePolar and DynamicSystem `booster` | [Resolved on each draw](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/DynamicParticle.java#L92-L104) by first point-mass match, and stays pending while missing. | Model Geometry to booster Data, or for a system member, system Geometry to booster Data; FallsBack to the model's own initial state. |
| `FunctionOf` | Model parameters, main and support functions; track data functions | OSP [excludes circular functions](https://github.com/OpenSourcePhysics/osp/blob/d49c6ecf41f48dc53fa85af1806241b46f7a90b4/src/org/opensourcephysics/tools/FunctionEditor.java#L684-L723), and functions that use them, from the evaluation order. | Expressions and parameter values are Definitions, and evaluated function values are Data. A model reads its parameters and functions through its own Definition, and a data function's Data reads the functions it uses; Blocks. |
| `ReferencesDataOf` | Data functions reading their track's columns; FilteredPointMass reading its source; calibration tools defining scale | A filtered clone holds its source by integer ID, derives its name from the source and is not saved separately. Data functions see only their own track's columns, parameters and other functions ([Tracker #14](https://github.com/OpenSourcePhysics/tracker/issues/14) asks for more). | Reader Data to source Data or Geometry; Blocks. |

`VectorChain` is a runtime chain of vector steps linked tip to tail. It is not saved and creates no
track-level reference.

The following references are deliberately not graph edges:

- **View and selection references.** Selected tracks, packed view selections, plot and table owners and
  plot guests choose what a view shows; they do not feed a computation. They live on the typed view
  objects (C7, I1), use `NameIndex` and `renderName` at load and save, and appear in `referencesTo`
  only if a family also records them as a dependency.
- **Video-image inputs.** RGB regions and line profiles (C6) sample decoded frames after the filter
  stack. Frames and filters belong to the media pipeline, outside the graph. C6 keys each sampled result
  by media generation, filter-stack revision and the region's effective revision.

## Tracker behavior and MoTrack's deliberate differences

Tracker blocks some cycles in its user interface but does not check the files it loads, and its
interface checks only direct relationships:

- The [center-of-mass](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/CenterOfMassInspector.java#L229-L230)
  and sum inspectors skip other centers and sums.
- The [booster dropdown](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/ModelBuilder.java#L394-L433)
  skips the model itself, systems, models boosted by it and members of its own system. It still offers a
  center of mass that contains the model.
- The attachment dialog [keeps models away from a calibration stick](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/AttachmentDialog.java#L473-L491)
  because that "creates circular dependency". It still offers a center of mass that contains a model.
  The reference-frame menu likewise offers a center of mass that contains a model other than the origin.
- Nothing equivalent exists in the loaders. A file can make a center of mass contain itself, two
  models boost each other, or a system list itself. Then
  [`isBoostedBy`](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/DynamicParticle.java#L540-L550)
  and the step listeners recurse without a visited set.

| Situation | Tracker (read from source) | MoTrack |
| --- | --- | --- |
| Duplicate names | No uniqueness check on load. Each field picks the first match, or, for centers of mass and systems, every match. | Ambiguous: unbound and reported; never chosen by order. |
| Missing target at load | Silently dropped, silently defaulted, or kept pending and retried on each draw, depending on the field. Attachments resolve [all or nothing](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/TTrack.java#L2072-L2107) ([MoTrack #252](https://github.com/mgradwohl/MoTrack/issues/252)), and a system with one missing member [drops the found one on the next draw](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/DynamicSystem.java#L185-L208) ([MoTrack #288](https://github.com/mgradwohl/MoTrack/issues/288)). | Every reference is bound independently. Unresolved ones are kept, written back verbatim while Faithful, and block or fall back explicitly. |
| Cycle in a file or through an indirect choice | Unbounded recursion or feedback is possible, with no diagnostic: boosters ([MoTrack #253](https://github.com/mgradwohl/MoTrack/issues/253)), centers of mass ([#289](https://github.com/mgradwohl/MoTrack/issues/289)), systems ([#290](https://github.com/mgradwohl/MoTrack/issues/290)), and centers of mass that contain a model ([#291](https://github.com/mgradwohl/MoTrack/issues/291)). | Recorded, reported with a path, and only the cycle and its dependents are blocked. New edits that would close a cycle are refused, however indirect. |
| Deleting a referenced track | Centers of mass remove the member. Tools [do not listen for removal](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/TapeMeasure.java#L493-L554); only an open attachment dialog clears its own tool ([MoTrack #251](https://github.com/mgradwohl/MoTrack/issues/251)). Models other than the selected one keep a deleted booster ([#286](https://github.com/mgradwohl/MoTrack/issues/286)). A resave writes the deleted names. | References are kept as dangling and reported. A command must refuse or make an explicit, reversible edit. |
| Undoing a deletion | [`TrackDelete.undo`](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/Undo.java#L835-L867) reloads the track's XML as a new object, so center-of-mass, sum, system, attachment and frame memberships are not restored ([MoTrack #287](https://github.com/mgradwohl/MoTrack/issues/287)). | `restoreObject` brings back the same identity with its own dependencies, and every dangling reference to it binds again. |
| Rename | References follow the object, and the new name is written at save. | The same, through identities. The writer additionally refuses a label that a reload would resolve differently. |

These differences preserve correct relationships where Tracker loses or corrupts them. MoTrack does
not accept a Tracker resave that loses relationships as compatibility evidence. Each family records its
known-defect cases with independent expected values before claiming round trips.

## How the typed model and the families use it

- **B2, the typed model.** Loaders allocate identities, add every object (including unknown classes
  that can hold names) to the graph and a `NameIndex` per panel, and bind each reference field with that
  field's `CandidateFilter`. They add the dependencies with `CyclePolicy::Record`. The document's graph
  is part of the editable state and is copied with worker snapshots. ID-addressed `Insert`, `Remove`
  and `SetField` commands call `addObject`, `removeObject` and `restoreObject`, add and remove
  dependencies, and call `markChanged` for value edits: a mass or parameter changes the Definition, and
  a mark changes the Geometry. Writers use `renderName` for every reference.
- **C1, coordinates.** The default coordinates are an object. Calibration tools that define the scale
  or the origin are its dependencies, and track Data reads the active frame.
- **C2, point masses.** Derived columns and filtered clones depend on their source's Data.
- **C3, measuring tools.** Tape measure, protractor and circle fitter attachments use FallsBack slots.
  Deleting a target is an explicit command: Tracker's stale attachment is not reproduced.
- **C4, centers of mass, sums and frames.** Centers of mass and sums use Blocks members. Reference
  frames use the frame edges above, which give the moving-frame work in
  [MoTrack #148](https://github.com/mgradwohl/MoTrack/issues/148) its evaluation order and cycle checks.
- **C5, models.** Boosters use FallsBack edges; system members, parameters and functions use Blocks
  edges. Inside a two-body system, the system reads each member's Definition and booster. Boosting a
  member from another member of the same system is then a real cycle, which Tracker's booster list also
  excludes. Booster and system cycles from files are reported instead of recursing.
- **C4/C5 compatibility case: a frame on a two-body system member.** The system computes its members in
  the active frame, so MoTrack reports a frame on a member as a cycle and blocks the system. Tracker applies
  its default-frame rule only to the object that computes, through `isUseDefaultReferenceFrame`. C4 and C5
  must record Tracker's actual result in an application case and resolve the difference before claiming parity.
- **C8, data functions.** Expressions refer to columns, parameters and functions by identity
  (`FunctionOf`/`ReferencesDataOf`). The K1 parser's referenced-variable list supplies the edges.
- **Evaluators** process `planSince(lastRevision)` in order. They key each cached result by node and
  effective revision, and drop results that are no longer `isCurrent`.

## Undo, redo and native identities

Undo and redo are new mutations: the revision advances and affected results are recomputed. They restore
a state that already existed, so they replay with `CyclePolicy::Record`. Deleting a center of mass that
a file made list itself, then undoing, brings the recorded cycle back, still reported, instead of
refusing the undo. Only new edits use `Reject`. A removal command stores the `RemovedObject`, and its
undo calls `restoreObject`, which leaves the record usable if it is refused. Redo of a creation also
restores the stored record rather than adding a new identity. Explicit dependency edits return the
removed `Dependency`, and their undo adds it back with `Record`.

Native version 7, which is planned in B2 and not yet implemented, will store the identity sidecar and
the identity counter, so a reopened document keeps its identities. The graph is derived state and is
not serialized; it is rebuilt from the typed model. An unresolved reference keeps its saved name in the
Tracker XML, so it is bound in the same way on reopen. When a reopened document still refers to an
identity whose object was removed before the save, the loader calls `retireIdentity` for it, so the
reference stays dangling rather than being refused. Removed identities are not reused within a session.
Across sessions, the persisted counter prevents reuse.
