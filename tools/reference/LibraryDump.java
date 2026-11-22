import java.io.OutputStream;
import java.io.PrintStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import javax.xml.parsers.DocumentBuilderFactory;
import org.opensourcephysics.controls.XML;
import org.opensourcephysics.controls.XMLControl;
import org.opensourcephysics.controls.XMLControlElement;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.tools.Library;
import org.opensourcephysics.tools.LibraryCollection;
import org.opensourcephysics.tools.LibraryComPADRE;
import org.opensourcephysics.tools.LibraryResource;
import org.opensourcephysics.tools.ResourceLoader;
import org.w3c.dom.Document;
import org.w3c.dom.NodeList;

/** Library oracle: dumps what the pinned OSP Digital Library classes load (docs/library-collections.md).
 *
 * Usage: LibraryDump pairs.txt, where each line is "kind<TAB>input<TAB>output" and kind is collection, library,
 * compadre-tree or compadre-records; or LibraryDump --generate DIRECTORY. Collection and library files are read
 * with XMLControlElement and loaded with LibraryResource/LibraryCollection/Library's own loaders, as the Library
 * Browser does. ComPADRE responses are passed to LibraryComPADRE.load and loadNode (as Tracker sets
 * desiredOSPType = "Tracker"), offline: the files are local and thumbnails are not cached. Collections are also
 * written back with XMLControlElement.toXML. The tool never opens a network connection of its own.
 */
public final class LibraryDump {
    static final String SCHEMA = "motrack.library-dump/1";
    static final String RELOAD_URL = "https://www.compadre.org/osp/services/REST/osp_tracker.cfm?OSPType=Tracker&OSPSubject=1";

