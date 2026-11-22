import java.lang.reflect.Field;
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

import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.Step;
import org.opensourcephysics.cabrillo.tracker.TFrame;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TrackerIO;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;

/**
 * Opens a Tracker tabset through the pinned application's normal TrackerIO/TFrame.Loader path,
 * reports every loaded tab in order, then writes the tabset Tracker itself would save for that frame.
 * TrackerIO.saveTabset is the same XMLControlElement(frame) write after its dialogs; the dialogs are
 * skipped because every loaded tab already has its file. Isolated Linux CI only.
 */
public final class TrackerTabsetReference {
    private static final CountDownLatch DONE = new CountDownLatch(1);
    private static final AtomicReference<Throwable> FAILURE = new AtomicReference<>();
    private static final AtomicReference<Map<String, Object>> REPORT = new AtomicReference<>();

    public static void main(String[] args) {
        int status = 1;
        try {
            if (args.length != 3) throw new IllegalArgumentException("Usage: TrackerTabsetReference tabset.trk output-directory expected-tabs");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
                FAILURE.compareAndSet(null, error);
                DONE.countDown();
            });
            Locale.setDefault(Locale.US);
            Path tabset = Path.of(args[0]).toAbsolutePath();
            Path output = Files.createDirectories(Path.of(args[1]).toAbsolutePath());
            int expected = Integer.parseInt(args[2]);
            if (expected < 1 || expected > 16) throw new IllegalArgumentException("Unexpected tab count");
            Path saved = output.resolve("tracker-tabset.trk");
            if (Files.exists(saved)) throw new IllegalArgumentException("Output must be a fresh test directory");
            TrackerSessionReference.initializeReferenceCache(Path.of(System.getProperty("user.home")));
            SwingUtilities.invokeAndWait(() -> {
                TFrame frame = new TFrame();
                TrackerIO.openURL(tabset.toString(), frame, () -> {});
                // The tabset loader completes asynchronously per file; wait for a stable, complete frame.
                int[] stable = {0};
                Timer poll = new Timer(100, null);
                poll.addActionListener(event -> {
                    try {
                        if (!loaded(frame, expected)) {
                            stable[0] = 0;
                            return;
                        }
                        if (++stable[0] < 5) return;
                        poll.stop();
                        REPORT.set(report(frame, saved));
                        DONE.countDown();
                    } catch (Throwable error) {
                        poll.stop();
                        FAILURE.compareAndSet(null, error);
                        DONE.countDown();
                    }
                });
                poll.start();
            });
            if (!DONE.await(40, TimeUnit.SECONDS)) throw new IllegalStateException("Tracker tabset loading timed out");
            if (FAILURE.get() != null) throw new IllegalStateException("Tracker tabset session failed", FAILURE.get());
            Files.writeString(output.resolve("tabset-session.json"), TrackerReference.json(REPORT.get()) + "\n", StandardCharsets.UTF_8);
            status = 0;
        } catch (Throwable error) {
            error.printStackTrace();
        }
        System.exit(status);
    }

    private static boolean loaded(TFrame frame, int expected) {
        if (frame.getTabCount() != expected) return false;
        for (int i = 0; i < expected; i++) {
            TrackerPanel panel = frame.getTrackerPanelForTab(i);
            if (panel == null || panel.getTracks().isEmpty()) return false;
        }
        return true;
    }

    private static Map<String, Object> report(TFrame frame, Path saved) throws Exception {
        List<Object> tabs = new ArrayList<>();
        for (int i = 0; i < frame.getTabCount(); i++) {
            TrackerPanel panel = frame.getTrackerPanelForTab(i);
            List<Object> masses = new ArrayList<>();
            for (TTrack track : panel.getTracks()) {
                if (!(track instanceof PointMass)) continue;
                PointMass mass = (PointMass) track;
                int samples = 0;
                for (Step step : mass.getSteps()) if (step != null) samples++;
                masses.add(TrackerReference.map("name", mass.getName(), "mass", mass.getMass(), "samples", samples));
            }
            tabs.add(TrackerReference.map("title", panel.getTitle(), "path", panel.getToolTipPath(), "point_masses", masses));
        }
        // TrackerIO.saveTabset sets this protected field before writing XMLControlElement(frame).
        Field tabsetFile = TFrame.class.getDeclaredField("tabsetFile");
        tabsetFile.setAccessible(true);
        tabsetFile.set(frame, saved.toFile());
        new XMLControlElement(frame).write(saved.toString());
        if (!Files.isRegularFile(saved) || Files.size(saved) == 0) throw new IllegalStateException("Tracker did not write its tabset");
        return TrackerReference.map("reference_version", OSPRuntime.VERSION, "verification_level", "tracker_tabset_session",
            "tabs", tabs);
    }
}
