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

/** Two independent masses on the pinned MOV, authored through the public Tracker 6.3.5 API. */
public final class MovieTracksFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 2 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5, linked-MOV directory and fresh output directory");
            Path source = Path.of(args[0]).toAbsolutePath();
            Path output = Path.of(args[1]).toAbsolutePath();
            Files.createDirectory(output);
            Locale.setDefault(Locale.US);
            PointMass first = new PointMass();
            first.setName("movie mass");
            first.setMass(2);
            first.createStep(0, 4, 16);
            first.createStep(2, 12, 4);
            PointMass second = new PointMass();
            second.setName("other mass");
            second.setMass(3);
            second.createStep(1, 6, 8);
            second.createStep(3, 10, 12);
            // Read XML controls only; no GUI/video factory on the authoring machine.
            XMLControlElement root = new XMLControlElement(source.resolve("linked-movie.trk").toString());
            if (root.failedToRead()) throw new IllegalStateException("Could not read pinned MOV controls");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), first, second)));
            root.setValue("description", "movie mass and other mass are words in this note.");
            Files.copy(source.resolve("constant-rate.mov"), output.resolve("constant-rate.mov"));
            Path path = output.resolve("linked-movie.trk");
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write movie tracks fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
