import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import org.opensourcephysics.cabrillo.tracker.ParticleModel;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.Step;
import org.opensourcephysics.cabrillo.tracker.TFrame;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TrackerIO;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.TPoint;
import org.opensourcephysics.media.core.VideoClip;

/** Exercises the normal asynchronous application loader and saver under an isolated virtual display.
 * This establishes Java's own resave baseline; it does not claim MoTrack export compatibility.
 */
public final class TrackerSessionReference {
    private static final CountDownLatch DONE = new CountDownLatch(1);
    private static final AtomicReference<Throwable> FAILURE = new AtomicReference<>();
    private static final AtomicReference<Map<String, Object>> REPORT = new AtomicReference<>();

    public static void main(String[] args) {
        int status = 1;
        try {
            if (args.length != 2) throw new IllegalArgumentException("Usage: TrackerSessionReference input.trk output-directory");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            Locale.setDefault(Locale.US);
            Path input = Path.of(args[0]).toAbsolutePath();
            Path directory = Path.of(args[1]).toAbsolutePath();
            Files.createDirectories(directory);
            Path saved = directory.resolve("tracker-resaved.trk");
            if (Files.exists(saved)) throw new IllegalArgumentException("Output must be a fresh test directory");
            SwingUtilities.invokeAndWait(guard(() -> {
                TFrame originalFrame = new TFrame();
                TrackerIO.openURL(input.toString(), originalFrame, guard(() -> whenEvaluated(originalFrame, () -> {
                    TrackerPanel panel = onlyPanel(originalFrame);
                    Map<String, Object> before = snapshot(panel);
                    if (TrackerIO.save(saved.toFile(), panel) == null || !Files.isRegularFile(saved))
                        throw new IllegalStateException("Tracker did not save the loaded session");
                    TFrame reopenedFrame = new TFrame();
                    TrackerIO.openURL(saved.toString(), reopenedFrame, guard(() -> whenEvaluated(reopenedFrame, () -> {
                        REPORT.set(TrackerReference.map("reference_version", OSPRuntime.VERSION,
                            "verification_level", "tracker_session_resave", "before", before,
                            "after", snapshot(onlyPanel(reopenedFrame))));
                        DONE.countDown();
                    })));
                })));
            }));
            if (!DONE.await(30, TimeUnit.SECONDS)) throw new IllegalStateException("Tracker session loading timed out");
            if (FAILURE.get() != null) throw new IllegalStateException("Tracker session failed", FAILURE.get());
            if (REPORT.get() == null) throw new IllegalStateException("Tracker produced no session report");
            Files.writeString(directory.resolve("session.json"), TrackerReference.json(REPORT.get()) + "\n", StandardCharsets.UTF_8);
            status = 0;
        } catch (Throwable error) {
            error.printStackTrace();
        }
        // Tracker owns background services and AWT windows. Exit only this isolated reference process.
        System.exit(status);
    }

    private static Runnable guard(Runnable action) {
        return () -> {
            try { action.run(); }
            catch (Throwable error) { FAILURE.compareAndSet(null, error); DONE.countDown(); }
        };
    }

    private static TrackerPanel onlyPanel(TFrame frame) {
        if (frame.getTabCount() != 1) throw new IllegalStateException("Expected exactly one fully loaded tab");
        TrackerPanel panel = frame.getTrackerPanelForTab(0);
        if (panel == null || panel.getTracks().isEmpty()) throw new IllegalStateException("Loaded tab has no tracks");
        if (panel.getVideo() != null) throw new IllegalStateException("Media session reference is not implemented yet");
        return panel;
    }

    private static void whenEvaluated(TFrame frame, Runnable action) {
        TrackerPanel panel = onlyPanel(frame);
        VideoClip clip = panel.getPlayer().getVideoClip();
        if (clip.getStepCount() < 1 || clip.getStepCount() > 10000)
            throw new IllegalArgumentException("Reference clip step limit exceeded");
        // ParticleModel evaluates from its normal draw path. Show/seek the real view in our private Xvfb,
        // then observe generated steps; a completed XML loader alone does not mean models were evaluated.
        frame.setVisible(true);
        frame.validate();
        panel.getPlayer().setStepNumber(clip.getStepCount() - 1);
        Timer timer = new Timer(25, event -> guard(() -> {
            panel.paintImmediately(panel.getVisibleRect());
            for (TTrack track : panel.getTracks()) if (track instanceof ParticleModel model) {
                for (int step = 0; step < clip.getStepCount(); step++) {
                    int number = clip.stepToFrame(step);
                    if (number >= model.getStartFrame() && number <= model.getEndFrame() && model.getStep(number) == null)
                        return;
                }
            }
            ((Timer) event.getSource()).stop();
            action.run();
        }).run());
        timer.start();
    }

    private static Map<String, Object> snapshot(TrackerPanel panel) {
        VideoClip clip = panel.getPlayer().getVideoClip();
        ImageCoordSystem coords = panel.getCoords();
        List<Object> tracks = new ArrayList<>();
        for (TTrack track : panel.getTracks()) {
            List<Object> samples = new ArrayList<>();
            Step[] steps = track.getSteps();
            if (steps.length > 10000) throw new IllegalArgumentException("Reference step limit exceeded");
            for (Step step : steps) if (step != null) {
                List<Object> points = new ArrayList<>();
                for (TPoint point : step.getPoints()) points.add(point == null ? null : List.of(point.x, point.y));
                samples.add(TrackerReference.map("frame", step.getFrameNumber(), "points", points));
            }
            if (track instanceof PointMass && samples.isEmpty())
                throw new IllegalStateException("Expected loaded point-mass/model samples");
            List<Integer> keys = null;
            if (track.getClass() == PointMass.class) {
                int[] values = (int[]) new XMLControlElement(track).getObject("keyFrames");
                keys = new ArrayList<>();
                if (values != null) for (int value : values) keys.add(value);
            }
            tracks.add(TrackerReference.map("class", track.getClass().getName(), "name", track.getName(),
                "mass", track instanceof PointMass mass ? mass.getMass() : null, "samples", samples, "key_frames", keys));
        }
        List<Object> coordinates = new ArrayList<>();
        int first = clip.getStartFrameNumber();
        int last = first + Math.max(0, clip.getStepCount() - 1) * clip.getStepSize();
        for (int frame : new int[] {first, last}) coordinates.add(TrackerReference.map("frame", frame,
            "image", List.of(13.5, 17.25),
            "world", List.of(coords.imageToWorldX(frame, 13.5, 17.25), coords.imageToWorldY(frame, 13.5, 17.25))));
        return TrackerReference.map("clip", TrackerReference.map("first_frame", first,
            "step_size", clip.getStepSize(), "step_count", clip.getStepCount(), "start_ms", clip.getStartTime()),
            "length_unit", panel.getLengthUnit(), "mass_unit", panel.getMassUnit(),
            "tracks", tracks, "coordinate_samples", coordinates);
    }
}
