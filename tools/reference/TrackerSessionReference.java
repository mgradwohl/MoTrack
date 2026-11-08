import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.HexFormat;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import javax.swing.SwingUtilities;
import javax.swing.Timer;
import org.opensourcephysics.cabrillo.tracker.CenterOfMass;
import org.opensourcephysics.cabrillo.tracker.DynamicParticle;
import org.opensourcephysics.cabrillo.tracker.DynamicSystem;
import org.opensourcephysics.cabrillo.tracker.ParticleModel;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.Step;
import org.opensourcephysics.cabrillo.tracker.TapeMeasure;
import org.opensourcephysics.cabrillo.tracker.TapeStep;
import org.opensourcephysics.cabrillo.tracker.TFrame;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TView;
import org.opensourcephysics.cabrillo.tracker.TrackChooserTView;
import org.opensourcephysics.cabrillo.tracker.TrackerIO;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.Vector;
import org.opensourcephysics.cabrillo.tracker.VectorStep;
import org.opensourcephysics.cabrillo.tracker.VectorSum;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.display.Dataset;
import org.opensourcephysics.display.DatasetManager;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.ImageVideo;
import org.opensourcephysics.media.core.TPoint;
import org.opensourcephysics.media.core.VideoClip;

/** Exercises the normal asynchronous application loader and saver under an isolated virtual display.
 * This establishes Java's own resave baseline; it does not claim MoTrack export compatibility.
 */
