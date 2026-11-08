import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.TableTView;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TView;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Headless fixture authoring; actual view loading/saving is tested separately in the application. */
public final class TableOwnersFixture {
    private static XMLProperty property(XMLControlElement owner, String name) {
        return owner.getPropsRaw().stream().filter(p -> p.getPropertyName().equals(name)).findFirst()
            .orElseThrow(() -> new IllegalStateException("Missing fixture property: " + name));
    }

    // Views need a live TFrame. Author their control structure with OSP's public XML API,
    // without constructing or repairing live views. Geometry uses the real track serializers.
    private static XMLControlElement object(XMLProperty parent, String name, Class<?> type) {
        XMLPropertyElement property = new XMLPropertyElement(parent, name, XMLProperty.TYPE_OBJECT, null);
        XMLControlElement control = new XMLControlElement(property);
        control.setObjectClass(type);
        property.getPropertyContent().clear();
        property.getPropertyContent().add(control);
        if (parent instanceof XMLControlElement owner) owner.getPropsRaw().add(property);
        else parent.getPropertyContent().add(property);
        return control;
    }

    public static void main(String[] args) {
        try {
            boolean subset = args.length == 2 && args[1].equals("subset");
            boolean defaults = args.length == 2 && args[1].equals("defaults");
            if ((args.length != 1 && !subset && !defaults) || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5, one fresh output path and optional subset/defaults mode");
            Path path = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(path)) throw new IllegalArgumentException("Fixture output already exists");
            Locale.setDefault(Locale.US);
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(4);
            ImageCoordSystem coords = new ImageCoordSystem(4);
            coords.setOriginXY(0, 20, 40);
            coords.setScaleXY(0, 2, 4);
            PointMass first = new PointMass();
            first.setName("mass A");
            first.setMass(2);
            first.createStep(0, 22, 32);
            first.createStep(2, 30, 16);
            PointMass second = new PointMass();
            second.setName("mass B");
            second.setMass(3);
            second.createStep(0, 40, 60);
            second.createStep(2, 44, 68);
            PointMass third = new PointMass();
            third.setName("mass C");
            third.setMass(4);
            third.createStep(1, 10, 20);
            third.createStep(3, 14, 28);
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), first, second, third)));
            root.setValue("selectedtrack", "mass B");
            root.setValue("selected_view_types", new int[] {TView.VIEW_TABLE, TView.VIEW_TABLE, TView.VIEW_WORLD, TView.VIEW_PAGE});
            root.setValue("views", new TView[][] {new TView[0]});
            XMLProperty chooser = (XMLProperty) property(root, "views").getPropertyContent().get(0);
            XMLControlElement table = object(chooser, "[1]", TableTView.class);
            table.setValue("selected_track", "mass B");
            table.setValue("track_columns", new String[][] {
                subset || defaults ? new String[] {"mass A", "t", "x", "y"} : new String[] {"mass A", "t", "x", "y", "frame"},
                subset || defaults ? new String[] {"mass B", "t", "y", "x"} : new String[] {"mass B", "t", "y", "x", "frame"},
                subset ? new String[] {"mass C", "t", "x"} : defaults ? new String[] {"mass C", "t", "x", "y"}
                    : new String[] {"mass C", "t", "x", "y", "frame"}});
            table.setValue("column_formats", new String[][][] {
                {{"mass A", "x", "0.000"}, {"mass A", "y", "0.00"}},
                {{"mass B", "y", "0.0000"}, {"mass B", "t", "0.00"}},
                {{"mass C", "x", "0.00000"}}});
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
