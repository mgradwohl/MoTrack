import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.DynamicParticle;
import org.opensourcephysics.cabrillo.tracker.DynamicSystem;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Headless source authoring; normal model/dependency evaluation is tested separately in Tracker. */
public final class ModelDependenciesFixture {
    private static List<XMLControl> objects(XMLProperty property) {
        List<XMLControl> result = new ArrayList<>();
        for (Object value : property.getPropertyContent()) {
            if (value instanceof XMLControl control) result.add(control);
            else if (value instanceof XMLProperty child) result.addAll(objects(child));
        }
        return result;
    }

    public static void main(String[] args) {
        try {
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output path");
            Path path = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(path)) throw new IllegalArgumentException("Fixture output already exists");
            Locale.setDefault(Locale.US);
            PointMass mass = new PointMass();
            mass.setName("mass A");
            mass.setMass(2);
            for (int frame = 0; frame < 3; frame++) mass.createStep(frame, 26, 32);
            DynamicParticle first = new DynamicParticle();
            first.setName("particle A");
            first.setMass(1);
            first.getInitEditor().setExpression("x", "1", false);
            first.getInitEditor().setExpression("y", "2", false);
            DynamicParticle second = new DynamicParticle();
            second.setName("particle B");
            second.setMass(3);
            second.getInitEditor().setExpression("x", "100", false);
            second.getInitEditor().setExpression("y", "100", false);
            // Nonempty system construction needs a live TrackerPanel. Serialize an empty
            // system, then author its named links through OSP controls, not private fields.
            DynamicSystem system = new DynamicSystem();
            system.setName("system");
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(3);
            ImageCoordSystem coords = new ImageCoordSystem(3);
            coords.setOriginXY(0, 20, 40);
            coords.setScaleXY(0, 2, 2);
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), mass, first, second, system)));
            root.setValue("selectedtrack", "system");
            XMLProperty tracks = root.getPropsRaw().stream().filter(p -> p.getPropertyName().equals("tracks")).findFirst().orElseThrow();
            for (XMLControl track : objects(tracks)) {
                String name = track.getString("name");
                if (name.equals("particle A") || name.equals("particle B")) track.setValue("in_system", true);
                if (name.equals("particle B")) track.setValue("booster", "mass A");
                if (name.equals("system")) track.setValue("particles", new String[] {"particle A", "particle B"});
            }
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
