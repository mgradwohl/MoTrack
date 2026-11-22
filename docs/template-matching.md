# Template matching

`motrack_matching` (`src/science/matching/`, namespace `motrack::science`) ports the template matcher
behind Tracker's AutoTracker. It is a Qt-free kernel (slice K7 of the
[implementation order](migration-plan.md#implementation-order-and-exit-gates)). The AutoTracker
workflow, wizard and track integration arrive with slice C6, which will replace the basic
`TemplateTracker` in `motrack_media`.

The reference is OSP `org.opensourcephysics.media.core.TemplateMatcher` at commit `d56207b0`, the source
of the pinned Tracker 6.3.5 JAR. It uses OSP's `KnownPolynomial`/`PolynomialLeastSquareFit`/
`LUPDecomposition` for its peak fit and Java2D for template blending. AutoTracker is Tracker
`dc064f92`. On 2026-11-21 these classes were unchanged on the OSP `swingJS` tip (`d49c6ecf`; only
`KnownPolynomial.hashCode` was added). The Tracker `SwingJS` tip (`34b1b503`) changed only a SwingJS
annotation and `OSPRuntime.isMac` in `AutoTracker`. Java2D arithmetic follows OpenJDK 17
(`java.awt.geom` and libawt's `AlphaMath.c`/`AlphaMacros.h`).

## How AutoTracker uses the matcher

C6 will reproduce this protocol on top of the kernel:

- **Key frames and masks.** Each key frame owns one `TemplateMatcher`. The mask is an
  `Ellipse2D.Double` or `Rectangle2D.Double` centered on the mask center. `refreshKeyFrame` sets its
  frame from the center and corner: the half extent is the corner distance (at least 4) times the
  corner direction's cosine and sine, each at least 1 pixel. The default corner offset is (9, 9), so the
  default ellipse is about 18 pixels across. The wizard's mask size is 8 to 99.
  `createTemplateMatcher` crops `mask.getBounds()` from the frame into a black `TYPE_INT_RGB` image and
  translates the mask to that origin.
- **Target offset.** The marked point is the mask center plus the key frame's target offset. A match
  at template origin `p` has center `p + maskCenter - bounds.origin`.
- **Search area.** It is `Rectangle2D.setFrameFromCenter(searchCenter, searchCorner).getBounds()`, moved
  (not shrunk) into the frame by `moveRectIntoImage`. The default corner offset is (40, 40), an
  80 x 80 area. *Look-ahead* moves it to a prediction from the last four steps:
  - the previous position;
  - a velocity estimate `2 x1 - x0` when it is consistent;
  - an acceleration estimate `3 x2 - 3 x1 + x0` when the jerk is small.
  
  The prediction is clamped to the image. *Follow* keeps the previous position, and *fixed* keeps the
  area where it is.
- **Each frame.** `findMatchTarget` calls `setTemplate(frameData.getTemplateToMatch())`, and
  `getMatchLocation(frame, searchRect, null, 0, 0, 0)` or, in 1D mode, `getSearchPoints` and then
  `getMatchLocation` along the coordinate system's x-axis. The returned score is then classified:
  - **None:** `p == null` or `score < possibleMatch` (1).
  - **Possible:** shown with an icon but not marked; NaN scores land here because they compare false.
  - **Good:** an infinite score or `score >= goodMatch` (the "accept" value, default 4, wizard 1 to
    10). Good matches are marked with `autoMarkAt`. In 1D mode the match center is first moved onto
    the axis.
  
  After a good match, AutoTracker evolves the template (below).
- **Evolution and tethering.** `buildEvolvedTemplateImage` crops the mask-bounds rectangle at
  `(int) Math.round(p)` and calls `buildTemplate(crop, evolveAlpha, tetherAlpha)`. The wizard's
  percentages map to alpha as `(int) (2.55 x percent)`: 20% evolution is 51, 5% tether is 12, and 100%
  is 254. A frame first searched uses the matcher's latest template. Per-frame `workingPixels` all
  alias the matcher's single working buffer, so re-searching an earlier frame continues from the
  latest working state. This was found by reading the source and is still to be confirmed in C6.
- **Stopping.** The *always stop*, *stop on no match* and *never stop* policies decide whether a
  non-good frame pauses, is skipped as a possible match, or is passed over. *Fast* mode skips
  repainting and the per-frame delay without changing the analysis (MoTrack #90).

## Algorithm

| Step | Tracker behavior (ported exactly) |
| --- | --- |
| Pixel inclusion | A pixel of the mask bounds is in the template only when all four corners are inside the translated mask, by `Path2D` containment: the ellipse is Java's four-cubic Bezier approximation, and boundaries are half-open, so whole-pixel rectangles lose their last row and column. Excluded pixels get alpha 0 and are trimmed from the edges (`trimLeft`, `trimTop`). |
| Score | For each test position, the sum over included pixels of the squared red, green and blue differences (alpha ignored) is computed exactly. The score is `mean/minimum - 1` over all searched positions. The first strict minimum in column-major order wins. A perfect match scores +inf; an area where every position matches perfectly scores 0/0 = NaN. |
| Search area | Positions are those of the trimmed template's pixel `(w/2, h/2)`. The area's origin is clamped to `[w/2, frame - (w/2 + w%2)]` (moved, not clipped), and its size is cut at the right and bottom so that one more column and row remain for the peak fit. With no room left the result is null, with width and height NaN. |
| Subpixel peak | `KnownPolynomial.fitData({-1, 0, 1}, ...)` through the best position and its horizontal and vertical neighbors (a least-squares parabola solved by OSP's LUP decomposition, ported operation for operation). The offset is `-c1/(2 c2)`, the width is `sqrt(2 c0/c2)` averaged over both axes, and a NaN offset becomes 0. A width above 1 divides the score. |
| Evolution | The crop, at alpha `evolve/255`, and then the key-frame original, at alpha `tether/255`, are drawn onto the working image with `AlphaComposite.SRC_OVER`. This uses libawt's IntArgb SrcOver MaskBlit: `extraA = (int)(alpha*255.0+0.5)` from the float alpha, then 8-bit `mul8table`/`div8table` arithmetic. Excluded pixels are cleared and the template is recut. Alphas are recorded unclamped and clamped to 0 to 255; with both 0 nothing changes. |
| 1D points | `getSearchPoints` intersects the axis with the area's edges. The axis passes through `(x0, y0)` with slope `-tan angle`; it is treated as horizontal below `1e-10` and vertical above `1e10`. The function collects the grid-line crossings in a `TreeMap` (Java `Double` order, where -0.0 is distinct) and returns the truncated midpoints of consecutive crossings, which are the edge-adjacent pixels of the axis in order of increasing x. |

## Parameters as Tracker exposes them

| Wizard control | Kernel input |
| --- | --- |
| Mask shape (circle/square), width and height | `MatchMask{Ellipse or Rectangle, frame}`; `MatchMask::fromCenter` mirrors `setFrameFromCenter`. |
| Search area | The `PixelRect` passed to `findMatch`, before Tracker's adjustment (the result reports the adjusted area). |
| Evolution rate, tether (percent) | `alphaFromPercent`, then `TemplateMatcher::evolve(frame, evolutionOrigin(result), evolve, tether)`. |
| Accept (good-match score) | `classifyMatch(result, goodMatch, possibleMatch)`; defaults 4 and 1 (`TRACKER_DEFAULT_GOOD_MATCH`, `TRACKER_POSSIBLE_MATCH`). |
| 1D (track along x-axis) | `findMatchAlongLine` with `SearchLine{origin x, origin y, axis angle}` for the frame, then `projectOntoLine` on the match center. |

Results are `MatchResult{status, searchArea, pixel, x, y, peakHeight, peakWidth, atSearchEdge}`. `x` and
`y` are Tracker's returned point, the subpixel mask-bounds origin. Searches are pure functions of
their arguments, safe to run concurrently, and poll `MatchOptions::cancelled` before each searched
column (2D) or block of 64 line pixels. `TemplateMatcher` is a value type: C6 can snapshot a template
for a worker and evolve it on the owning thread. Frames are owned `ArgbImage` buffers of
`0xAARRGGBB` pixels, so the adapter that converts decoded, filtered frames is the only Qt/FFmpeg
boundary.

## Deliberate differences and reference defects

Interior matches, every template, every evolution step and every horizontal 1D search equal Tracker's
bit for bit. MoTrack departs only where the reference is demonstrably wrong:

| Case | Tracker 6.3.5 | MoTrack | Tracking |
| --- | --- | --- | --- |
| Best position on the search-area boundary | Neighbors come from a copy of the area that has no column on the left or row on top. The left neighbor in the first column wraps to the previous row's end, and the upper neighbor in the first row is NaN. A concave or flat fit is extrapolated without bound: the oracle shows a match moved 9.65 px *away* from a target beyond the right edge. | All four neighbors are scored in the frame (NaN only outside it). The vertex is used only when the parabola opens upward and lies within one pixel; otherwise the axis is not refined. `atSearchEdge` reports such matches. | [MoTrack #216](https://github.com/mgradwohl/MoTrack/issues/216) |
| 1D subpixel offset | Moved by `offset x (cos, sin)`. Along the axis that is `sign(cos) x cos 2 angle`: reversed for axes pointing left or steeper than 45 degrees, zero at 45 degrees, and NaN is kept. Index units are also uneven along tilted axes. | The along-axis component of the 2D peak fit. It is identical for horizontal axes. On tilted and reversed axes, errors on the line are 0.01 to 0.05 px, against Tracker's 0.13 to 0.69 px. | [MoTrack #217](https://github.com/mgradwohl/MoTrack/issues/217) |
| 1D near image edges | Points are computed for the unadjusted rectangle and read against the adjusted one, so a shifted parallel line is searched. | Points are computed for the searched area. | #217 |
| 1D axis misses the area | `getSearchPoints` returns null, and `getMatchLocation` silently searches the whole area in 2D. | `NoSearchPoints`. | #217 |
| 1D with no point inside the area | `ArrayIndexOutOfBoundsException`; an empty rectangle throws `NegativeArraySizeException`. | `NoSearchPoints`; an empty point list. | #217 |
| Match preview | `refreshMatchImage` cuts the preview at `match + (1, 1)` instead of the trim offset. Whole-pixel rectangle masks (trim 0) preview pixels one row and column off. This is display only. | Not built by the kernel. C6 must cut previews at the trim offset. | [MoTrack #218](https://github.com/mgradwohl/MoTrack/issues/218) (with #71 / Tracker #24) |
| Mask with no included pixel | A 1 x 1 transparent template that scores NaN everywhere. AutoTracker's minimum mask size makes this unreachable. | `TemplateMatcher::create` refuses it, as it refuses masks above 2^22 bounds pixels. | Defensive. |

None of these has an upstream report (searched 2026-11-21). They are recorded in the
[reference-defect table](upstream-issues.md#defects-discovered-by-the-reference-harness). Two
quirks are kept for compatibility because neither misplaces a match. At 100%, evolution blends 254/255
of the new crop. NaN scores, from uniform areas, count as possible matches.

## Verification

`tools/reference/MatchOracle.java` runs the pinned `TemplateMatcher` headless on deterministic synthetic
scenes. Each scene is an opaque textured, optionally spinning, growing and recoloring disc with a known
subpixel trajectory over a static cluttered background, with seeded noise and an optional occluder.
Rendering uses only integer arithmetic and IEEE `+ - * /`, `floor` and `abs`. The C++ tests re-render
every frame and check its hash, so the generator itself is cross-checked. `python tools/corpus.py
match-oracle --java ...` regenerates and diffs `tests/science/match-reference.json` (`--record` replaces
it for review). CI runs the check on Linux. Comparisons are exact, except for 1D positions, which
depend on `Math.tan/cos/sin` and are allowed 1e-9 px.

| Fixture section | Cases | Result |
| --- | --- | --- |
| `composite` | Every alpha 1 to 255 over all 65,536 channel pairs, opaque and translucent | Bit-identical to Java2D. |
| `masks` | 14 ellipse/rectangle frames: whole-pixel, fractional, AutoTracker default, minimum, tiny, empty, elongated, negative, far, maximum | Containment on a quarter-pixel grid, trims, templates and working images bit-identical; the empty mask is a documented difference. |
| `evolution` | Ten `buildTemplate` steps: defaults, extremes, both-zero no-op, negative and clamped alphas, crops beyond the frame | Templates, working images and alphas bit-identical. |
| `search_points` | 18 axes: horizontal, reversed, vertical, near-vertical, diagonal through corners, missing, negative rectangles, empty rectangles | Identical; the empty rectangle is a documented difference. |
| `matches` | Exact, outside, partly outside, oversized, uniform, template larger than frame, 1-pixel template, six subpixel shifts, six beyond-the-edge targets, even rectangle, evolved, low contrast, six 1D axes, 1D edge and failure cases | Non-defect cases are bit-identical. Fixed-template shifts are within 0.15 px of the truth. |
| `sequences` | 12 tracked runs: clean, noisy, occluded, low contrast, evolving appearance with and without evolution, fast, rectangle mask, leaving the frame, tilted and horizontal 1D | Every step equals Java except the documented boundary fits and the tilted 1D run. Every accepted match is within the declared tolerance of the true path. |

Independent accuracy (worst accepted match against the true center, from the C++ test output):

| Sequence | Good matches | Worst error | Declared |
| --- | --- | --- | --- |
| clean, fixed template | 30/30 | 0.09 px | 0.15 |
| clean, 20% evolution / 5% tether | 30/30 | 0.33 px | 0.40 |
| noisy (+/-12 levels) | 30/30 | 0.34 px | 0.40 |
| occlusion (frames 6 to 9 covered) | 20/24 | 0.18 px | 0.30 |
| low contrast | 19/25 | 0.33 px | 0.50 |
| evolving appearance, with evolution / fixed template | 17/30 / 7/30 | 0.61 / 0.06 px | 0.75 / 0.15 |
| fast (2.65, 1.9 px/frame) | 24/24 | 0.52 px | 0.60 |
| tilted 1D axis (MoTrack / Tracker) | 30/30 | 0.08 / 0.30 px | 0.30 |

Evolution drifts by design: each step blends a crop taken at the *rounded* match position. That is why
evolving runs are less accurate than fixed templates on stable targets, and more robust when the
appearance changes (17 against 7 accepted frames). The 2D unit tests cover the API contracts:
construction and refusal, evolution arithmetic, search-area adjustment, NaN and infinite scores,
classification, cancellation, concurrent searches, line points and projection.

## Remaining work for C6

The kernel covers a single search and its template state. C6 builds AutoTracker on it:
- key frames;
- target offsets;
- search-area policies with look-ahead;
- stop, skip and retry;
- live and fast modes with identical results (#90);
- previews at subpixel positions (#71, #218);
- undoable batch marking;
- running searches on filtered frames (K6) off the GUI thread, with stale-result rejection.

C6 must also confirm or correct the working-pixel aliasing noted above against the actual application.
