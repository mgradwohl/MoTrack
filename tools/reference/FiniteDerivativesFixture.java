import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.cabrillo.tracker.PointMass;
import org.opensourcephysics.cabrillo.tracker.TTrack;
import org.opensourcephysics.cabrillo.tracker.TrackerPanel;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLPropertyElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.media.core.ClipControl;
import org.opensourcephysics.media.core.ImageCoordSystem;
import org.opensourcephysics.media.core.VideoClip;

/** Authors the media-free finite-derivative case (MoTrack #332) through the pinned Java serializers.
 * A calibrated point mass has 15 marks on a 16-step clip (start frame 4, step size 3, 30 fps, start time 400 ms)
 * with step 8 (frame 28) missing, so Tracker's own refreshData yields finite velocities and five-point
 * accelerations on both sides of a one-step gap. Image positions are polynomials in the step number k with dyadic
 * coefficients, so the saved marks are exact and the image-space stencils have closed forms:
 * x = 100 + 16k + 0.75k² − 0.0625k³ + k⁴/256 and y = 400 − 40k + 2.5k² + 0.03125k³ pixels. The quartic term tells
 * Tracker's five-point acceleration apart from three-point and five-point central differences, which agree on a
 * cubic. The fixed calibration has its origin at (252.5, 318.75), a 30° angle and 125 and 100 pixels per metre.
 */
public final class FiniteDerivativesFixture {
    static final int START_FRAME = 4;
    static final int STEP_SIZE = 3;
    static final int STEP_COUNT = 16;
    static final int MISSING_STEP = 8;

    public static void main(String[] args) {
        try {
            if (args.length != 1 || !OSPRuntime.VERSION.equals("6.3.5"))
                throw new IllegalArgumentException("Usage (Tracker 6.3.5): FiniteDerivativesFixture fresh-output.trk");
            Path path = Path.of(args[0]).toAbsolutePath();
            if (Files.exists(path)) throw new IllegalArgumentException("Fixture output already exists");
            Locale.setDefault(Locale.US);
            // Like TrackerPanel's saver, omit trailing null array elements: they would set every array's Java length.
            XMLPropertyElement.defaultWriteNullFinalArrayElements = false;
            VideoClip clip = new VideoClip(null);
            clip.setStepCount(STEP_COUNT);
            clip.setStartFrameNumber(START_FRAME);
            clip.setStepSize(STEP_SIZE);
            clip.setStepCount(STEP_COUNT);
            clip.setStartTime(400);
            if (clip.getStartFrameNumber() != START_FRAME || clip.getStepSize() != STEP_SIZE || clip.getStepCount() != STEP_COUNT
                    || clip.getEndFrameNumber() != START_FRAME + (STEP_COUNT - 1) * STEP_SIZE || clip.getStartTime() != 400)
                throw new IllegalStateException("Unexpected clip: start " + clip.getStartFrameNumber() + ", step size "
                    + clip.getStepSize() + ", count " + clip.getStepCount() + ", end " + clip.getEndFrameNumber());
            // A media-free clip is played by a StepperClipControl, whose saver writes rate, delta_t and frame. A new
            // control reports frame 0 until it is stepped; select step 0 (frame 4), as an application save would.
            ClipControl control = ClipControl.getControl(clip);
            control.setFrameDuration(1000.0 / 30);
            control.setStepNumber(0);
            if (control.getMeanFrameDuration() != 1000.0 / 30 || control.getFrameNumber() != START_FRAME)
                throw new IllegalStateException("Unexpected clip control");
            int frames = START_FRAME + STEP_COUNT * STEP_SIZE;
            ImageCoordSystem coords = new ImageCoordSystem(frames);
            coords.setOriginXY(0, 252.5, 318.75);
            coords.setAngle(0, Math.PI / 6);
            coords.setScaleXY(0, 125, 100);
            PointMass mass = new PointMass();
            mass.setName("projectile");
            mass.setMass(0.75);
            for (int k = 0; k < STEP_COUNT; k++) {
                if (k == MISSING_STEP) continue;
                mass.createStep(START_FRAME + k * STEP_SIZE,
                    100 + 16 * k + 0.75 * k * k - 0.0625 * k * k * k + k * k * k * k / 256.0,
                    400 - 40 * k + 2.5 * k * k + 0.03125 * k * k * k);
            }
            XMLControlElement root = new XMLControlElement(TrackerPanel.class);
            root.setValue("semantic_version", OSPRuntime.VERSION);
            root.setValue("videoclip", clip);
            root.setValue("clipcontrol", control);
            root.setValue("coords", coords);
            root.setValue("length_unit", "m");
            root.setValue("mass_unit", "kg");
            root.setValue("tracks", new ArrayList<TTrack>(List.of(new CoordAxes(), mass)));
            if (root.write(path.toString()) == null) throw new IllegalStateException("Could not write fixture");
            // The repository pins LF bytes on every host, independently of the Java host newline.
            Files.writeString(path, Files.readString(path, StandardCharsets.UTF_8).replace("\r\n", "\n"), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }
}
