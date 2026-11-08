# Tracker compatibility corpus

This is the first reproducible seed for [full-port phase 1](../../docs/migration-plan.md#implementation-order-and-exit-gates).
It compares MoTrack's importer and coordinate calculations with snapshots produced by the actual OSP
XML loader and `ImageCoordSystem` implementation bundled in the official **Tracker 6.3.5** JAR.

The original report names its verification level `xml_and_coordinate_api`. A separate session probe
uses Tracker's actual application/async-loader lifecycle to open the media-free experiments,
save through Tracker's normal saver and reopen in a fresh Java process. Its report is labeled
`tracker_session_resave`. It checks loaded track classes/names, mass values, step points, clip bounds,
units and coordinate probes before/after Java's own resave, and compares available point-mass/coordinate
data with the independent XML baseline. Independent vectors are checked by their loaded tails,
components and original frame numbers before and after Java resave.

Neither check proves MoTrack `.trk`/`.trz` export, complete model/function parameter fidelity, rendering,
media decoding, cross-platform Java UI behavior or the required paired-file round trip. A successful
parser or Java-only resave check must never be reported as a complete MoTrack round trip.

A third command, `roundtrip`, adds **limited** MoTrack standard-export cases. The mass case, labeled
`motrack_mass_roundtrip`, changes the first point mass's mass in the real protractor experiment,
saves/reopens native data after removing its input copy, exports `.trk`, opens/resaves/reopens through
Tracker's application, then reimports Tracker's resave in MoTrack. The expected mass is derived from
the committed Java baseline, independently of the native probe's output. A normalized DOM-tree comparison
checks that no other source content changed, including unknown objects, comments and processing instructions.
The loaded mass, classes/names, samples, coordinates and clip are checked against that expected edit.
The second case, `motrack_marks_roundtrip`, moves the first mass's original mark, adds a mark at frame 3,
and replaces the second mass's original mark with one at frame 7. It independently checks image/world
coordinates and Java's loaded manual keyframe lists `{0,3}` and `{7}`, before and after resave. Sparse
frames also exercise OSP's line-oriented array reader: an accidental blank line must not hide later marks.
Two further mark cases delete every mark from the first mass, resave that empty track through Java,
then add its first mark in MoTrack. They verify empty key arrays and newly created frame data in the
actual Tracker loader, saver and native reimport. Empty point masses are allowed; expected samples
and keyframes still have to match exactly.
The vector cases change tails/components, insert/delete samples, clear a vector, then add its first
sample after Java resaves the empty vector. They compare unchanged XML, image-space tails/components,
loaded Java vectors and native reimport. Clip ranges, fixed calibration and length labels have additional
cases described below. Selected/unselected point-mass names have a conservative protractor round trip.
Broader names/dependencies, partly fixed calibration and track-membership edits, media/archive
round trips, desktop export UI and paired saves remain unfinished.

## Sources and cases

[manifest.json](manifest.json) pins each upstream URL, byte count and SHA-256, the upstream revision,
recorded producing Tracker version, relevant requirement IDs and known features. Examples and the
reference JAR are downloaded unchanged into ignored `.dev-tools/tracker-reference`; they are not
vendored. The manifest links the upstream license and records the unaudited media-redistribution limit.
The fourth case is a small committed synthetic fixture authored by Tracker 6.3.5's own serializers using
[VectorFixture.java](../../tools/reference/VectorFixture.java), with LF-normalized bytes and a pinned
checksum. Its expected snapshot comes from the Java reference, never from MoTrack. Local fixtures are
verified in place and restricted to this corpus's `fixtures` directory; they are never fetched or replaced.
The fifth case uses [KeyedCalibrationFixture.java](../../tools/reference/KeyedCalibrationFixture.java)
to author a varying coordinate system and point-mass samples at calibration-key boundaries. Its bytes
are likewise pinned and reproducible; expected values come only from the Java reference.

| Case | Producing version | Checked imported content | Unsupported content retained in the source fixture |
| --- | --- | --- | --- |
| Ball toss | 5.1.5 | Eleven marks at frames 2–12, mass, clip bounds/time origin and world coordinates | Tape measure, display settings and archive media |
| Protractor | 6.0.7 | Two named point masses, their marks/calibration; mass-value and mark edits through the standard round-trip runner | Protractor and its saved settings |
| Independent vectors (synthetic) | 6.3.5 | Two independent vectors, sparse frames, a zero vector, nonuniform scale; actual Java session resave | Track colors/footprints and coordinate axes |
| Keyed calibration (synthetic) | 6.3.5 | Origin, rotation and nonuniform scale vary at frames 0/4/9; marks before and at each transition; import and Java session resave | Coordinate axes and display settings |
| Center of mass (synthetic) | 6.3.5 | Two unequal point masses at sparse frames; independently expected constituent names, total mass and weighted image/world positions in the Java session | Native center-of-mass editing/evaluation and moving reference frames |
| Vector sum (synthetic) | 6.3.5 | Two named vectors and independent sums including zero/missing frames expose a pinned 6.3.5 member-initialization defect; explicitly classified failure, not accepted compatibility | Native dependent-vector evaluation/editing and safe standard-file handling |
| Plot guests (synthetic) | 6.3.5 | Three point masses with sparse marks/nonuniform calibration; two plots with ordered C/B and B comparisons; native reference index and actual Java session resave | Native comparison-plot UI and safe standard rename export |
| Model dependencies (synthetic) | 6.3.5 | A named booster, two Cartesian particles and their system; independent static positions, component masses and center across Java resave; native source-reference index | Native models/solvers, polar/interacting dynamics and edited model export |
| Dynamic sine | 5.1.2 | Clip and calibration; dynamic particle is reported as unsupported | Model parameters, functions and plot settings |

The probe reads the `.trk` member selected by the manifest from BallToss's `.trz`. It does not exercise
MoTrack's archive extractor, so the existing archive/workspace tests remain necessary. No archive member
paths are extracted by the corpus script. Add genuine multi-tab archives, upstream vector examples/dependent vectors,
current 6.3.5 features, partly fixed/reference-frame calibration, media/timing cases and failure cases next.

The executable's SHA-256 is verified and its runtime version reports 6.3.5. The June 22, 2026 upstream
commit is recorded as a **source candidate**, not a verified binary-to-source mapping. Full source and
dependency provenance, the complete requirement inventory and media/archived-session loading remain phase-1 work.
Initial snapshots were generated on Windows with Eclipse Temurin 17.0.21+9; CI repeats the reference
calculation using its Java 17 JDK on Linux. No Java runtime is added to the MoTrack application.


The saved-view baseline visits each customized table through Tracker's normal selection/refresh path,
waits for populated column names, and restores its original selection before taking snapshots. Reports
explicitly label this preparation. An immediate unvisited protractor resave on pinned Tracker 6.3.5
was observed to write null column slots and lose the angle column on reopen. The runner does not
accept that loss as the expected result or change the original-file expectations. This visited-view
case does **not** establish fidelity for an immediate unvisited Tracker resave. Hidden customized
tables and that upstream save behavior still need separate acceptance/handling before full parity.

## Run the checks

Activate the usual MoTrack development environment and build with testing enabled. The ordinary
`coverage` preset builds the test-only `motrack_corpus_probe` target.

```sh
python tools/corpus.py fetch
python tools/corpus.py verify
python tools/corpus.py check --probe build/coverage/motrack_corpus_probe
python tools/corpus.py reference --java /path/to/jdk-17/bin/java
python tools/corpus.py session --java /path/to/jdk-17/bin/java
python tools/corpus.py roundtrip --probe build/coverage/motrack_corpus_probe --java /path/to/jdk-17/bin/java
python -m unittest discover -s tests -p test_corpus.py
```

On Windows, use `build/coverage/motrack_corpus_probe.exe` and the path to `java.exe`. Python 3.10 or newer
is supported. The reference command uses Java source-file mode and therefore needs a JDK, not just a
JRE. The first `fetch` needs network access; later checks are offline against verified cached files.
`--cache PATH` changes the cache location. The reference JVM uses a private home inside that cache for
any preferences or helper files. Downloads have time/size limits, subprocesses have a timeout
and the Java heap is bounded. A corrupt existing cache entry is reported and preserved, never silently
replaced or treated as success.

The `session` and `roundtrip` commands require Linux with Xvfb installed and compile their helpers with
the adjacent JDK `javac`. It starts and owns a new Xvfb server using `-displayfd` to allocate its display;
Java always uses that display, even when the contributor already has a desktop `DISPLAY`. The runner
terminates only its own server on success or failure, with bounded startup/shutdown waits. It refuses
to run on Windows. CI's Ubuntu runner supplies Xvfb and Java 17. Each case gets a fresh private Java home
and output directory. Save and reopen each get a fresh JVM/private home, a 30-second loader
deadline inside the 45-second subprocess limit, and a 256 MiB heap. This avoids collisions between
Tracker's static drawing/snapping registries when distinct application windows reuse panel IDs.
Snapshots must agree before saving, immediately after saving, and after the separate process reopens. Temporary compiled classes,
preferences and Java-resaved experiments are removed on success or failure; successful comparison
reports remain under the cache's `reports` directory. MoTrack itself does not use Java at runtime.

The protractor manifest independently records its selected `mass B`, plot/table selections for all
four view choosers, and the packed selection string from the pinned original XML. The session helper
reads the live selected track objects and the normal serializer's packed representation. These must
match the manifest as well as each other across load/save/reopen, including the existing mass/mark/unit
export cases. Regression checks reject consistently wrong or missing targets, not only resave drift.
Saved-view checks also require the protractor's customized table to belong to `protractor A`, while
its selected table remains `mass B`. The visible columns are ordered `t`, theta; number-format patterns
are checked for time, angle, step and frame. The dynamic-sine plot belongs to `model A`, with time on
x, position y on y, and axes unlinked. Expectations are transcribed from the pinned original XML,
not generated by the native importer or copied from the new session output. The helper serializes
live loaded views without instantiating their nested XML objects. Owner groups and per-column format
rows are normalized by key because their storage order is not semantic; visible columns, plots and
guest lists retain their order. A consistently wrong owner, column, format, selection or axis choice
fails independently of the resave comparison. Transient auto-scaled ranges and rendering are outside
this snapshot. Nonempty dataset indices/plot guests, attachments and further dependent-track cases
remain prerequisites for the corresponding standard-file rename exports.

The session helper shows and seeks each loaded frame on that private display, allowing Tracker's
normal draw-driven particle-model evaluation to run. It observes the expected model steps before
snapshotting, within the existing deadline; it does not assume the XML-load callback means models
have already populated their cached steps.

CI compares native imports on Windows, macOS and Linux after ordinary coverage collection, and runs
both Java baseline probes early on Linux. The edited standard round trip runs on Linux after the
native build. Corpus checks do not inflate the production coverage claim.

## Review and extend baselines

The checked-in [expected snapshots](expected) were generated by the pinned Java reference, independently
of MoTrack's parser. Compare all object keys and list membership. Names, unsupported type names and frame
identities must match exactly; floating-point values use manifest tolerances of `1e-10` absolute and
`1e-12` relative for these coordinate cases. Missing values remain null, and nonfinite values fail.
These tolerances are not a universal rule for future fits, tracking or model solvers.

To deliberately regenerate snapshots for review:

```sh
python tools/corpus.py reference --java /path/to/jdk-17/bin/java --record
```

Inspect the diff and explain every change; CI never records or silently updates a baseline. Keep the
reference version and source-file checksums pinned. Add provenance, expected behavior and requirement
coverage with each case. Never regenerate from MoTrack's output to make a failing comparison pass.

Extend the mass/mark/vector tests to every editable type and operation, media, archives and paired-save
failures. Preservation of unsupported objects and other archive tabs remains mandatory. The writer's
mass, framedata and keyFrames properties follow the pinned upstream [PointMass loader](https://github.com/OpenSourcePhysics/tracker/blob/020692a3d23781763dd8a7a19d73bf004c3faefe/src/org/opensourcephysics/cabrillo/tracker/PointMass.java);
the actual 6.3.5 application check, not source inspection alone, determines acceptance.

The vector fixture can be regenerated to a **fresh temporary path** with a Java 17 JDK:

```sh
java -Djava.awt.headless=true -Xmx256m --class-path .dev-tools/tracker-reference/tracker-6.3.5.jar tools/reference/VectorFixture.java /temporary/path/independent-vectors.trk
```

Compare its bytes/hash with the committed fixture; the generator refuses to overwrite an existing file.
The `roundtrip` command additionally exercises `vectors`, `empty-vectors` and `first-vector` exports
through native save/reopen and separate Tracker application processes.

The `clip` case trims that fixture to frames 2 through 8 with stride 2. Its independent oracle changes
only the three range properties and the coordinate probe frame numbers; fixed calibration and all
vector samples remain unchanged, including samples outside the selected steps. Full-tree comparison
rejects unrelated changes before actual Tracker load/save/reopen and native reimport. Clip timing,
media-backed clips and legacy frame-shift handling remain outside this writer slice. The media-free
frame limit and loader arithmetic are checked against the pinned 6.3.5 JAR, not assumed from current OSP source.
The `clip-boundary` case separately uses a Java-authored axes-only document and selects the single
frame 500000. It exercises the same native persistence/export and actual-application save/reopen path
at Tracker's media-free upper boundary. It retains the existing 256 MiB Java heap and process timeouts;
no measurement/model arrays or long clip-step loops are needed for this boundary case.

The `snap-origin` and `snap-vector` cases instead verify rejected exports leave an existing destination
unchanged. Java independently authors the same near-target geometry, then its actual application must
demonstrate the expected first-draw snap and retain that snapped geometry through save/reopen. These are
explicit rejection tests, not successful native exports or relaxed comparisons of requested geometry.

The `calibration` case changes the vector fixture's existing fixed origin to (100, 120), rotation to
90 degrees and scale to (4, 8) image pixels per world unit. The independent inverse-transform oracle
maps image (13.5, 17.25) to world (12.84375, 21.625); Java's coordinate API and application must agree
before and after resave. All vector image positions/components and unrelated XML remain unchanged.
Native tests additionally exercise undo/redo, native reopen, locked/unsupported coordinate modes,
nonfinite/singular transforms and destination preservation. Partly fixed/moving system export remains pending.

Vector session snapshots compare the scientific tip/tail positions and independently expected
tail/components. Transient drag handles and display points are excluded: Tracker does not serialize
them, and an automatic snap can leave the drag handle at its old location until a fresh reload.
The numerical tolerance and checks on all saved vector geometry remain unchanged.

The `length-unit` case changes only the calibration command's label to micro-metres (`µm`), with no
scale or geometry change. Full XML preservation and native/Java snapshots check that label explicitly,
including after Java resave and native reimport. The Java snapshots now include length and mass labels;
the four baselines were regenerated from the pinned Java reference with all prior values unchanged.
Tracker's omitted length label is explicitly recorded as `m` for the vector session; Java materializes
that default on save, while the original native import still represents an absent label as empty.

The XML reference uses Tracker's normal resource-path reader. OSP's alternate `File` constructor uses
the platform default charset and misdecodes UTF-8 on Java 17 Windows; a bounded comparison confirmed
that the resource reader preserves the micro sign without changing VM-wide encoding settings. Native
string writes use CDATA rather than entities, matching OSP's string reader. Edited unit properties with
comments or processing instructions reject without replacing a file because OSP requires an adjacent
CDATA/property terminator; preserving an intervening comment can make its reader hang.
The actual application round trip remains the final acceptance gate; this adapter fix alone is not GUI evidence.

The `mass-unit` case changes the real protractor experiment's project label from `kg` to `g` with the
native reversible command. A full-tree XML oracle permits only that string change; native/Java
snapshots must retain every numerical mass, mark, calibration and unrelated object. The runner saves
and reopens the native project, exports `.trk`, loads/saves/reopens it in actual Tracker, then reimports
the Java resave. Native tests also cover Unicode labels, absent/existing properties, interleaved mass
value edits and saved-state undo/redo. This verifies label semantics, not automatic unit conversion or
the unfinished units UI.

The `keyed-calibration` baseline has three fully varying keys: frame 0 has origin (320,240), angle 0,
scale (2,4); frame 4 has origin (100,120), angle 90 degrees, scale (4,8); frame 9 has origin (110,130),
angle -90 degrees, scale (2,8). A mark at image (10,20) maps to world (-155,55) at frames 0/3,
(12.5,22.5) at frames 4/8, and (-13.75,-50) at frame 9 within the unchanged corpus tolerance.
All point-mass session samples now include world coordinates evaluated by the loaded Tracker panel;
the harness checks these against the independent baseline before save, after save and after reopening.
Matching wrong values across all three stages must still fail. This establishes a reference for future
keyed-calibration export, independently of the edited standard-file case below.

Regenerate the fixture only to a fresh temporary path and compare bytes before proposing a baseline change:

```sh
java -Djava.awt.headless=true -Xmx256m --class-path .dev-tools/tracker-reference/tracker-6.3.5.jar tools/reference/KeyedCalibrationFixture.java /temporary/path/keyed-calibration.trk
```

The `keyed-calibration` standard round trip uses three reversible native commands: change frame 4's
scales to (8,16), place its origin at (120,140) with zero rotation, and add a key at frame 3 with origin
(80,90), angle -90 degrees and inherited scales (2,4). All image marks and keys 0/9 remain unchanged.
The independent world-coordinate expectations at image (10,20) become (-17.5,-35) at frame 3 and
(-13.75,7.5) at frames 4/8. A whole-tree XML comparison permits only those key edits/addition, then
native reparse, actual Tracker load/save/reopen and native reimport check the edited scientific values.
The loaded Tracker panel's fixed flags and every calibration key's frame, origin, angle and scale are
also checked against independent constants before and after resave; unchanged mark positions alone
cannot prove that all calibration parameters survived.
Partly fixed systems, key deletion, varying vector coordinates, attachments and reference frames are
separate follow-ups; this case must not be reported as full calibration parity.

## Named center-of-mass reference

`CenterOfMassFixture.java` creates `fixtures/center-of-mass.trk` with the pinned Tracker 6.3.5
serializers. It has no media, three clip frames and two recorded frames (0 and 2). The generator
uses a fresh output path, US locale and LF normalization. Two independent runs reproduced the
7798-byte fixture with SHA-256 `813bc52c54e95ac6034bdb33bccdbaa547f592f4443fa963a5e2159ee00989a2`.
The committed XML/coordinate snapshot was produced by the pinned Java helper, not MoTrack.

Mass A is 2 kg with image points (10,20) and (14,28); mass B is 3 kg with (40,60) and (44,68).
The manifest independently requires member names A/B, total mass 5 kg and weighted image points
(28,44) and (32,52). With origin (20,20), scales (2,4) and zero rotation, the corresponding world
points are (4,-6) and (6,-8). Frame 1 stays unmarked. The actual application helper waits for the
normal draw path to resolve members, then checks these values before/after save and after a fresh
reopen. Missing or consistently wrong membership, total mass, frames or positions must fail.

MoTrack imports the two plain masses and reports the center as unsupported. This is reference
coverage for R08 and named relationships, not native dependent-track implementation or standard
rename/export acceptance. Moving reference frames, nested centers and edited constituent updates
still need their own cases. Run the generator with the same bounded Java settings as the keyed
fixture, substituting `CenterOfMassFixture.java` and a new `.trk` output path.

## Partial native reference index

The protractor case also runs the native probe with `--references`. Its separate
`expected/protractor-references.json` records the original track ordinals and names from the pinned
XML, native point-mass IDs 1/2, and selection targets already verified in the Java session. These
expectations were authored independently of the native index output. They require selected `mass B`
and its plot/table references to target source node 3; axes and the unsupported protractor retain
source identities with no native editable ID. Description text is not treated as a relationship.

This check covers the current partial, read-only index, not standard rename export, full view
settings, attachments or dependent constituents. The runner compares exact paths, names, states and
target lists; it rejects consistently wrong source identities and does not regenerate this expected
file with `reference --record`.

The native read-only reference-index oracle additionally checks nested protractor table selections,
column and format owners, plus the dynamic-sine plot selection and owner. Expected source ordinals
are transcribed from pinned original files, including unsupported source nodes with no native ID.
This does not enable rename export or imply the remaining dependency graph is complete.

## Vector-sum reference

`vector-sum.trk` is authored by [VectorSumFixture.java](../../tools/reference/VectorSumFixture.java)
using the pinned Java APIs, OSP XML controls and Vector FrameData serializer. Two independent vectors
have overlapping marks at frames 0, 2 and 4; only one member is marked at frames 1 and 3. The sum's
cached components are deliberately zero, so an application that fails to resolve its members and
recompute its data must fail even if all three snapshots agree. Fixture generation was reproduced
byte-for-byte in two isolated Java processes; no native output supplies expected values.

Independent expected image-space sums are (10,4), (0,0), (-2,4) at frames 0/2/4. With nonuniform scales
(2,4), zero rotation and image y increasing downward, world components are (5,-1), (0,0), (-1,-1).
The saved tails are (240,260), (280,280), (320,300); no sum sample may exist at frames 1 or 3.
The normal application loader and draw path are exercised before snapshotting. **Tracker 6.3.5 fails
this case:** member names remain pending, the live sum has no members or steps, and its own save writes
an empty member collection. The pinned binary's `VectorSum.Loader` does not reset the inherited
`initialized` flag (unlike `CenterOfMass.Loader`), so normal initialization returns immediately.
The first application run timed out waiting for members; an explicit diagnostic confirmed that state.

The correct independent expectations remain in `session_sums`; the ordinary compatibility validator
rejects the observed result. A separate, version/case-specific known-issue check requires exactly empty
live membership and step data at all three stages and an empty member collection in the actual resaved
XML. All unaffected tracks, units and coordinates must still match. If the defect changes or is fixed,
this check fails for investigation. Reports use `tracker_session_known_issue` with
`compatibility_accepted: false`; CI prints **KNOWN TRACKER DEFECT**, never a passed round trip. This
case cannot serve as native export acceptance and the harness does not repair the Java objects.

This establishes a compatibility blocker, not native vector-sum editing or export acceptance. Duplicate
member names, nested sums, attachment/snapping relationships and edited dependencies remain. Run
`VectorSumFixture.java` with the same bounded Java settings as the other fixture generators and a
fresh temporary output path; do not overwrite a pinned fixture or bless changed expectations.

## Native constituent reference index

Independent source-index expectations for `center-of-mass` and `vector-sum` identify axes at node 0,
the two native base tracks at nodes 1/2 (IDs 1/2), and the unsupported dependent track at node 3.
The selected-track reference targets node 3 and both serialized constituent slots target nodes 1/2.
These expectations are transcribed from the pinned originals, not regenerated from the native probe.
They validate read-only preservation metadata, not dependent-track evaluation or standard rename export.
In particular, the vector-sum index can correctly describe its source while the real Tracker application
still fails to restore the relationship. Native tests cover cross-type names, duplicates, unclassified
same-named classes, malformed/oversized lists, shared resource bounds and native edit/undo/save/reopen.

## Plot comparison reference

[PlotGuestsFixture.java](../../tools/reference/PlotGuestsFixture.java) authors the geometry with real
Tracker point-mass/coordinate serializers and the view control structure with OSP's public XML API.
It does not construct live GUI views or claim GUI authoring. The application-session test subsequently
uses the normal loader, saver and fresh-process reopen without repairing loaded views. Two fresh,
bounded headless generator processes produced identical LF-normalized bytes; the manifest pins them.
Use the same Java invocation/settings as the other generators with a fresh output path.

The selected plot owner is mass A. Plot 0 displays x against t with guests mass C then mass B; plot 1
displays y against t with mass B only. Manifest expectations independently transcribe these names,
order, variables and linked-axis flag. The existing live-view serializer check must retain them before
saving, after saving and after reopening; consistently lost/reordered guests cannot pass. The separate
Java XML/coordinate baseline checks all three masses at their original sparse frames. With origin
(20,40) and scales (2,4), A's world points are (1,2)/(5,6), B's (10,-5)/(12,-7), and C's
(-5,5)/(-3,3). This does not certify rendered curve pixels or all derived plot variables.

The native index expectation is independently transcribed: axes at source node 0, A/B/C at nodes 1/2/3
with IDs 1/2/3, selected owner A, then ordered guest targets 3/2 and 2. Unit cases additionally cover
legacy plot fields, repeated/duplicate/missing names, unsupported source nodes, malformed arrays,
shared resource limits and native rename/delete/undo/save/reopen. Legacy application round trips,
guest editing, media-backed table dataset behavior, attachments and standard rename export remain separate work.

## Table dataset source references

[TableDatasetsFixture.java](../../tools/reference/TableDatasetsFixture.java) uses pinned Tracker 6.3.5
point-mass/LineProfile serializers and OSP XML controls. Its codec check decodes the nonempty
`String[][]` dataset rows through OSP. Two bounded headless generator processes reproduce identical
LF-normalized bytes. The generator is developer-only and adds no Java application dependency.

The original source order is axes (node 0), mass A (node 1, native ID 1), profile A (node 2) and profile B
(node 3). The selected track/table and first column row target A; the second column row targets B.
Dataset rows deliberately reverse that order: B with setting 3, then A with setting 2. The native-index
oracle transcribes these targets independently; the settings are data, not names. Both profiles remain
unsupported source nodes with no native ID. The separate Java XML/coordinate baseline verifies mass
A's image points (22,32)/(30,16), world points (1,2)/(5,6), and clip/calibration metadata.

This is a **source-structure case only**. There is no media, no `session_tracks` manifest opt-in, and no
claim that loaded table columns, profile measurements or a Java application resave are verified. A
media-backed application case remains necessary before dataset editing/export acceptance. Native tests
cover malformed/noncanonical settings, missing/duplicate names, shared budgets and preservation across
native rename, reorder, deletion, undo/redo and save/reopen. They do not enable standard rename export.

## Tape attachment reference

[AttachmentsFixture.java](../../tools/reference/AttachmentsFixture.java) authors two point masses and
three read-only tapes using the pinned Tracker APIs/serializers. Two fresh bounded headless runs produce
identical bytes. The initial attached tape endpoints are deliberately stale. The application probe
uses normal loading and waits for Tracker's deferred attachment refresh to establish the requested
live targets, attached points and endpoint coordinates; it does not call protected refresh methods,
change private state, or sleep for a guessed duration.

At frames 0/1/2, mass A is at image (20,40)/(22,40)/(24,40); B is at (26,48)/(31,52)/(36,56).
Origin (20,40), scale (2,2), and image y increasing downward give world points A=(0,0)/(1,0)/(2,0)
and B=(3,-4)/(5.5,-6)/(8,-8). The fully attached tape must measure 5/7.5/10 m. The partly attached
tape follows A at its first end while its unattached end stays at (40,40), giving 10/9/8 m. A third
tape fixes its first end at (40,40) and follows B at the second, giving sqrt(65)/7.5/sqrt(68) m. The manifest
records these independent endpoints, lengths and target names; a consistent wrong load or lossy resave
cannot pass. Tape snapshots include scientific endpoints in the clip, excluding transient drag handles.
The normal public length getter evaluates measurements after attachments are ready. Empty live endpoints must remain unattached rather than become the literal track name `null`.
The first Linux application attempt exposed normal OSP omission of trailing null extent on save:
`[mass A, null]` is serialized as `[mass A]`. The public tape attachment getter restores two slots.
A headless codec probe also confirms omitted interior null cells (`[1]=mass B` decodes as
`[null, mass B]`). The harness normalizes only its expected tape-slot shape; measurements and live
objects are unchanged. The native index fills bounded implicit null gaps, charging them to the shared
entry budget, but does not invent trailing extent beyond the encoded array.

The native oracle independently transcribes axes at source node 0, masses at nodes 1/2 (native IDs 1/2),
and tapes at nodes 3/4/5 (unsupported, no invented IDs). It retains both full attachment targets plus
each partial tape's target and empty slot. Unit cases add protractor/circle source fields, `step_attachment`,
literal-null names, missing/ambiguous/unknown targets, malformed arrays, shared bounds and native
edit/undo/redo/save/reopen. This is Java's own resave baseline, not MoTrack export acceptance. Native
persistent tools, protractor/circle application behavior, missing target frames, cycles and edited
attachment round trips still need their own coverage. Calibration creation issues in MoTrack #78 remain
separate; these read-only tapes do not exercise interactive calibration or tool placement.

## Model dependency reference

[ModelDependenciesFixture.java](../../tools/reference/ModelDependenciesFixture.java) uses real
Tracker serializers for a point mass, two Cartesian dynamic particles and an empty system. Constructing
a nonempty system requires a live panel, so its named members and one component's booster are authored
through public OSP XML controls. This is source authoring, not a repair of loaded application objects.
Two fresh bounded headless runs reproduce identical pinned bytes.

At frames 0/1/2, the marked booster mass A stays at image (26,32), world (3,4), with mass 2 kg.
Particle A has initial world position (1,2), zero velocity/force and mass 1 kg. Particle B has mass 3 kg,
zero velocity/force and deliberately stale initial position (100,100); its named booster must initialize
it to (3,4). With origin (20,40), scale (2,2) and downward image y, the expected image positions are
A=(22,36), B=(26,32). The system must have mass 4 kg and center
`(1*(1,2) + 3*(3,4))/4 = (2.5,3.5)`, image (25,33), on all three frames.
The booster changes initial state, not the component's mass. These expectations are independently
calculated in the manifest, not recorded from session output or produced by MoTrack.

The normal application loader/draw path must resolve both named relationships. The harness observes
the live models' normal serializers and evaluated steps, then compares names, ordered members, masses
and positions before save, after save and after fresh-process reopen. Consistently stale coordinates,
wrong membership, wrong mass or dropped boosters must fail. This is a static dependency case, not
evidence for moving/polar particles, interacting forces, general numerical solvers or native model export.

The native oracle transcribes axes at node 0, mass A at node 1 (native ID 1), particles A/B at nodes 2/3,
and system at node 4. Selection targets node 4; B's booster targets node 1; ordered system members target
nodes 2/3. Unsupported models receive no invented native IDs. Unit cases cover polar source targets,
wrong base types, unknown classes, repeated/missing/ambiguous names, malformed arrays, shared bounds
and retained source identity across native rename/delete/undo/redo/save/reopen.

## Point-mass standard rename round trip

The `name` and `unselected-name` export modes rename protractor's mass B and mass A respectively to
`élève β & <B>`. The native probe applies repeated renames with undo/redo, saves/reopens a native
document after removing its input copy, and exports from the retained source/identity bindings.
The pinned source download remains unchanged. A temporary source copy adds a note containing both
old names so the full-source DOM comparison rejects blind text replacement.

Independent Python expectations change only the chosen track identity and affected root/packed/nested
table selections. The original custom table belongs to protractor A, so its owners, angle column and
number formats must stay intact. The complete XML comparison also checks comments, other properties
and the note. Java load, normal save and fresh-process reopen then validate selected live track/view
objects, unchanged scientific data and the Unicode label; native reimport must match. The selected
case cannot pass by changing the displayed track name while leaving stale references behind.

The writer admits only the audited media-free mass/axes/unattached-protractor/table schema described
in the [name-reference guide](../../docs/tracker-name-references.md). Unknown attributes/properties,
ambiguous/missing names, unsupported dependent objects and unrepresentable new labels fail before
replacing an existing file. Unit cases cover name swaps against original identities, interleaved mass
edits, reorder/delete/undo, malformed/bounded settings and unchanged column labels containing old names.
Dependent renames, creation/deletion, archive packaging, paired saves and desktop standard Save
remain. Java's known immediate unvisited-table resave limitation remains separately tracked in #64;
these cases use the explicitly reported visited-table preparation.

The `vector-name` mode extends the same workflow to vector B in the pinned independent-vector fixture.
A temporary source copy adds selected-track/plot/table names and a note containing vector A/B; the
committed fixture/checksum is unchanged. The name becomes `élève β & <B>` while every tail, component,
original frame, zero vector, calibration value and null frame-array extent must remain unchanged.
Independent baseline data and the complete-source comparison verify that name export does not rewrite
geometry. Application checks require the selected vector/view targets to survive save/fresh reopen.
The existing first-draw snapping guards remain active; native regressions combine renamed vectors with
unsafe geometry to ensure export still refuses before replacing a file. VectorSum #63 remains blocked.

The `plot-owner-name` and `plot-guest-name` modes use the pinned three-mass plot-guests fixture.
They rename mass A (selected plot and plot owner) and mass B (guest in both plots) separately, through
repeated edits, undo/redo and native reopen. A temporary source note keeps old names as ordinary text.
The independent complete-source comparison permits only the selected track's name and its known
root/plot owner/guest references. It also checks null PointMass frame-data extent and every measurement.
Application expectations require the selected root track, selected plot owner, per-track plot owner,
C/B then B guest ordering, variables, line/point visibility and unchanged sparse mass/coordinate values across Java resave.
Consistently wrong selections, reordered/dropped guests or wrong ownership fail even if all snapshots
agree with one another. Native tests additionally cover later collection ordinals, duplicate/missing
names, malformed arrays and variable labels identical to renamed tracks.

Only modern track-view collections with the audited unscaled plot fields are eligible; legacy direct
plot slots, fixed ranges/scaling, dataset settings and dependent tracks remain unsupported for rename
export. These checks preserve saved relationships and scientific data, not rendered plot pixels or
native comparison-plot editing.
The line/point flags are independently transcribed as true/true from both the pinned plot-guests and
dynamic-sine source panels, and checked through the live serializers before/after Java save/reopen.


The `center-member-name` mode renames mass A in the pinned center-of-mass fixture, retaining the
original checksum and using a temporary old-name note. Native history, save/reopen after input-copy
removal, export and reimport follow the other name cases. The independent complete-source comparison
permits only the PointMass name and its CenterOfMass member string to change. The selected center,
other member, 5 kg total and world positions (4,-6) at frame 0 and (6,-8) at frame 2 must survive
actual Tracker load/save/fresh-process reopen. Negative oracles reject stale/dropped members, wrong
weighted positions, missing centers and wrong selections even when all three snapshots agree.
Native regressions cover either constituent, reorder/delete/undo, unknown/missing/duplicate/nested
relationships and atomic refusal for empty/missing member collections. Native dependent evaluation
and broader dependency-name export remain unfinished.


The `attachment-first-name` and `attachment-last-name` modes rename mass A and mass B in the pinned
three-tape attachment fixture. They retain the fixture checksum, add an old-name note only in a
temporary input, and exercise repeated edits/undo/redo/native reopen before standard export. Independent
full-source comparison permits only the PointMass name and matching attachment strings to change;
empty endpoint slots, stored geometry and stored lengths remain intact. Live application checks require
both fully attached and partly attached tapes to follow the same masses with the baseline endpoints,
attached flags and independent lengths at frames 0/1/2 through save and fresh-process reopen. The
selected tape remains both ends. Consistently stale targets, filled empty slots, changed lengths or
endpoints, lost tapes and wrong selection all fail the negative oracle. Native reimport also runs.
Only the documented read-only, fixed, non-calibrating tape subset is eligible for rename export.
