import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.LocalDateTime;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Deterministic standard Java ZIP packaging of the existing Tracker-authored image experiment. */
public final class ArchiveImagesFixture {
    private record Member(String name, byte[] bytes) {}

    private static Member read(Path root, String source, String target) throws Exception {
        Path path = root.resolve(source);
        if (!Files.isRegularFile(path) || Files.size(path) > 1024 * 1024)
            throw new IllegalArgumentException("Missing or oversized fixture input: " + source);
        return new Member(target, Files.readAllBytes(path));
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2 && args.length != 3)
            throw new IllegalArgumentException("Usage: ArchiveImagesFixture linked-image-directory fresh-output.trz [second-experiment.trk]");
        Path root = Path.of(args[0]);
        List<Member> members = new ArrayList<>();
        members.add(read(root, "linked-images.trk", "experiment.trk"));
        if (args.length == 3) {
            Path second = Path.of(args[2]).toAbsolutePath();
            members.add(read(second.getParent(), second.getFileName().toString(), "second.trk"));
        }
        for (int i = 0; i < 4; i++) members.add(read(root, "images/frame" + i + ".png", "images/frame" + i + ".png"));
        members.add(new Member("support/notes é.txt", "Untouched supporting document: α, β and image measurements.\n".getBytes(StandardCharsets.UTF_8)));
        members.add(new Member("support/opaque.bin", new byte[] {0, 1, 2, 127, (byte) 128, (byte) 255}));
        try (var output = new ZipOutputStream(Files.newOutputStream(Path.of(args[1]), StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE), StandardCharsets.UTF_8)) {
            output.setComment("MoTrack archive-preservation reference; comments must survive.");
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
