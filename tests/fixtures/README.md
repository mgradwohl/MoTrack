# Test fixtures

`point-mass.trk` and `image-sequence.trk` are small, synthetic Tracker-format projects.

`vfr.mp4` contains four generated H.264 frames with presentation times of 40, 168, 288, and 368 milliseconds. A B-frame exercises the distinction between decode and presentation order. The AAC stream exercises non-video packet handling. Regenerate it with:

```sh
ffmpeg -f lavfi -i 'testsrc2=size=32x24:rate=25:duration=0.16' \
  -f lavfi -i 'sine=frequency=440:sample_rate=8000:duration=0.28' \
  -vf "setpts='if(lt(N,2),N,N*2)'" -fps_mode vfr -c:v libx264 \
  -x264-params 'bframes=1:b-adapt=0:scenecut=0' -c:a aac -use_editlist 0 \
  -video_track_timescale 1000 vfr.mp4
```
