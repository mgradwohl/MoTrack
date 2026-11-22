import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.opensourcephysics.controls.XML;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.controls.XMLProperty;
import org.opensourcephysics.display.OSPRuntime;

/** Oracle O1: dumps the control tree that the pinned OSP XMLControlElement reader builds.
 *
 * Usage: XmlControlDump pairs.txt, where each line is "input<TAB>output". Every input is read with the
 * resource reader Tracker uses (UTF-8) and written as canonical JSON (schema in docs/osp-xmlcontrol.md).
 * XML.NEW_LINE is set to "\n" so multi-line values do not depend on the platform line separator. The tool
 * only reads documents; it never loads Tracker objects from them.
 */
public final class XmlControlDump {
    static final String SCHEMA = "motrack.osp-xmlcontrol-dump/1";
    private static Field className;
    private static Method arrayValue;

    public static void main(String[] args) {
        try {
            boolean generate = args.length == 2 && args[0].equals("--generate");
            if (args.length != 1 && !generate)
                throw new IllegalArgumentException("Usage: XmlControlDump pairs.txt | --generate directory");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            XML.NEW_LINE = "\n";
            if (generate) {
                generate(Path.of(args[1]));
                System.exit(0);
            }
            className = Class.forName("org.opensourcephysics.controls.XMLNode").getDeclaredField("className");
            className.setAccessible(true);
            arrayValue = XMLControlElement.class.getDeclaredMethod("arrayValue", String.class, Class.class);
            arrayValue.setAccessible(true);
            // OSP 6.3.5 prints every compact array it decodes (XMLControlElement line 2003).
            PrintStream console = System.out;
            System.setOut(new PrintStream(OutputStream.nullOutputStream()));
            int count = 0;
            for (String line : Files.readAllLines(Path.of(args[0]), StandardCharsets.UTF_8)) {
                if (line.isEmpty()) continue;
                String[] pair = line.split("\t");
                if (pair.length != 2) throw new IllegalArgumentException("Invalid pair line");
                dump(Path.of(pair[0]), Path.of(pair[1]));
                count++;
            }
            console.println("Dumped " + count + " OSP documents");
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    /** Documents written by OSP's own writer (XMLControlElement.toXML), used as reader inputs and as expected
     * output for MoTrack's writer. */
    private static void generate(Path directory) throws Exception {
        Files.createDirectories(directory);
        // The document of OSP's swingJS demo src/test/SetXMLControlTest.java, without its tree dialog.
        XMLControlElement demo = new XMLControlElement();
        demo.setValue("name", "myName");
        demo.setValue("pi", 3.14);
        demo.setValue("data", new double[] {10.0, 20.0, 30.0});
        demo.setValue("data2", new double[][] {{1, 10.0}, {2, 20.0}, {3, 30.0}});
        Files.writeString(directory.resolve("set-xml-control-test.xml"), demo.toXML(), StandardCharsets.UTF_8);
        // The remaining XMLPropertyElement writer branches.
        XMLControlElement values = new XMLControlElement();
        values.setVersion("6.3.5");
        values.setValue("text", "Ball & \"toss\" <1>");
        values.setValue("empty", "");
        values.setValue("multiline", "first\nsecond");
        values.setValue("count", -7);
        values.setValue("small", 1.0E-5);
        values.setValue("large", 1.0E7);
        values.setValue("shown", true);
        values.setValue("no ints", new int[0]);
        values.setValue("flags", new boolean[] {true, false});
        values.setValue("rows", new double[][] {null, {0.5}});
        values.setValue("labels", new String[] {"a", null, "c", null});
        values.setValue("list", new ArrayList<Object>(List.of("x", 2.5, 3)));
        values.setValue("point", new java.awt.Point(3, 4));
        Files.writeString(directory.resolve("osp-writer-values.xml"), values.toXML(), StandardCharsets.UTF_8);
        // Reference defect: OSP writes a later empty row as "{}", which arrayValue cannot read back
        // ("".split(",") yields one empty field); boolean rows silently gain a false element.
        XMLControlElement jagged = new XMLControlElement();
        jagged.setValue("ints", new int[][] {{1, 2}, {}});
        jagged.setValue("doubles", new double[][] {{1.5}, {}});
        jagged.setValue("booleans", new boolean[][] {{true}, {}});
        Files.writeString(directory.resolve("osp-jagged-arrays.xml"), jagged.toXML(), StandardCharsets.UTF_8);
    }

    private static void dump(Path input, Path output) throws Exception {
        // The String constructor reads a named resource through ResourceLoader's UTF-8 reader.
        XMLControlElement root = new XMLControlElement(input.toString());
        Map<String, Object> report = new LinkedHashMap<>();
        report.put("schema", SCHEMA);
        report.put("read", !root.failedToRead());
        if (!root.failedToRead()) report.put("root", object(root, true));
        Files.writeString(output, json(report) + "\n", StandardCharsets.UTF_8);
        root.dispose();
    }

    private static Map<String, Object> object(XMLControl control, boolean isRoot) throws Exception {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("class", control.getObjectClassName());
        if (isRoot) result.put("version", ((XMLControlElement) control).getVersion());
        List<Object> properties = new ArrayList<>();
        for (XMLProperty property : control.getPropsRaw()) properties.add(property(property));
        result.put("properties", properties);
        return result;
    }

    private static Map<String, Object> property(XMLProperty property) throws Exception {
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("name", property.getPropertyName());
        int type = property.getPropertyType();
        result.put("type", typeName(type));
        List<Object> content = property.getPropertyContent();
        switch (type) {
        case XMLProperty.TYPE_ARRAY:
        case XMLProperty.TYPE_COLLECTION: {
            String name = (String) className.get(property);
            result.put("class", name);
            List<Object> elements = new ArrayList<>();
            for (Object element : content) elements.add(property((XMLProperty) element));
            result.put("elements", elements);
            // The JVM allows at most 255 array dimensions; MoTrack applies the same bound.
            if (type == XMLProperty.TYPE_ARRAY && name.matches("\\[{1,255}[IDZ]") && !content.isEmpty()) {
                XMLProperty first = (XMLProperty) content.get(0);
                if ("array".equals(first.getPropertyName()) && isText(first.getPropertyType()))
                    result.put("compact", compact((String) first.getPropertyContent().get(0), name));
            }
            break;
        }
        case XMLProperty.TYPE_OBJECT:
            result.put("object", content.isEmpty() ? null : object((XMLControl) content.get(0), false));
            break;
        default: {
            String text = (String) content.get(0);
            result.put("content", text);
            if (type == XMLProperty.TYPE_INT) {
                Object value;
                try {
                    value = Integer.parseInt(text);
                } catch (NumberFormatException error) {
                    value = null;
                }
                result.put("int", value);
            } else if (type == XMLProperty.TYPE_DOUBLE) {
                Object value;
                try {
                    value = bits(Double.parseDouble(text));
                } catch (NumberFormatException error) {
                    value = null;
                }
                result.put("double", value);
            } else if (type == XMLProperty.TYPE_BOOLEAN) {
                result.put("boolean", text.equals("true"));
            } else if (type == XMLProperty.TYPE_STRING) {
                Object value;
                try {
                    value = XML.removeCDATA(text);
                } catch (RuntimeException error) {
                    value = null;
                }
                result.put("string", value);
            }
        }
        }
        return result;
    }

    private static boolean isText(int type) {
        return type != XMLProperty.TYPE_ARRAY && type != XMLProperty.TYPE_COLLECTION && type != XMLProperty.TYPE_OBJECT;
    }

    /** XMLControlElement.arrayValue(String, Class) with the component type that the array class names. */
    private static Object compact(String text, String arrayClass) throws Exception {
        Object value;
        try {
            value = arrayValue.invoke(null, text, Class.forName(arrayClass).getComponentType());
        } catch (InvocationTargetException | ClassNotFoundException error) {
            // Java cannot decode this value (or load its class); either way the dump records no array.
            return null;
        }
        return compactValue(value);
    }

    private static Object compactValue(Object value) {
        if (value == null) return null;
        List<Object> result = new ArrayList<>();
        if (value instanceof int[]) {
            for (int item : (int[]) value) result.add(item);
        } else if (value instanceof double[]) {
            for (double item : (double[]) value) result.add(bits(item));
        } else if (value instanceof boolean[]) {
            for (boolean item : (boolean[]) value) result.add(item);
        } else {
            for (Object item : (Object[]) value) result.add(compactValue(item));
        }
        return result;
    }

    private static String bits(double value) {
        return String.format("%016x", Double.doubleToLongBits(value));
    }

    private static String typeName(int type) {
        switch (type) {
        case XMLProperty.TYPE_INT: return "int";
        case XMLProperty.TYPE_DOUBLE: return "double";
        case XMLProperty.TYPE_BOOLEAN: return "boolean";
        case XMLProperty.TYPE_STRING: return "string";
        case XMLProperty.TYPE_ARRAY: return "array";
        case XMLProperty.TYPE_COLLECTION: return "collection";
        case XMLProperty.TYPE_OBJECT: return "object";
        default: return "unknown";
        }
    }

    @SuppressWarnings("unchecked")
    static String json(Object value) {
        if (value == null) return "null";
        if (value instanceof String) return quote((String) value);
        if (value instanceof Boolean || value instanceof Integer) return value.toString();
        StringBuilder result = new StringBuilder();
        if (value instanceof Map) {
            result.append('{');
            boolean first = true;
            for (Map.Entry<String, Object> entry : ((Map<String, Object>) value).entrySet()) {
                if (!first) result.append(',');
                first = false;
                result.append(quote(entry.getKey())).append(':').append(json(entry.getValue()));
            }
            return result.append('}').toString();
        }
        result.append('[');
        boolean first = true;
        for (Object item : (List<Object>) value) {
            if (!first) result.append(',');
            first = false;
            result.append(json(item));
        }
        return result.append(']').toString();
    }

    private static String quote(String text) {
        StringBuilder result = new StringBuilder("\"");
        for (int index = 0; index < text.length(); index++) {
            char character = text.charAt(index);
            if (character == '"' || character == '\\') result.append('\\').append(character);
            else if (character < 0x20) result.append(String.format("\\u%04x", (int) character));
            else result.append(character);
        }
        return result.append('"').toString();
    }
}
