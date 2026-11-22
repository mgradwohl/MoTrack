# Camera capture

**File → Capture from camera…** records a clip from a camera and opens it as a new movie
experiment (requirement R28 in the [migration plan](migration-plan.md#requirements-and-status-matrix)).
The [README](../README.md#record-a-clip-from-a-camera) describes the workflow. This page records how
capture is built, how its timing is kept, what each platform needs, how it differs from Tracker, and
how to check it with a real camera.

## Design

| Part | Responsibility |
| --- | --- |
| `CameraSource` (`src/camera_source.hpp`) | Camera list, formats, permission, start/stop. `QtCameraSource` uses Qt Multimedia's `QCamera`, `QMediaCaptureSession` and `QVideoSink`; tests use a fake that delivers frames from another thread. |
| `CapturePipeline` (`src/capture_pipeline.hpp`) | One worker thread converts the newest frame for preview and, while recording, writes every frame. Frames arrive from the platform's capture thread into a locked queue; signals reach the window on the GUI thread. |
| `CaptureEncoder` (`src/capture_encoder.hpp`) | Writes a QuickTime (`.mov`) file of Motion JPEG frames with MoTrack's own FFmpeg, one presentation time per frame. |
| `CaptureDialog` (`src/capture_dialog.hpp`) | Camera and format choice, preview, Record/Stop, elapsed time and frame count, save, discard and cancel. |
| `WorkspaceWindow` | Owns the capture window and opens the saved clip with `openMovie`, the same path as **Open video**. |

The recording goes to a private temporary folder. It is offered to the workspace only after
**Save and open…** has moved it to a permanent file, so a saved project never refers to a file that
MoTrack later deletes. A save to another volume copies on a worker thread and replaces the target only
when the copy is complete. A camera that stops during a recording ends it: the frames so far are kept
to save or discard. Discarding, cancelling, a failed recording, closing the capture window,
closing the workspace or destroying either window stops the recording, closes the movie and removes
the temporary folder. Destruction joins the worker before the folder is removed, so the file is
closed first even on Windows. No camera work blocks the GUI thread: frame conversion, encoding and the
copy run on workers, and the window waits for their signals.

Capture opens no network listener and uses no browser.

## Timing

Each frame keeps the presentation time the platform's camera layer gives it
(`QVideoFrame::startTime`): the Media Foundation sample time on Windows, the AVFoundation sample
buffer time on macOS, and the V4L2 buffer time stamp on Linux, in microseconds. The first recorded
frame is time zero; later frames keep their offsets from it, stored at the QuickTime track's 1/90000 s
timescale (an offset is rounded to the nearest 11.1 µs). The movie index then reports those offsets as
the experiment's frame times, so an uneven camera rate stays uneven in plots, tables and derivatives.
MoTrack never substitutes a nominal frame rate.

- A camera that supplies no frame times is timed by when each frame reached MoTrack (a monotonic
  clock), and the summary says so.
- A frame that does not advance past the previous one at the track timescale, or that lacks the clock
  the recording started with, is skipped and counted.
- When frames arrive faster than they can be written, the arriving frame is dropped and counted. The
  remaining frames keep their real times; the summary reports the number dropped.

Frame dimensions are those of the first frame. Rotation and mirroring flags on camera frames are not
applied; the clip is stored as the camera delivers it.

## Recording format

Motion JPEG stores every frame independently, so each frame decodes on its own and seeking is exact.
MoTrack writes it with a fixed JPEG quantizer of 4, in full-range BT.601 4:2:0, which keeps flat colours
within a few levels of the source (`CaptureEncoder` tests). Expect a few megabytes per second at 720p30
and several times that at 1080p.

MoTrack records with its own FFmpeg rather than `QMediaRecorder`. In Qt 6.8.3, Qt's FFmpeg recorder
passes no bit rate or quantizer to FFmpeg's built-in encoders (Motion JPEG, MPEG-4 Part 2), so they run
at FFmpeg's 200 kb/s default, and its H.264 path depends on a hardware encoder on Linux (VA-API or
NVENC). Writing the movie in MoTrack keeps quality, container and timing the same on every platform and
lets tests record synthetic frames without a camera.

## Platform requirements

Qt 6.8 Multimedia uses its FFmpeg media backend by default on Windows, macOS and Linux
([`qplatformmediaintegration.cpp`](https://github.com/qt/qtmultimedia/blob/v6.8.3/src/multimedia/platform/qplatformmediaintegration.cpp)).
That backend reads cameras through each platform's native interface. MoTrack uses only the camera and
frames from it.

| | Windows x64 | macOS | Linux x64 |
| --- | --- | --- | --- |
| Camera interface | Media Foundation | AVFoundation (the macOS FFmpeg backend's camera) | V4L2 (`/dev/video*`; the user needs access, usually the `video` group) |
| Qt Multimedia | `qtmultimedia` add-on, installed by `tools/dev_sdk.py` (archives pinned in `tools/qt-archives.sha256`) and CI | same | same |
| Qt's bundled FFmpeg | FFmpeg 7.1 DLLs (`avcodec-61.dll` …) in Qt's `bin` | FFmpeg 7.1 dylibs in Qt's `lib` | FFmpeg 7.1 (`libavcodec.so.61` …) in Qt's `lib`, with Qt's VA-API stub libraries |
| MoTrack's FFmpeg (decoding, recording) | vcpkg FFmpeg 9 DLLs (`avcodec-63.dll` …) | vcpkg FFmpeg 9, linked statically | vcpkg FFmpeg 9 linked statically, or a system FFmpeg 6.1+ |
| System requirements | Camera allowed under Settings → Privacy & security → Camera | Camera permission (MoTrack asks on first use) | `libpulse0` (Qt Multimedia links PulseAudio's client library); the camera backend loads `libEGL`, `libX11`, `libXext` and `libXrandr` |
| Staged app | `windeployqt` (through Qt's deployment script) copies the multimedia plugin and Qt's FFmpeg DLLs | `macdeployqt` copies `QtMultimedia.framework`, the multimedia plugins and Qt's FFmpeg dylibs; `Info.plist` has `NSCameraUsageDescription` | Qt's deployment script copies the multimedia plugin and its libraries into `lib` |

The two FFmpeg builds do not collide: their Windows DLL names differ, Qt's Linux libraries use
versioned symbols (`LIBAVCODEC_61`), MoTrack's static copies are not exported from its executable, and
macOS resolves each library by its own install name. Qt's FFmpeg build is LGPL; release notices for
bundled libraries belong to the deferred release work (R27).

macOS refuses camera access to an app without a usage string. The staged bundle uses
[`cmake/MoTrackInfo.plist.in`](../cmake/MoTrackInfo.plist.in); developer builds, which are not bundles,
embed the same key in the executable's `__TEXT,__info_plist` section. Windows and Linux have no
permission prompt in Qt; a camera blocked by the system fails to start, and the window shows Qt's
message with a hint about privacy settings.

## Differences from Tracker

Tracker after 6.3.5 captures through **Video → Capture…** ([upstream delta U4](upstream-delta.md)):
it starts an HTTP server on `127.0.0.1` with an ephemeral port, opens the system browser at
`/capture`, and receives either an MP4 recorded by the browser (imported with its encoded timing) or a
stack of JPEG frames (written to temporary files and imported at the frame rate the page sends), into
the current tab. MoTrack deliberately differs:

| Tracker | MoTrack |
| --- | --- |
| Localhost HTTP server and a browser page | A native Qt window; no listener, browser or JavaScript |
| Video → Capture… loads into the current tab | File → Capture from camera… opens a new experiment, next to the other ways to open media; the current experiment is unchanged |
| Browser MP4 timing, or a JPEG stack at a requested frame rate | The camera's own frame times, never a requested rate |
| Captures stay in the system temporary folder (`capture-<id>.mp4` or numbered JPEG files) and are not removed | The clip is saved to a file the user chooses before it opens; temporary files are removed |
| Camera choice and format belong to the browser | Camera and reported resolution/frame-rate choice in the window |

Reading the upstream source (`TrackerCameraServer.java`, `Tracker.importVideoCapture` and
`importMP4Capture` at `34b1b503`) also shows defects that MoTrack's design avoids, recorded in
[#262](https://github.com/mgradwohl/MoTrack/issues/262) as candidates for upstream reports (not filed):

1. `/upload` accepts unauthenticated POSTs with `Access-Control-Allow-Origin: *`, so any web page the
   user visits can find the port and inject a video or image stack into Tracker.
2. The image-stack path is built as `java.io.tmpdir + "capture-…"` without a separator; on Linux
   (`/tmp`) the frames are written as `/tmpcapture-…jpg`, outside the temporary folder.
3. The frame count and each frame length are read from the request and allocated without a limit.
4. The `fps` query value is parsed without validation; zero, negative or non-finite rates reach
   `loadVideo`.
5. Captured files are never deleted, and after the first capture the server keeps listening until
   Tracker's window closes.

## Checking with a real camera

Automated tests use a fake camera, so CI needs no hardware. Before marking R28 complete on a platform,
check a staged build with a real camera:

1. Run `build/stage/installed/bin/motrack` (`motrack.app` on macOS, `bin/motrack.exe` on Windows) and
   choose **File → Capture from camera…**. On macOS, accept the permission prompt; on Linux, confirm the
   user can open `/dev/video0`.
2. Confirm the camera list and the reported formats, then pick a format with a known rate. The preview
   should follow movement smoothly while the window stays responsive.
3. Record about five seconds of a ball toss or a phone stopwatch, press **Stop**, and read the summary:
   frame count, duration, average rate and any dropped frames.
4. Choose **Save and open…**, keep the `.mov` name and confirm a new experiment opens. Step through
   frames and compare the frame times with the stopwatch; an uneven rate should appear as uneven times.
5. Repeat while unplugging the camera mid-recording, while another application holds the camera, with
   camera access turned off in the system privacy settings, and with no camera attached. Each should
   show a message and leave the workspace unchanged, with no `motrack-capture-*` folder left in the
   temporary directory.
6. Cancel and close the window during a recording; confirm the temporary folder is removed.
7. Optionally open the saved clip in Tracker to confirm it plays there.
