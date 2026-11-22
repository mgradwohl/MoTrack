import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.Vector;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Authors the frame-data loader fixtures (MoTrack #313, #314) through the pinned Java serializers.
 * Tracker 6.3.5's savers write neither a clip `frameshift` nor duplicate array indices, so each mode makes one
 * documented change to the serialized document: `frame-shift` adds `frameshift` 2 to the saved VideoClip through
 * the XMLControl API; `duplicate-coords` renames the coordinate key [1] to a second [0] and appends a null [3].
 */
public final class FrameDataFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 2 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Usage (Tracker 6.3.5): FrameDataFixture frame-shift|duplicate-coords fresh-output.trk");
            String mode = args[0];
            if (!List.of("frame-shift", "duplicate-coords").contains(mode)) throw new IllegalArgumentException("Unknown fixture mode");
            Path path = Path.of(args[1]).toAbsolutePath();
            if (Files.exists(path)) throw new IllegalArgumentException("Fixture output already exists");
            Locale.setDefault(Locale.US);
            // Like TrackerPanel's saver, omit trailing null array elements: they would set every array's Java length.
            XMLPropertyElement.defaultWriteNullFinalArrayElements = false;
            XMLControlElement root = mode.equals("frame-shift") ? frameShift() : duplicateCoords();
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            // The repository pins LF bytes on every host, independently of the Java host newline.
            String text = Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n");
            if (mode.equals("duplicate-coords")) {
                String keyClass = "<object class=\"org.opensourcephysics.media.core.ImageCoordSystem$FrameData\">";
                text = once(text, "<property name=\"[1]\" type=\"object\">\n            " + keyClass,
                    "<property name=\"[0]\" type=\"object\">\n            " + keyClass);
                text = once(text, "        </property>\n    </object>\n    </property>\n    <property name=\"length_unit\"",
                    "            <property name=\"[3]\" type=\"object\">null</property>\n"
                        + "        </property>\n    </object>\n    </property>\n    <property name=\"length_unit\"");
            }
            Files.writeString(path, text, StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    /** Replaces the only occurrence of `from`, so a changed serializer layout fails instead of authoring another file. */
    private static String once(String text, String from, String to) {
        int first = text.indexOf(from);
        if (first < 0 || text.indexOf(from, first + 1) >= 0) throw new IllegalStateException("Expected one occurrence of: " + from);
        return text.substring(0, first) + to + text.substring(first + from.length());
    }

    private static XMLControlElement panel(VideoClip clip, ImageCoordSystem coords, List<TTrack> tracks) {
        XMLControlElement root = new XMLControlElement(TrackerPanel.class);
        root.setValue("semantic_version", OSPRuntime.VERSION);
        root.setValue("videoclip", clip);
        root.setValue("coords", coords);
        root.setValue("length_unit", "m");
        root.setValue("mass_unit", "kg");
        root.setValue("tracks", new ArrayList<TTrack>(tracks));
        return root;
    }

    /** Six clip steps from frame 3, keys at frames 0 and 4, and tracks whose arrays end before, at and after the shift. */
    private static XMLControlElement frameShift() {
        VideoClip clip = new VideoClip(null);
        clip.setStepCount(6);
        clip.setStartFrameNumber(3);
        clip.setStepCount(6);
        ImageCoordSystem coords = new ImageCoordSystem(10);
        coords.setFixedOrigin(false);
        coords.setFixedAngle(false);
        coords.setFixedScale(false);
        coords.setOriginXY(0, 320, 240);
        coords.setScaleXY(0, 2, 4);
        coords.setOriginXY(4, 100, 120);
        coords.setAngle(4, Math.PI / 2);
        coords.setScaleXY(4, 4, 8);
        PointMass shifted = new PointMass();
        shifted.setName("mass A");
        shifted.setMass(2);
        // Each saved position encodes its saved frame n: (10 + 10n, 20 + 5n).
        for (int frame : new int[] {0, 1, 2, 3, 5, 7}) shifted.createStep(frame, 10 + 10 * frame, 20 + 5 * frame);
        PointMass single = new PointMass();
        single.setName("single");
        single.createStep(0, 10, 30);
        PointMass dropped = new PointMass();
        dropped.setName("dropped");
        dropped.createStep(1, 40, 50);
        Vector vector = new Vector();
        vector.setName("vector A");
        vector.createStep(1, 30, 60, 4, -8);
        vector.createStep(4, 60, 90, 2, 8);
        XMLControlElement root = panel(clip, coords, List.of(new CoordAxes(), shifted, single, dropped, vector));
        root.getChildControl("videoclip").setValue("frameshift", 2);
        return root;
    }

    /** Keys at frames 0, 1 and 3; the document's key [1] becomes a second [0] and a null [3] follows the key [3]. */
    private static XMLControlElement duplicateCoords() {
        VideoClip clip = new VideoClip(null);
        clip.setStepCount(5);
        ImageCoordSystem coords = new ImageCoordSystem(10);
        coords.setFixedOrigin(false);
        coords.setFixedAngle(false);
        coords.setFixedScale(false);
        coords.setOriginXY(0, 320, 240);
        coords.setScaleXY(0, 2, 4);
        coords.setOriginXY(1, 100, 50);
        coords.setScaleXY(1, 5, 10);
        coords.setOriginXY(3, 10, 10);
        coords.setScaleXY(3, 8, 8);
        PointMass mass = new PointMass();
        mass.setName("mass A");
        mass.setMass(1.5);
        for (int frame = 0; frame < 5; frame++) mass.createStep(frame, 10 + 10 * frame, 20);
        return panel(clip, coords, List.of(new CoordAxes(), mass));
    }
}
