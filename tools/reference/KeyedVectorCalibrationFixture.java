import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;

/** Public-API changing coordinates over frozen independent-vector image/MOV controls. */
public final class KeyedVectorCalibrationFixture {
    public static void main(String[] args) {
        try {
            if (args.length != 3 || !OSPRuntime.VERSION.equals("6.3.5") || !List.of("images", "movie").contains(args[2]))
                throw new IllegalArgumentException("Expected Tracker 6.3.5, vector source directory, fresh output directory, images|movie");
            Path source = Path.of(args[0]).toAbsolutePath();
            Path output = Path.of(args[1]).toAbsolutePath();
            Files.createDirectory(output);
            Locale.setDefault(Locale.US);
            for (int flags = 0; flags < 7; ++flags) {
                Path directory = output.resolve("keyed-vector-" + args[2] + "-" + flags);
                Files.createDirectory(directory);
                ImageCoordSystem coords = new ImageCoordSystem(4);
                coords.setFixedOrigin((flags & 1) != 0);
                coords.setFixedAngle((flags & 2) != 0);
                coords.setFixedScale((flags & 4) != 0);
                coords.setOriginXY(0, 2, 20);
                coords.setScaleXY(0, 2, 4);
                coords.setOriginXY(2, 10, 18);
                coords.setAngle(2, Math.PI / 2);
                coords.setScaleXY(2, 4, 8);
                String project = args[2].equals("movie") ? "linked-movie.trk" : "linked-images.trk";
                XMLControlElement root = new XMLControlElement(source.resolve(project).toString());
                if (root.failedToRead()) throw new IllegalStateException("Could not read pinned vector controls");
                root.setValue("coords", coords);
                if (args[2].equals("movie")) {
                    Files.copy(source.resolve("constant-rate.mov"), directory.resolve("constant-rate.mov"));
                } else {
                    Files.createDirectory(directory.resolve("images"));
                    for (int frame = 0; frame < 4; ++frame)
                        Files.copy(source.resolve("images/frame" + frame + ".png"), directory.resolve("images/frame" + frame + ".png"));
                }
                Path path = directory.resolve(project);
                if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write keyed vector fixture");
                Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            }
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
