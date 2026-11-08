import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Authors all six partly fixed coordinate systems with pinned public Java setters/serializers. */
public final class MixedCalibrationFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output directory");
            Path directory = Path.of(args[0]).toAbsolutePath();
            Files.createDirectory(directory);
            Locale.setDefault(Locale.US);
            // Bits 1/2/4 mean fixed origin/angle/scale. 0 and 7 have separate existing cases.
            for (int flags = 1; flags < 7; ++flags) {
                VideoClip clip = new VideoClip(null);
                clip.setStepCount(10);
                ImageCoordSystem coords = new ImageCoordSystem(10);
                coords.setFixedOrigin((flags & 1) != 0);
                coords.setFixedAngle((flags & 2) != 0);
                coords.setFixedScale((flags & 4) != 0);
                coords.setOriginXY(0, 320, 240);
                coords.setScaleXY(0, 2, 4);
                coords.setOriginXY(4, 100, 120);
                coords.setAngle(4, Math.PI / 2);
                coords.setScaleXY(4, 4, 8);
                coords.setOriginXY(9, 110, 130);
                coords.setAngle(9, -Math.PI / 2);
                coords.setScaleXY(9, 2, 8);
                PointMass mass = new PointMass();
                mass.setName("calibration probe");
                mass.setMass(2.5);
                for (int frame : new int[] {0, 3, 4, 8, 9}) mass.createStep(frame, 10, 20);
                XMLControlElement root = new XMLControlElement(TrackerPanel.class);
                root.setValue("semantic_version", OSPRuntime.VERSION);
                root.setValue("videoclip", clip);
                root.setValue("coords", coords);
                root.setValue("length_unit", "m");
                root.setValue("mass_unit", "kg");
                root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), mass)));
                Path path = directory.resolve("mixed-calibration-" + flags + ".trk");
                if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
                Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            }
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
