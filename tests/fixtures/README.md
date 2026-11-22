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
They pin how today's native reader opens each version before B2.4a switches it to typed documents.
`baseline.trk` is a synthetic, media-free Tracker document in OSP's layout: two point masses (with saved key frames),
a vector without samples (B1 exports no vector samples beside point masses), coordinate axes, a fixed calibration
and a selected track. `LegacyViewParity.WriteNativeFixtures` wrote every `.motrack` file and golden with B2.1's build (on `main` with
#330, whose native writers B2.1 does not change) on Windows, with `MOTRACK_WRITE_NATIVE_FIXTURES` naming this directory; regenerate rather than edit them,
and review every changed golden.

- `v5-plain.motrack` is MoTrack's version-5 writer output (`trackerReading` 2) after the fixture edits: rename, mark, unmark and weigh a
  point mass, change the mass unit, delete the second point mass, create and mark another, rename the vector,
  calibrate in centimeters and change the clip range and timing. `v5-archive.motrack` (one edited member) and
  `v6-group.motrack` (both edited members) embed `tests/corpus/fixtures/linked-images/multitab-images.trz`.
- `v4-known`, `v3-source`, `v2-source` and `v1` are that writer's output reduced by hand to the schemas
  [docs/native-format.md](../../docs/native-format.md) documents for each version (no container fields, `timeUnit`,
  `movieTimeBase` or `trackerReading`, so they count as reading revision 0; v3 without bindings; v2 without
  identities; v1 without a source or vectors). v1 to v3 also
  hold a vector created with a sample. No earlier MoTrack build was used: building one needs its own SDK pins.
- `v4-stale` stores a baseline with a blank line inside `tracks`, so the bindings that MoTrack's former DOM reader
  recorded no longer match OSP's reading; `v4-refused-baseline` stores one that the current reader refuses.
- `v4-frame-shift`, `v4-duplicate-keys` and `v4-empty-mass-unit` store an unedited import of a baseline variant
  (a clip `frameshift`, a repeated calibration key, an empty saved `mass_unit`) as reading revision 0, the reader
  before the loader-fidelity fixes (#313–#315), read it. Opening migrates `v4-duplicate-keys` (the current reading's
  calibration is adopted); `v4-frame-shift` stays at revision 0 with its associations dropped, because the shift
  would drop stored marks, and the typed document keeps revision 0 without transforming its values again.

Each `*.document.json` is the typed document the fixture converts into (`tests/model/document_dump.hpp`), and each
`*.export.txt` is B1's standard export of it, the exported member for archives, or `refused:` and B1's reason.
