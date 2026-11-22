#include "library/collection.hpp"
#include "library/library.hpp"
#include "library/resource.hpp"
#include "osp/source.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{

namespace library = motrack::library;
using library::LoadError;
using library::Resource;

/// Runs `work` on a thread with a stack of `bytes`, as small as a macOS worker thread's (512 KiB).
void runWithStack(std::size_t bytes, const std::function<void()>& work)
{
#ifdef _WIN32
    const auto entry = [](LPVOID parameter) -> DWORD
    {
        (*static_cast<const std::function<void()>*>(parameter))();
        return 0;
    };
    HANDLE thread =
        CreateThread(nullptr, bytes, entry, const_cast<std::function<void()>*>(&work), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    ASSERT_NE(thread, nullptr);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, bytes);
    pthread_t thread;
    const auto entry = [](void* parameter) -> void*
    {
        (*static_cast<const std::function<void()>*>(parameter))();
        return nullptr;
    };
    ASSERT_EQ(pthread_create(&thread, &attributes, entry, const_cast<std::function<void()>*>(&work)), 0);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
#endif
}

/// A macOS worker thread's default stack. At motrack_osp's depth limit its parser needs most of it in Debug builds
/// (about 470 KiB on Windows); loading the parsed document needs about 300 KiB.
constexpr std::size_t WORKER_STACK = std::size_t{512} * 1024;

/// `levels` collections, each the only child of the previous one, ending in a resource, in OSP's layout.
std::string nestedCollections(std::size_t levels)
{
    std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    std::string indent;
    const auto line = [&text, &indent](std::string_view content)
    {
        text += indent;
        text += content;
        text += '\n';
    };
    for (std::size_t level = 0; level < levels; ++level)
    {
        line(R"(<object class="org.opensourcephysics.tools.LibraryCollection">)");
        line(R"(    <property name="name" type="string">Level )" + std::to_string(level) + "</property>");
        line(R"(    <property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">)");
        line(R"(        <property name="[0]" type="object">)");
        indent += "        ";
    }
    line(R"(<object class="org.opensourcephysics.tools.LibraryResource">)");
    line(R"(    <property name="name" type="string">Leaf</property>)");
    line(R"(    <property name="target" type="string">leaf.trk</property>)");
    line("</object>");
    for (std::size_t level = levels; level-- > 0;)
    {
        indent.resize(indent.size() - 8);
        text += indent + "        </property>\n";
        text += indent + "    </property>\n";
        text += indent + "</object>\n";
    }
    return text;
}

/// The deepest nesting motrack_osp accepts (its depth limit counts objects and properties).
std::size_t deepestNesting()
{
    std::size_t levels = 1;
    while (motrack::osp::parse(nestedCollections(levels + 1)))
    {
        ++levels;
    }
    return levels;
}

TEST(LibraryLimits, LoadsSavesAndWritesTheDeepestCollectionOnAWorkerStack)
{
    const auto levels = deepestNesting();
    EXPECT_GE(levels, 80U) << "Tracker opens collections nested this deep";
    const auto text = nestedCollections(levels);
    bool loaded = false;
    bool savedUnedited = false;
    bool savedEdit = false;
    bool written = false;
    std::string leafName;
    runWithStack(WORKER_STACK,
                 [&]()
                 {
                     auto document = library::loadCollection(text);
                     loaded = document.has_value();
                     if (!document)
                     {
                         return;
                     }
                     const auto unedited = library::saveCollection(*document);
                     savedUnedited = unedited && *unedited == text;
                     Resource* leaf = &document->root;
                     while (!leaf->children.empty())
                     {
                         leaf = &leaf->children.front();
                     }
                     library::setName(*leaf, "Edited leaf");
                     const auto edited = library::saveCollection(*document);
                     if (edited)
                     {
                         const auto reopened = library::loadCollection(*edited);
                         savedEdit = reopened.has_value();
                         if (reopened)
                         {
                             const Resource* deepest = &reopened->root;
                             while (!deepest->children.empty())
                             {
                                 deepest = &deepest->children.front();
                             }
                             leafName = deepest->name;
                         }
                     }
                     // A copy without a source is merged and rewritten from the logical content.
                     library::CollectionDocument copy{.root = document->root};
                     const auto merged = library::saveCollection(copy);
                     const auto fresh = library::writeCollection(document->root);
                     written = merged.has_value() && fresh.has_value();
                 });
    EXPECT_TRUE(loaded);
    EXPECT_TRUE(savedUnedited);
    EXPECT_TRUE(savedEdit);
    EXPECT_EQ(leafName, "Edited leaf");
    EXPECT_TRUE(written);
    const auto tooDeep = library::loadCollection(nestedCollections(levels + 1));
    ASSERT_FALSE(tooDeep);
    EXPECT_EQ(tooDeep.error().code, LoadError::Code::Parse);
}

