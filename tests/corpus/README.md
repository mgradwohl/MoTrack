# Tracker compatibility corpus

This is the first reproducible seed for [full-port phase 1](../../docs/migration-plan.md#implementation-order-and-exit-gates).
It compares MoTrack's importer and coordinate calculations with snapshots produced by the actual OSP
XML loader and `ImageCoordSystem` implementation bundled in the official **Tracker 6.3.5** JAR.

The original report names its verification level `xml_and_coordinate_api`. A separate session probe
uses Tracker's actual application/async-loader lifecycle to open the two experiments without media,
save through Tracker's normal saver and reopen in a fresh application frame. Its report is labeled
`tracker_session_resave`. It checks loaded track classes/names, mass values, step points, clip bounds,
units and coordinate probes before/after Java's own resave, and compares available point-mass/coordinate
data with the independent XML baseline.

Neither check proves MoTrack `.trk`/`.trz` export, complete model/function parameter fidelity, rendering,
media decoding, cross-platform Java UI behavior or the required paired-file round trip. A successful
parser or Java-only resave check must never be reported as a complete MoTrack round trip.

## Sources and cases

[manifest.json](manifest.json) pins each upstream URL, byte count and SHA-256, the upstream revision,
recorded producing Tracker version, relevant requirement IDs and known features. Examples and the
reference JAR are downloaded unchanged into ignored `.dev-tools/tracker-reference`; they are not
vendored. The manifest links the upstream license and records the unaudited media-redistribution limit.

| Case | Producing version | Checked imported content | Unsupported content retained in the source fixture |
| --- | --- | --- | --- |
| Ball toss | 5.1.5 | Eleven marks at frames 2–12, mass, clip bounds/time origin and world coordinates | Tape measure, display settings and archive media |
| Protractor | 6.0.7 | Two named point masses, their marks and calibration | Protractor and its saved settings |
| Dynamic sine | 5.1.2 | Clip and calibration; dynamic particle is reported as unsupported | Model parameters, functions and plot settings |

The probe reads the `.trk` member selected by the manifest from BallToss's `.trz`. It does not exercise
MoTrack's archive extractor, so the existing archive/workspace tests remain necessary. No archive member
paths are extracted by the corpus script. Add genuine multi-tab archives, independent/dependent vectors,
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
xvfb-run -a python tools/corpus.py session --java /path/to/jdk-17/bin/java
python -m unittest discover -s tests -p test_corpus.py
```

On Windows, use `build/coverage/motrack_corpus_probe.exe` and the path to `java.exe`. Python 3.10 or newer
is supported. The reference command uses Java source-file mode and therefore needs a JDK, not just a
JRE. The first `fetch` needs network access; later checks are offline against verified cached files.
`--cache PATH` changes the cache location. The reference JVM uses a private home inside that cache for
any preferences or helper files. Downloads have time/size limits, subprocesses have a timeout
and the Java heap is bounded. A corrupt existing cache entry is reported and preserved, never silently
replaced or treated as success.

The `session` command currently requires Linux with a virtual display and compiles its helpers with
the adjacent JDK `javac`. It refuses to run on the user's Windows desktop. CI's Ubuntu runner supplies
Xvfb and Java 17. Each case gets a fresh private Java home and output directory, a 30-second loader
deadline inside the 45-second subprocess limit, and a 256 MiB heap. Temporary compiled classes,
preferences and Java-resaved experiments are removed on success or failure; successful comparison
reports remain under the cache's `reports` directory. MoTrack itself does not use Java at runtime.

CI compares native imports on Windows, macOS and Linux, and reruns both Java probes on Linux. These
checks follow ordinary coverage collection; they do not inflate the production coverage claim.

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

Later stages must add a distinct full-session test: open/edit/save in MoTrack, close/reopen the native
companion, export the standard file, open and verify it in Tracker, resave there and reimport in MoTrack.
Preservation of unsupported objects and other archive tabs is mandatory in that test.
