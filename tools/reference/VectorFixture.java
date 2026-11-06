import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.Vector;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Authors a synthetic reference fixture through the pinned Java serializers, independently of MoTrack. */
public final class VectorFixture {
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
            first.createStep(5, 30, 40, -5, 12);
            Vector second = new Vector();
            second.setName("vector B");
            second.createStep(2, 50, 60, 0, 0);
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(10);
            ImageCoordSystem coords = new ImageCoordSystem(10);
            coords.setOriginXY(0, 320, 240);
            coords.setScaleXY(0, 2, 4);
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            root.setValue("coords", coords);
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), first, second)));
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            // The repository pins LF bytes on every host, independently of the Java host newline.
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