    public static void main(String[] args) {
        try {
            boolean generate = args.length == 2 && args[0].equals("--generate");
            if (args.length != 1 && !generate)
                throw new IllegalArgumentException("Usage: LibraryDump pairs.txt | --generate directory");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            Locale.setDefault(Locale.US);
            XML.NEW_LINE = "\n";
            headlessLog();
            if (generate) {
                generate(Path.of(args[1]));
                System.exit(0);
            }
            // Library.Loader adopts a file's cache only while none is set; keep the fixtures from choosing one.
            ResourceLoader.setOSPCache(System.getProperty("java.io.tmpdir"));
            PrintStream console = System.out;
            System.setOut(new PrintStream(OutputStream.nullOutputStream()));
            int count = 0;
            for (String line : Files.readAllLines(Path.of(args[0]), StandardCharsets.UTF_8)) {
                if (line.isEmpty()) continue;
                String[] item = line.split("\t");
                if (item.length != 3) throw new IllegalArgumentException("Invalid pair line");
                Map<String, Object> report = dump(item[0], Path.of(item[1]));
                Files.writeString(Path.of(item[2]), json(report) + "\n", StandardCharsets.UTF_8);
                count++;
            }
            console.println("Dumped " + count + " library documents");
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    /** MovieFactory's static initializer (reached through VideoIO when a target has an unfamiliar extension)
     * registers a listener on OSPLog's frame, and creating that frame fails headless. Install a frameless OSPLog
     * instance first; OSPLog then skips the listener. */
    private static void headlessLog() throws Exception {
        Class<?> log = Class.forName("org.opensourcephysics.controls.OSPLog");
        Field instance = log.getDeclaredField("OSPLOG");
        instance.setAccessible(true);
        if (instance.get(null) != null) return;
        Field unsafeField = Class.forName("sun.misc.Unsafe").getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        Object unsafe = unsafeField.get(null);
        Object frameless = unsafe.getClass().getMethod("allocateInstance", Class.class).invoke(unsafe, log);
        // A logger without handlers, so that warnings (for example a document that fails to read) stay quiet.
        Field logger = log.getDeclaredField("logger");
        logger.setAccessible(true);
        java.util.logging.Logger quiet = java.util.logging.Logger.getAnonymousLogger();
        quiet.setUseParentHandlers(false);
        logger.set(frameless, quiet);
        instance.set(null, frameless);
    }
    private static Map<String, Object> dump(String kind, Path input) throws Exception {
        Map<String, Object> report = new LinkedHashMap<>();
        report.put("schema", SCHEMA);
        report.put("kind", kind);
        switch (kind) {
        case "collection":
        case "library":
            dumpDocument(kind, input, report);
            break;
        case "compadre-tree":
            dumpTree(input, report);
            break;
        case "compadre-records":
            dumpRecords(input, report);
            break;
        default:
            throw new IllegalArgumentException("Unknown kind " + kind);
        }
        return report;
    }

    private static void dumpDocument(String kind, Path input, Map<String, Object> report) throws Exception {
        XMLControlElement control = new XMLControlElement(input.toString());
        report.put("read", !control.failedToRead());
        if (control.failedToRead()) return;
        report.put("class", control.getObjectClassName());
        Class<?> type = control.getObjectClass();
        boolean collection = kind.equals("collection");
        boolean accepted = type != null && (collection ? LibraryResource.class.isAssignableFrom(type) : type == Library.class);
        Object loaded = null;
        if (accepted) {
            try {
                loaded = collection ? control.loadObject(null) : control.loadObject(new Library());
            } catch (Exception error) {
                report.put("error", error.getClass().getName());
            }
        }
        report.put("loaded", loaded != null);
        if (loaded == null) return;
        if (collection) {
            LibraryResource resource = (LibraryResource) loaded;
            report.put("root", resource(resource));
            report.put("written", new XMLControlElement(resource).toXML());
        } else {
            report.put("library", library((Library) loaded, control));
        }
    }

    private static void dumpTree(Path input, Map<String, Object> report) throws Exception {
        LibraryCollection collection = new LibraryCollection("tree");
        Method load = LibraryComPADRE.class.getDeclaredMethod("load", LibraryCollection.class, String.class);
        load.setAccessible(true);
        report.put("loaded", load.invoke(null, collection, input.toUri().toURL().toString()));
        report.put("root", resource(collection));
        report.put("written", new XMLControlElement(collection).toXML());
    }

    /** LibraryComPADRE.loadResources without the network: the response is read from a file and every record goes
     * through loadNode, continuing only when it calls onFound or onNothingNew, as Tracker's chain does. */
    private static void dumpRecords(Path input, Map<String, Object> report) throws Exception {
        LibraryComPADRE.desiredOSPType = "Tracker";
        OSPRuntime.doCacheThumbnail = false;
        LibraryCollection collection = new LibraryCollection("records");
        collection.setDescription("<p>Loading</p>");
        collection.setTarget("&OSPSubject=1");
        NodeList records = null;
        try {
            Document document = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(input.toFile());
            records = document.getElementsByTagName("record");
        } catch (Exception error) {
            report.put("error", error.getClass().getName());
        }
        int count = records == null ? 0 : records.getLength();
        Class<?> treeNode = Class.forName("org.opensourcephysics.tools.LibraryTreeNode");
        Method loadNode = LibraryComPADRE.class.getDeclaredMethod("loadNode", org.w3c.dom.Node.class, LibraryCollection.class, treeNode,
                String.class, Runnable.class, Runnable.class);
        loadNode.setAccessible(true);
        boolean completed = true;
        boolean success = false;
        for (int index = 0; index < count; index++) {
            boolean[] next = {false};
            Runnable proceed = () -> next[0] = true;
            loadNode.invoke(null, records.item(index), collection, null, RELOAD_URL, proceed, proceed);
            if (!next[0]) {
                completed = false;
                break;
            }
            success = true;
        }
        if (completed) {
            collection.setDescription(null);
            collection.setTarget(null);
        }
        report.put("records", count);
        report.put("completed", completed);
        report.put("success", success);
        report.put("root", resource(collection));
        report.put("written", new XMLControlElement(collection).toXML());
    }

    private static Object field(Object object, Class<?> owner, String name) throws Exception {
        Field field = owner.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    private static Object call(Object object, String name) throws Exception {
        Method method = LibraryResource.class.getDeclaredMethod(name);
        method.setAccessible(true);
        return method.invoke(object);
    }

    private static Map<String, Object> resource(LibraryResource resource) throws Exception {
        Map<String, Object> result = new LinkedHashMap<>();
        boolean collection = resource instanceof LibraryCollection;
        result.put("class", collection ? "LibraryCollection" : "LibraryResource");
        result.put("name", resource.getName());
        result.put("description", resource.getDescription());
        result.put("base_path", resource.getBasePath());
        result.put("html_path", resource.getHTMLPath());
        result.put("target", field(resource, LibraryResource.class, "target"));
        result.put("type", resource.getType());
        result.put("type_field", field(resource, LibraryResource.class, "type"));
        result.put("thumbnail", resource.getThumbnail());
        List<Object> metadata = new ArrayList<>();
        Collection<?> set = resource.getMetadata();
        if (set != null) {
            for (Object item : set) {
                String[] data = (String[]) item.getClass().getMethod("getData").invoke(item);
                metadata.add(Arrays.asList(data[0], data[1]));
            }
        }
        result.put("metadata", metadata);
        List<Object> properties = new ArrayList<>();
        for (String name : resource.getPropertyNames()) properties.add(Arrays.asList(name, resource.getProperty(name)));
        result.put("properties", properties);
        result.put("absolute_target", resource.getAbsoluteTarget());
        result.put("absolute_html", call(resource, "getAbsoluteHTMLPath"));
        result.put("inherited_base", call(resource, "getInheritedBasePath"));
        result.put("external_html", call(resource, "hasExternalHTML"));
        if (collection) {
            List<Object> children = new ArrayList<>();
            for (LibraryResource child : ((LibraryCollection) resource).getResources()) children.add(resource(child));
            result.put("resources", children);
        }
        return result;
    }

    private static List<Object> declared(XMLControl control, String name) {
        String[] values = (String[]) control.getObject(name);
        return values == null ? new ArrayList<>() : new ArrayList<>(Arrays.asList(values));
    }

    @SuppressWarnings("unchecked")
    private static Map<String, Object> library(Library library, XMLControl control) throws Exception {
        Map<String, Object> result = new LinkedHashMap<>();
        // A missing name is replaced by one derived from the user's home folder; MoTrack keeps it null.
        result.put("name", control.getString("name") == null ? null : library.toString());
        List<String> paths = (List<String>) field(library, Library.class, "pathList");
        Map<String, String> names = (Map<String, String>) field(library, Library.class, "pathToNameMap");
        List<Object> collections = new ArrayList<>();
        for (String path : paths) collections.add(Arrays.asList(path, names.get(path)));
        result.put("collections", collections);
        // Library.Loader opens these and keeps only readable libraries; MoTrack keeps the declared paths.
        result.put("sublibrary_paths", declared(control, "sublibrary_paths"));
        result.put("imported_library_paths", declared(control, "imported_library_paths"));
        result.put("recently_opened", new ArrayList<>((List<String>) field(library, Library.class, "recentTabs")));
        List<Object> pathNames = new ArrayList<>();
        for (Map.Entry<String, String> entry : new TreeMap<>((Map<String, String>) field(library, Library.class, "allPathsToNameMap")).entrySet())
            pathNames.add(Arrays.asList(entry.getKey(), entry.getValue()));
        result.put("path_names", pathNames);
        result.put("no_search_paths", new ArrayList<>((Collection<String>) field(library, Library.class, "noSearchSet")));
        String[] tabs = (String[]) field(library, Library.class, "openTabPaths");
        result.put("open_tabs", tabs == null ? null : new ArrayList<>(Arrays.asList(tabs)));
        result.put("chooser_directory", field(library, Library.class, "chooserDir"));
        result.put("cache", control.getString("cache"));
        return result;
    }

    /** Documents written by OSP's own LibraryCollection/Library loaders; MoTrack's writer must reproduce them. */
    private static void generate(Path directory) throws Exception {
        Files.createDirectories(directory);
        Class<?> metadataClass = Class.forName("org.opensourcephysics.tools.LibraryResource$Metadata");
        Constructor<?> metadata = metadataClass.getDeclaredConstructor(String.class, String.class);
        metadata.setAccessible(true);
        Method addMetadata = LibraryResource.class.getMethod("addMetadata", metadataClass);
        LibraryCollection root = new LibraryCollection("Generated collection");
        root.setDescription("<p>Written by OSP's LibraryCollection.Loader & XMLControlElement.</p>");
        root.setBasePath("https://example.org/library/");
        root.setHTMLPath("intro.html");
        addMetadata.invoke(root, metadata.newInstance("author", "A. Example"));
        addMetadata.invoke(root, metadata.newInstance("Keywords", "motion, oracle"));
        addMetadata.invoke(root, metadata.newInstance("generator", "dropped"));
        root.setProperty("custom", "value");
        root.setProperty("unset", null);
        LibraryResource experiment = new LibraryResource("Experiment");
        experiment.setTarget("experiments/run.trz");
        experiment.setHTMLPath("experiments/run.trz!/run_info.html");
        LibraryResource page = new LibraryResource("Info page");
        page.setHTMLPath("info.html");
        LibraryCollection empty = new LibraryCollection("Empty sub-collection");
        empty.setTarget("&OSPSubject=1");
        LibraryResource untitled = new LibraryResource(null);
        untitled.setTarget("doc.pdf");
        root.addResource(experiment);
        root.addResource(page);
        root.addResource(empty);
        root.addResource(untitled);
        Files.writeString(directory.resolve("osp-collection.xml"), new XMLControlElement(root).toXML(), StandardCharsets.UTF_8);

        Library library = new Library();
        Method setName = Library.class.getDeclaredMethod("setName", String.class);
        setName.setAccessible(true);
        setName.invoke(library, "Generated Library");
        library.addCollection("https://example.org/library/collection.xml", "Example collection");
        library.addCollection("C:/Users/student/my_collection.xml", "My Collection");
        Method addRecent = Library.class.getDeclaredMethod("addRecent", String.class, boolean.class);
        addRecent.setAccessible(true);
        addRecent.invoke(library, "C:/Users/student/experiments/b.trk", false);
        addRecent.invoke(library, "C:/Users/student/my_collection.xml", false);
        @SuppressWarnings("unchecked")
        Collection<String> noSearch = (Collection<String>) field(library, Library.class, "noSearchSet");
        noSearch.add("https://example.org/zeta.xml");
        noSearch.add("https://example.org/alpha.xml");
        Field chooser = Library.class.getDeclaredField("chooserDir");
        chooser.setAccessible(true);
        chooser.set(library, "C:\\Users\\student");
        Files.writeString(directory.resolve("osp-library.xml"), new XMLControlElement(library).toXML(), StandardCharsets.UTF_8);
    }

    /** Compact JSON with the same escaping as XmlControlDump (a source-file program cannot share its code). */
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