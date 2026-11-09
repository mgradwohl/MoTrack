import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.LocalDateTime;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Packages pinned Tracker-authored movie and image experiments without modifying their XML. */
public final class ArchiveMovieFixture {
    private record Member(String name, byte[] bytes) {}

    private static Member read(Path root, String source, String target) throws Exception {
        Path path = root.resolve(source);
        if (!Files.isRegularFile(path) || Files.size(path) > 1024 * 1024)
            throw new IllegalArgumentException("Missing or oversized fixture input: " + source);
        return new Member(target, Files.readAllBytes(path));
    }

    public static void main(String[] args) throws Exception {
        if ((args.length != 3 && args.length != 4) || (args.length == 4 && !args[3].equals("mov")))
            throw new IllegalArgumentException("Usage: ArchiveMovieFixture linked-movie-directory linked-image-directory fresh-output.trz [mov]");
        Path movies = Path.of(args[0]);
        Path images = Path.of(args[1]);
        List<Member> members = new ArrayList<>();
        members.add(read(movies, "linked-movie.trk", "experiment.trk"));
        members.add(read(images, "secondary-images.trk", "second.trk"));
        String movieName = args.length == 4 ? "constant-rate.mov" : "constant-rate.avi";
        members.add(read(movies, movieName, movieName));
        for (int i = 0; i < 4; i++) members.add(read(images, "images/frame" + i + ".png", "images/frame" + i + ".png"));
        members.add(new Member("support/notes é.txt", "Untouched movie and image experiments: α, β.\n".getBytes(StandardCharsets.UTF_8)));
        members.add(new Member("support/opaque.bin", new byte[] {0, 1, 2, 127, (byte) 128, (byte) 255}));
        try (var output = new ZipOutputStream(Files.newOutputStream(Path.of(args[2]), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE), StandardCharsets.UTF_8)) {
            output.setComment("MoTrack movie archive reference; preserve both experiments and every media byte.");
            for (Member member : members) {
                ZipEntry entry = new ZipEntry(member.name());
                entry.setTimeLocal(LocalDateTime.of(1980, 1, 2, 0, 0));
                entry.setComment("Original metadata: " + member.name());
                output.putNextEntry(entry);
                output.write(member.bytes());
                output.closeEntry();
            }
        }
    }
}