std::string collectionWithIndex(std::string_view index)
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryCollection">
    <property name="name" type="string">Sparse</property>
    <property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">
        <property name=")" +
           std::string(index) + R"(" type="object">
        <object class="org.opensourcephysics.tools.LibraryResource">
            <property name="name" type="string">Far away</property>
        </object>
        </property>
    </property>
</object>
)";
}

TEST(LibraryLimits, LargeIndicesCostNothing)
{
    // Java allocates a reference array of the index's length; MoTrack stores only the slots the file fills.
    for (const auto* index : {"[200000]", "[2000000000]", "[2147483646]"})
    {
        const auto text = collectionWithIndex(index);
        auto document = library::loadCollection(text);
        ASSERT_TRUE(document) << index;
        ASSERT_EQ(document->root.children.size(), 1U);
        EXPECT_EQ(document->root.children[0].name, "Far away");
        const auto unedited = library::saveCollection(*document);
        ASSERT_TRUE(unedited);
        EXPECT_EQ(*unedited, text);
        document->root.children.push_back(library::makeResource("Appended"));
        const auto appended = library::saveCollection(*document);
        ASSERT_TRUE(appended) << index << ": " << appended.error().message;
        EXPECT_EQ(library::loadCollection(*appended)->root.children.size(), 2U);
    }
    // The largest index leaves no room after it: the list is rewritten instead.
    auto full = library::loadCollection(collectionWithIndex("[2147483647]"));
    ASSERT_TRUE(full);
    full->root.children.push_back(library::makeResource("Appended"));
    const auto rewritten = library::saveCollection(*full);
    ASSERT_TRUE(rewritten) << rewritten.error().message;
    EXPECT_EQ(library::loadCollection(*rewritten)->root.children.size(), 2U);

    // A metadata array that long has null rows, which Java's loader dereferences.
    const auto metadata = library::loadCollection(R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryResource">
    <property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="[2000000000]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string">Author</property>
            <property name="[1]" type="string">A</property>
        </property>
    </property>
</object>
)");
    ASSERT_FALSE(metadata);
    EXPECT_EQ(metadata.error().code, LoadError::Code::Malformed);
}

TEST(LibraryLimits, LibraryListsAreBoundedByTheDocument)
{
    const std::string head = R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.Library">
)";
    // Sparse lists that the model reads by index load at any length.
    const auto sparse = library::loadLibrary(head + R"(    <property name="collection_paths" type="array" class="[Ljava.lang.String;">
        <property name="[2000000000]" type="string">far.xml</property>
    </property>
    <property name="collection_names" type="array" class="[Ljava.lang.String;">
        <property name="[2000000000]" type="string">Far</property>
    </property>
    <property name="recently_opened" type="array" class="[Ljava.lang.String;">
        <property name="[2000000000]" type="string">far.trk</property>
    </property>
    <property name="recently_opened_names" type="array" class="[Ljava.lang.String;">
        <property name="[2000000000]" type="string">Far experiment</property>
    </property>
</object>
)");
    ASSERT_TRUE(sparse) << sparse.error().message;
    ASSERT_EQ(sparse->library.collections.size(), 1U);
    EXPECT_EQ(library::recentName(sparse->library, "far.trk"), "Far experiment");
    // Lists the model keeps whole are refused when they are longer than the document has nodes.
    for (const auto* name : {"open_tabs", "sublibrary_paths", "imported_library_paths"})
    {
        const auto refused = library::loadLibrary(head + "    <property name=\"" + name +
                                                  R"(" type="array" class="[Ljava.lang.String;">
        <property name="[200000]" type="string">x.xml</property>
    </property>
</object>
)");
        ASSERT_FALSE(refused) << name;
        EXPECT_EQ(refused.error().code, LoadError::Code::Malformed);
    }
    const auto gap = library::loadLibrary(head + R"(    <property name="no_search_paths" type="array" class="[Ljava.lang.String;">
        <property name="[3]" type="string">x.xml</property>
    </property>
</object>
)");
    EXPECT_FALSE(gap) << "TreeSet.add(null) throws";
}

} // namespace
