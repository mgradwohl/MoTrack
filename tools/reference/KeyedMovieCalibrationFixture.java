import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;

/** Adds public-API calibration keys to the independently authored, pinned four-frame MOV fixture. */
public final class KeyedMovieCalibrationFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 2 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5, linked-MOV directory and fresh output directory");
            Path source = Path.of(args[0]).toAbsolutePath();
            Path output = Path.of(args[1]).toAbsolutePath();
            Files.createDirectory(output);
            Locale.setDefault(Locale.US);
            // Bits 1/2/4 mean fixed origin/angle/scale. All-fixed geometry has its own accepted case.
            for (int flags = 0; flags < 7; ++flags) {
                Path directory = output.resolve("keyed-movie-" + flags);
                Files.createDirectory(directory);
                Files.copy(source.resolve("constant-rate.mov"), directory.resolve("constant-rate.mov"));
                ImageCoordSystem coords = new ImageCoordSystem(4);
                coords.setFixedOrigin((flags & 1) != 0);
                coords.setFixedAngle((flags & 2) != 0);
                coords.setFixedScale((flags & 4) != 0);
                coords.setOriginXY(0, 2, 20);
                coords.setScaleXY(0, 2, 4);
                coords.setOriginXY(2, 10, 18);
                coords.setAngle(2, Math.PI / 2);
                coords.setScaleXY(2, 4, 8);
                PointMass mass = new PointMass();
                mass.setName("movie mass");
                mass.setMass(2);
                for (int frame = 0; frame < 4; ++frame) mass.createStep(frame, 4, 16);
                // Reading controls does not construct a GUI/video factory. Linux application tests
                // independently load the complete fixture through the normal Tracker open path.
                XMLControlElement root = new XMLControlElement(source.resolve("linked-movie.trk").toString());
                if (root.failedToRead()) throw new IllegalStateException("Could not read pinned MOV controls");
                root.setValue("coords", coords);
                root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), mass)));
                Path path = directory.resolve("linked-movie.trk");
                if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write keyed movie fixture");
                Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            }
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
