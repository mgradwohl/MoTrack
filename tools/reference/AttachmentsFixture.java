import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TapeMeasure;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Headless authoring through Tracker serializers; application loading/evaluation is tested separately. */
public final class AttachmentsFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output path");
            Path path = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(path)) throw new IllegalArgumentException("Fixture output already exists");
            Locale.setDefault(Locale.US);
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(3);
            ImageCoordSystem coords = new ImageCoordSystem(3);
            coords.setOriginXY(0, 20, 40);
            coords.setScaleXY(0, 2, 2);
            PointMass first = new PointMass();
            first.setName("mass A");
            first.setMass(2);
            PointMass second = new PointMass();
            second.setName("mass B");
            second.setMass(3);
            for (int frame = 0; frame < 3; frame++) {
                first.createStep(frame, 20 + 2 * frame, 40);
                second.createStep(frame, 26 + 5 * frame, 48 + 4 * frame);
            }
            TapeMeasure both = new TapeMeasure();
            both.setName("both ends");
            both.setReadOnly(true);
            both.createStep(0, 100, 100, 110, 100); // Deliberately stale endpoints.
            both.getAttachments()[0] = first;
            both.getAttachments()[1] = second;
            TapeMeasure one = new TapeMeasure();
            one.setName("one end");
            one.setReadOnly(true);
            one.createStep(0, 100, 100, 40, 40); // End 1 must follow A; end 2 remains fixed.
            one.getAttachments()[0] = first;
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), first, second, both, one)));
            root.setValue("selectedtrack", "both ends");
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
