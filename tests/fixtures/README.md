# Test fixtures

`point-mass.trk`, `image-sequence.trk` and `movie.trk` are small, synthetic Tracker-format projects. They follow
OSP's line layout (one property per line, each object on its own line with its class, array classes), because
Tracker's reader is line-oriented and MoTrack reads them the same way.

`archive.trz` is a ZIP fixture containing `image-sequence.trk`, `frames/frame00.ppm`, and `frames/frame02.ppm`. The saved clip skips the missing middle image.

`vfr.mp4` contains four generated H.264 frames with presentation times of 40, 168, 288, and 368 milliseconds. A B-frame exercises the distinction between decode and presentation order. The AAC stream exercises non-video packet handling. Regenerate it with:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=32x24:rate=25:duration=0.16' \
  -f lavfi -i 'sine=frequency=440:sample_rate=8000:duration=0.28' \
  -vf "setpts='if(lt(N,2),N,N*2)'" -fps_mode vfr -c:v libx264 \
  -x264-params 'bframes=1:b-adapt=0:scenecut=0' -c:a aac -use_editlist 0 \
  -video_track_timescale 1000 vfr.mp4
```

`osp/` holds small synthetic documents for the line-oriented OSP XMLControl reader (see
[docs/osp-xmlcontrol.md](../../docs/osp-xmlcontrol.md)). Each isolates one reader behavior; files named
`fail-*` must be rejected. Their expected trees were recorded from Tracker 6.3.5's `XMLControlElement` and
are re-checked by `python tools/corpus.py xml-oracle`, which also derives CRLF, CR and byte-order-mark
variants. `unicode.xml` is written by byte value so that U+2028, U+2029 and U+0085 stay literal. `osp/generated/`
holds documents written by OSP's own writer (`tools/reference/XmlControlDump.java --generate`); regenerate them
rather than editing them by hand.

`native/` holds the native migration fixtures of slice B2.1 ([typed document design](../../docs/tracker-document.md#5-oracles-and-tests)).
They pin how today's native reader opens each version before B2.4a switches it to typed documents. `baseline.trk`
is a synthetic, media-free Tracker document in OSP's layout: two point masses (with saved key frames), a vector
without samples (B1 exports no vector samples beside point masses), coordinate axes, a fixed calibration and a
selected track. `LegacyViewParity.WriteNativeFixtures` wrote every `.motrack` file and golden with B2.1's build (on
`main` with #330) on Windows, with `MOTRACK_WRITE_NATIVE_FIXTURES` naming this directory; regenerate rather than edit
them, and review every changed golden. (Since #315 changed the reading, the generator's `.motrack` output differs from
the committed files, which keep B2.1's bytes: write it to a scratch directory and copy only the goldens.)

- `v5-plain.motrack` is B2.1's version-5 writer's output (`trackerReading` 2) after the fixture edits: rename,
  mark, unmark and weigh a point mass, change the mass unit, delete the second point mass, create and mark another,
  rename the vector, calibrate in centimeters and change the clip range and timing. `v5-archive.motrack` (one edited
  member) and `v6-group.motrack` (both edited members) embed
  `tests/corpus/fixtures/linked-images/multitab-images.trz`.
- The files named after a build hold the same writer's output reduced to exactly the keys that build's writer
  wrote (`git show <build>:src/workspace_document.cpp`): v1 as first written by `2dd1d25` (no mass unit, track
  masses, source frame count, orientation or vectors), then `f740830` (mass unit, masses, source frame count,
  orientation, archive), `e9599bf` (movie alignment) and `cd710be` (vectors); v2 `ecd43dd` (the preserved source);
  v3 `69be876` (identities); v4 `5367eaf` (bindings); and v5 `0847a4d` (the container fields, before `timeUnit`,
  `movieTimeBase` and `trackerReading`, so it counts as reading revision 0, like every earlier version). The
  versions without associations also hold a vector created with a sample, from `cd710be` on. They were not written
  by those builds themselves, which need their own SDK pins.
- `v4-stale` stores a baseline with a blank line inside `tracks`, so the bindings that MoTrack's former DOM reader
  recorded no longer match OSP's reading; `v4-refused-baseline` stores one that the current reader refuses.
  `v5-dom-unknown-track` stores one where only the coordinate axes, last after a blank line, fall outside OSP's
  `tracks`: B1 keeps the bindings and refuses only export, and the typed document keeps every stored value with
  stale associations.
- `v4-frame-shift`, `v4-duplicate-keys` and `v4-empty-mass-unit` store an unedited import of a baseline variant
  (a clip `frameshift`, a repeated calibration key, an empty saved `mass_unit`) as reading revision 0, the reader
  before the loader-fidelity fixes (#313–#315), read it. Opening migrates `v4-duplicate-keys` (the current reading's
  calibration is adopted); `v4-frame-shift` stays at revision 0 with its associations dropped, because the shift
  would drop stored marks, and the typed document keeps revision 0 without transforming its values again. Since #315,
  `v4-empty-mass-unit` migrates its stored `kg` to the empty label.
- `v5-empty-mass-unit`, `v5-edited-empty-mass-unit`, `v5-unloadable-track-frames` and `v5-classless-coords` were
  written by the reader before #315's revision 3 (`main` at `2525d90`, `trackerReading` 2) with a scratch driver that
  imports one Tracker file, optionally sets the mass unit, and calls `writeWorkspaceDocument`. Their sources are
  `tests/corpus/fixtures/empty-units.trk` (unedited, and with the mass unit set to `g`), `unloadable-track-frames.trk`
  and `classless-coords.trk` below. The first migrates to the empty label; the second stays outdated with its `g`; the
  last two hold sources the current reader refuses and open native-only with every stored value.

Each `*.document.json` is the typed document the fixture converts into (`tests/model/document_dump.hpp`), with the
baseline's SHA-256, and each `*.export.txt` is B1's standard export of it, the exported member for archives, or
`refused:` and B1's reason.

`unloadable-track-frames.trk` and `classless-coords.trk` are `native/baseline.trk` with one edit each (#315): mass A's
`framedata` element `[1]` renamed `[5]`, so its indices `[0] [5] [3]` leave `[5]` beyond the length the last element
sets, and the calibration's `framedata` without its `class` attribute. Tracker 6.3.5's application loader opened the
first without any of its tracks and could not open the second (`TrackerSessionReference` under a private Xvfb display,
checked for #315); MoTrack refuses both.
