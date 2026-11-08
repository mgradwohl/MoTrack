import java.util.ArrayList;
import java.util.List;
import org.opensourcephysics.cabrillo.tracker.CoordAxes;
import org.opensourcephysics.controls.XMLControlElement;
import java.util.concurrent.atomic.AtomicInteger;

/** Headless checks for bounded stack selection; never constructs or operates Tracker windows. */
public final class TrackerSessionDiagnosticsTest {
    private static StackTraceElement frame(String name, int line) {
        return new StackTraceElement(name, "method", "Example.java", line);
    }

    public static void main(String[] args) {
        if (!Boolean.getBoolean("java.awt.headless")) throw new AssertionError("Diagnostics test must run headless");
        for (int count = 0; count < 20; ++count) {
            List<StackTraceElement> platform = new ArrayList<>();
            for (int i = 0; i < count; ++i) platform.add(frame("java.awt.Dialog", i));
            List<StackTraceElement> actual = TrackerSessionReference.diagnosticFrames(platform.toArray(StackTraceElement[]::new));
            if (!actual.equals(platform.subList(0, Math.min(8, count)))) throw new AssertionError("Platform bounds/order");
        }
        List<StackTraceElement> stack = new ArrayList<>();
        for (int i = 0; i < 12; ++i) stack.add(frame("javax.swing.JOptionPane", i));
        List<StackTraceElement> callers = List.of(frame("org.opensourcephysics.cabrillo.tracker.TrackerIO$AsyncLoader", 100),
            frame("org.opensourcephysics.controls.XMLControlElement", 200), frame("TrackerSessionReference", 300));
        stack.addAll(callers);
        stack.add(frame("org.opensourcephysics.cabrillo.tracker.Other", 400));
        List<StackTraceElement> actual = TrackerSessionReference.diagnosticFrames(stack.toArray(StackTraceElement[]::new));
        List<StackTraceElement> expected = new ArrayList<>(callers);
        expected.addAll(stack.subList(0, 5));
        if (!actual.equals(expected)) throw new AssertionError("Buried application callers missing or unbounded");
        StackTraceElement repeated = callers.get(0);
        actual = TrackerSessionReference.diagnosticFrames(new StackTraceElement[] {repeated, repeated, stack.get(0)});
        if (!actual.equals(List.of(repeated, stack.get(0)))) throw new AssertionError("Duplicate frame handling");
        AtomicInteger actions = new AtomicInteger();
        Runnable completed = TrackerSessionReference.once(actions::incrementAndGet);
        completed.run();
        completed.run();
        if (actions.get() != 1) throw new AssertionError("Duplicate callback repeated the reference action");
        Runnable[] reentrant = new Runnable[1];
        reentrant[0] = TrackerSessionReference.once(() -> { actions.incrementAndGet(); reentrant[0].run(); });
        reentrant[0].run();
        if (actions.get() != 2) throw new AssertionError("Reentrant callback repeated the reference action");
        RuntimeException failure = new IllegalStateException("expected");
        completed = TrackerSessionReference.once(() -> { actions.incrementAndGet(); throw failure; });
        try { completed.run(); throw new AssertionError("Failure was swallowed"); }
        catch (RuntimeException error) { if (error != failure) throw new AssertionError("Failure changed"); }
        completed.run();
        if (actions.get() != 3) throw new AssertionError("Failed reference action was retried");
        CoordAxes axes = new CoordAxes();
        String original = new XMLControlElement(axes).toXML();
        axes.getStep(0).getPoints()[0].setLocation(99, 88);
        if (!original.equals(new XMLControlElement(axes).toXML())
                || TrackerSessionReference.persistentSteps(axes).length != 0)
            throw new AssertionError("Transient axes handles entered persistent geometry");
        if (!TrackerSessionReference.axesGrid(axes).equals(TrackerReference.map("visible", false, "alpha", null, "argb", null)))
            throw new AssertionError("Default axes grid changed");
        XMLControlElement custom = new XMLControlElement(axes);
        custom.setValue("grid_visible", true);
        custom.setValue("grid_alpha", 73);
        custom.setValue("grid_RGB", 0xff123456);
        custom.loadObject(axes);
        if (!TrackerSessionReference.axesGrid(axes).equals(TrackerReference.map("visible", true, "alpha", 73, "argb", 0x49123456)))
            throw new AssertionError("Persistent axes grid settings were lost: " + TrackerSessionReference.axesGrid(axes));
        System.out.println("Tracker axes serializer/geometry boundary passed (headless)");
        System.out.println("Tracker diagnostic frame selection passed (headless, bounded)");
        System.out.println("Tracker reference completion passed (duplicate/reentrant/failed callbacks, headless)");
    }
}
