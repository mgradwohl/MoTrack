# Loaded-state oracle dumps (O3/O4)

This guide defines the canonical dump of a project as **Tracker 6.3.5 loads it**, the differ that compares two
dumps, and the known-defect registry. It implements wave-0 slice A3 of the
[full-port plan](migration-plan.md): oracle O3 (MoTrack's typed load equals Tracker's loaded `TrackerPanel`)
and O4 (every track's data columns at every frame match Tracker's `DatasetManager` within declared
tolerances). The [corpus guide](../tests/corpus/README.md) remains the authority for pinned inputs.

Tracker is used here as a **loader** oracle. A dump records what Tracker's live objects hold after the normal
application load; it is never derived from a Tracker resave, and a dump that disagrees with an independent
expectation is not accepted as compatibility evidence (see [known defects](#known-defect-registry)).

## Producing Tracker dumps

[`TrackerStateDump.java`](../tools/reference/TrackerStateDump.java) opens one `.trk`, or one member of a
`.trz`, through `TrackerIO.openURL` into a `TFrame` on the private Xvfb display that the session harness
already uses. It then runs the same draw-driven evaluation as
[`TrackerSessionReference`](../tools/reference/TrackerSessionReference.java): show the frame, seek the last
clip step, paint, and wait until tape attachments, model boosters and system members resolve, centers of
mass acquire their members, and particle models have steps for every clip frame. Customized tables are
visited through Tracker's normal selection path and their saved selection is restored. Unlike the resave
baseline, that visit is bounded to one second per table: Tracker 6.3.5 can leave columns unpopulated (#64),
and the dump records that loaded state rather than timing out. A table that Tracker does not show, or whose
owner or selection is missing, is dumped without a visit. Reports label this preparation
`expand_visit_custom_tables_bounded_restore_selection`.

Each case runs in a fresh JVM with a private home and OSP cache, a 256 MiB heap, a 30-second internal
deadline and the runner's 45-second subprocess limit. Archives are copied to a private directory first,
because Tracker may extract media beside the container. For an archive, Tracker loads every member as a tab;
the dump selects the member named by the manifest, by the tab title Tracker gives it (the member basename
without a leading `<archive basename>_`). Other tabs are not dumped; each is covered by its own corpus case.

### Why not a headless loader

Pinned Tracker has a frameless loader: `TrackerIO.openURL(path, null, …)` constructs `new TrackerPanel(false)`
and runs synchronously. It loads the panel, clip, coordinate system and tracks, and vector data columns work.
It is not used, because it does not fully load data:

- `PointMass.getData` (and every point-mass-derived class, such as centers of mass and models) throws a
  `NullPointerException` in `TTrack.panel`, which resolves panel IDs through a required `TFrame`. A `TFrame`
  is a `JFrame` and cannot exist headless. The panel saver also fails, since it saves every track.
- A failed refresh leaves partially computed datasets behind: a second request returns rows whose velocities
  are `NaN` artifacts rather than Tracker results. A dump built that way would be misleading.
- Particle models evaluate only from their draw path, which also needs the frame.

The frame-requiring path (#203) and OSP's headless video initialization (#280) are recorded as reference-harness
findings in
[Tracker issue triage](upstream-issues.md#defects-discovered-by-the-reference-harness). Dumps are therefore
generated only on Linux with Xvfb (`state-oracle`); the committed dumps let every platform compare without
Java.

## Schema (version 1)

A dump is one JSON object `{"schema_version": 1, "producer": {...}, "panel": {...}}`.

`producer` is `{"kind": "tracker", "version": "6.3.5", "loading": "tracker_session", "view_preparation": ...}`
for Tracker, or `{"kind": "motrack", "supported_classes": [...]}` for MoTrack.

`panel` contains, in this order for Tracker dumps:

| Key | Content |
| --- | --- |
| `units` | `length`, `mass`, `time` labels as Tracker reports them (including its default length unit) and `angles_in_radians`. |
| `selected_track` | Name of the selected track, or `null`. |
| `properties` | The `TrackerPanel` saver tree (below), without `tracks` (dumped separately), the running `semantic_version`, window geometry (`magnification`, `center_x/y`, `dividers`, `track_control_x/y`, `info_x/y`, `toolbar`) and view properties (dumped as `views`). It includes `videoclip`, `clipcontrol` and `coords` as saved. |
| `clip` | `first_frame`, `step_size`, `step_count`, `end_frame`, `frame_count`, `frame_shift`, `extra_frames`, `start_time_ms`, `frame_duration_ms` (mean), `rate`, `looping`, and `steps`: one `[step, original frame, time in ms]` row per clip step from `VideoPlayer.getStepTime`. |
| `video` | `null`, or the video `class`, `frame_count`, `start_frame`, `end_frame`, `frame_times_ms` for every frame and the filter class names. Pixels are not hashed here; the session baseline already does. |
| `coords` | The active `ImageCoordSystem`: `class`, `fixed_origin/angle/scale`, `locked`, `key_frames`, and one row per original frame `0 … length-1`: `[frame, origin x, origin y, angle, scale x, scale y, [m00, m10, m01, m11, m02, m12]]`. Origin is in image pixels (y down), scales in pixels per world unit, the angle in radians from `getAngle` (`atan2(sin, cos)`, in (-π, π]), and the last element is the `getToWorldTransform` matrix in `AffineTransform.getMatrix` order. |
| `tracks` | Every track of `getTracks()` in Tracker's order (below). |
| `views` | Tables and plots of the four view choosers as their savers report them: per table the selected track, `track_columns`, `column_formats` and `dataset_indices`; per plot the selected track and each owner's plots (axes, lines/points, guests). Owner and format rows are sorted because their storage order is not semantic. |

Each track is `{"class", "name", "properties", "steps", "data"}`:

- `properties` is the track's own saver tree, written as Tracker writes it into a project file (trailing
  `null` array elements omitted, as `TrackerPanel`'s saver does). This makes every class, including ones
  without special handling, contribute all of its loader-visible properties.
- `steps` lists every non-null step with its original `frame` and image-pixel points. Known step types name
  their scientific points: `position` (point masses and their subclasses), `tail`, `tip` and `components`
  (vectors and sums), `end1`/`end2` (tapes). Other types list all `points` in Tracker's order. Coordinate
  axes have no persistent steps: their handles are transient screen state.
- `data` is `null` or the track's `DatasetManager`: `columns` (`name` and Tracker's `dimensions`, such as
  `L`, `T`, `L/T`, `L/TT`, `A`, `I` for counts and `P` for pixels), `constants` (for example the mass `m`)
  and `rows`. Tracker builds every dataset of a track over the same independent variable, so the first
  column is that variable (`t`, in seconds) and each row holds one value per column. The dumper fails rather
  than pairing misaligned datasets. Rows exist only where Tracker reports them, so a missing frame is
  visible, not filled.

The saver tree maps an OSP `XMLControl` to `{"@class": "...", property: value, ...}`. Integers, doubles,
booleans and strings keep their JSON types; CDATA wrappers are removed. Arrays become
`{"@array": "<Java class>", "items": [...]}` with absent elements as `null` (compact `{a,b}` primitive arrays
are expanded), and collections become `{"@collection": "<Java class>", "items": [...]}`.

JSON cannot carry IEEE specials, so `NaN`, `Infinity` and `-Infinity` are written as those strings; they are
never replaced by zero or `null`. Absolute locations of the case's input directory and private OSP cache are
replaced by `$INPUT_DIR` and `$OSP_CACHE`.

Committed dumps live in [`tests/corpus/state`](../tests/corpus/state), one file per case, in a canonical
layout with one track, step, data row and coordinate frame per line, so a review diff points at the changed
row.

### MoTrack dumps

`motrack_corpus_probe --state input.trk output.json` writes what MoTrack's importer models today, in the same
schema with `kind: "motrack"` and `supported_classes` (point mass and vector). It reads MoTrack's own values
rather than recomputing them: data rows are the frames of `measurementPlot`'s position series, `t` is that
series' time, the derivative columns are `measurementPlot`'s quantities, and steps, step times and the end frame
come from the production clip helpers (`clipStepOfFrame`, `clipFrameOfStep`, `clipFrameTimeMs`). Tracks follow
their original source order. Point masses carry `mass`, their marks as steps and the computed columns (`t`, `x`,
`y`, `v_{x}`, `v_{y}`, `v`, `a_{x}`, `a_{y}`, `p_{x}`, `p_{y}`, `K`, `step`, `frame`, `pixel_{x}`, `pixel_{y}`);
vectors carry their samples as steps. Clip step rows exist for untimed clips too, with a `null` (unknown) time.
`coords.key_frames` lists the calibration keys unless origin, angle and scale are all fixed, matching
`ImageCoordSystem`.

What MoTrack does not report is a gap, never a silent pass (see [the gap baseline](#gap-baseline)). Time and
derivative columns are absent for untimed clips, world quantities for uncalibrated projects, and the length unit
when the file has none. Tracker materializes a 100 ms frame duration and the `m` label when a media-free file
saves neither; MoTrack does not yet (#214). The XML-only probe does not decode media, so movie cases are untimed
here even though MoTrack's decoder times them in the application.

**What O4 covers today:** positions (`x`, `y`), pixels, steps and frames, compared in 43 point-mass cases, plus
vector tails, tips and components in 36 cases. **No finite derivative value is compared in any case.** Tracker
has finite velocities in 17 cases (BallToss, the 14 keyed movies, `attachments` and `model-dependencies`), all of
which are untimed in the probe; the remaining timed cases have no three consecutive marks.

## Comparing dumps

`StateComparison` in [`tools/corpus.py`](../tools/corpus.py) compares two validated dumps:

- **Exact:** track classes, names and order; step and row frame numbers; each clip step's frame; column names
  and dimensions; strings, booleans, `null`, the specials above, and integer-valued properties.
- **Tolerance:** other numbers, from the manifest's `state_tolerance` table. The default (absolute `1e-10`,
  relative `1e-12`) equals the coordinate tolerance of the existing corpus checks. Data columns use their
  dimensions: first-derivative quantities (`L/T`, `ML/T`, `A/T`, and `MLL/TT` kinetic energy) allow absolute and
  relative `1e-9`; second derivatives (`L/TT`, `A/TT`) allow absolute `1e-7`. These bounds cover rounding only:
  central differences divide rounded position differences by Δt and Δt², so differentiating world positions
  instead of image displacements differs by rounding amplified by those factors. They do not cover an algorithm
  difference: MoTrack's acceleration uses a three-point formula where Tracker uses a five-point fit (#207), which
  these tolerances must not hide. The derivative tolerances are **not exercised yet** (see above). These bounds are
  for this oracle, not a universal rule for future fits, tracking or solvers.
- Data rows are matched by their `frame` column and clip steps by their index, so missing, extra and remapped
  rows or steps are reported individually.

In **subset mode** (a MoTrack dump), only what MoTrack reports is compared with Tracker. Everything MoTrack does
report must match, a supported Tracker track must appear, coordinates must be a contiguous range from frame 0,
and a column or key that Tracker does not report is a failure. Reports are bounded and name the full path, for
example `panel.tracks[name=mass A].data.rows[frame=3].v_{x}: expected 1.5, got 1.6`.

### Gap baseline

Everything Tracker reports that MoTrack does not (an unmodeled class, column, property or section, a timing or
calibration value, coordinate frames beyond the clip) is a gap. [`tests/corpus/state-gaps.json`](../tests/corpus/state-gaps.json)
commits every case's gaps, each with its reason: `unmodeled-class`, `unmodeled-field`, `unmodeled-column`,
`probe-media-timing` (the XML-only probe cannot time a movie), `default-timing` and `default-length-unit`
(Tracker's defaults, #214), `uncalibrated`, `coords-range` and `known-defect-unmodeled`. `state-check` is a
ratchet: a **new** gap fails, so a value MoTrack stops reporting (lost calibration or timing, missing steps or
mass, fewer coordinate frames) cannot pass unnoticed; a **closed** gap or a changed reason fails until the
baseline is re-recorded with `state-check --record` and reviewed. Gaps shared by every case are listed once.

## Known-defect registry

[`tests/corpus/known-defects.json`](../tests/corpus/known-defects.json) records where pinned Tracker is wrong,
so that MoTrack matches an independent value instead of inheriting the defect. Each entry names:

- one corpus `case`; `object` `{"track": name}` (exactly one track) or `null` for the panel; and a `path`
  inside it. Paths are dot-separated keys with `[index]` and `[field=value]` selectors, for example
  `data.rows[frame=0]` or `clip.steps[11]`. Wildcards are rejected.
- the MoTrack `issue` (and upstream report, when one exists) and a summary.
- `expected`: the independent value MoTrack must match.
- `tracker`: the deviating value Tracker shows, or `"@absent"` when Tracker omits the value.

**Independence.** Every `expected` value is recomputed by `derive_known_defect_values` from the saved project XML,
the media's own container tables (MP4/QuickTime `stts`/`stsz`) and cited pinned defaults, never from Tracker's
output: table columns, selections and formats from the saved views and per-track number formats plus
`TTrack.getDefaultFormatPatterns` (time `0.000`, step and frame `0`); vector rows from saved tails/components and
the fixed calibration; vector sums from their named members; BallToss's frame count, times, positions and
Tracker's derivative definitions (spill-1 central velocity, five-point acceleration). `state-oracle` and
`state-check` fail when a registered value differs from its derivation, and each derivation must also reproduce
Tracker on frames where Tracker is not defective (25 such checks), which validates the formulas themselves.
Angles are omitted where the direction is undefined (a zero vector) or on `atan2`'s branch cut.

**Matching.** On the Tracker side, the listed part must still match (other keys are compared by the strict
dump-against-dump check). On the MoTrack side, an entry overrides only what it lists: where Tracker has an object
at the path, unlisted keys stay compared with Tracker; where Tracker has no value, every key MoTrack reports must
be listed. Listed keys MoTrack does not report, or reports as `null`, are gaps. An entry whose `expected` matches
its `tracker` value is rejected, as are duplicates and entries naming more than one case; no entry waives a
property across files.

A **whole-file** entry has `object` and `path` `null`, `tracker: {"load_failure": [signatures]}` and
`expected: {"state_of": "<case>", "project_sha256": "<hash>"}`: Tracker cannot load the file, its failure text
must contain every signature, and MoTrack's load must equal Tracker's state for another case whose project bytes
hash to the same value. `archived-avi` holds a member byte-identical to `linked-movie.trk` (#140).

`state-oracle` requires every entry to be **reproduced**. An entry fails as `STALE` when Tracker now reports the
independent value (or loads a whole-file case), and as `CHANGED` when the deviation, or a load failure's
signature, changed; re-pinning the reference therefore surfaces every entry for review.

The entries cover the unaccepted cases in the corpus guide: vector-sum members, steps and data (#63), the
omitted negative-time vector rows (#146), the AVI archive load failure (#140), and the custom table columns,
formats and selections Tracker loses (#64). The first dumps also found that Tracker's Xuggle engine indexes 11 of
the 13 samples of BallToss's MP4 (#209): `XuggleVideo.finalizeLoading` counts `packetCount - firstDisplayPacket`
and never drains the decoder at the end of the stream, so an H.264 file with a reorder delay of N loses its last
N frames. The clip is cut to frames 0–10, the ball's frames 11–12 lose their rows, and the velocity at frame 10
and accelerations at frames 9–10 become NaN.

## Commands and CI

```sh
python tools/corpus.py state-oracle --java /path/to/jdk-17/bin/java            # Linux with Xvfb
python tools/corpus.py state-oracle --java /path/to/jdk-17/bin/java --record   # replace dumps for review
python tools/corpus.py state-check --probe build/dev/motrack_corpus_probe       # any platform, no Java
python tools/corpus.py state-check --probe build/dev/motrack_corpus_probe --record   # re-record the gaps
python tools/corpus.py state-diff expected.json actual.json
```

`state-oracle` verifies the pinned inputs, compiles the reference helpers with the JDK's `javac`, dumps every
corpus case, compares each dump with the committed one, then checks the registry and its derivations. The dumper
logs how each customized-table visit ended (`populated`, `bounded` or `skipped`); a case whose visit ends on the
40-poll bound is dumped again with a 400-poll (10 s) bound and must give the same state. On failure it leaves the
regenerated dumps in the cache's `state-generated` directory. `--record` replaces the committed dumps only when
every case loaded and every registry entry reproduced; inspect and explain each changed row before committing.

CI runs `state-oracle` in its own Linux job and uploads the regenerated dumps when it fails; the final coverage
job waits for it. `state-check` runs after the corpus check on Windows, macOS and Linux.

### Provenance of committed dumps

Most dumps derive from MoTrack's own Java-authored fixtures. The `ball-toss`, `dynamic-sine` and `protractor`
dumps contain data derived from upstream Tracker examples (`BallToss.trz`, `asinewave.trk`, `protractor.trk`),
which the manifest otherwise fetches unchanged rather than vendoring; they are distributed under the upstream
license linked in the manifest's `upstream.license_notice` (GPL).

## Limits

- Only the selected member of an archive is dumped; views, notes, drawings and Data Tool tabs other than the
  listed view properties are not yet part of the schema (wave 2, C7).
- Particle models are evaluated by drawing up to the last clip step; the dump does not exercise playback.
- Video pixels, filters' settings beyond the saver tree, and rendering are outside this oracle.
- The MoTrack side covers the importer's current model only. The wave-1 typed model is expected to produce
  saver trees and every class, at which point the gaps close and O3/O4 become required gates.
