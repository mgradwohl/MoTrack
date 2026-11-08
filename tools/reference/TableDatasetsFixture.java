import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.LineProfile;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TableTView;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TView;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Source-structure fixture only: no media, live table, or application resave acceptance. */
public final class TableDatasetsFixture {
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
            PointMass mass = new PointMass();
            mass.setName("mass A");
            mass.setMass(2);
            mass.createStep(0, 22, 32);
            mass.createStep(2, 30, 16);
            LineProfile first = new LineProfile();
            first.setName("profile A");
            first.createStep(0, 10, 20, 30, 20);
            LineProfile second = new LineProfile();
            second.setName("profile B");
            second.createStep(0, 15, 25, 35, 25);
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), mass, first, second)));
            root.setValue("selectedtrack", "profile A");
            root.setValue("views", new TView[][] {new TView[0]});
            XMLProperty views = root.getPropsRaw().stream().filter(p -> p.getPropertyName().equals("views")).findFirst().orElseThrow();
            XMLProperty chooser = (XMLProperty) views.getPropertyContent().get(0);
            XMLPropertyElement slot = new XMLPropertyElement(chooser, "[1]", XMLProperty.TYPE_OBJECT, null);
            XMLControlElement table = new XMLControlElement(slot);
            table.setObjectClass(TableTView.class);
            slot.getPropertyContent().clear();
            slot.getPropertyContent().add(table);
            chooser.getPropertyContent().add(slot);
            table.setValue("selected_track", "profile A");
            table.setValue("track_columns", new String[][] {{"profile A", "n"}, {"profile B", "n"}});
            String[][] indices = {{"profile B", "3"}, {"profile A", "2"}};
            table.setValue("dataset_indices", indices);
            // Exercise the pinned OSP String[][] codec; this does not load a TableTView.
            XMLControlElement decoded = new XMLControlElement();
            decoded.readXML(table.toXML());
            if (!Arrays.deepEquals(indices, (String[][]) decoded.getObject("dataset_indices")))
                throw new IllegalStateException("Dataset rows did not survive the OSP XML codec");
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
