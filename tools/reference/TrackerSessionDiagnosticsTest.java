import java.util.ArrayList;
import java.util.List;

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
        System.out.println("Tracker diagnostic frame selection passed (headless, bounded)");
    }
}
