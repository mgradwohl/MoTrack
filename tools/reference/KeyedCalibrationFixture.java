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

/** Authors a small varying-calibration reference using the pinned Java serializers. */
public final class KeyedCalibrationFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output path");
            Path path = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(path)) throw new IllegalArgumentException("Fixture output already exists");
            Locale.setDefault(Locale.US);
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(10);
            ImageCoordSystem coords = new ImageCoordSystem(10);
            coords.setFixedOrigin(false);
            coords.setFixedAngle(false);
            coords.setFixedScale(false);
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
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
