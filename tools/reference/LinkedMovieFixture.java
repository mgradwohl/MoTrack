import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.xuggle.XuggleVideo;
import org.opensourcephysics.media.core.VideoClipControl;
import org.opensourcephysics.media.core.VideoClip;

/** Authors an exact four-frame raw AVI and OSP controls; Linux tests load the real movie/application. */
public final class LinkedMovieFixture {
    private static byte[] bytes(String value) { return value.getBytes(StandardCharsets.US_ASCII); }
    private static ByteBuffer buffer(int size) { return ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN); }
    private static byte[] join(byte[]... values) {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        for (byte[] value : values) output.writeBytes(value);
        return output.toByteArray();
    }
    private static byte[] chunk(String tag, byte[] content) {
        ByteBuffer output = buffer(8 + content.length + content.length % 2);
        output.put(bytes(tag)).putInt(content.length).put(content);
        return output.array();
    }
    private static byte[] list(String type, byte[]... chunks) {
        return chunk("LIST", join(bytes(type), join(chunks)));
    }
    private static byte[] movie() {
        // Classic RIFF AVI: Microsoft AVI RIFF File Reference, AVIMAINHEADER and AVIOLDINDEX.
        // One bottom-up BGR24 stream, 32x24 pixels, four frames at exactly eight frames/second.
        int width = 32, height = 24, count = 4, rate = 8, frameBytes = width * height * 3;
        ByteBuffer main = buffer(56);
        for (int value : new int[] {125000, frameBytes * rate, 0, 0x10, count, 0, 1, frameBytes, width, height, 0, 0, 0, 0})
            main.putInt(value);
        ByteBuffer stream = buffer(56);
        stream.put(bytes("vids")).put(bytes("DIB ")).putInt(0).putShort((short) 0).putShort((short) 0);
        for (int value : new int[] {0, 1, rate, 0, count, frameBytes, -1, 0}) stream.putInt(value);
        stream.putShort((short) 0).putShort((short) 0).putShort((short) width).putShort((short) height);
        ByteBuffer bitmap = buffer(40);
        bitmap.putInt(40).putInt(width).putInt(height).putShort((short) 1).putShort((short) 24);
        for (int value : new int[] {0, frameBytes, 0, 0, 0, 0}) bitmap.putInt(value);
        byte[][] frames = new byte[count][];
        ByteBuffer index = buffer(count * 16);
        int offset = 4; // Index offsets are relative to the movi LIST data, including its FOURCC.
        for (int frame = 0; frame < count; frame++) {
            ByteBuffer pixels = buffer(frameBytes);
            for (int y = height - 1; y >= 0; y--) for (int x = 0; x < width; x++)
                pixels.put((byte) (frame * 32)).put((byte) y).put((byte) (x + frame * 16));
            frames[frame] = chunk("00db", pixels.array());
            index.put(bytes("00db")).putInt(0x10).putInt(offset).putInt(frameBytes);
            offset += frames[frame].length;
        }
        return chunk("RIFF", join(bytes("AVI "),
            list("hdrl", chunk("avih", main.array()), list("strl", chunk("strh", stream.array()), chunk("strf", bitmap.array()))),
            list("movi", frames), chunk("idx1", index.array())));
    }

    private static XMLControlElement object(XMLControlElement parent, String name, Class<?> type) {
        XMLPropertyElement property = new XMLPropertyElement(parent, name, XMLProperty.TYPE_OBJECT, null);
        XMLControlElement control = new XMLControlElement(property);
        control.setObjectClass(type);
        property.getPropertyContent().clear();
        property.getPropertyContent().add(control);
        parent.getPropsRaw().add(property);
        return control;
    }

    public static void main(String[] args) {
        try {
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output directory");
            Path directory = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(directory)) throw new IllegalArgumentException("Output directory already exists");
            Files.createDirectories(directory);
            Files.write(directory.resolve("constant-rate.avi"), movie());
            Locale.setDefault(Locale.US);
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(4);
            clip.setStartTime(-250);
            ImageCoordSystem coords = new ImageCoordSystem(4);
            coords.setOriginXY(0, 2, 20);
            coords.setScaleXY(0, 2, 4);
            PointMass mass = new PointMass();
            mass.setName("movie mass");
            mass.setMass(2);
            mass.createStep(0, 4, 16);
            mass.createStep(2, 12, 4);
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            // The application/decoder is exercised separately under Linux Xvfb. Author the public
            // movie path control headlessly, without constructing a GUI-backed video factory here.
            XMLControlElement video = object((XMLControlElement) root.getChildControl("videoclip"), "video", XuggleVideo.class);
            video.setValue("path", "constant-rate.avi");
            XMLControlElement control = object(root, "clipcontrol", VideoClipControl.class);
            control.setValue("rate", 1.0);
            control.setValue("delta_t", 125.0);
            control.setValue("frame", 0);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), mass)));
            root.setValue("selectedtrack", mass.getName());
            Path path = directory.resolve("linked-movie.trk");
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
