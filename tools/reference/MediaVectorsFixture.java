import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.Vector;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;

/** Public-API independent vectors over frozen image/MOV controls; no GUI/video loading. */
public final class MediaVectorsFixture {
    public static void main(String[] args) {
        try {
            if ((args.length != 3 && args.length != 4) || !OSPRuntime.VERSION.equals("6.3.5")
                    || !List.of("images", "movie").contains(args[2])
                    || (args.length == 4 && !args[3].equals("negative-time")))
                throw new IllegalArgumentException("Expected Tracker 6.3.5, source directory, fresh output directory, images|movie [negative-time]");
            Path source = Path.of(args[0]).toAbsolutePath();
            Path output = Path.of(args[1]).toAbsolutePath();
            Files.createDirectory(output);
            Locale.setDefault(Locale.US);
            Vector first = new Vector();
            first.setName("vector A");
            first.createStep(0, 20, 4, 3, 4);
            first.createStep(2, 16, 4, -4, -2);
            Vector second = new Vector();
            second.setName("vector B");
            second.createStep(1, 20, 20, -3, 0);
            second.createStep(3, 30, 20, 0, 0);
            Vector empty = new Vector();
            empty.setName("empty vector");
            String project = args[2].equals("movie") ? "linked-movie.trk" : "linked-images.trk";
            XMLControlElement root = new XMLControlElement(source.resolve(project).toString());
            if (root.failedToRead()) throw new IllegalStateException("Could not read pinned media controls");
            // Keep the original negative-time variant as an explicit reference-defect fixture.
            // Ordinary acceptance starts at zero so every in-clip vector has a live analysis row.
            if (args.length == 3) root.getChildControl("videoclip").setValue("starttime", 0.0);
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), first, second, empty)));
            root.setValue("selectedtrack", first.getName());
            root.setValue("description", "vector A and vector B are words in this note.");
            if (args[2].equals("movie")) {
                Files.copy(source.resolve("constant-rate.mov"), output.resolve("constant-rate.mov"));
            } else {
                Files.createDirectory(output.resolve("images"));
                for (int i = 0; i < 4; i++)
                    Files.copy(source.resolve("images/frame" + i + ".png"), output.resolve("images/frame" + i + ".png"));
            }
            Path path = output.resolve(project);
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write media vector fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
