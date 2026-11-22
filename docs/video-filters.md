# Video filters

`motrack_filters` (`src/science/filters/`, namespace `motrack::science`) ports the video filters that Tracker
6.3.5 offers under Video > Filters. It is a Qt-free kernel (slice K6 of the
[implementation order](migration-plan.md#implementation-order-and-exit-gates)); the media pipeline, inspectors and
saved filter XML arrive with slice C6. The reference is OSP commit `d56207b0` (`src/org/opensourcephysics/media/core/`),
the source of the pinned Tracker 6.3.5 JAR, with Tracker `dc064f92`. On 2026-11-21 the OSP `swingJS` tip (`d49c6ecf`)
had no changes to these classes, and the Tracker `SwingJS` tip (`34b1b503`) kept the registry and help page; it only
adds validation to Video > Filters > Paste.

## Registry

`TrackerPanel` registers 14 filters. The [help page](https://github.com/OpenSourcePhysics/tracker/blob/dc064f922fd924826fb42030b9cd5f5bbc720c9f/src/org/opensourcephysics/cabrillo/tracker/resources/help/video_filters.html)
has 16 numbered sections, of which 14 describe filters; that is where earlier plan text counted 16.
`BarrelPincushionFilter` exists in the JAR but is commented out of the registry: Tracker cannot create one, and
`videoFilterKindFromClassName` treats it as an unknown class that C6 must preserve, not filter.

| Filter (class) | Saved properties (defaults) | Computation | State |
| --- | --- | --- | --- |
| Brightness/contrast (`BrightnessFilter`) | `brightness` int (0), `contrast` (50) | `(int)(tan θ (c + sin²θ b − 127) + 127 + cos²θ b)` clamped to 0..255, θ = π contrast / 200 | — |
| Highlight/shadow (`LogFilter`) | `level` int (0), `grayscale` (false) | 256-entry log lookup of the HSB brightness (single-precision `java.awt.Color`), or of (r + g + b) / 3 shown grey | — |
| Deinterlace (`DeinterlaceFilter`) | `field` `even`/`odd` (even) | rows 0, 2, 4… (or 1, 3, 5…) written twice; an odd last row copied | — |
| Strobe (`StrobeFilter`) | `fade` (0), `bright_trails` (false) | faded previous output replaces pixels whose mean it outshines (bright) or undercuts (dark); dark trails fade toward white as the dark ghost does | previous output |
| Ghost / dark ghost (`GhostFilter`, `DarkGhostFilter`) | `fade` (0.05) | remembered grey (int)((1 − f) g) (dark: 255 − (int)((1 − f)(255 − g)); Tracker (int)(255 − (1 − f)(255 − g))) shown where brighter (darker) than the input mean | grey per pixel |
| Grayscale (`GrayScaleFilter`) | `weights` (0.30, 0.59, 0.11) | (int)((wr r + wg g + wb b) / (wr + wg + wb)) with Java int packing | — |
| Negative (`NegativeFilter`) | — | 255 − c | — |
| Baseline (`BaselineFilter`) | `imagepath` | max(c − baseline, 0); the frame size selects the baseline's top-left region | baseline image |
| Sum/average (`SumFilter`) | — (Tracker saves neither setting) | (int)min(Σc × f, 255), f = 1 / count (mean) or the fraction (1) | channel sums |
| Resize (`ResizeFilter`) | `width_factor`, `height_factor` (1) | Java2D nearest-neighbour `ScaledBlit` to (int)(w wf) × (int)(h hf) | — |
| Rotate/reflect (`RotateFilter`) | `rotation` `None`/`CCW`/`CW`/`180`, `reverse` (false) | mirror, then quarter turns | — |
| Perspective (`PerspectiveFilter`) | `in_corners`/`out_corners` key frames, `fixed_in` (false), `fixed_out` (true) | projective map of the output quad onto the input quad, bilinear sampling | — |
| Radial distortion (`RadialDistortionFilter`) | `fixed_radius` (0.75), `input_fov` (π/2), `input_type`/`output_type` (`Rectilinear`) | radial stretch between lens projections, nearest-of-2×2 sampling | — |

Setters enforce what OSP's setters do: fades and the sum fraction become `|value|` (fades at most 1); resize factors
`|f|` within [0.01, 10]; the radial radius [0.2, 1] and field of view [π/18, π − 0.0005], with NaN ignored. Brightness,
contrast, level and weights accept any value as OSP does (its inspectors offer −128..128, 0..100, −100..100 and
0..1). Java's NaN propagation through `Math.min/max`, `(int)` narrowing (NaN → 0, saturation) and wrapping int
packing are reproduced, so out-of-range saved values give Tracker's pixels.

## Images

`FilterImage` owns packed 32-bit pixels in Java's `int` layout (0xAARRGGBB), row-major from the top-left. Filters treat
every input as Tracker's opaque `TYPE_INT_RGB` buffer: they read the colour bytes and write opaque pixels. Tracker's
`VideoAdapter` draws each decoded frame onto its single, uncleared `TYPE_INT_RGB` buffer, and a filter given any other
image type first draws it onto a black one. `compositeSourceOver` reproduces that `drawImage` (Java2D SrcOver with its
`mul8table`, `MUL8(a, src) + MUL8(255 − a, dst)`), so C6 can flatten frames with alpha exactly as Tracker does.

## Stack, state and seeking

`VideoFilterStack` applies enabled filters in order and returns the frame itself when none is enabled. It stands in for
`VideoAdapter`'s buffer: frames of one size count as the same buffer. A filter **re-initializes** (allocating a new
black output with a new identity) when it is first evaluated, when its input is a different buffer or size, and after
the changes OSP implements as `source = null`: enabling, disabling or clearing a ghost, dark ghost or strobe filter,
switching strobe trails, any resize factor change (even to the same value), and a rotation change (not a reverse
change). A new output identity re-initializes every enabled filter after it, so the reset cascades: disabling,
inserting or removing an upstream filter, or changing a resize factor, restarts the ghosts, strobes and sums after it.
A new frame size or `replaceSource()` (for example a different video) restarts everything.

Temporal filters advance on **every evaluation**, not on frame numbers:

- Seeking backward, skipping frames and changing the clip do not reset anything; the next evaluation continues from
  the current state with whatever frame it is given.
- Re-evaluating the same frame advances the state again: a ghost or strobe trail fades one more step. Tracker
  evaluates after each frame change and after any filter property change (each fires `PROPERTY_FILTER_IMAGE`), so
  dragging an unrelated filter's slider fades trails. That makes results depend on repaint cadence
  ([#282](https://github.com/mgradwohl/MoTrack/issues/282)); C6 decides when to evaluate, and the kernel only defines one step.
- `SumFilter` adds the input of the first evaluation after each `stepChanged()` (`addNextImage`). Tracker calls it on
  every step-number change, forward or backward, even while the filter is disabled; repeated evaluations of a frame
  add nothing. Clear restarts the sum from the last input and resets the fraction to 1.
- `BaselineFilter` accepts a baseline at least as large as the current frame (any size before the first frame); a later,
  larger frame drops a baseline that no longer covers it, as Tracker does.

A filter or stack belongs to one worker; parameter changes are marshalled to it, not made while it evaluates.
`VideoFilter::clone()` and `VideoFilterStack::clone()` snapshot every parameter, temporal state, output buffer and
identity, including a pending sum step, so a seek that may be cancelled can be evaluated on a copy and then kept or
discarded without advancing the original (a dropped evaluation must not fade trails or consume `stepChanged()`).

Perspective corners are key-framed by **video frame number** (`VideoPanel.getFrameNumber`, not the clip step), passed
to `apply`. A fixed plane always uses key 0; otherwise a frame uses the latest key at or before it. Tracker's corner
edits store at key 0 when fixed (`setCorners`), fixing a plane keeps the corners shown and drops the other keys, and key
0 cannot be deleted. Without input keys, the first evaluation sets both planes to (w/4, h/4), (3w/4, h/4), (3w/4,
3h/4), (w/4, 3h/4) in integer arithmetic.

## Coordinate transforms

Image coordinates are pixels, x right and y down, with pixel (i, j) covering [i, i + 1) × [j, j + 1), as in Tracker's
image space. Tracker tracks on the filtered image, so positions recorded through a geometric filter are in output
coordinates; the maps below convert them.

| Filter | Input → output | Pixel sampling |
| --- | --- | --- |
| Resize | (x, y) → (wf x, hf y) | Java2D's fixed-point `ScaledBlit`: output column i copies source column `(sxloc + i · sxinc) >> shift` (rows likewise), with `sxloc` recomputed per power-of-two tile for large outputs; within 1e-4 of the source size it is a copy |
| Rotate | reverse first: (x, y) → (w − x, y); then CW (x, y) → (h − y, x), CCW (x, y) → (y, w − x), 180 (x, y) → (w − x, h − y) | exact pixel permutation; quarter turns swap the output size |
| Perspective | `inputToOutput(frame)`; output → input is H = squareToQuad(in) · quadToSquare(out) | output pixel (i, j) samples the input at H(i, j) with the *same* corner-origin convention in both images: col = ⌊x⌋, u = x − col; outside is black and the last row/column are copied |
| Radial distortion | `outputToInput(p, w, h)` = c + s(r)(p − c), c = (w/2, h/2), r = \|p − c\| | output pixel (i, j) samples c + s(r)(p − c) + (0.0001, 0.0001), taking the nearer pixel of the 2 × 2 neighbourhood |

`ProjectiveTransform::map` follows OSP, including leaving a point with a zero homogeneous coordinate in place.
Perspective and radial sampling treat a pixel's top-left corner, not its centre, as its position in both images (OSP's
convention, kept for parity). Under a magnification s the content is therefore offset by ½(1 − 1/s) input pixels
relative to centre-based resampling (¼ px at 2×), and RGB/line sampling (K8) and position mapping in C6 must use the
same corner convention, or convert explicitly, when they relate filtered and unfiltered pixels.
The radial inverse has no closed form and is not provided; RGB/line sampling (K8) should state which image it samples.

## Reference oracle

`tools/reference/FilterOracle.java` drives each OSP filter through a `FilterStack` exactly as Tracker does: one
persistent `TYPE_INT_RGB` buffer, initial parameters through each filter's XML loader, later changes through the setters
its inspector calls, `SumFilter.addNextImage` on step changes and `refreshCorners` on video-frame changes. Frames are
integer-only synthetic patterns (gradients, xorshift noise, checkerboards, moving bright and dark disks, straight
alpha) that `tests/science/video_filter_test.cpp` regenerates identically. 123 cases cover every filter with
representative and edge parameters (degenerate weights and quads, extreme brightness/contrast, out-of-range levels,
1-, 2- and 3-pixel images, 720 × 480 DV and a tiled 3840 × 2052 resize, every rotation with and without reversal,
every projection as input and as output), temporal
sequences with repeats, backward seeks, clears, enable toggles and source changes, and stacks that exercise the reset
cascade. The fixture `tests/science/video-filter-reference.json` stores per-evaluation SHA-256 hashes of the RGB bytes
(every pixel for images of at most 48 pixels; an `apply` may repeat a frame and record only its last evaluation),
`sin`/`cos` bits, log lookup hashes, compositing, the saved properties of each loader and the Java exceptions below.

```sh
python tools/corpus.py filter-oracle --java /path/to/jdk-17/bin/java            # regenerate and diff exactly
python tools/corpus.py filter-oracle --java /path/to/jdk-17/bin/java --record   # replace the fixture for review
```

Every comparison is **exact**. With `FilterArithmetic::Tracker635`, all 123 cases match pixel for pixel. Brightness
truncation exposes the last bit of `sin`/`cos`: HotSpot's x86-64 `Math.sin/cos` returned the correctly rounded value at
all 107 probed contrasts, while the Windows UCRT is one ulp off at 9 of them, so the kernel evaluates `sin`/`cos` in
double-double arithmetic for |θ| ≤ 2²⁰, verified correctly rounded on a sweep (the oracle contrasts and, in review,
139,578 arguments against a 90-digit evaluation). The other transcendental uses keep the platform library: log lookups
stay at least 2.6 × 10⁻⁷ (relative) from a rounding tie for every level −100..100, but the radial margin of
5.8 × 10⁻⁶ px from a pixel or half-pixel edge covers only the fixture's samples; on arbitrary frames a sample can fall
within an ulp of an edge, so a one-pixel cross-platform difference is possible. Only the radial output field of view,
an internal value probed for diagnosis, is compared within 4 ulps. These helpers are private to the filters for now;
[#263](https://github.com/mgradwohl/MoTrack/issues/263) consolidates the kernels' Java-math ports.

## Deliberate differences and upstream findings

- **Perspective bilinear weights ([#219](https://github.com/mgradwohl/MoTrack/issues/219), corrected).** OSP weights the right neighbour by the vertical fraction and the
  lower one by the horizontal fraction. MoTrack interpolates correctly by default; `Tracker635` reproduces OSP.
- **Brightness truncation ([#220](https://github.com/mgradwohl/MoTrack/issues/220), corrected).** Where the mapping
  is an exact integer (c + b at contrast 50, 127 + b at contrast 75 for c = 127 − b/2, 127 + b/2 at contrast 25 for
  c = 127 − b), Java can land a few ulps below it and lose a level: 6,939 results over integer contrasts 0–99,
  brightness −128..128 and channels 0..255, e.g. 64 of 256 values at brightness 128, contrast 50. MoTrack truncates
  `value + 1e-9`, exact on that whole grid; `Tracker635` reproduces the reference.
- **Dark trails ([#281](https://github.com/mgradwohl/MoTrack/issues/281), corrected).** OSP's dark ghost and dark
  strobe fade `(int)(255 − (1 − f)(255 − g))` rounds the remaining darkness up, so trails stop at grey 236 for fade 0.05.
  MoTrack fades to `255 − (int)((1 − f)(255 − g))`, mirroring the bright fade; `Tracker635` reproduces the reference.
- `FilterArithmetic::Tracker635` exists for the oracle tests only; MoTrack has no user-facing Tracker-compatible mode
  (maintainer decision, 2026-11-22).
- **Exceptions ([#222](https://github.com/mgradwohl/MoTrack/issues/222)).** A resize whose output has a zero dimension throws `IllegalArgumentException` in Java; MoTrack
  leaves the image unchanged and the filter uninitialized, as it does for any output above INT_MAX pixels (Java cannot
  allocate one either). Below that limit resize allocates what Tracker would: ×10 of a 4K frame is 3.3 GB, so C6 should
  bound or confirm large factors. Perspective corners without key 0 (edited XML) throw
  `NullPointerException` for earlier frames; MoTrack uses the first key.
- **Saved state ([#221](https://github.com/mgradwohl/MoTrack/issues/221), for C6).** Only the perspective filter saves its enabled state; the other 13 reopen enabled. A
  saved disabled perspective filter makes its loader throw `NullPointerException` (observed headless). `SumFilter` saves
  neither its mean flag nor its fraction. Tracker also saves values the filters adjust themselves: a 720 × 480 input at
  unit factors sets the resize width factor to 0.889, and the radial filter can move its fixed radius to its lower limit.
- **Sum clear** restarts from the last input the filter saw; Java uses the current content of the shared input buffer,
  which is the same frame whenever Clear can be pressed (the button is disabled while the filter is). Keeping that copy
  costs 4 bytes per pixel beside the 12 of the channel sums; it keeps Clear exact even when a step change arrives before
  the next evaluation.

## C6 integration

- Decode each frame and flatten it into a per-video buffer with `compositeSourceOver` (onto the previous frame, as
  `VideoAdapter` does), then evaluate one `VideoFilterStack` per video off the GUI thread with the video frame number.
  Keep the stack on one worker at a time and marshal parameter changes to it. For a seek that may be cancelled,
  evaluate a `clone()` and keep it only if the result is used, because each evaluation advances temporal state.
- Choose evaluation points deliberately: Tracker's repaint-driven evaluation makes trails depend on UI activity. A
  cache of filtered frames is valid only for stacks without temporal filters (`isTemporalVideoFilter`), keyed by
  the frame, the stack's parameters and its revision.
- Load and save Tracker's filter XML through the property names in the table, preserving unknown properties and classes
  (including `BarrelPincushionFilter`) and resolving [#221](https://github.com/mgradwohl/MoTrack/issues/221). Copy/paste
  uses the same XML (the post-6.3.5 tip validates pasted XML first). The perspective plane being edited is shown unfiltered (OSP's `isEnabled` override); the
  perspective track's corner key frames map to `loadKeyFrame`/`setCorners`.
- Feed the filtered image to RGB region and line-profile sampling (K8) and template matching (K7), and map positions with
  the transforms above when a geometric filter changes.
