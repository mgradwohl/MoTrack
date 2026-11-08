import java.awt.image.BufferedImage;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import javax.imageio.ImageIO;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.ImageVideo;
import org.opensourcephysics.media.core.StepperClipControl;
import org.opensourcephysics.media.core.VideoClip;

/** Authors tiny media and OSP controls headlessly; Linux tests load the real ImageVideo/application. */
public final class LinkedImagesFixture {
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
            if ((args.length != 1 && args.length != 2) || !OSPRuntime.VERSION.equals("6.3.5")
                    || (args.length == 2 && !args[1].equals("secondary")))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output directory [secondary]");
            boolean secondary = args.length == 2;
            Path directory = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(directory)) throw new IllegalArgumentException("Output directory already exists");
            Files.createDirectories(directory.resolve("images"));
            Locale.setDefault(Locale.US);
            String[] paths = new String[4];
            for (int frame = 0; frame < 4; frame++) {
                BufferedImage image = new BufferedImage(32, 24, BufferedImage.TYPE_INT_RGB);
                for (int y = 0; y < 24; y++) for (int x = 0; x < 32; x++)
                    image.setRGB(x, y, ((x + frame * 16) << 16) | (y << 8) | (frame * 32));
                paths[frame] = "images/frame" + frame + ".png";
                if (!ImageIO.write(image, "png", directory.resolve(paths[frame]).toFile()))
                    throw new IllegalStateException("PNG writer unavailable");
            }
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(4);
            clip.setStartTime(-250);
            ImageCoordSystem coords = new ImageCoordSystem(4);
            coords.setOriginXY(0, 2, 20);
            coords.setScaleXY(0, 2, 4);
            PointMass mass = new PointMass();
            mass.setName(secondary ? "other image mass" : "image mass");
            mass.setMass(secondary ? 3.5 : 2);
            mass.createStep(0, 4, 16);
            mass.createStep(2, 12, 4);
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            // ImageVideo's file constructor initializes the GUI-only media factory. The pinned
            // loader saves paths, first path and delta_t; author those public XML values here.
            XMLControlElement video = object((XMLControlElement) root.getChildControl("videoclip"), "video", ImageVideo.class);
            video.setValue("paths", paths);
            video.setValue("path", paths[0]);
            video.setValue("delta_t", 125.0);
            XMLControlElement control = object(root, "clipcontrol", StepperClipControl.class);
            control.setValue("rate", 1.0);
            control.setValue("delta_t", 125.0);
            control.setValue("frame", 0);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), mass)));
            root.setValue("selectedtrack", mass.getName());
            Path path = directory.resolve("linked-images.trk");
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
