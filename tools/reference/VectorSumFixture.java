import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.Vector;
import org.opensourcephysics.cabrillo.tracker.VectorSum;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Authors a named vector dependency through the pinned Java APIs and serializers. */
public final class VectorSumFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output path");
            Path path = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(path)) throw new IllegalArgumentException("Fixture output already exists");
            Locale.setDefault(Locale.US);
            Vector first = new Vector();
            first.setName("vector A");
            first.createStep(0, 10, 20, 3, -4);
            first.createStep(1, 30, 40, 5, 6);
            first.createStep(2, 50, 60, -5, 12);
            first.createStep(4, 70, 80, 4, 8);
            Vector second = new Vector();
            second.setName("vector B");
            second.createStep(0, 100, 120, 7, 8);
            second.createStep(2, 130, 140, 5, -12);
            second.createStep(3, 160, 170, 1, 2);
            second.createStep(4, 190, 200, -6, -4);
            VectorSum sum = new VectorSum();
            sum.setName("resultant");
            Vector cached = new Vector();
            for (int frame : new int[] {0, 2, 4}) cached.createStep(frame, 240 + 20 * frame, 260 + 10 * frame, 0, 0);
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(5);
            ImageCoordSystem coords = new ImageCoordSystem(5);
            coords.setOriginXY(0, 400, 400);
            coords.setScaleXY(0, 2, 4);
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), first, second, sum)));
            root.setValue("selectedtrack", "resultant");
            // VectorSum.addVector needs a live TrackerPanel to repaint. Author the pending member
            // names with OSP's own XML API and seed tails with its Vector FrameData serializer.
            // Cached components are deliberately zero: the application test must resolve members
            // and recompute the nonzero sums instead of merely accepting serialized geometry.
            boolean found = false;
            for (XMLProperty property : root.getPropsRaw()) if (property.getPropertyName().equals("tracks"))
                for (XMLControl child : property.getChildControls()) if (child.getObjectClassName().equals(VectorSum.class.getName())) {
                    child.setValue("vectors", new ArrayList<String>(List.of(first.getName(), second.getName())));
                    child.setValue("framedata", new XMLControlElement(cached).getObject("framedata"));
                    found = true;
                }
            if (!found) throw new IllegalStateException("Missing serialized vector sum");
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
