import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.PlotTView;
import org.opensourcephysics.cabrillo.tracker.PlotTrackView;
import org.opensourcephysics.cabrillo.tracker.TrackPlottingPanel;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TView;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Headless fixture authoring; actual view loading/saving is tested separately in the application. */
public final class PlotGuestsFixture {
    private static XMLProperty property(XMLControlElement owner, String name) {
        return owner.getPropsRaw().stream().filter(p -> p.getPropertyName().equals(name)).findFirst().orElseThrow();
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
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Expected Tracker 6.3.5 and one fresh output path");
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
            root.setValue("selectedtrack", "mass A");
            root.setValue("views", new TView[][] {new TView[0]});
            XMLProperty chooser = (XMLProperty) property(root, "views").getPropertyContent().get(0);
            XMLControlElement view = object(chooser, "[0]", PlotTView.class);
            view.setValue("selected_track", "mass A");
            view.setValue("track_views", new ArrayList<>());
            XMLControlElement owner = object(property(view, "track_views"), "item", PlotTrackView.class);
            owner.setValue("track", "mass A");
            owner.setValue("linked", false);
            for (int i = 0; i < 2; i++) {
                XMLControlElement plot = object(owner, "plot" + i, TrackPlottingPanel.class);
                plot.setValue("x_var", "t");
                plot.setValue("y_var", i == 0 ? "x" : "y");
                plot.setValue("lines", true);
                plot.setValue("points", true);
                plot.setValue("guests", i == 0 ? new String[] {"mass C", "mass B"} : new String[] {"mass B"});
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
