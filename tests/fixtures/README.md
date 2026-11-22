# Test fixtures

`point-mass.trk` and `image-sequence.trk` are small, synthetic Tracker-format projects.

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
