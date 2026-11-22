# Image sampling kernels (RGB regions and line profiles)

Slice K8 of [Wave 0](https://github.com/mgradwohl/MoTrack/issues/170) ports the measurements behind
Tracker's RGB-region and line-profile tracks (requirement R13) as the Qt-free library
`motrack_sampling` (`src/science/sampling/`, namespace `motrack::science`). The kernels take one
frame and the geometry of one step and return Tracker's data columns. Tracks, key frames, the
filter stack, calibration, views and persistence arrive with slice C6; [integration](#integration-in-slice-c6)
lists what C6 must supply.

The reference is the pinned Tracker 6.3.5 JAR (source candidate
[`dc064f92`](https://github.com/OpenSourcePhysics/tracker/tree/dc064f922fd924826fb42030b9cd5f5bbc720c9f)):
[`RGBStep`](https://github.com/OpenSourcePhysics/tracker/blob/dc064f922fd924826fb42030b9cd5f5bbc720c9f/src/org/opensourcephysics/cabrillo/tracker/RGBStep.java),
[`RGBRegion`](https://github.com/OpenSourcePhysics/tracker/blob/dc064f922fd924826fb42030b9cd5f5bbc720c9f/src/org/opensourcephysics/cabrillo/tracker/RGBRegion.java),
[`LineProfileStep`](https://github.com/OpenSourcePhysics/tracker/blob/dc064f922fd924826fb42030b9cd5f5bbc720c9f/src/org/opensourcephysics/cabrillo/tracker/LineProfileStep.java)
and [`LineProfile`](https://github.com/OpenSourcePhysics/tracker/blob/dc064f922fd924826fb42030b9cd5f5bbc720c9f/src/org/opensourcephysics/cabrillo/tracker/LineProfile.java),
together with the OpenJDK 17 `java.awt.geom` classes that decide pixel inclusion. The measurement
code is unchanged on Tracker's `SwingJS` tip; only polygon editing changed there (U12, below).

## Frames and coordinates

`ArgbImage` owns 32-bit `0xAARRGGBB` words with a row stride. The kernels read only the red,
green and blue bytes, as Tracker reads its `TYPE_INT_RGB` rasters, so alpha never affects a result.
All geometry is in **image pixels**: the origin is the top-left corner, x grows right and y grows
down, and pixel (x, y) covers [x, x + 1) × [y, y + 1) with its centre at (x + 0.5, y + 0.5). These
are the coordinates Tracker saves for RGB-region positions and line-profile ends. Outputs are in
the same space; channel values are 8-bit (0–255) and counts are pixels. World units come from the
coordinate system in slice C1/C6; the kernels never apply calibration.

`trackerLuma(R, G, B)` is `RGBRegion.getLuma`, the CCIR 601 weighting `0.299 R + 0.587 G + 0.114 B`.
The library and its tests are compiled with `-ffp-contract=off` so that sums of products round
twice, as in Java.

## RGB regions

### Shapes

| Shape (`shape_type`) | Geometry | Tracker limits |
| --- | --- | --- |
| Ellipse (0) | Integer width and height | New regions are 20 × 20. Edits clamp each edge to [0, max], where max is 200 until a video is open and then one less than the smaller image dimension (`rgbMaxEdgeLength`, `clampRgbEdgeLength`). Loaded sizes are not clamped. |
| Rectangle (1) | Integer width and height | As for ellipses. |
| Polygon (2) | Vertices relative to the step position | Marking starts at the position, so vertex 0 is normally (0, 0). Closed when it has at least three vertices and the last is exactly (0, 0); an open polygon is still measured. |

A missing `shape_type` loads as an ellipse; any other unknown value is measured as a rectangle, because
`RGBStep.createRGBShape` falls through to its rectangle branch (`rgbShapeKindFromSaved`).

Ellipses and rectangles span `[-(size / 2), size - size / 2]` around the position using integer
division, so an odd size reaches **half a pixel further right or down**: a 5-pixel rectangle at
x = 20 covers [18, 23). Tracker draws the region at the same place, so the measured and displayed
regions agree, but the reported x, y is then not the region's centroid. MoTrack preserves this
placement so that saved Tracker experiments measure the same pixels ([#236](https://github.com/mgradwohl/MoTrack/issues/236)).

`RgbPolygon` models Tracker's polygon editing: `append`, `close` (`setClosed(true)`), `reopen`
(the "edit polygon" button, which Tracker offers only for a closed polygon), and handles `i` that
address vertex `i + 1` (vertex 0 moves with the position). `removeHandleVertex` uses the bounds
check of the Tracker tip ([`50d39015`](https://github.com/OpenSourcePhysics/tracker/commit/50d39015),
upstream-delta U12): 6.3.5 accepted the handle one past the last vertex, cleared the polygon's path
and then threw, leaving an empty in-memory shape. The other U12 changes (handle selection after
adding or removing a vertex, restoring the graphics composite, toolbar layout) are interface
behavior for slice C6.

### Inclusion rule

A pixel is measured when its **centre** is inside the region outline (`RgbRegionOutline`), which
is the Java2D shape `RGBStep.getRGBShape` builds:

- The ellipse is Java2D's four-cubic Bezier approximation of the conic (radial error below 0.03 %
  of the radius), not the exact ellipse. Containment follows `Curve.pointCrossingsForCubic`,
  subdividing up to 52 levels.
- Containment is `Path2D.contains` with the default non-zero winding rule: both lobes of a
  self-intersecting bow tie, the centre of a pentagram and a doubly wound polygon are inside.
- The ray-crossing test counts an edge for y in [min, max), and a point is outside when it is on or
  right of the crossing. Axis-aligned edges therefore include the left and top edge and exclude the
  right and bottom edge: a rectangle covers the centres in [left, right) × [top, bottom).
- Paths are implicitly closed. A polygon with fewer than two vertices, collinear vertices, a zero
  width or height, or a negative size contains no pixels.

The port repeats every Java operation, including the translation to the position and the
floor/ceiling of `Shape.getBounds()`, so the included pixel set is identical to Tracker's.

### Image edges

Tracker reads the region's whole pixel bounding rectangle with one `Raster.getDataElements` call and
treats the resulting exception as "no data". A region therefore has **no data when any part of its
bounding rectangle is outside the image**, even if no included pixel centre is: an ellipse whose
left edge is at x = −0.1 has bounds starting at −1 and is rejected. MoTrack reports this as
`OutsideImage` and keeps Tracker's rule; it does not clip regions to the image ([#236](https://github.com/mgradwohl/MoTrack/issues/236)).

### Statistics

With the included pixels' integer channel sums `ΣR` and `ΣR²` (likewise G, B) and count `n`:

| Column | Value | Units |
| --- | --- | --- |
| `R`, `G`, `B` | `ΣR / n` | 8-bit channel value |
| `luma` | `trackerLuma(R, G, B)` of the means | 8-bit channel value |
| `pixels` | `n` | pixels |
| `Rsd`, `Gsd`, `Bsd` | `sqrt((ΣR² − ΣR · R) / (n − 1))`, NaN when n = 1 | 8-bit channel value |

Tracker's track adds `t`, `x`, `y`, `step` and `frame`; x and y are the step position (image pixels
before calibration). `measureRgbRegion` returns `Measured`, `NoPixels` (nothing included),
`OutsideImage` (above) or `InvalidGeometry` (NaN or infinite position or vertex, which Tracker never
saves); Tracker shows no data for every status other than `Measured`. The result also carries the
exact sums for later aggregation.

## Line profiles

### Algorithm selection

`lineProfileMode(alwaysHorizontal, axisAngle)` repeats `LineProfileStep.getProfileData`: a profile
whose orientation is "horizontal", or any profile while |sin(x-axis angle)| < 10⁻⁵, uses the
horizontal algorithm. Otherwise Tracker keeps the line parallel to the rotated x-axis and uses the
tilted algorithm. The spread is clamped to [0, 100] when edited or loaded (`clampLineProfileSpread`); the
profile is 2 × spread + 1 pixels wide.

### Horizontal profiles

The end x coordinates are truncated toward zero (Java `(int)`), the smaller is raised to 0 and the
larger lowered to the image width, and the columns in between are sampled, **excluding the column of
the larger end**. The row is `(int) end0.y`; end1's y is ignored. Each sample averages rows
y − spread to y + spread with equal weights and is centred on the pixel centre (x + 0.5, y + 0.5).
Because of truncation toward zero, a line up to one pixel above the image samples row 0.
A band of rows that leaves the image gives no data (`OutsideImage`); ends that truncate to the same
column give `TooShort`.

### Tilted profiles

A probe one pixel long along the line and 2 × spread + 1 pixels wide starts at end0. While either
start corner is outside the image, the start advances by one pixel and the profile loses a sample;
likewise the far corners retreat from end1. The profile then has `floor(length)` minus those steps
samples. Sample i is the probe between start + i·u and start + (i + 1)·u (corners accumulated by
repeated addition, as in Tracker), reported at the centre of the probe corners' bounding box.
Each sample's channel values are the means of the pixels weighted by their **exact overlap area**
with the probe (Sutherland–Hodgman clipping per pixel). Lines shorter than one pixel are
`TooShort`; profiles with no probe inside the image are `OutsideImage`.

Tracker computes the overlaps with its own grid-intersection walk, which silently returns wrong
weights for every direction with sin θ · cos θ < 0, for downward vertical lines (θ = +90°, although
the floating-point product is +6 × 10⁻¹⁷; the upward line is exact) and for some first probes that
start on a pixel corner, and throws on other grid-aligned probes. MoTrack uses the exact weights for
every direction (see the defects below).

Each probe clips only the pixel columns under its row band (plus a one-column margin), so a
4K-wide profile with spread 100 costs a fraction of a second. The call has no internal
cancellation point; C6 runs it off the GUI thread like other per-frame measurements.

### Columns

| Column | Value | Units |
| --- | --- | --- |
| `n` | sample index from 0 | — |
| `x`, `y` | sample centre | image pixels (world units after C1/C6 calibration) |
| `R`, `G`, `B`, `luma` | weighted channel means and their luma | 8-bit channel value |
| `pixels` | 2 × spread + 1 | pixels |

## Reference defects and deliberate differences

Each defect was reproduced with the pinned JAR by the [kernel oracle](#kernel-oracle); the fixture
records Tracker's actual output, and the C++ test checks MoTrack against independent expectations
instead. None was found in OpenSourcePhysics/tracker or osp issues (searched 2026-11-21: RGB,
region, luma, standard deviation, profile, line profile); none has been filed upstream.

| Tracker 6.3.5 behavior | MoTrack behavior | Issue |
| --- | --- | --- |
| RGB-region sums use Java `int`s, which wrap above 2³¹ − 1; regions may be up to one pixel less than the smaller image dimension. Sums of squares wrap from 33,026 pixels of value 255: uniform white regions show NaN deviations, and a 1000 × 1000 region with 9.2 % white pixels shows 33.47 instead of 73.6. Channel sums wrap from 8,421,505 such pixels (reachable on 8K video, up to 4319 × 4319), making the means wrong too. | Exact 64-bit sums; `trackerSumOverflow` marks results that Tracker shows differently. | [#232](https://github.com/mgradwohl/MoTrack/issues/232) |
| Tilted line profiles silently weight pixels incorrectly (1) whenever sin θ · cos θ < 0, which includes every x-axis rotated counter-clockwise by less than 90° (in a 4,000-line scan every such line with non-integer start points was wrong by more than 10⁻³, by up to 138 of 255); (2) for downward vertical lines, i.e. an x-axis rotated 90° clockwise as used for vertical motion (every sample of the recorded cases is wrong, by up to 26); and (3) for some first probes that start on a pixel corner in the other quadrants (15 of 1,939 random lines in an independent review, by up to 18). | Exact overlap weights in every direction. | [#233](https://github.com/mgradwohl/MoTrack/issues/233) |
| Other tilted probes whose edges meet grid vertices (vertical lines on pixel boundaries, integer start points, 45° lines between integer points) throw `NullPointerException` or `ArrayIndexOutOfBoundsException`, or print a stack trace and show no data. | Measured like any other probe. | [#234](https://github.com/mgradwohl/MoTrack/issues/234) |
| A horizontal profile whose every pixel is pure black is treated as an unloaded image ("LineProfileStep image failed") and shows no data. | Reports the zero values; `trackerOmitsBlackStrip` marks them. | [#235](https://github.com/mgradwohl/MoTrack/issues/235) |

Preserved Tracker behavior, recorded in [#236](https://github.com/mgradwohl/MoTrack/issues/236):
the half-pixel offset of odd-size regions, the whole-bounding-rectangle edge rule, horizontal
truncation toward zero, and the Bezier ellipse. MoTrack-only rejections of non-finite geometry,
spreads outside [0, 100] and tilted lines longer than 2²⁴ pixels replace Java's cast or unbounded loop
behavior for inputs Tracker never saves. A tilted probe corner that rounding places a fraction of a
pixel past the edge contributes no weight there instead of failing.

## Kernel oracle

`tools/reference/SamplingOracle.java` runs the actual `RGBStep.getRGBData` and
`LineProfileStep.getProfileData` from the pinned JAR headless. The panel is a `TrackerPanel`
subclass allocated without its GUI constructor that reports frame 0, a synthetic `Video` whose
`getImage()` returns the frame, and a coordinate system whose x-axis angle selects the profile
algorithm while its image-to-world transform is the identity, so positions stay in image pixels.
Tracker's own `getRGBShape` and `getBounds()` also provide each region's bounds and an FNV-1a
hash of its included pixels, so inclusion is compared even where Tracker returns no data.

Frames are generated from integer formulas in both languages and checked by FNV-1a hash: SplitMix64
noise (96 × 72 and 160 × 120), a linear gradient, a checkerboard, anti-aliased edges at x = 20.3 and
y = 30.6, a hard vertical edge, a black band, uniform white and bright noise (260 × 260) and a sparse
1008 × 1008 frame for the overflow cases. The 55 region and 49 profile cases cover integer,
half-pixel and subpixel positions, odd and even sizes, 0, 1, 2 and 3-pixel shapes, negative sizes,
edge contact, concave, self-intersecting, multiply wound, collinear, single-vertex and offset
polygons, truncation, clamping and band limits, the algorithm switch, all four direction quadrants,
upward and downward vertical lines, inward stepping at both ends, wide probes and grid-aligned probes.
The channel-sum overflow needs an 8K-sized region and is covered by a unit test instead.

The committed fixture is `tests/science/sampling-reference.json`:

```sh
python tools/corpus.py sampling-oracle --java /path/to/jdk-17/bin/java          # regenerate and diff
python tools/corpus.py sampling-oracle --java /path/to/jdk-17/bin/java --record # replace for review
```

Agreement classes, used by both `tools/corpus.py` and `tests/science/sampling_reference_test.cpp`:

- **Exact** for region inclusion, bounds and statistics and for horizontal profiles: integer sums
  and IEEE divisions, square roots and the luma products, which round identically everywhere.
- **Absolute 10⁻⁹** (pixels and 8-bit channel values) for tilted profiles. Their direction comes
  from `atan2`, `cos` and `sin`, which Java and each C++ library compute only to about one ulp, and
  MoTrack's exact clipping sums overlaps in a different order from Tracker's walk. Observed
  differences stay below 10⁻¹⁰ (largest on the noise frame, where neighbouring pixels differ by up
  to 255); Tracker's real weight errors are at least 10⁻³.

Independent expectations keep a consistently wrong port from passing: analytic means and
deviations on the gradient and checkerboard, rectangle pixel counts from the half-open rule,
ellipse membership against the exact conic outside the Bezier error band, exact integer deviations
for the overflow cases (and Tracker's value reproduced from 32-bit wrapped sums), and, for every
tilted sample, overlap areas integrated slice by slice (`tests/science/sampling_expectations.hpp`),
plus a 512 × 512 point lattice inside each probe across a hard edge.

## Integration in slice C6

- **Which frame.** Tracker measures `Video.getImage()`: the frame after the enabled filter stack, at
  the video's own resolution (not the display zoom), and invalidates region data whenever the image or
  filters change. C6 passes the filtered frame from its K6 pipeline; for image sequences whose images
  differ in size, the frame of the current image (Tracker's `ImageVideo.getRGBSize` checks). The
  `ArgbImage` type is minimal and will be unified with the K6 image type.
- **Key frames.** An RGB region measures step 0's position when "fixed position" is set, otherwise the
  latest position key frame at or before the frame; shapes follow the latest shape key frame (or step
  0 when "fixed shape"). Polygons with a fixed shape share one polygon. A line profile is likewise
  fixed or key-framed. C6 resolves these before calling the kernels.
- **Orientation.** Tracker rotates line-profile ends about their centre to keep the line horizontal or
  parallel to the x-axis; C6 owns that constraint and the switch via `lineProfileMode`.
- **Calibration.** x and y are image pixels; C6 converts them with the frame's coordinate system, and
  derives `t`, `step` and `frame` from the clip.
- **Saved data.** Tracker saves region RGB data (`rgb`: R, G, B, pixels, Rsd, Gsd, Bsd per frame) and
  displays the saved values until they are invalidated; profiles are never saved. Saved region sizes
  are doubles that Tracker truncates with `(int)`. C6 must decide
  when recomputed MoTrack values replace saved ones, and can use `trackerSumOverflow` to explain a
  difference from a saved NaN.
- **Threading.** The kernels are pure functions of their arguments with no shared state and may run on
  worker threads. They have no cancellation points (a large region or a wide 4K profile takes up to
  a few hundred milliseconds per frame), so C6 keeps them off the GUI thread and owns cancellation
  between frames and stale-result rejection.