public final class TrackerSessionReference {
    private static final CountDownLatch DONE = new CountDownLatch(1);
    private static final AtomicReference<Throwable> FAILURE = new AtomicReference<>();
    private static final AtomicReference<String> WAITING = new AtomicReference<>("application loader callback");
    private static final AtomicReference<Map<String, Object>> REPORT = new AtomicReference<>();
    private static final AtomicInteger LOADER_CALLBACKS = new AtomicInteger();

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
            List<AttachmentRequirement> attachmentRequirements = attachmentRequirements(input);
            List<ModelRequirement> modelRequirements = modelRequirements(input);
            SwingUtilities.invokeAndWait(guard(() -> {
                TFrame originalFrame = new TFrame();
                Runnable loaded = once(guard(() -> whenEvaluated(originalFrame, attachmentRequirements, modelRequirements, () -> {
                    TrackerPanel panel = onlyPanel(originalFrame);
                    Map<String, Object> before = snapshot(panel);
                    if (save && (TrackerIO.save(saved.toFile(), panel) == null || !Files.isRegularFile(saved) || saved.toFile().length() == 0))
                        throw new IllegalStateException("Tracker did not save the loaded session");
                    REPORT.set(TrackerReference.map("reference_version", OSPRuntime.VERSION,
                        "verification_level", "tracker_session_" + args[2],
                        "view_preparation", "visit_custom_tables_restore_selection", "loaded", before,
                        "after_save", save ? snapshot(panel) : null));
                    DONE.countDown();
                })));
                // Pinned AsyncLoader can reach doneLoading from both checkDone and doneAsync.
                // A repeated callback must not launch another timer/save after DONE is released.
                TrackerIO.openURL(input.toString(), originalFrame, () -> {
                    LOADER_CALLBACKS.incrementAndGet();
                    loaded.run();
                });
            }));
            if (!DONE.await(30, TimeUnit.SECONDS)) {
                printWaitingThreads();
                throw new IllegalStateException("Tracker session loading timed out: " + WAITING.get());
            }
            if (FAILURE.get() != null) throw new IllegalStateException("Tracker session failed", FAILURE.get());
            if (REPORT.get() == null) throw new IllegalStateException("Tracker produced no session report");
            REPORT.get().put("loader_callbacks", LOADER_CALLBACKS.get());
            Files.writeString(directory.resolve("session-" + args[2] + ".json"), TrackerReference.json(REPORT.get()) + "\n", StandardCharsets.UTF_8);
            status = 0;
        } catch (Throwable error) {
            System.err.println("Tracker reference input=" + bounded(args.length > 0 ? args[0] : "<missing>", 512)
                + " stage=" + bounded(args.length > 2 ? args[2] : "<missing>", 32)
                + " waiting=" + bounded(WAITING.get(), 512));
            error.printStackTrace();
        }
        // Tracker owns background services and AWT windows. Exit only this isolated reference process.
        System.exit(status);
    }

    private static String bounded(String text, int limit) {
        return text.length() <= limit ? text : text.substring(0, limit) + "...";
    }

    private static void printWaitingThreads() {
        // Observe only this isolated JVM. Do not call the possibly blocked event thread or alter its loader.
        try {
            StringBuilder report = new StringBuilder("Tracker timeout threads (bounded):\n");
            Thread.getAllStackTraces().entrySet().stream()
                .filter(entry -> {
                    String name = entry.getKey().getName();
                    return name.startsWith("AWT-EventQueue") || name.startsWith("SwingWorker")
                        || name.equals("TimerQueue") || name.equals("main");
                })
                .sorted(Comparator.comparing(entry -> entry.getKey().getName())).limit(8)
                .forEach(entry -> {
                    report.append(bounded(entry.getKey().getName(), 80)).append(' ')
                        .append(entry.getKey().getState()).append('\n');
                    diagnosticFrames(entry.getValue()).stream()
                        .forEach(frame -> report.append("  ").append(bounded(frame.toString(), 200)).append('\n'));
                });
            System.err.println(bounded(report.toString(), 3000));
        } catch (Throwable diagnosticFailure) {
            System.err.println("Tracker timeout thread diagnostics unavailable: " + diagnosticFailure.getClass().getName());
        }
    }

    static List<StackTraceElement> diagnosticFrames(StackTraceElement[] stack) {
        // A modal dialog can bury its Tracker caller below eight platform frames. Keep that
        // caller without expanding the existing per-thread/output bounds or touching AWT.
        List<StackTraceElement> result = new ArrayList<>();
        Arrays.stream(stack).filter(frame -> frame.getClassName().startsWith("org.opensourcephysics.")
                || frame.getClassName().startsWith("TrackerSessionReference"))
            .distinct().limit(3).forEach(result::add);
        for (StackTraceElement frame : stack) {
            if (result.size() == 8) break;
            if (!result.contains(frame)) result.add(frame);
        }
        return List.copyOf(result);
    }

    static Runnable once(Runnable action) {
        AtomicBoolean started = new AtomicBoolean();
        return () -> {
            if (started.compareAndSet(false, true)) action.run();
        };
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
        if (panel.getVideo() != null && !(panel.getVideo() instanceof ImageVideo))
            throw new IllegalStateException("Only image-sequence media sessions are audited");
        return panel;
    }

    private record AttachmentRequirement(String owner, String[] targets) {}
    private record ModelRequirement(String owner, String booster, String[] particles) {}

    private static List<ModelRequirement> modelRequirements(Path input) {
        XMLControlElement source = new XMLControlElement(input.toString());
        List<ModelRequirement> result = new ArrayList<>();
        for (XMLControl track : TrackerReference.objects(TrackerReference.property(source, "tracks"))) {
            String kind = track.getObjectClassName();
            if (!(kind.equals(DynamicParticle.class.getName()) || kind.equals(DynamicSystem.class.getName())
                    || kind.equals("org.opensourcephysics.cabrillo.tracker.DynamicParticlePolar"))) continue;
            String booster = track.getString("booster");
            String[] particles = kind.equals(DynamicSystem.class.getName()) ? (String[]) track.getObject("particles") : null;
            if (particles != null && (particles.length > 2 || Arrays.stream(particles).anyMatch(name -> name == null)))
                throw new IllegalStateException("Unexpected system member requirements");
            if (booster != null || particles != null) {
                if (result.size() >= 10000) throw new IllegalStateException("Model requirement limit exceeded");
                result.add(new ModelRequirement(track.getString("name"), booster, particles));
            }
        }
        source.dispose();
        return result;
    }

    private static boolean modelDependenciesReady(TrackerPanel panel, List<ModelRequirement> requirements) {
        for (ModelRequirement requirement : requirements) {
            XMLControlElement live = new XMLControlElement(uniqueTrack(panel, requirement.owner()));
            if (requirement.booster() != null && !requirement.booster().equals(live.getString("booster"))) return false;
            if (requirement.particles() != null && !Arrays.equals(requirement.particles(), (String[]) live.getObject("particles")))
                return false;
        }
        return true;
    }

    private static List<AttachmentRequirement> attachmentRequirements(Path input) {
        XMLControlElement source = new XMLControlElement(input.toString());
        List<AttachmentRequirement> result = new ArrayList<>();
        for (XMLControl track : TrackerReference.objects(TrackerReference.property(source, "tracks"))) {
            if (TrackerReference.property(track, "attachments") == null) continue;
            if (!track.getObjectClassName().equals(TapeMeasure.class.getName()))
                throw new IllegalStateException("Attachment application reference currently supports tape measures only");
            String[] targets = (String[]) track.getObject("attachments");
            if (targets == null || targets.length > 2 || result.size() >= 10000)
                throw new IllegalStateException("Unexpected tape attachment requirements in " + input.getFileName()
                    + " for " + track.getString("name") + ": " + Arrays.toString(targets));
            // Normal Tracker saves omit trailing null array entries. TapeMeasure's public
            // getAttachments() restores its two endpoint slots on load; normalize only our
            // expected shape, never the loaded objects or the independent manifest values.
            result.add(new AttachmentRequirement(track.getString("name"), Arrays.copyOf(targets, 2)));
        }
        source.dispose();
        return result;
    }

    private static TTrack uniqueTrack(TrackerPanel panel, String name) {
        List<TTrack> matches = panel.getTracks().stream().filter(track -> track.getName().equals(name)).toList();
        if (matches.size() != 1) throw new IllegalStateException("Missing or ambiguous reference track: " + name);
        return matches.get(0);
    }

    private static boolean attachmentsReady(TrackerPanel panel, List<AttachmentRequirement> requirements) {
        VideoClip clip = panel.getPlayer().getVideoClip();
        for (AttachmentRequirement requirement : requirements) {
            TTrack owner = uniqueTrack(panel, requirement.owner());
            TTrack[] targets = owner.getAttachments();
            if (targets == null || targets.length != requirement.targets().length) return false;
            for (int slot = 0; slot < targets.length; slot++) {
                String name = requirement.targets()[slot];
                if (name == null) {
                    if (targets[slot] != null) throw new IllegalStateException("Empty attachment acquired a target");
                    for (int stepIndex = 0; stepIndex < clip.getStepCount(); stepIndex++) {
                        Step step = owner.getStep(clip.stepToFrame(stepIndex));
                        if (step == null) throw new IllegalStateException("Expected unattached tape endpoint");
                        if (step.getPoints()[slot].isAttached()) return false;
                    }
                    continue;
                }
                TTrack target = uniqueTrack(panel, name);
                if (targets[slot] != target) return false;
                for (int stepIndex = 0; stepIndex < clip.getStepCount(); stepIndex++) {
                    int frame = clip.stepToFrame(stepIndex);
                    Step targetStep = target.getStep(frame), ownerStep = owner.getStep(frame);
                    if (targetStep == null || ownerStep == null)
                        throw new IllegalStateException("Attachment fixture needs marked target and owner steps");
                    TPoint point = ownerStep.getPoints()[slot], expected = targetStep.getPoints()[0];
                    if (!point.isAttached() || point.x != expected.x || point.y != expected.y) return false;
                }
            }
        }
        return true;
    }

    private static void whenEvaluated(TFrame frame, List<AttachmentRequirement> attachments, List<ModelRequirement> models, Runnable action) {
        TrackerPanel panel = onlyPanel(frame);
        WAITING.set("showing the loaded experiment");
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
            // Tracker schedules attachment refresh after load. Observe normal resolution and point
            // attachment instead of sleeping or calling its protected refresh/repair methods.
            if (!attachmentsReady(panel, attachments)) { WAITING.set("attachment target/point refresh"); return; }
            if (!modelDependenciesReady(panel, models)) { WAITING.set("model dependency resolution"); return; }
            // Named constituents are resolved by CenterOfMass's normal initialize/draw path.
            for (TTrack track : panel.getTracks()) if (track instanceof CenterOfMass center
                    && center.getMasses().length == 0) { WAITING.set("center members: " + center.getName()); return; }
            // The normal loader and draw path have run. Snapshot vector sums as they are:
            // Tracker 6.3.5 leaves pending names uninitialized. Waiting for nonempty members
            // would hide this persistent load/save defect behind a generic timeout.
            for (TTrack track : panel.getTracks()) if (track instanceof ParticleModel model) {
                for (int step = 0; step < clip.getStepCount(); step++) {
                    int number = clip.stepToFrame(step);
                    if (number >= model.getStartFrame() && number <= model.getEndFrame() && model.getStep(number) == null) {
                        WAITING.set("particle model: " + model.getName() + ", frame=" + number);
                        return;
                    }
                }
            }
            ((Timer) event.getSource()).stop();
            WAITING.set("custom table preparation");
            visitCustomTables(panel, action);
        }).run());
        timer.start();
    }

    private record TableVisit(TrackChooserTView view, TTrack track, TTrack restore) {}

    private static void visitCustomTables(TrackerPanel panel, Runnable action) {
        List<TableVisit> visits = new ArrayList<>();
        for (TView[] chooser : panel.getTFrame().getTViews(panel, false)) {
            if (!(chooser[TView.VIEW_TABLE] instanceof TrackChooserTView table)) continue;
            for (List<String> row : stringRows(new XMLControlElement(table).getObject("track_columns"), 0)) {
                List<TTrack> owners = panel.getTracks().stream().filter(track -> track.getName().equals(row.get(0))).toList();
                if (owners.size() != 1 || !table.isShowing() || table.getSelectedTrack() == null)
                    throw new IllegalStateException("Reference needs a visible table with an unambiguous owner");
                visits.add(new TableVisit(table, owners.get(0), table.getSelectedTrack()));
                if (visits.size() > 10000) throw new IllegalStateException("Table visit limit exceeded");
            }
        }
        if (visits.isEmpty()) { action.run(); return; }
        // Tracker refreshes a table's column model only while that track is displayed. In 6.3.5,
        // immediately resaving an unvisited custom table can serialize null columns. Exercise the
        // normal selection/refresh path, observe populated columns, then restore the saved selection.
        // This is a visited-view baseline, not proof of fidelity for an immediate unvisited resave.
        int[] index = {0};
        visits.get(0).view().setSelectedTrack(visits.get(0).track());
        Timer timer = new Timer(25, event -> guard(() -> {
            TableVisit visit = visits.get(index[0]);
            if (visit.view().getSelectedTrack() != visit.track())
                throw new IllegalStateException("Tracker did not select the table owner");
            visit.view().paintImmediately(visit.view().getVisibleRect());
            boolean found = false;
            for (List<String> row : stringRows(new XMLControlElement(visit.view()).getObject("track_columns"), 0)) {
                if (!visit.track().getName().equals(row.get(0))) continue;
                found = true;
                if (row.stream().anyMatch(value -> value == null)) return;
            }
            if (!found) throw new IllegalStateException("Customized table disappeared during selection");
            visit.view().setSelectedTrack(visit.restore());
            if (++index[0] == visits.size()) {
                ((Timer) event.getSource()).stop();
                action.run();
            } else visits.get(index[0]).view().setSelectedTrack(visits.get(index[0]).track());
        }).run());
        timer.start();
    }

    private static List<Object> calibratedVectorSamples(Vector vector, TrackerPanel panel) {
        // Read the live analysis datasets rather than recomputing values from the XML/coordinate API.
        // Pinned 6.3.5 columns are x, y, magnitude, angle, tail x/y, step and original frame.
        DatasetManager data = vector.getData(panel);
        int[] indices = {0, 1, 2, 7};
        String[] names = {"x", "y", "mag", "frame"};
        double[][] columns = new double[4][];
        int count = -1;
        for (int index = 0; index < indices.length; index++) {
            Dataset column = data.getDataset(indices[index]);
            if (!names[index].equals(column.getYColumnName()) || column.getIndex() > 10000)
                throw new IllegalStateException("Unexpected vector analysis column");
            columns[index] = column.getYPoints();
            if (count < 0) count = columns[index].length;
            if (columns[index].length != count)
                throw new IllegalStateException("Mismatched vector analysis rows");
        }
        List<Object> result = new ArrayList<>();
        for (int row = 0; row < count; row++) {
            double frame = columns[3][row];
            if (!Double.isFinite(frame) || frame < 0 || frame > 500000 || frame != Math.rint(frame)
                    || !Double.isFinite(columns[0][row]) || !Double.isFinite(columns[1][row])
                    || !Double.isFinite(columns[2][row]))
                throw new IllegalStateException("Invalid vector analysis row");
            result.add(TrackerReference.map("frame", (int) frame,
                "components", List.of(columns[0][row], columns[1][row]), "magnitude", columns[2][row]));
        }
        return result;
    }

    private static Map<String, Object> snapshot(TrackerPanel panel) {
        VideoClip clip = panel.getPlayer().getVideoClip();
        ImageCoordSystem coords = panel.getCoords();
        List<Object> tracks = new ArrayList<>();
        List<Object> centers = new ArrayList<>();
        List<Object> sums = new ArrayList<>();
        List<Object> attachments = new ArrayList<>();
        List<Object> models = new ArrayList<>();
        for (TTrack track : panel.getTracks()) {
            List<Object> samples = new ArrayList<>();
            List<Object> vectorSamples = new ArrayList<>();
            Step[] steps = track.getSteps();
            if (steps.length > 10000) throw new IllegalArgumentException("Reference step limit exceeded");
            for (Step step : steps) if (step != null) {
                if (track instanceof TapeMeasure && !clip.includesFrame(step.getFrameNumber())) continue;
                List<Object> points = new ArrayList<>();
                // Vector/tape drag handles and display points are transient UI state, not saved geometry.
                // Compare both scientific endpoints here and tail/components independently below.
                TPoint[] savedPoints = track instanceof Vector
                    ? new TPoint[] {((VectorStep) step).getTip(), ((VectorStep) step).getTail()}
                    : track instanceof TapeMeasure
                    ? new TPoint[] {((TapeStep) step).getEnd1(), ((TapeStep) step).getEnd2()} : step.getPoints();
                for (TPoint point : savedPoints) points.add(point == null ? null : List.of(point.x, point.y));
                int frame = step.getFrameNumber();
                List<Double> world = null;
                if (track.getClass() == PointMass.class || track.getClass() == CenterOfMass.class) {
                    TPoint point = savedPoints[0];
                    world = List.of(coords.imageToWorldX(frame, point.x, point.y),
                                    coords.imageToWorldY(frame, point.x, point.y));
                }
                samples.add(TrackerReference.map("frame", frame, "points", points, "world", world));
                if (track instanceof Vector) {
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
                "vector_samples", vectorSamples,
                "calibrated_vector_samples", track.getClass() == Vector.class
                    ? calibratedVectorSamples((Vector) track, panel) : List.of(),
                "appearance", TrackerReference.map("visible", track.isVisible(), "trail", track.isTrailVisible(),
                    "locked", track.isLocked(), "footprint", track.getFootprintName(), "argb", track.getColor().getRGB())));
            if (track instanceof DynamicParticle model) {
                XMLControlElement live = new XMLControlElement(model);
                String[] members = model instanceof DynamicSystem ? (String[]) live.getObject("particles") : null;
                List<Object> functions = new ArrayList<>();
                for (XMLControl function : TrackerReference.objects(TrackerReference.property(live, "main_functions")))
                    functions.add(TrackerReference.map("name", function.getString("name"), "expression", function.getString("expression")));
                List<Object> positions = new ArrayList<>();
                for (Step step : steps) if (step != null) {
                    int frame = step.getFrameNumber();
                    TPoint point = step.getPoints()[0];
                    positions.add(TrackerReference.map("frame", frame, "image", List.of(point.x, point.y),
                        "world", List.of(coords.imageToWorldX(frame, point.x, point.y), coords.imageToWorldY(frame, point.x, point.y))));
                }
                models.add(TrackerReference.map("name", model.getName(), "mass", model.getMass(),
                    "booster", live.getString("booster"), "particles", members == null ? List.of() : Arrays.asList(members),
                    "samples", positions, "functions", functions));
            }
            if (track instanceof TapeMeasure tape && tape.isAttached()) {
                List<String> targets = new ArrayList<>();
                for (TTrack target : tape.getAttachments()) targets.add(target == null ? null : target.getName());
                List<Object> measurements = new ArrayList<>();
                for (int stepIndex = 0; stepIndex < clip.getStepCount(); stepIndex++) {
                    int frame = clip.stepToFrame(stepIndex);
                    TapeStep step = (TapeStep) tape.getStep(frame);
                    if (step == null) throw new IllegalStateException("Expected attached tape step");
                    measurements.add(TrackerReference.map("frame", frame,
                        "endpoints", List.of(List.of(step.getEnd1().x, step.getEnd1().y), List.of(step.getEnd2().x, step.getEnd2().y)),
                        "attached", List.of(step.getEnd1().isAttached(), step.getEnd2().isAttached()),
                        "world_length", step.getTapeLength(true)));
                }
                attachments.add(TrackerReference.map("name", tape.getName(), "targets", targets, "samples", measurements));
            }
            if (track instanceof VectorSum sum) {
                List<String> members = new ArrayList<>();
                for (Vector member : sum.getVectors()) members.add(member.getName());
                List<Object> components = new ArrayList<>();
                for (Step step : steps) if (step != null) {
                    VectorStep vector = (VectorStep) step;
                    int frame = step.getFrameNumber();
                    double x = vector.getXComponent(), y = vector.getYComponent();
                    components.add(TrackerReference.map("frame", frame,
                        "tail", List.of(vector.getTail().getX(), vector.getTail().getY()), "components", List.of(x, y),
                        "world_components", List.of(coords.imageToWorldXComponent(frame, x, y),
                                                    coords.imageToWorldYComponent(frame, x, y))));
                }
                sums.add(TrackerReference.map("name", sum.getName(), "members", members, "samples", components));
            }
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
        List<Object> times = new ArrayList<>();
        for (int frame : new int[] {first, last})
            times.add(TrackerReference.map("frame", frame, "time_ms", panel.getPlayer().getFrameTime(frame)));
        return TrackerReference.map("clip", TrackerReference.map("first_frame", first,
            "step_size", clip.getStepSize(), "step_count", clip.getStepCount(), "start_ms", clip.getStartTime()),
            "length_unit", panel.getLengthUnit(), "mass_unit", panel.getMassUnit(),
            "time_unit", panel.getTimeUnit(), "time_samples", times,
            "frame_duration_ms", panel.getPlayer().getClipControl().getMeanFrameDuration(),
            "name_references", nameReferences(panel), "media", imageMedia(panel),
            "saved_views", savedViews(panel),
            "centers_of_mass", centers,
            "vector_sums", sums,
            "attachments", attachments,
            "models", models,
            "calibration", TrackerReference.map("fixed_origin", coords.isFixedOrigin(), "fixed_angle", coords.isFixedAngle(),
                "fixed_scale", coords.isFixedScale(), "keys", calibrationKeys),
            "tracks", tracks, "coordinate_samples", coordinates);
    }

    private static Map<String, Object> imageMedia(TrackerPanel panel) {
        if (panel.getVideo() == null) return null;
        if (!(panel.getVideo() instanceof ImageVideo video) || !video.getFilterStack().isEmpty())
            throw new IllegalStateException("Expected an unfiltered image sequence");
        int count = video.getFrameCount();
        if (count < 1 || count > 100) throw new IllegalStateException("Image reference frame limit exceeded");
        List<Object> frames = new ArrayList<>();
        int original = video.getFrameNumber();
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            for (int frame = 0; frame < count; frame++) {
                video.setFrameNumber(frame);
                java.awt.image.BufferedImage image = video.getImage();
                if (image == null || image.getWidth() > 256 || image.getHeight() > 256)
                    throw new IllegalStateException("Missing or oversized reference image");
                digest.reset();
                for (int y = 0; y < image.getHeight(); y++) for (int x = 0; x < image.getWidth(); x++) {
                    int rgb = image.getRGB(x, y);
                    digest.update((byte) (rgb >> 16));
                    digest.update((byte) (rgb >> 8));
                    digest.update((byte) rgb);
                }
                frames.add(TrackerReference.map("frame", frame, "width", image.getWidth(), "height", image.getHeight(),
                    "rgb_sha256", HexFormat.of().formatHex(digest.digest()), "time_ms", video.getFrameTime(frame)));
            }
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException(error);
        } finally {
            video.setFrameNumber(original);
        }
        return TrackerReference.map("class", video.getClass().getName(), "frame_count", count, "frames", frames);
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
                                "lines", item.getBoolean("lines"), "points", item.getBoolean("points"),
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
