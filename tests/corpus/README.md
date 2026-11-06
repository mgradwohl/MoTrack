# Tracker compatibility corpus

This is the first reproducible seed for [full-port phase 1](../../docs/migration-plan.md#implementation-order-and-exit-gates).
It compares MoTrack's importer and coordinate calculations with snapshots produced by the actual OSP
XML loader and `ImageCoordSystem` implementation bundled in the official **Tracker 6.3.5** JAR.

The original report names its verification level `xml_and_coordinate_api`. A separate session probe
uses Tracker's actual application/async-loader lifecycle to open the three experiments without media,
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
loaded Java vectors and native reimport. Names, clip/calibration and track-membership edits, media/archive
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

| Case | Producing version | Checked imported content | Unsupported content retained in the source fixture |
| --- | --- | --- | --- |
| Ball toss | 5.1.5 | Eleven marks at frames 2–12, mass, clip bounds/time origin and world coordinates | Tape measure, display settings and archive media |
| Protractor | 6.0.7 | Two named point masses, their marks/calibration; mass-value and mark edits through the standard round-trip runner | Protractor and its saved settings |
| Independent vectors (synthetic) | 6.3.5 | Two independent vectors, sparse frames, a zero vector, nonuniform scale; actual Java session resave | Track colors/footprints and coordinate axes |
| Dynamic sine | 5.1.2 | Clip and calibration; dynamic particle is reported as unsupported | Model parameters, functions and plot settings |

The probe reads the `.trk` member selected by the manifest from BallToss's `.trz`. It does not exercise
MoTrack's archive extractor, so the existing archive/workspace tests remain necessary. No archive member
paths are extracted by the corpus script. Add genuine multi-tab archives, upstream vector examples/dependent vectors,
current 6.3.5 features, varying calibration, media/timing cases and failure cases next.

The executable's SHA-256 is verified and its runtime version reports 6.3.5. The June 22, 2026 upstream
commit is recorded as a **source candidate**, not a verified binary-to-source mapping. Full source and
dependency provenance, the complete requirement inventory and media/archived-session loading remain phase-1 work.
Initial snapshots were generated on Windows with Eclipse Temurin 17.0.21+9; CI repeats the reference
calculation using its Java 17 JDK on Linux. No Java runtime is added to the MoTrack application.

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
