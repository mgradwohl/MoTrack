import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;

/** Reads with the pinned OSP loader and evaluates coordinates with its actual Java implementation.
 * This deliberately does not claim to load a complete GUI Tracker session or validate an export.
 */
public final class TrackerReference {
    private static final String TRACKER = "org.opensourcephysics.cabrillo.tracker.";

    public static void main(String[] args) {
        try {
            if (args.length != 2) throw new IllegalArgumentException("Usage: TrackerReference input.trk output.json");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            // Use Tracker's resource reader. The File constructor decodes with the platform default
            // charset instead, which corrupts UTF-8 labels on Java 17 Windows.
            XMLControlElement root = new XMLControlElement(Path.of(args[0]).toString());
            if (root.failedToRead() || !root.getObjectClassName().equals(TRACKER + "TrackerPanel"))
                throw new IllegalArgumentException("Reference rejected the TrackerPanel document");
            Map<String, Object> report = map("reference_version", OSPRuntime.VERSION,
                "verification_level", "xml_and_coordinate_api", "project", snapshot(root));
            Files.writeString(Path.of(args[1]), json(report) + "\n", StandardCharsets.UTF_8);
            root.dispose();
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    private static Map<String, Object> snapshot(XMLControlElement root) {
        XMLControl clip = root.getChildControl("videoclip");
        int first = integer(clip, "startframe", 0);
        int stride = integer(clip, "stepsize", 1);
        Integer count = has(clip, "stepcount") ? clip.getInt("stepcount") : null;
        if (first < 0 || stride < 1 || (count != null && count < 0))
            throw new IllegalArgumentException("Invalid clip range");
        ImageCoordSystem coords = has(root, "coords") ? (ImageCoordSystem) root.getObject("coords") : null;
        List<Object> masses = new ArrayList<>();
        List<Object> vectors = new ArrayList<>();
        List<Object> unsupported = new ArrayList<>();
        for (XMLControl track : objects(property(root, "tracks"))) {
            String type = track.getObjectClassName();
            boolean mass = type.equals(TRACKER + "PointMass");
            boolean vector = type.equals(TRACKER + "Vector");
            if (!mass && !vector) {
                unsupported.add(type);
                continue;
            }
            List<Object> samples = new ArrayList<>();
            for (XMLControl sample : objects(property(track, "framedata"))) {
                String index = sample.getPropertyName();
                int frame = Integer.parseInt(index.substring(1, index.length() - 1));
                if (mass) {
                    double x = sample.getDouble("x");
                    double y = sample.getDouble("y");
                    samples.add(map("frame", frame, "image", List.of(x, y), "world", world(coords, frame, x, y)));
                } else {
                    samples.add(map("frame", frame,
                        "tail", List.of(sample.getDouble("xtail"), sample.getDouble("ytail")),
                        "components", List.of(sample.getDouble("xcomponent"), sample.getDouble("ycomponent"))));
                }
            }
            Map<String, Object> item = map("name", string(track, "name", ""), "samples", samples);
            if (mass) {
                item.put("mass", number(track, "mass", 1.0));
                masses.add(item);
            } else vectors.add(item);
        }
        List<Object> coordinateSamples = new ArrayList<>();
        long last = count == null || count == 0 ? first : first + (long) stride * (count - 1);
        if (last > Integer.MAX_VALUE) throw new IllegalArgumentException("Clip frame overflow");
        for (int frame : new int[] {first, (int) last})
            coordinateSamples.add(map("frame", frame, "image", List.of(13.5, 17.25),
                "world", world(coords, frame, 13.5, 17.25)));
        Map<String, Object> result = map("source_version", string(root, "semantic_version", ""),
            "length_unit", string(root, "length_unit", ""), "mass_unit", string(root, "mass_unit", "kg"),
            "time_unit", string(root, "time_unit", "s"),
            "clip", map("first_frame", first, "step_size", stride, "step_count", count,
                "start_ms", number(clip, "starttime", 0.0)),
            "point_masses", masses, "vectors", vectors, "unsupported_tracks", unsupported,
            "coordinate_samples", coordinateSamples);
        if (coords != null) coords.dispose();
        return result;
    }

    private static Object world(ImageCoordSystem coords, int frame, double x, double y) {
        return coords == null ? null : List.of(coords.imageToWorldX(frame, x, y), coords.imageToWorldY(frame, x, y));
    }

    static XMLProperty property(XMLControl control, String name) {
        if (control != null) for (XMLProperty value : control.getPropsRaw())
            if (value.getPropertyName().equals(name)) return value;
        return null;
    }

    // Stop at each object: nested color, frame and function objects are not sibling tracks.
    static List<XMLControl> objects(XMLProperty property) {
        List<XMLControl> result = new ArrayList<>();
        if (property != null) for (Object value : property.getPropertyContent()) {
            if (value instanceof XMLControl control) result.add(control);
            else if (value instanceof XMLProperty child) result.addAll(objects(child));
        }
        return result;
    }

    private static boolean has(XMLControl control, String name) { return property(control, name) != null; }
    private static String string(XMLControl c, String name, String fallback) { return has(c, name) ? c.getString(name) : fallback; }
    private static int integer(XMLControl c, String name, int fallback) { return has(c, name) ? c.getInt(name) : fallback; }
    private static double number(XMLControl c, String name, double fallback) { return has(c, name) ? c.getDouble(name) : fallback; }

    static Map<String, Object> map(Object... entries) {
        Map<String, Object> result = new LinkedHashMap<>();
        for (int i = 0; i < entries.length; i += 2) result.put((String) entries[i], entries[i + 1]);
        return result;
    }

    static String json(Object value) {
        if (value == null) return "null";
        if (value instanceof Boolean b) return b.toString();
        if (value instanceof Number n) {
            if (!Double.isFinite(n.doubleValue())) throw new IllegalArgumentException("Nonfinite reference value");
            return n.toString();
        }
        if (value instanceof Map<?, ?> m) {
            List<String> parts = new ArrayList<>();
            for (Map.Entry<?, ?> e : m.entrySet()) parts.add(json(e.getKey()) + ":" + json(e.getValue()));
            return "{" + String.join(",", parts) + "}";
        }
        if (value instanceof List<?> values) {
            List<String> parts = new ArrayList<>();
            for (Object item : values) parts.add(json(item));
            return "[" + String.join(",", parts) + "]";
        }
        StringBuilder result = new StringBuilder("\"");
        for (char c : value.toString().toCharArray()) {
            if (c == '"' || c == '\\') result.append('\\').append(c);
            else if (c < 32) result.append(String.format("\\u%04x", (int) c));
            else result.append(c);
        }
        return result.append('"').toString();
    }
}
