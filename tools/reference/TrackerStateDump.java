import java.awt.geom.AffineTransform;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import javax.swing.SwingUtilities;
import org.opensourcephysics.cabrillo.tracker.PositionStep;
import org.opensourcephysics.cabrillo.tracker.Step;
import org.opensourcephysics.cabrillo.tracker.TFrame;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TapeStep;
import org.opensourcephysics.cabrillo.tracker.TrackerIO;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.VectorStep;
import org.opensourcephysics.controls.XML;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.Dataset;
import org.opensourcephysics.display.DatasetManager;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ClipControl;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.TPoint;
import org.opensourcephysics.media.core.Video;
import org.opensourcephysics.media.core.VideoClip;
import org.opensourcephysics.media.core.VideoPlayer;

/** Writes the canonical state of a TrackerPanel loaded by the pinned Tracker (oracles O3/O4).
 * The schema is defined in docs/oracle-dumps.md. The normal application loader opens the file in a
 * TFrame on a private Xvfb display, then the same draw-driven evaluation as TrackerSessionReference runs.
 * Tracker's frameless loader is deliberately not used: pinned 6.3.5 computes point-mass data only through
 * a TFrame, a failed attempt leaves partially refreshed datasets behind, and particle models evaluate only
 * when drawn. Live objects are serialized by their own Tracker/OSP savers, so every class, including
 * ones without special handling here, contributes all of its loader-visible properties.
 */
public final class TrackerStateDump {
    static final int SCHEMA_VERSION = 1;
    static final int LIMIT = 10000;
    static final String PREPARATION = "expand_visit_custom_tables_bounded_restore_selection";
    // Saved panel properties that are window geometry, dumped separately (tracks, views) or the
    // running application version rather than loaded state. Every other saved property is retained.
    static final Set<String> PANEL_EXCLUDED = Set.of("tracks", "semantic_version", "magnification", "center_x",
        "center_y", "dividers", "track_control_x", "track_control_y", "info_x", "info_y", "views",
        "selected_views", "selected_view_types", "selected_track_views", "toolbar");

    public static void main(String[] args) {
        int status = 1;
        try {
            if (args.length < 2 || args.length > 3)
                throw new IllegalArgumentException("Usage: TrackerStateDump input.trk|trz output.json [archive-member.trk]");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            Locale.setDefault(Locale.US);
            Path input = Path.of(args[0]).toAbsolutePath().normalize();
            Path output = Path.of(args[1]).toAbsolutePath();
            String member = args.length == 3 ? args[2] : null;
            if (member != null && !member.matches("[A-Za-z0-9_.-]+\\.trk"))
                throw new IllegalArgumentException("Invalid archive member");
            if (Files.exists(output)) throw new IllegalArgumentException("Output must be a fresh path");
            Path cache = TrackerSessionReference.initializeReferenceCache(Path.of(System.getProperty("user.home")));
            Map<String, Object> report = load(input, member, new Sanitizer(input.getParent(), cache));
            Files.writeString(output, json(report) + "\n", StandardCharsets.UTF_8);
            status = 0;
        } catch (Throwable error) {
            System.err.println("Tracker state dump input=" + (args.length > 0 ? args[0] : "<missing>")
                + " waiting=" + TrackerSessionReference.WAITING.get());
            error.printStackTrace();
        }
        // Tracker owns background services and AWT windows. Exit only this isolated process.
        System.exit(status);
    }

