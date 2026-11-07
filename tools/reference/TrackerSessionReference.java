import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import org.opensourcephysics.cabrillo.tracker.CenterOfMass;
import org.opensourcephysics.cabrillo.tracker.ParticleModel;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.Step;
import org.opensourcephysics.cabrillo.tracker.TFrame;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TView;
import org.opensourcephysics.cabrillo.tracker.TrackChooserTView;
import org.opensourcephysics.cabrillo.tracker.TrackerIO;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.Vector;
import org.opensourcephysics.cabrillo.tracker.VectorStep;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLProperty;
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
            if (args.length != 3 || !(args[2].equals("save") || args[2].equals("reopen")))
                throw new IllegalArgumentException("Usage: TrackerSessionReference input.trk output-directory save|reopen");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            Locale.setDefault(Locale.US);
            Path input = Path.of(args[0]).toAbsolutePath();
            Path directory = Path.of(args[1]).toAbsolutePath();
            boolean save = args[2].equals("save");
            Files.createDirectories(directory);
            Path saved = directory.resolve("tracker-resaved.trk");
            if (save && Files.exists(saved)) throw new IllegalArgumentException("Output must be a fresh test directory");
            SwingUtilities.invokeAndWait(guard(() -> {
                TFrame originalFrame = new TFrame();
                TrackerIO.openURL(input.toString(), originalFrame, guard(() -> whenEvaluated(originalFrame, () -> {
                    TrackerPanel panel = onlyPanel(originalFrame);
                    Map<String, Object> before = snapshot(panel);
                    if (save && (TrackerIO.save(saved.toFile(), panel) == null || !Files.isRegularFile(saved)))
                        throw new IllegalStateException("Tracker did not save the loaded session");
                    REPORT.set(TrackerReference.map("reference_version", OSPRuntime.VERSION,
                        "verification_level", "tracker_session_" + args[2], "loaded", before,
                        "after_save", save ? snapshot(panel) : null));
                    DONE.countDown();
                })));
            }));
            if (!DONE.await(30, TimeUnit.SECONDS)) throw new IllegalStateException("Tracker session loading timed out");
            if (FAILURE.get() != null) throw new IllegalStateException("Tracker session failed", FAILURE.get());
            if (REPORT.get() == null) throw new IllegalStateException("Tracker produced no session report");
            Files.writeString(directory.resolve("session-" + args[2] + ".json"), TrackerReference.json(REPORT.get()) + "\n", StandardCharsets.UTF_8);
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
            // Named constituents are resolved by CenterOfMass's normal initialize/draw path.
            for (TTrack track : panel.getTracks()) if (track instanceof CenterOfMass center
                    && center.getMasses().length == 0) return;
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
        List<Object> centers = new ArrayList<>();
        for (TTrack track : panel.getTracks()) {
            List<Object> samples = new ArrayList<>();
            List<Object> vectorSamples = new ArrayList<>();
            Step[] steps = track.getSteps();
            if (steps.length > 10000) throw new IllegalArgumentException("Reference step limit exceeded");
            for (Step step : steps) if (step != null) {
                List<Object> points = new ArrayList<>();
                // Vector drag handles and display points are transient UI state, not saved geometry.
                // Compare both scientific endpoints here and tail/components independently below.
                TPoint[] savedPoints = track.getClass() == Vector.class
                    ? new TPoint[] {((VectorStep) step).getTip(), ((VectorStep) step).getTail()} : step.getPoints();
                for (TPoint point : savedPoints) points.add(point == null ? null : List.of(point.x, point.y));
                int frame = step.getFrameNumber();
                List<Double> world = null;
                if (track.getClass() == PointMass.class || track.getClass() == CenterOfMass.class) {
                    TPoint point = savedPoints[0];
                    world = List.of(coords.imageToWorldX(frame, point.x, point.y),
                                    coords.imageToWorldY(frame, point.x, point.y));
                }
                samples.add(TrackerReference.map("frame", frame, "points", points, "world", world));
                if (track.getClass() == Vector.class) {
                    VectorStep vector = (VectorStep) step;
                    vectorSamples.add(TrackerReference.map("frame", step.getFrameNumber(),
                        "tail", List.of(vector.getTail().getX(), vector.getTail().getY()),
                        "components", List.of(vector.getXComponent(), vector.getYComponent())));
                }
            }
            if (track instanceof ParticleModel && samples.isEmpty())
                throw new IllegalStateException("Expected evaluated model samples");
            List<Integer> keys = null;
            if (track.getClass() == PointMass.class) {
                int[] values = (int[]) new XMLControlElement(track).getObject("keyFrames");
                keys = new ArrayList<>();
                if (values != null) for (int value : values) keys.add(value);
            }
            tracks.add(TrackerReference.map("class", track.getClass().getName(), "name", track.getName(),
                "mass", track instanceof PointMass mass ? mass.getMass() : null, "samples", samples, "key_frames", keys,
                "vector_samples", vectorSamples));
            if (track instanceof CenterOfMass center) {
                List<String> members = new ArrayList<>();
                for (PointMass member : center.getMasses()) members.add(member.getName());
                List<Object> positions = new ArrayList<>();
                for (Step step : steps) if (step != null) {
                    TPoint point = step.getPoints()[0];
                    int frame = step.getFrameNumber();
                    positions.add(TrackerReference.map("frame", frame, "image", List.of(point.x, point.y),
                        "world", List.of(coords.imageToWorldX(frame, point.x, point.y),
                                         coords.imageToWorldY(frame, point.x, point.y))));
                }
                centers.add(TrackerReference.map("name", center.getName(), "members", members,
                    "mass", center.getMass(), "samples", positions));
            }
        }
        List<Object> calibrationKeys = new ArrayList<>();
        java.util.TreeSet<Integer> keyFrames = new java.util.TreeSet<>(coords.getKeyFrames());
        keyFrames.add(0);
        if (keyFrames.size() > 10000) throw new IllegalArgumentException("Reference calibration limit exceeded");
        for (int frame : keyFrames) calibrationKeys.add(TrackerReference.map("frame", frame,
            "origin", List.of(coords.getOriginX(frame), coords.getOriginY(frame)),
            "angle", coords.getAngle(frame) * 180 / Math.PI,
            "scale", List.of(coords.getScaleX(frame), coords.getScaleY(frame))));
        List<Object> coordinates = new ArrayList<>();
        int first = clip.getStartFrameNumber();
        int last = first + Math.max(0, clip.getStepCount() - 1) * clip.getStepSize();
        for (int frame : new int[] {first, last}) coordinates.add(TrackerReference.map("frame", frame,
            "image", List.of(13.5, 17.25),
            "world", List.of(coords.imageToWorldX(frame, 13.5, 17.25), coords.imageToWorldY(frame, 13.5, 17.25))));
        return TrackerReference.map("clip", TrackerReference.map("first_frame", first,
            "step_size", clip.getStepSize(), "step_count", clip.getStepCount(), "start_ms", clip.getStartTime()),
            "length_unit", panel.getLengthUnit(), "mass_unit", panel.getMassUnit(),
            "name_references", nameReferences(panel),
            "saved_views", savedViews(panel),
            "centers_of_mass", centers,
            "calibration", TrackerReference.map("fixed_origin", coords.isFixedOrigin(), "fixed_angle", coords.isFixedAngle(),
                "fixed_scale", coords.isFixedScale(), "keys", calibrationKeys),
            "tracks", tracks, "coordinate_samples", coordinates);
    }

    private static Map<String, Object> nameReferences(TrackerPanel panel) {
        TView[][] views = panel.getTFrame().getTViews(panel, false);
        if (views == null || views.length != 4)
            throw new IllegalStateException("Expected four loaded view choosers");
        List<Object> selections = new ArrayList<>();
        for (TView[] chooser : views) {
            if (chooser == null || chooser.length != 4)
                throw new IllegalStateException("Expected four view slots per chooser");
            List<Object> pair = new ArrayList<>();
            for (int type : new int[] {TView.VIEW_PLOT, TView.VIEW_TABLE}) {
                TView view = chooser[type];
                if (view != null && !(view instanceof TrackChooserTView))
                    throw new IllegalStateException("Expected a track chooser view");
                TTrack selected = view == null ? null : ((TrackChooserTView) view).getSelectedTrack();
                pair.add(selected == null ? null : selected.getName());
            }
            selections.add(pair);
        }
        TTrack selected = panel.getSelectedTrack();
        // Compare live object selections and the normal serializer's packed representation.
        // The independent manifest expectation comes from the pinned original experiment.
        return TrackerReference.map("selected_track", selected == null ? null : selected.getName(),
            "view_tracks", selections,
            "packed_view_tracks", new XMLControlElement(panel).getString("selected_track_views"));
    }

    // Capture the normal serializers of live, loaded views. Do not instantiate saved TrackView
    // controls: their loaders require the existing panel/track context. Only primitive string
    // arrays are decoded with getObject. Owner-map order is immaterial; visible column and plot
    // order remains significant. This deliberately excludes transient autoscale/rendering state.
    private static Map<String, Object> savedViews(TrackerPanel panel) {
        TView[][] views = panel.getTFrame().getTViews(panel, false);
        List<Object> tables = new ArrayList<>();
        List<Object> plots = new ArrayList<>();
        for (int chooser = 0; chooser < views.length; chooser++) {
            TView table = views[chooser][TView.VIEW_TABLE];
            if (table != null) {
                XMLControl control = new XMLControlElement(table);
                List<List<String>> columns = stringRows(control.getObject("track_columns"), 0);
                List<List<String>> indices = stringRows(control.getObject("dataset_indices"), 2);
                List<List<String>> formats = new ArrayList<>();
                Object raw = control.getObject("column_formats");
                if (raw != null) {
                    if (!(raw instanceof String[][][] groups) || groups.length > 10000)
                        throw new IllegalStateException("Unexpected table format groups");
                    for (String[][] group : groups) {
                        List<List<String>> rows = stringRows(group, 3);
                        if (rows.isEmpty()) throw new IllegalStateException("Empty table format group");
                        String owner = rows.get(0).get(0);
                        for (List<String> row : rows) {
                            if (!owner.equals(row.get(0))) throw new IllegalStateException("Mixed table format owners");
                            formats.add(row);
                            if (formats.size() > 10000) throw new IllegalStateException("Table format limit exceeded");
                        }
                    }
                }
                formats.sort(Comparator.<List<String>, String>comparing(row -> row.get(0)).thenComparing(row -> row.get(1)));
                if (!columns.isEmpty() || !formats.isEmpty() || !indices.isEmpty())
                    tables.add(TrackerReference.map("chooser", chooser, "selected_track", control.getString("selected_track"),
                        "track_columns", columns, "column_formats", formats, "dataset_indices", indices));
            }
            TView plot = views[chooser][TView.VIEW_PLOT];
            if (plot != null) {
                XMLControl control = new XMLControlElement(plot);
                XMLProperty property = TrackerReference.property(control, "track_views");
                Map<String, Object> owners = new TreeMap<>();
                if (property != null) {
                    XMLControl[] children = property.getChildControls();
                    if (children.length > 10000) throw new IllegalStateException("Plot owner limit exceeded");
                    for (XMLControl child : children) {
                        List<Object> settings = new ArrayList<>();
                        // The pinned application supports up to three ordered plots per track.
                        for (int index = 0; index < 3; index++) {
                            XMLControl item = child.getChildControl("plot" + index);
                            if (item == null) break;
                            Object guests = item.getObject("guests");
                            if (guests != null && (!(guests instanceof String[]) || ((String[]) guests).length > 10000))
                                throw new IllegalStateException("Unexpected plot guests");
                            settings.add(TrackerReference.map("x_var", item.getString("x_var"), "y_var", item.getString("y_var"),
                                "guests", guests == null ? List.of() : List.of((String[]) guests)));
                        }
                        String owner = child.getString("track");
                        if (owner == null || settings.isEmpty() || owners.put(owner, TrackerReference.map("track", owner,
                                "linked", child.getBoolean("linked"), "plots", settings)) != null)
                            throw new IllegalStateException("Invalid or duplicate plot owner");
                    }
                }
                if (!owners.isEmpty()) plots.add(TrackerReference.map("chooser", chooser,
                    "selected_track", control.getString("selected_track"), "track_views", new ArrayList<>(owners.values())));
            }
        }
        return TrackerReference.map("tables", tables, "plots", plots);
    }

    private static List<List<String>> stringRows(Object value, int width) {
        List<List<String>> result = new ArrayList<>();
        if (value == null) return result;
        if (!(value instanceof String[][] rows) || rows.length > 10000)
            throw new IllegalStateException("Unexpected table rows");
        for (String[] row : rows) {
            if (row == null || row.length == 0 || row.length > 10000 || (width != 0 && row.length != width))
                throw new IllegalStateException("Unexpected table row");
            result.add(new ArrayList<>(Arrays.asList(row)));
        }
        result.sort(Comparator.comparing(row -> row.get(0)));
        return result;
    }
}