    private static Map<String, Object> load(Path input, String member, Sanitizer sanitizer) throws Exception {
        // Unhandled loader/EDT failures fail this dump immediately with their cause.
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            TrackerSessionReference.FAILURE.compareAndSet(null, error);
            TrackerSessionReference.DONE.countDown();
        });
        // Expected application requirements come from the original project XML, never the live load.
        Path metadata = member == null ? input : extractMember(input, member);
        var attachments = TrackerSessionReference.attachmentRequirements(metadata);
        var models = TrackerSessionReference.modelRequirements(metadata);
        AtomicReference<Map<String, Object>> result = new AtomicReference<>();
        // Customized tables are visited through Tracker's normal selection path, but only for a bounded
        // time: pinned 6.3.5 can leave columns unpopulated (#64), and that loaded state is what is dumped.
        TrackerSessionReference.boundedTableVisits = true;
        TrackerSessionReference.tableVisitPolls = Integer.getInteger("motrack.tableVisitPolls", 40);
        if (TrackerSessionReference.tableVisitPolls < 1 || TrackerSessionReference.tableVisitPolls > 800)
            throw new IllegalArgumentException("Table visit polls must be 1-800 (25 ms each, within the 30 s deadline)");
        SwingUtilities.invokeAndWait(TrackerSessionReference.guard(() -> {
            TFrame frame = new TFrame();
            // Pinned AsyncLoader can call its completion hook twice; dump only once.
            Runnable loaded = TrackerSessionReference.once(TrackerSessionReference.guard(() -> {
                TrackerPanel panel = selectPanel(frame, input, member);
                frame.setSelectedTab(panel);
                TrackerSessionReference.whenEvaluated(panel, attachments, models, () -> {
                    TrackerSessionReference.WAITING.set("state dump");
                    result.set(report(dumpPanel(panel, sanitizer)));
                    TrackerSessionReference.DONE.countDown();
                });
            }));
            TrackerIO.openURL(input.toString(), frame, loaded);
        }));
        if (!TrackerSessionReference.DONE.await(30, TimeUnit.SECONDS)) {
            TrackerSessionReference.printWaitingThreads();
            throw new IllegalStateException("Tracker state loading timed out: " + TrackerSessionReference.WAITING.get());
        }
        if (TrackerSessionReference.FAILURE.get() != null)
            throw new IllegalStateException("Tracker state load failed", TrackerSessionReference.FAILURE.get());
        if (result.get() == null) throw new IllegalStateException("Tracker produced no state dump");
        return result.get();
    }

    private static Path extractMember(Path archive, String member) throws IOException {
        try (ZipFile zip = new ZipFile(archive.toFile())) {
            ZipEntry entry = zip.getEntry(member);
            if (entry == null || entry.isDirectory() || entry.getSize() > 32L * 1024 * 1024)
                throw new IllegalArgumentException("Missing or oversized archive member");
            // A private temporary copy: the archive itself is opened only by Tracker's own loader.
            Path destination = Files.createTempFile("tracker-state-member-", ".trk");
            destination.toFile().deleteOnExit();
            try (InputStream stream = zip.getInputStream(entry)) {
                byte[] bytes = stream.readNBytes(32 * 1024 * 1024 + 1);
                if (bytes.length > 32 * 1024 * 1024) throw new IllegalArgumentException("Oversized archive member");
                Files.write(destination, bytes);
            }
            return destination;
        }
    }

    /** Finds the experiment tab Tracker created for the requested file or archive member. */
    private static TrackerPanel selectPanel(TFrame frame, Path input, String member) {
        if (member == null) {
            if (frame.getTabCount() != 1) throw new IllegalStateException("Expected exactly one loaded tab");
            return frame.getTrackerPanelForTab(0);
        }
        // Pinned TrackerIO titles an archive tab with its member basename, dropping a leading
        // "<archive basename>_" (for example BallToss.trz!/BallToss_BallToss.trk is titled BallToss).
        String parent = XML.stripExtension(input.getFileName().toString());
        String title = XML.stripExtension(member);
        if (title.startsWith(parent) && parent.length() + 1 < title.length()) title = title.substring(parent.length() + 1);
        TrackerPanel match = null;
        for (int i = 0; i < frame.getTabCount(); i++) {
            TrackerPanel panel = frame.getTrackerPanelForTab(i);
            if (panel != null && title.equals(panel.getTitle())) {
                if (match != null) throw new IllegalStateException("Ambiguous archive tab: " + title);
                match = panel;
            }
        }
        if (match == null) throw new IllegalStateException("Archive did not load member: " + member);
        return match;
    }

    static Map<String, Object> report(Map<String, Object> panel) {
        return TrackerReference.map("schema_version", SCHEMA_VERSION,
            "producer", TrackerReference.map("kind", "tracker", "version", OSPRuntime.VERSION,
                "loading", "tracker_session", "view_preparation", PREPARATION),
            "panel", panel);
    }

    static Map<String, Object> dumpPanel(TrackerPanel panel, Sanitizer sanitizer) {
        Map<String, Object> properties = canonicalControl(new XMLControlElement(panel), sanitizer);
        properties.keySet().removeAll(PANEL_EXCLUDED);
        // The clip control saves the player's current frame, which the evaluation seek moved to the
        // last clip step. It is preparation state, not loaded state.
        if (properties.get("clipcontrol") instanceof Map<?, ?> control) control.remove("frame");
        List<TTrack> live = panel.getTracks();
        if (live.size() > LIMIT) throw new IllegalStateException("Track limit exceeded");
        List<Object> tracks = new ArrayList<>();
        for (TTrack track : live) tracks.add(dumpTrack(panel, track, sanitizer));
        TTrack selected = panel.getSelectedTrack();
        return TrackerReference.map(
            "units", TrackerReference.map("length", panel.getLengthUnit(), "mass", panel.getMassUnit(),
                "time", panel.getTimeUnit(), "angles_in_radians", panel.isAnglesInRadians()),
            "selected_track", selected == null ? null : selected.getName(),
            "properties", properties,
            "clip", dumpClip(panel),
            "video", dumpVideo(panel.getVideo()),
            "coords", dumpCoords(panel.getCoords()),
            "tracks", tracks,
            "views", panel.getTFrame() == null ? null : TrackerSessionReference.savedViews(panel));
    }

    static Map<String, Object> dumpClip(TrackerPanel panel) {
        VideoPlayer player = panel.getPlayer();
        VideoClip clip = player.getVideoClip();
        ClipControl control = player.getClipControl();
        int count = clip.getStepCount();
        if (count < 0 || count > LIMIT) throw new IllegalStateException("Clip step limit exceeded");
        List<Object> steps = new ArrayList<>();
        for (int step = 0; step < count; step++) steps.add(List.of(step, clip.stepToFrame(step), number(player.getStepTime(step))));
        return TrackerReference.map("first_frame", clip.getStartFrameNumber(), "step_size", clip.getStepSize(),
            "step_count", count, "end_frame", clip.getEndFrameNumber(), "frame_count", clip.getFrameCount(),
            "frame_shift", clip.frameShift, "extra_frames", clip.getExtraFrames(),
            "start_time_ms", number(clip.getStartTime()), "frame_duration_ms", number(control.getMeanFrameDuration()),
            "rate", number(control.getRate()), "looping", control.isLooping(), "steps", steps);
    }

    static Map<String, Object> dumpVideo(Video video) {
        if (video == null) return null;
        int count = video.getFrameCount();
        if (count < 0 || count > LIMIT) throw new IllegalStateException("Video frame limit exceeded");
        List<Object> times = new ArrayList<>();
        for (int frame = 0; frame < count; frame++) times.add(number(video.getFrameTime(frame)));
        List<Object> filters = new ArrayList<>();
        for (Object filter : video.getFilterStack().getFilters()) filters.add(filter.getClass().getName());
        return TrackerReference.map("class", video.getClass().getName(), "frame_count", count,
            "start_frame", video.getStartFrameNumber(), "end_frame", video.getEndFrameNumber(),
            "frame_times_ms", times, "filters", filters);
    }

    /** The active coordinate system at every original frame: Tracker's parameters and its affine map. */
    static Map<String, Object> dumpCoords(ImageCoordSystem coords) {
        int length = coords.getLength();
        if (length < 1 || length > LIMIT) throw new IllegalStateException("Coordinate frame limit exceeded");
        List<Object> frames = new ArrayList<>();
        double[] matrix = new double[6];
        for (int frame = 0; frame < length; frame++) {
            AffineTransform world = coords.getToWorldTransform(frame);
            world.getMatrix(matrix);
            List<Object> transform = new ArrayList<>();
            for (double value : matrix) transform.add(number(value));
            frames.add(List.of(frame, number(coords.getOriginX(frame)), number(coords.getOriginY(frame)),
                number(coords.getAngle(frame)), number(coords.getScaleX(frame)), number(coords.getScaleY(frame)), transform));
        }
        return TrackerReference.map("class", coords.getClass().getName(), "fixed_origin", coords.isFixedOrigin(),
            "fixed_angle", coords.isFixedAngle(), "fixed_scale", coords.isFixedScale(), "locked", coords.isLocked(),
            "key_frames", new ArrayList<>(new TreeSet<>(coords.getKeyFrames())), "frames", frames);
    }

    static Map<String, Object> dumpTrack(TrackerPanel panel, TTrack track, Sanitizer sanitizer) {
        // Like TrackerPanel's saver, omit trailing null array elements, so each track's saved arrays
        // are exactly those Tracker writes into a project file.
        boolean writeNullFinal = XMLPropertyElement.defaultWriteNullFinalArrayElements;
        Map<String, Object> properties;
        try {
            XMLPropertyElement.defaultWriteNullFinalArrayElements = false;
            properties = canonicalControl(new XMLControlElement(track), sanitizer);
        } finally {
            XMLPropertyElement.defaultWriteNullFinalArrayElements = writeNullFinal;
        }
        properties.remove("@class");
        return TrackerReference.map("class", track.getClass().getName(), "name", track.getName(),
            "properties", properties, "steps", dumpSteps(track), "data", dumpData(panel, track));
    }

    static List<Object> dumpSteps(TTrack track) {
        List<Object> steps = new ArrayList<>();
        Step[] live = TrackerSessionReference.persistentSteps(track);
        if (live.length > LIMIT) throw new IllegalStateException("Step limit exceeded");
        for (Step step : live) {
            if (step == null) continue;
            Map<String, Object> item = TrackerReference.map("frame", step.getFrameNumber());
            // Name the scientific points of the known step types; their other TPoints are transient
            // drag handles. Other step types keep every point in Tracker's own order.
            if (step instanceof PositionStep position) {
                item.put("position", point(position.getPosition()));
            } else if (step instanceof VectorStep vector) {
                item.put("tail", point(vector.getTail()));
                item.put("tip", point(vector.getTip()));
                item.put("components", List.of(number(vector.getXComponent()), number(vector.getYComponent())));
            } else if (step instanceof TapeStep tape) {
                item.put("end1", point(tape.getEnd1()));
                item.put("end2", point(tape.getEnd2()));
            } else {
                List<Object> points = new ArrayList<>();
                for (TPoint value : step.getPoints()) points.add(point(value));
                item.put("points", points);
            }
            steps.add(item);
        }
        return steps;
    }

    private static Object point(TPoint point) {
        return point == null ? null : List.of(number(point.x), number(point.y));
    }

    /** A track's DatasetManager. Tracker builds every dataset of a track over the same independent rows. */
    static Map<String, Object> dumpData(TrackerPanel panel, TTrack track) {
        DatasetManager data = track.getData(panel);
        if (data == null) return null;
        List<Dataset> datasets = data.getDatasetsRaw();
        if (datasets.size() > 1000) throw new IllegalStateException("Dataset limit exceeded");
        List<Object> columns = new ArrayList<>();
        List<List<Object>> rows = new ArrayList<>();
        String independent = null;
        double[] shared = null;
        for (int index = 0; index < datasets.size(); index++) {
            Dataset dataset = datasets.get(index);
            double[] x = dataset.getXPoints(), y = dataset.getYPoints();
            if (x.length != y.length || x.length > LIMIT) throw new IllegalStateException("Invalid dataset size");
            if (index == 0) {
                independent = dataset.getXColumnName();
                shared = x;
                for (double value : x) rows.add(new ArrayList<>(List.of(number(value))));
                columns.add(column(track, independent));
            } else if (!dataset.getXColumnName().equals(independent) || !Arrays.equals(x, shared)) {
                // Fail rather than silently mispairing values; a new layout needs a schema change.
                throw new IllegalStateException("Dataset rows are not aligned for " + track.getName() + ": "
                    + dataset.getXColumnName() + "/" + dataset.getYColumnName());
            }
            for (int row = 0; row < y.length; row++) rows.get(row).add(number(y[row]));
            columns.add(column(track, dataset.getYColumnName()));
        }
        Map<String, Object> constants = new LinkedHashMap<>();
        for (String name : data.getConstantNames()) constants.put(name, number(data.getConstantValue(name)));
        return TrackerReference.map("columns", columns, "constants", constants, "rows", rows);
    }

    private static Map<String, Object> column(TTrack track, String name) {
        return TrackerReference.map("name", name, "dimensions", TTrack.getVariableDimensions(track, name));
    }

    /** JSON cannot carry IEEE specials; they are written as the strings NaN, Infinity and -Infinity. */
    static Object number(Double value) {
        if (value == null) return null;
        if (Double.isNaN(value)) return "NaN";
        if (Double.isInfinite(value)) return value > 0 ? "Infinity" : "-Infinity";
        return value;
    }

    /** OSP saver tree as JSON: "@class" names an object; arrays and collections keep their Java class. */
    static Map<String, Object> canonicalControl(XMLControl control, Sanitizer sanitizer) {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("@class", control.getObjectClassName());
        for (XMLProperty property : control.getPropsRaw()) {
            String name = property.getPropertyName();
            if (name.startsWith("@") || result.containsKey(name))
                throw new IllegalStateException("Duplicate or reserved saved property: " + name);
            result.put(name, canonicalProperty(property, sanitizer));
        }
        return result;
    }

    static Object canonicalProperty(XMLProperty property, Sanitizer sanitizer) {
        List<Object> content = property.getPropertyContent();
        switch (property.getPropertyType()) {
        case XMLProperty.TYPE_INT:
            return Integer.parseInt((String) content.get(0));
        case XMLProperty.TYPE_DOUBLE:
            return number(Double.parseDouble((String) content.get(0)));
        case XMLProperty.TYPE_BOOLEAN:
            return Boolean.parseBoolean((String) content.get(0));
        case XMLProperty.TYPE_STRING:
            return sanitizer.apply(text((String) content.get(0)));
        case XMLProperty.TYPE_OBJECT:
            if (content.isEmpty() || content.get(0) instanceof String) return null;
            return canonicalControl((XMLControl) content.get(0), sanitizer);
        case XMLProperty.TYPE_COLLECTION: {
            List<Object> items = new ArrayList<>();
            for (Object item : content) items.add(canonicalProperty((XMLProperty) item, sanitizer));
            return TrackerReference.map("@collection", className(property), "items", items);
        }
        case XMLProperty.TYPE_ARRAY:
        case XMLProperty.TYPE_WRAPPED_ARRAY: {
            String type = property.getPropertyType() == XMLProperty.TYPE_WRAPPED_ARRAY ? "[D" : className(property);
            List<Object> items = new ArrayList<>();
            if (content.size() == 1 && content.get(0) instanceof XMLProperty child && child.getPropertyName().equals("array")) {
                // Primitive arrays above XMLControlElement.compactArraySize are stored as "{a,b,...}".
                items = compactArray(text((String) child.getPropertyContent().get(0)), type);
            } else {
                for (Object item : content) {
                    XMLProperty element = (XMLProperty) item;
                    String index = element.getPropertyName();
                    if (!index.matches("\\[[0-9]{1,9}\\]")) throw new IllegalStateException("Invalid array element: " + index);
                    int position = Integer.parseInt(index.substring(1, index.length() - 1));
                    // OSP omits null elements, so absent indices are null.
                    if (position < items.size() || position > LIMIT) throw new IllegalStateException("Invalid array index");
                    while (items.size() < position) items.add(null);
                    items.add(canonicalProperty(element, sanitizer));
                }
            }
            return TrackerReference.map("@array", type, "items", items);
        }
        default:
            throw new IllegalStateException("Unknown saved property type: " + property.getPropertyName());
        }
    }

    private static String className(XMLProperty property) {
        Class<?> type = property.getPropertyClass();
        return type == null ? null : type.getName();
    }

    private static String text(String value) {
        if (value != null && value.startsWith(XML.CDATA_PRE) && value.endsWith(XML.CDATA_POST))
            return value.substring(XML.CDATA_PRE.length(), value.length() - XML.CDATA_POST.length());
        return value;
    }

    /** Parses OSP's compact primitive-array text, including nested braces of multidimensional arrays. */
    static List<Object> compactArray(String text, String type) {
        String trimmed = text.trim();
        int[] position = {0};
        Object value = compactValue(trimmed, position, type == null ? "" : type.replaceFirst("^\\[+", ""));
        if (position[0] != trimmed.length() || !(value instanceof List<?>))
            throw new IllegalStateException("Malformed compact array");
        @SuppressWarnings("unchecked")
        List<Object> list = (List<Object>) value;
        return list;
    }

    private static Object compactValue(String text, int[] position, String base) {
        if (position[0] < text.length() && text.charAt(position[0]) == '{') {
            position[0]++;
            List<Object> items = new ArrayList<>();
            if (position[0] < text.length() && text.charAt(position[0]) == '}') {
                position[0]++;
                return items;
            }
            while (true) {
                items.add(compactValue(text, position, base));
                if (items.size() > LIMIT) throw new IllegalStateException("Compact array limit exceeded");
                if (position[0] >= text.length()) throw new IllegalStateException("Unterminated compact array");
                char next = text.charAt(position[0]++);
                if (next == '}') return items;
                if (next != ',') throw new IllegalStateException("Malformed compact array");
            }
        }
        int start = position[0];
        while (position[0] < text.length() && text.charAt(position[0]) != ',' && text.charAt(position[0]) != '}') position[0]++;
        String token = text.substring(start, position[0]).trim();
        switch (base) {
        case "I": return Integer.parseInt(token);
        case "D": return number(Double.parseDouble(token));
        case "Z": return Boolean.parseBoolean(token);
        default: throw new IllegalStateException("Unsupported compact array type: " + base);
        }
    }

    /** Replaces this run's absolute input and cache locations with stable placeholders. */
    static final class Sanitizer {
        private final List<String[]> replacements = new ArrayList<>();

        Sanitizer(Path input, Path cache) {
            add(input, "$INPUT_DIR");
            add(cache, "$OSP_CACHE");
            // Longest first, so a URI form is replaced before the bare path it contains.
            replacements.sort((a, b) -> Integer.compare(b[0].length(), a[0].length()));
        }

        private void add(Path location, String placeholder) {
            if (location == null) return;
            String path = XML.forwardSlash(location.toAbsolutePath().normalize().toString());
            String bare = path.replaceFirst("^/+", "");
            for (String prefix : new String[] {"jar:file:///", "jar:file:/", "file:///", "file:/", "/"})
                replacements.add(new String[] {prefix + bare, placeholder});
            replacements.add(new String[] {bare, placeholder});
            replacements.add(new String[] {location.toAbsolutePath().normalize().toString(), placeholder});
        }

        String apply(String value) {
            if (value == null) return null;
            String result = value;
            for (String[] pair : replacements) result = result.replace(pair[0], pair[1]);
            return result;
        }
    }

    /** Compact deterministic JSON. Nonfinite numbers must already be encoded as strings. */
    static String json(Object value) {
        StringBuilder output = new StringBuilder();
        write(value, output);
        return output.toString();
    }

    private static void write(Object value, StringBuilder output) {
        if (value == null) output.append("null");
        else if (value instanceof Boolean b) output.append(b);
        else if (value instanceof Double d) {
            if (!Double.isFinite(d)) throw new IllegalArgumentException("Unencoded nonfinite value");
            output.append(d);
        } else if (value instanceof Integer || value instanceof Long) output.append(value);
        else if (value instanceof Map<?, ?> map) {
            output.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                if (!first) output.append(',');
                first = false;
                write(entry.getKey().toString(), output);
                output.append(':');
                write(entry.getValue(), output);
            }
            output.append('}');
        } else if (value instanceof List<?> list) {
            output.append('[');
            for (int i = 0; i < list.size(); i++) {
                if (i > 0) output.append(',');
                write(list.get(i), output);
            }
            output.append(']');
        } else if (value instanceof String text) {
            output.append('"');
            for (char c : text.toCharArray()) {
                if (c == '"' || c == '\\') output.append('\\').append(c);
                else if (c < 32) output.append(String.format("\\u%04x", (int) c));
                else output.append(c);
            }
            output.append('"');
        } else throw new IllegalArgumentException("Unsupported dump value: " + value.getClass().getName());
    }
}
