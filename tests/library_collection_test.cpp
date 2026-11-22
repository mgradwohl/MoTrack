#include "library/collection.hpp"
#include "library/library.hpp"
#include "library/resource.hpp"
#include "osp/source.hpp"
#include "utf8_path.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace library = motrack::library;
using library::LoadError;
using library::Resource;
using library::ResourceKind;
using library::ResourceType;
using library::SaveError;

std::filesystem::path fixturePath(std::string_view name)
{
    return motrack::utf8Path(MOTRACK_FIXTURES) / "library" / name;
}

std::string readFixture(std::string_view name)
{
    std::ifstream stream(fixturePath(name), std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error("missing fixture " + std::string(name));
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

library::CollectionDocument load(std::string bytes)
{
    auto document = library::loadCollection(std::move(bytes));
    if (!document)
    {
        throw std::runtime_error("load failed: " + document.error().location + ": " + document.error().message);
    }
    return *std::move(document);
}

std::string saved(const library::CollectionDocument& document)
{
    auto output = library::saveCollection(document);
    if (!output)
    {
        throw std::runtime_error("save failed: " + output.error().message);
    }
    return *output;
}

std::string replaced(std::string text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    if (position == std::string::npos)
    {
        throw std::runtime_error("text not found: " + std::string(from));
    }
    return text.replace(position, from.size(), to);
}

std::string absolute(const Resource& root, std::initializer_list<std::size_t> path)
{
    std::vector<const Resource*> lineage{&root};
    for (const auto index : path)
    {
        lineage.push_back(&lineage.back()->children.at(index));
    }
    return library::absoluteTarget(lineage);
}

std::vector<std::filesystem::path> collectionFixtures()
{
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixturePath("")))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xml")
        {
            paths.push_back(entry.path());
        }
    }
    return paths;
}

TEST(LibraryCollection, LoadsTheRepresentativeCollection)
{
    const auto document = load(readFixture("collection-tree.xml"));
    const auto& root = document.root;
    EXPECT_TRUE(document.diagnostics.empty());
    EXPECT_EQ(root.kind, ResourceKind::Collection);
    EXPECT_EQ(root.name, "Motion Experiments");
    EXPECT_EQ(root.description, "<p>Synthetic collection for MoTrack's library tests & oracles.</p>");
    EXPECT_EQ(root.type, ResourceType::Unknown);
    EXPECT_EQ(library::effectiveType(root), ResourceType::Collection);
    // "Generator" is dropped, "author" is renamed and the duplicate collapses; "AUTHOR" stays as written.
    const std::vector<library::Metadata> metadata = {{.key = "AUTHOR", .value = "B. Example"},
                                                     {.key = "Author", .value = "A. Example"},
                                                     {.key = "Keywords", .value = "motion, kinematics"}};
    EXPECT_EQ(root.metadata, metadata);
    ASSERT_EQ(root.children.size(), 6U);
    const auto& freeFall = root.children[0];
    EXPECT_EQ(freeFall.kind, ResourceKind::Collection);
    EXPECT_EQ(freeFall.type, ResourceType::Unknown) << "a collection ignores its type property";
    ASSERT_EQ(freeFall.children.size(), 5U);
    EXPECT_EQ(absolute(root, {0, 0}), "https://example.org/library/CAB/experiments/BallDrop.trz");
    EXPECT_EQ(absolute(root, {0, 1}), "https://example.org/library/CAB/videos/BallDrop.mp4");
    EXPECT_EQ(absolute(root, {0, 4}), "https://example.org/library/shared/drop.zip");
    EXPECT_FALSE(library::hasExternalHtml(freeFall.children[2]));
    EXPECT_TRUE(library::hasExternalHtml(freeFall.children[3]));
    const auto& local = root.children[1];
    EXPECT_EQ(absolute(root, {1, 0}), "C:/Users/Student/Documents/Exp\xC3\xA9riences/chute libre \xC3\xA9.trk");
    EXPECT_EQ(absolute(root, {1, 1}), "D:/Data/run 1.trk");
    EXPECT_EQ(absolute(root, {1, 2}), "//server/share/lab.trz");
    EXPECT_EQ(absolute(root, {1, 3}), "file:/Users/student/Tracker%20Library/run.trk");
    EXPECT_EQ(local.children[3].properties.at("custom"), "a \"quoted\" <value>");
    EXPECT_EQ(local.children[4].type, ResourceType::Data);
    EXPECT_EQ(absolute(root, {2}),
              "https://www.compadre.org/osp/services/REST/"
              "osp_tracker.cfm?OSPType=Tracker&OSPPrimary=Subject&OSPSubject=223&OSPSubjectDetail=241");
    EXPECT_EQ(root.children[3].type, ResourceType::Html) << "an HTML page without a target is HTML";
    EXPECT_EQ(root.children[4].type, ResourceType::Collection) << "a plain resource accepts the Collection type";
    EXPECT_EQ(library::effectiveType(root.children[4]), ResourceType::Collection);
    EXPECT_EQ(root.children[5].name, "");
    EXPECT_EQ(root.children[5].type, ResourceType::Pdf);
    EXPECT_TRUE(root.origin && root.origin->source == document.source);
}

TEST(LibraryCollection, LoadsLikeOspInEdgeCases)
{
    const auto document = load(readFixture("collection-quirks.xml"));
    const auto& root = document.root;
    EXPECT_EQ(root.name, "Quirks (last name wins)");
    EXPECT_EQ(root.description, "");
    EXPECT_EQ(root.target, "  unclassified.xml  ") << "a collection with resources keeps its target untrimmed";
    ASSERT_EQ(root.properties.size(), 2U);
    EXPECT_EQ(root.properties.at("alpha"), std::nullopt);
    EXPECT_EQ(root.properties.at("zeta"), "replaced");
    // A compact String[][] decodes to rows of nulls.
    const std::vector<library::Metadata> empty = {{.key = "", .value = ""}};
    EXPECT_EQ(root.metadata, empty);
    ASSERT_EQ(root.children.size(), 5U);
    EXPECT_EQ(root.children[0].target, "  spaced name.trk  ");
    EXPECT_EQ(root.children[0].type, ResourceType::Unknown) << "type names are case-sensitive";
    EXPECT_EQ(root.children[1].htmlPath, "page.html");
    EXPECT_EQ(root.children[1].type, ResourceType::Html);
    const auto& duplicates = root.children[2];
    EXPECT_EQ(duplicates.target, "  kept raw  ");
    ASSERT_EQ(duplicates.children.size(), 1U);
    EXPECT_EQ(duplicates.children[0].name, "Later duplicate index wins");
    EXPECT_EQ(duplicates.children[0].type, ResourceType::Unknown) << "an explicit Unknown type replaces the inferred HTML";
    EXPECT_EQ(root.children[3].target, "  raw because resources is not null  ");
    EXPECT_TRUE(root.children[3].children.empty());
    EXPECT_EQ(root.children[4].target, "trimmed");
    ASSERT_EQ(document.diagnostics.size(), 1U);
    EXPECT_NE(document.diagnostics[0].message.find("org.example.FutureResource"), std::string::npos);
    EXPECT_EQ(document.diagnostics[0].location, "root.resources[1]");
}

TEST(LibraryCollection, LoadsASingleResourceAndAnArrayList)
{
    const auto single = load(readFixture("resource-single.xml"));
    EXPECT_EQ(single.root.kind, ResourceKind::Resource);
    EXPECT_EQ(single.root.description, "A resource copied to the clipboard\nwith a two-line description.");
    const std::vector<library::Metadata> metadata = {{.key = "Contact", .value = "teacher@example.org"}, {.key = "Keywords", .value = ""}};
    EXPECT_EQ(single.root.metadata, metadata);
    EXPECT_EQ(library::documentKind(*single.source), library::DocumentKind::Resource);

    const auto list = load(readFixture("collection-resources-arraylist.xml"));
    ASSERT_EQ(list.root.children.size(), 2U);
    EXPECT_EQ(list.root.children[1].target, "second.trz");
    ASSERT_EQ(list.diagnostics.size(), 1U);
    EXPECT_NE(list.diagnostics[0].message.find("6.3.5"), std::string::npos);
}

TEST(LibraryCollection, ReportsWhatOspCannotLoad)
{
    const std::vector<std::pair<std::string_view, std::string_view>> failures = {
        {"error-metadata-short-row.xml", "root.metadata"},
        {"error-metadata-wrong-class.xml", "root.metadata"},
        {"error-metadata-null-row.xml", "root.metadata"},
        {"error-properties-null-key.xml", "root.properties"},
        {"error-resources-string.xml", "root.resources"},
        {"error-resources-index.xml", "root.resources[3]"},
        {"error-resources-element-type.xml", "root.resources[0]"},
        {"error-nested-collection.xml", "root.resources[0].properties[0]"},
    };
    for (const auto& [name, location] : failures)
    {
        const auto document = library::loadCollection(readFixture(name));
        ASSERT_FALSE(document) << name;
        EXPECT_EQ(document.error().code, LoadError::Code::Malformed) << name;
        EXPECT_EQ(document.error().location, location) << name;
        EXPECT_FALSE(document.error().message.empty()) << name;
    }
    // A nested CDATA marker makes XML.removeCDATA throw (inline: motrack_osp refuses to render such a value).
    const auto cdata = library::loadCollection(std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryResource">
    <property name="name" type="string"><![CDATA[nested <![CDATA[marker]]></property>
</object>
)"));
    ASSERT_FALSE(cdata);
    EXPECT_EQ(cdata.error().location, "root.name");
    const auto other = library::loadCollection(readFixture("not-a-collection.xml"));
    ASSERT_FALSE(other);
    EXPECT_EQ(other.error().code, LoadError::Code::NotLibraryDocument);
    const auto libraryFile = library::loadCollection(readFixture("library-my.xml"));
    ASSERT_FALSE(libraryFile);
    EXPECT_EQ(libraryFile.error().code, LoadError::Code::NotLibraryDocument);
    const auto unreadable = library::loadCollection(std::string("not xml"));
    ASSERT_FALSE(unreadable);
    EXPECT_EQ(unreadable.error().code, LoadError::Code::Parse);
    EXPECT_TRUE(unreadable.error().parse);
    EXPECT_FALSE(library::loadCollection(std::shared_ptr<const motrack::osp::Source>{}));
}

TEST(LibraryCollection, MalformedValuesNeverCrash)
{
    // Variations of every property MoTrack decodes, each either loading or reporting Malformed.
    const std::string_view resource = R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryCollection">
    <property name="name" type="string">N</property>
    %s
</object>
)";
    const std::vector<std::string_view> bodies = {
        R"(<property name="metadata" type="int">x</property>)",
        R"(<property name="metadata" type="int">7</property>)",
        R"(<property name="metadata" type="double">x</property>)",
        R"(<property name="metadata" type="double">1.5</property>)",
        R"(<property name="metadata" type="boolean">true</property>)",
        R"(<property name="metadata" type="string"><![CDATA[a<![CDATA[b]]></property>)",
        R"(<property name="metadata" type="mystery">x</property>)",
        R"(<property name="metadata" type="collection" class="java.util.ArrayList">
        <property name="item" type="string">x</property>
    </property>)",
        R"(<property name="metadata" type="collection" class="org.example.List">
        <property name="item" type="string">x</property>
    </property>)",
        R"(<property name="metadata" type="object">
    <object class="java.awt.Point"/>
    </property>)",
        R"(<property name="metadata" type="object">
    <object class="org.example.Unknown"/>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Lorg.example.Missing;">
    </property>)",
        R"(<property name="metadata" type="array" class="[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[Ljava.lang.String;">
        <property name="array" type="string">{{}}</property>
    </property>)",
        R"(<property name="metadata" type="array" class="java.lang.String">
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;"/>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="array" type="string">not braced</property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="array" type="string">{{a,b}</property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="array" type="object">null</property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="array" type="array" class="[I">
            <property name="[0]" type="int">1</property>
        </property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[I">
        <property name="array" type="string">{{1,x}}</property>
    </property>)",
        R"(<property name="metadata" type="array" class="[D">
        <property name="array" type="string">{1.5,y}</property>
    </property>)",
        R"(<property name="metadata" type="array" class="[I">
        <property name="array" type="string">{1,2}</property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="x" type="array" class="[Ljava.lang.String;">
        </property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="[-3]" type="array" class="[Ljava.lang.String;">
        </property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="[0]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="int">1</property>
            <property name="[1]" type="string">v</property>
        </property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="[0]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string">k</property>
            <property name="[1]" type="collection" class="org.example.List">
            </property>
        </property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="[0]" type="array" class="[Ljava.lang.Object;">
        </property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="[0]" type="mystery">skipped</property>
        <property name="[1]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string">k</property>
            <property name="[1]" type="string">v</property>
        </property>
    </property>)",
        R"(<property name="metadata" type="array" class="[[Ljava.lang.String;">
        <property name="[0]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string">k</property>
            <property name="[1]" type="object">
            <object class="org.example.Unknown"/>
            </property>
        </property>
    </property>)",
        R"(<property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryCollection;">
        <property name="[0]" type="object">
        <object class="org.opensourcephysics.tools.LibraryResource">
            <property name="name" type="string">not a collection</property>
        </object>
        </property>
    </property>)",
        R"(<property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryCollection;">
        <property name="[0]" type="object">
        <object class="org.opensourcephysics.tools.LibraryCollection">
            <property name="name" type="string">a collection</property>
        </object>
        </property>
    </property>)",
        R"(<property name="resources" type="array" class="[Ljava.lang.Object;">
    </property>)",
        R"(<property name="resources" type="collection" class="java.util.LinkedList">
    </property>)",
        R"(<property name="resources" type="collection" class="java.util.ArrayList">
        <property name="item" type="string">text</property>
    </property>)",
        R"(<property name="resources" type="collection" class="java.util.ArrayList">
        <property name="item" type="int">3</property>
        <property name="item" type="object">
        <object class="org.opensourcephysics.tools.LibraryResource">
            <property name="name" type="string"><![CDATA[a<![CDATA[b]]></property>
        </object>
        </property>
    </property>)",
        R"(<property name="resources" type="object">
    <object class="org.opensourcephysics.tools.LibraryResource"/>
    </property>)",
        R"(<property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">
        <property name="[0]" type="object">
        <object class="org.opensourcephysics.tools.Library"/>
        </property>
    </property>)",
        R"(<property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">
        <property name="[-1]" type="mystery">x</property>
    </property>)",
        R"(<property name="target" type="string"><![CDATA[a<![CDATA[b]]></property>)",
        R"(<property name="description" type="string"><![CDATA[a<![CDATA[b]]></property>)",
        R"(<property name="base_path" type="string"><![CDATA[a<![CDATA[b]]></property>)",
        R"(<property name="html_path" type="string"><![CDATA[a<![CDATA[b]]></property>)",
        R"(<property name="type" type="string"><![CDATA[a<![CDATA[b]]></property>)",
        R"(<property name="properties" type="string">x</property>)",
        R"(<property name="properties" type="array" class="[[Ljava.lang.String;">
        <property name="array" type="string">{{a,b},,}</property>
    </property>)",
        R"(<property name="properties" type="array" class="[[Ljava.lang.String;">
        <property name="[0]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string">only a key</property>
        </property>
    </property>)",
    };
    // Whether Java's loader returns (true) or throws (false) for each body, worked out from XMLControlElement and
    // the library loaders (see the comments in osp_values.cpp).
    const std::vector<bool> loads = {false, false, false, false, false, false, true,  false, true,  false, true,  false,
                                     false, false, true,  true,  false, false, true,  false, false, false, false, false,
                                     false, true,  false, false, true,  false, true,  false, false, false, true,  false,
                                     false, true,  false, false, false, false, false, false, false, false};
    ASSERT_EQ(loads.size(), bodies.size());
    for (std::size_t index = 0; index < bodies.size(); ++index)
    {
        const auto body = bodies[index];
        std::string text(resource);
        text.replace(text.find("%s"), 2, body);
        const auto document = library::loadCollection(text);
        ASSERT_EQ(document.has_value(), loads[index]) << "body " << index + 1 << ": " << body;
        if (document)
        {
            const auto output = library::saveCollection(*document);
            ASSERT_TRUE(output) << body;
            EXPECT_EQ(*output, text) << body;
        }
        else
        {
            EXPECT_EQ(document.error().code, LoadError::Code::Malformed) << body;
        }
    }
}

TEST(LibraryCollection, UneditedSavesAreByteIdentical)
{
    std::size_t checked = 0;
    for (const auto& path : collectionFixtures())
    {
        std::ifstream stream(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        for (const auto* ending : {"\n", "\r\n", "\r"})
        {
            std::string variant;
            for (const char character : bytes)
            {
                variant += character == '\n' ? std::string(ending) : std::string(1, character);
            }
            if (const auto document = library::loadCollection(variant))
            {
                EXPECT_EQ(saved(*document), variant) << motrack::utf8String(path.filename());
                ++checked;
            }
            else if (const auto libraryDocument = library::loadLibrary(variant))
            {
                const auto output = library::saveLibrary(*libraryDocument);
                ASSERT_TRUE(output) << motrack::utf8String(path.filename());
                EXPECT_EQ(*output, variant) << motrack::utf8String(path.filename());
                ++checked;
            }
        }
    }
    EXPECT_EQ(checked, 27U) << "six collections and three libraries in three line-ending variants";
}

TEST(LibraryCollection, EditsChangeOnlyTheirProperties)
{
    const auto original = readFixture("collection-tree.xml");
    auto document = load(original);
    auto& drop = document.root.children[0].children[0];
    library::setName(drop, "Ball Drop & Bounce");
    EXPECT_EQ(saved(document),
              replaced(original,
                       R"(<property name="name" type="string">Ball Drop</property>)",
                       R"(<property name="name" type="string"><![CDATA[Ball Drop & Bounce]]></property>)"));

    document = load(original);
    library::setDescription(document.root.children[0].children[1], "A movie");
    EXPECT_EQ(saved(document),
              replaced(original,
                       "                    <property name=\"name\" type=\"string\">Ball Drop Video</property>\n",
                       "                    <property name=\"name\" type=\"string\">Ball Drop Video</property>\n"
                       "                    <property name=\"description\" type=\"string\">A movie</property>\n"));

    document = load(original);
    library::setDescription(document.root, "");
    library::setBasePath(document.root.children[1], "");
    auto output = saved(document);
    EXPECT_EQ(output.find("Synthetic collection"), std::string::npos);
    EXPECT_EQ(output.find("Tracker Library</property>"), std::string::npos);
    const auto reloaded = load(output);
    EXPECT_EQ(absolute(reloaded.root, {1, 1}), "D:/Data/run 1.trk");

    // A property of another type is replaced by a string property.
    auto typed = load(R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryResource">
    <property name="name" type="int">5</property>
</object>
)");
    EXPECT_EQ(typed.root.name, "");
    library::setName(typed.root, "Named");
    EXPECT_EQ(saved(typed), R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryResource">
    <property name="name" type="string">Named</property>
</object>
)");
    EXPECT_EQ(absolute(reloaded.root, {1, 0}), "https://example.org/library/Exp\xC3\xA9riences/chute libre \xC3\xA9.trk");
}

TEST(LibraryCollection, SavesTypesMetadataAndProperties)
{
    const auto original = readFixture("collection-tree.xml");
    auto document = load(original);
    auto& page = document.root.children[3];
    library::setType(page, ResourceType::Unknown);
    library::addMetadata(page, {.key = "keywords", .value = "about"});
    library::setProperty(page, "flag", std::nullopt);
    auto& video = document.root.children[0].children[1];
    library::setType(video, ResourceType::Image);
    video.properties.clear();
    document.root.metadata.clear();
    const auto output = saved(document);
    const auto reloaded = load(output);
    const auto& reloadedPage = reloaded.root.children[3];
    EXPECT_EQ(reloadedPage.type, ResourceType::Unknown) << "an explicit Unknown keeps the page from becoming HTML";
    EXPECT_NE(output.find(R"(<property name="type" type="string">Unknown</property>)"), std::string::npos);
    ASSERT_EQ(reloadedPage.metadata.size(), 1U);
    EXPECT_EQ(reloadedPage.metadata[0].key, "Keywords");
    EXPECT_EQ(reloadedPage.properties.at("flag"), std::nullopt);
    EXPECT_EQ(reloaded.root.children[0].children[1].type, ResourceType::Image);
    EXPECT_TRUE(reloaded.root.metadata.empty());
    EXPECT_EQ(output.find("Keywords</property>\n            <property name=\"[1]\" type=\"string\">motion"), std::string::npos);

    // Clearing the explicit type and the metadata again removes both properties.
    auto again = load(output);
    library::setType(again.root.children[3], ResourceType::Html);
    again.root.children[3].metadata.clear();
    again.root.children[3].properties.clear();
    const auto cleared = saved(again);
    EXPECT_EQ(cleared.find("Unknown</property>"), std::string::npos);
    EXPECT_EQ(load(cleared).root.children[3].type, ResourceType::Html);
    auto plain = load(cleared);
    library::setType(plain.root.children[3], ResourceType::Unknown);
    library::setHtmlPath(plain.root.children[3], "");
    const auto noType = saved(plain);
    EXPECT_EQ(load(noType).root.children[3].type, ResourceType::Unknown);
}

TEST(LibraryCollection, EditsChildListsInPlaceWhenPossible)
{
    const auto original = readFixture("collection-tree.xml");
    auto document = load(original);
    auto& freeFall = document.root.children[0];
    freeFall.children.erase(freeFall.children.begin() + 2);
    auto added = library::makeResource("Added");
    library::setTarget(added, "added.trk", {});
    freeFall.children.push_back(added);
    const auto output = saved(document);
    const auto reloaded = load(output);
    ASSERT_EQ(reloaded.root.children[0].children.size(), 5U);
    EXPECT_EQ(reloaded.root.children[0].children[2].name, "Mixed-case archive");
    EXPECT_EQ(reloaded.root.children[0].children[4].name, "Added");
    EXPECT_EQ(reloaded.root.children[0].children[4].type, ResourceType::Tracker);
    EXPECT_NE(output.find(R"(<property name="[5]" type="object">)"), std::string::npos) << "new elements follow the largest index";
    EXPECT_EQ(output.find("Page inside the archive"), std::string::npos);
    // Everything outside the edited array is unchanged.
    const auto tail = original.substr(original.find(
        "        <property name=\"[1]\" type=\"object\">\n        <object class=\"org.opensourcephysics.tools.LibraryCollection\">\n       "
        "     <property name=\"name\" type=\"string\">Local copies"));
    EXPECT_NE(output.find(tail), std::string::npos);

    // Removing every child removes the array.
    auto emptied = load(original);
    emptied.root.children[0].children.clear();
    const auto none = saved(emptied);
    EXPECT_TRUE(load(none).root.children[0].children.empty());
}

TEST(LibraryCollection, RewritesReorderedListsWithTheirContent)
{
    const auto original = readFixture("collection-quirks.xml");
    auto document = load(original);
    auto& children = document.root.children;
    std::swap(children[0], children[1]);
    children.insert(children.begin(), library::makeResource("First"));
    // A moved collection keeps its own unknown content and its children.
    auto moved = children[3];
    children.erase(children.begin() + 3);
    children.push_back(moved);
    const auto output = saved(document);
    const auto reloaded = load(output);
    ASSERT_EQ(reloaded.root.children.size(), 6U);
    EXPECT_EQ(reloaded.root.children[0].name, "First");
    EXPECT_EQ(reloaded.root.children[1].name, "After a gap");
    EXPECT_EQ(reloaded.root.children[2].name, "Untrimmed target");
    EXPECT_EQ(reloaded.root.children[5].name, "Collection target with children");
    EXPECT_EQ(reloaded.root.children[5].children.at(0).name, "Later duplicate index wins");
    // The element OSP ignores (an unknown class) survives the rewrite; root properties stay untouched.
    EXPECT_NE(output.find("org.example.FutureResource"), std::string::npos);
    EXPECT_NE(output.find(R"(<property name="thumbnail" type="string">ignored_thumbnail.png</property>)"), std::string::npos);
    EXPECT_NE(output.find(R"(<property name="custom_setting" type="int">42</property>)"), std::string::npos);
    EXPECT_EQ(reloaded.diagnostics.size(), 1U);
}

TEST(LibraryCollection, ChangingAResourceKindReplacesItsElement)
{
    const auto original = readFixture("collection-quirks.xml");
    auto document = load(original);
    auto& page = document.root.children[1];
    page.kind = ResourceKind::Collection;
    page.type = ResourceType::Unknown;
    page.children.push_back(library::makeResource("Child of a converted page"));
    auto& formerCollection = document.root.children[2];
    formerCollection.kind = ResourceKind::Resource;
    formerCollection.children.clear();
    library::setType(formerCollection, ResourceType::Tracker);
    const auto output = saved(document);
    const auto reloaded = load(output);
    EXPECT_EQ(reloaded.root.children[1].kind, ResourceKind::Collection);
    EXPECT_EQ(reloaded.root.children[1].children.at(0).name, "Child of a converted page");
    EXPECT_EQ(reloaded.root.children[2].kind, ResourceKind::Resource);
    EXPECT_EQ(reloaded.root.children[2].type, ResourceType::Tracker);

    // The root's class changes by rewriting the document.
    auto root = load(readFixture("resource-single.xml"));
    root.root.kind = ResourceKind::Collection;
    root.root.type = ResourceType::Unknown;
    root.root.children.push_back(library::makeResource("child"));
    const auto rewritten = saved(root);
    const auto reloadedRoot = load(rewritten);
    EXPECT_EQ(reloadedRoot.root.kind, ResourceKind::Collection);
    EXPECT_EQ(reloadedRoot.root.basePath, "/home/student/tracker library");
}

TEST(LibraryCollection, CopiesBetweenDocumentsKeepUnknownProperties)
{
    const auto quirks = load(readFixture("collection-quirks.xml"));
    library::CollectionDocument fresh{.root = library::makeCollection("New")};
    fresh.root.children.push_back(quirks.root.children[2]);
    fresh.root.children.push_back(quirks.root);
    const auto output = saved(fresh);
    EXPECT_NE(output.find(R"(<property name="custom_setting" type="int">42</property>)"), std::string::npos);
    const auto reloaded = load(output);
    ASSERT_EQ(reloaded.root.children.size(), 2U);
    EXPECT_EQ(reloaded.root.children[0].name, "Collection target with children");
    EXPECT_EQ(reloaded.root.children[1].children.size(), 5U);

    // A copy of a resource within one document, next to the original.
    auto document = load(readFixture("collection-tree.xml"));
    auto& freeFall = document.root.children[0];
    freeFall.children.push_back(freeFall.children[0]);
    auto copy = load(saved(document));
    ASSERT_EQ(copy.root.children[0].children.size(), 6U);
    EXPECT_EQ(copy.root.children[0].children[5].name, "Ball Drop");
}

TEST(LibraryCollection, RefusesValuesOspCannotReadBack)
{
    auto document = load(readFixture("collection-tree.xml"));
    library::setName(document.root, "bad ]]></property> name");
    const auto output = library::saveCollection(document);
    ASSERT_FALSE(output);
    EXPECT_EQ(output.error().code, SaveError::Code::Render);
    auto fresh = library::makeResource("bad ]]></property> name");
    EXPECT_FALSE(library::writeCollection(fresh));
}

TEST(LibraryCollection, WriterMatchesOsp)
{
    // The collection tools/reference/LibraryDump.java --generate builds with OSP's API.
    auto root = library::makeCollection("Generated collection");
    library::setDescription(root, "<p>Written by OSP's LibraryCollection.Loader & XMLControlElement.</p>");
    library::setBasePath(root, "https://example.org/library/");
    library::setHtmlPath(root, "intro.html");
    library::addMetadata(root, {.key = "author", .value = "A. Example"});
    library::addMetadata(root, {.key = "Keywords", .value = "motion, oracle"});
    library::addMetadata(root, {.key = "generator", .value = "dropped"});
    library::setProperty(root, "custom", "value");
    library::setProperty(root, "unset", std::nullopt);
    auto experiment = library::makeResource("Experiment");
    library::setTarget(experiment, "experiments/run.trz", {});
    library::setHtmlPath(experiment, "experiments/run.trz!/run_info.html");
    auto page = library::makeResource("Info page");
    library::setHtmlPath(page, "info.html");
    auto empty = library::makeCollection("Empty sub-collection");
    library::setTarget(empty, "&OSPSubject=1", {});
    auto untitled = library::makeResource("");
    library::setTarget(untitled, "doc.pdf", {});
    root.children = {experiment, page, empty, untitled};
    const auto written = library::writeCollection(root);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written, readFixture("generated/osp-collection.xml"));
    const auto crlf = library::writeCollection(root, motrack::osp::LineEnding::CrLf);
    ASSERT_TRUE(crlf);
    EXPECT_NE(crlf->find("\r\n"), std::string::npos);
    // OSP omits an HTML path inside the target archive and derives it again from the archive.
    const auto value = library::collectionObjectValue(experiment);
    for (const auto& property : value.properties)
    {
        EXPECT_NE(property.name, "html_path");
    }
    const auto reloaded = library::reloadedForm(experiment);
    EXPECT_EQ(reloaded.htmlPath, "");
    EXPECT_EQ(reloaded.type, ResourceType::Tracker);

    // A document without a source is written in OSP's layout, except that saving keeps the page in the archive.
    library::CollectionDocument fresh{.root = root};
    const auto reopened = load(saved(fresh)).root;
    EXPECT_EQ(reopened.children[0].htmlPath, "experiments/run.trz!/run_info.html");
    auto expected = library::reloadedForm(root);
    expected.children[0].htmlPath = "experiments/run.trz!/run_info.html";
    EXPECT_TRUE(library::sameContent(reopened, expected));
}

TEST(LibraryCollection, ReloadedFormNormalizesLikeTheLoader)
{
    Resource resource;
    resource.name = "  padded ";
    resource.htmlPath = " page.html ";
    resource.metadata = {{.key = "keywords", .value = "x"}, {.key = "robots", .value = "y"}};
    const auto reloaded = library::reloadedForm(resource);
    EXPECT_EQ(reloaded.name, "padded");
    EXPECT_EQ(reloaded.htmlPath, "page.html");
    EXPECT_EQ(reloaded.type, ResourceType::Html);
    ASSERT_EQ(reloaded.metadata.size(), 1U);
    EXPECT_EQ(reloaded.metadata[0].key, "Keywords");
    auto collection = library::makeCollection("c");
    collection.target = "  query  ";
    EXPECT_EQ(library::reloadedForm(collection).target, "query");
    collection.children.push_back(library::makeResource("child"));
    EXPECT_EQ(library::reloadedForm(collection).target, "  query  ");
}

TEST(LibraryCollection, EditedSavesWriteArraysThatTracker635Reads)
{
    const auto original = readFixture("collection-resources-arraylist.xml");
    auto unedited = load(original);
    EXPECT_EQ(saved(unedited), original) << "an unedited save keeps the ArrayList";

    // Editing a child, or anything else in the document, turns the list into an array.
    auto document = load(original);
    library::setName(document.root.children[1], "Second, renamed");
    const auto output = saved(document);
    EXPECT_EQ(output.find("java.util.ArrayList"), std::string::npos);
    EXPECT_NE(output.find(R"(<property name="resources" type="array" class="[Lorg.opensourcephysics.tools.LibraryResource;">)"),
              std::string::npos);
    const auto reopened = load(output);
    ASSERT_EQ(reopened.root.children.size(), 2U);
    EXPECT_EQ(reopened.root.children[1].name, "Second, renamed");
    EXPECT_TRUE(reopened.diagnostics.empty());
    auto renamed = load(original);
    library::setName(renamed.root, "Renamed collection");
    EXPECT_EQ(saved(renamed).find("java.util.ArrayList"), std::string::npos);

    // An ArrayList that OSP ignores (a malformed element makes collectionValue return null) is kept, shadowed,
    // when children are added: every element survives and the new array is the one OSP reads.
    const std::string ignored = R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryCollection">
    <property name="name" type="string">Ignored list</property>
    <property name="resources" type="collection" class="java.util.ArrayList">
        <property name="item" type="object">
        <object class="org.opensourcephysics.tools.LibraryResource">
            <property name="name" type="string">Hidden but valid</property>
        </object>
        </property>
        <property name="item" type="object">
        <object class="org.opensourcephysics.tools.LibraryResource">
            <property name="name" type="string"><![CDATA[a<![CDATA[b]]></property>
        </object>
        </property>
    </property>
</object>
)";
    auto hidden = load(ignored);
    EXPECT_TRUE(hidden.root.children.empty());
    hidden.root.children.push_back(library::makeResource("Added"));
    const auto shadowed = saved(hidden);
    EXPECT_NE(shadowed.find("Hidden but valid"), std::string::npos);
    EXPECT_NE(shadowed.find("java.util.ArrayList"), std::string::npos);
    const auto reread = load(shadowed);
    ASSERT_EQ(reread.root.children.size(), 1U);
    EXPECT_EQ(reread.root.children[0].name, "Added");

    // Unknown-class elements of a rewritten ArrayList are kept as array elements.
    const std::string withUnknown = R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.LibraryCollection">
    <property name="name" type="string">List with an unknown element</property>
    <property name="resources" type="collection" class="java.util.ArrayList">
        <property name="item" type="object">
        <object class="org.example.FutureResource">
            <property name="name" type="string">Future</property>
        </object>
        </property>
        <property name="item" type="object">
        <object class="org.opensourcephysics.tools.LibraryResource">
            <property name="name" type="string">Known</property>
        </object>
        </property>
    </property>
</object>
)";
    auto future = load(withUnknown);
    ASSERT_EQ(future.root.children.size(), 1U);
    library::setName(future.root, "Edited");
    const auto kept = saved(future);
    EXPECT_NE(kept.find("org.example.FutureResource"), std::string::npos);
    EXPECT_EQ(kept.find("java.util.ArrayList"), std::string::npos);
    EXPECT_EQ(load(kept).root.children.size(), 1U);
}

TEST(LibraryCollection, CollectionsNeverGetAType)
{
    auto collection = library::makeCollection("c");
    collection.type = ResourceType::Tracker; // set directly, which LibraryCollection.setType would refuse
    for (const auto& property : library::collectionObjectValue(collection).properties)
    {
        EXPECT_NE(property.name, "type");
    }
    const auto written = library::writeCollection(collection);
    ASSERT_TRUE(written);
    EXPECT_EQ(written->find(R"(name="type")"), std::string::npos);
}
library::LibraryDocument loadLibraryFixture(std::string_view name)
{
    auto document = library::loadLibrary(readFixture(name));
    if (!document)
    {
        throw std::runtime_error("library load failed: " + document.error().message);
    }
    return *std::move(document);
}

TEST(LibraryLibrary, LoadsAsLibraryLoaderDoes)
{
    const auto document = loadLibraryFixture("library-my.xml");
    const auto& library = document.library;
    EXPECT_EQ(library.name, "student Library");
    const std::vector<library::LibraryCollectionEntry> collections = {
        {.path = "https://example.org/library/CAB/cabrillo_collection.xml", .name = "Tracker Sampler (renamed)"},
        {.path = "C:/Users/student/Documents/OSP/my_collection.xml", .name = "My Collection"},
        {.path = "https://example.org/library/CAB/cabrillo_collection.xml", .name = "Tracker Sampler (renamed)"},
    };
    EXPECT_EQ(library.collections, collections);
    const std::vector<std::string> recent = {"C:/Users/student/Documents/OSP/my_collection.xml",
                                             "C:/Users/student/Documents/experiments/b.trk",
                                             "C:/Users/student/Documents/experiments/a.trk",
                                             "C:/Users/student/Documents/experiments/c.trk",
                                             "C:/Users/student/Documents/experiments/d.trk",
                                             "C:/Users/student/Documents/experiments/e.trk"};
    EXPECT_EQ(library.recentlyOpened, recent);
    EXPECT_EQ(library::recentName(library, recent[0]), "Recent collection");
    EXPECT_EQ(library::recentName(library, recent[1]), "b.trk") << "a null name falls back to the file name";
    EXPECT_EQ(library::recentName(library, recent[2]), "Experiment A");
    EXPECT_EQ(library.pathNames.at(recent[1]), std::nullopt);
    const std::vector<std::string> noSearch = {"https://example.org/alpha.xml", "https://example.org/zeta.xml"};
    EXPECT_EQ(library.noSearchPaths, noSearch);
    ASSERT_TRUE(library.openTabs.has_value());
    const auto tabs = library.openTabs.value_or(std::vector<std::optional<std::string>>{});
    EXPECT_EQ(tabs.size(), 3U);
    EXPECT_EQ(tabs[1], std::nullopt);
    EXPECT_EQ(library.chooserDirectory, "C:\\Users\\student\\Documents");
    EXPECT_EQ(library.subLibraryPaths, (std::vector<std::optional<std::string>>{"missing/sub_library.xml"}));
    EXPECT_FALSE(library.cache);

    const auto nameless = loadLibraryFixture("library-nameless.xml");
    EXPECT_FALSE(nameless.library.name);
    ASSERT_EQ(nameless.library.collections.size(), 1U);
    EXPECT_EQ(nameless.library.collections[0].name, "Only");
}

TEST(LibraryLibrary, ReportsWhatLibraryLoaderCannotRead)
{
    const auto shortNames = library::loadLibrary(readFixture("error-library-names-short.xml"));
    ASSERT_FALSE(shortNames);
    EXPECT_EQ(shortNames.error().code, LoadError::Code::Malformed);
    const std::string_view head = R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.tools.Library">
)";
    const std::vector<std::string_view> failures = {
        R"(    <property name="collection_paths" type="array" class="[Ljava.lang.String;">
        <property name="[0]" type="string">a.xml</property>
    </property>
)",
        R"(    <property name="recently_opened" type="array" class="[Ljava.lang.String;">
        <property name="[0]" type="string">a.xml</property>
    </property>
    <property name="recently_opened_names" type="array" class="[Ljava.lang.String;">
        <property name="[0]" type="string">A</property>
        <property name="[1]" type="string">B</property>
    </property>
)",
        R"(    <property name="no_search_paths" type="array" class="[Ljava.lang.String;">
        <property name="[1]" type="string">a.xml</property>
    </property>
)",
        R"(    <property name="name" type="string"><![CDATA[a<![CDATA[b]]></property>
)",
        R"(    <property name="chooser_directory" type="string"><![CDATA[a<![CDATA[b]]></property>
)",
        R"(    <property name="cache" type="string"><![CDATA[a<![CDATA[b]]></property>
)",
        R"(    <property name="open_tabs" type="string">x</property>
)",
        R"(    <property name="sublibrary_paths" type="string">x</property>
)",
        R"(    <property name="imported_library_paths" type="string">x</property>
)",
        R"(    <property name="recently_opened" type="string">x</property>
)",
        R"(    <property name="recently_opened_names" type="string">x</property>
)",
        R"(    <property name="collection_paths" type="string">x</property>
)",
        R"(    <property name="collection_paths" type="array" class="[Ljava.lang.String;">
        <property name="[0]" type="string">a.xml</property>
    </property>
    <property name="collection_names" type="string">x</property>
)",
        R"(    <property name="no_search_paths" type="string">x</property>
)",
    };
    for (const auto body : failures)
    {
        const auto document = library::loadLibrary(std::string(head) + std::string(body) + "</object>\n");
        ASSERT_FALSE(document) << body;
        EXPECT_EQ(document.error().code, LoadError::Code::Malformed) << body;
    }
    // Null paths are skipped before their names are read, and a null recent path has no name.
    const auto skipped =
        library::loadLibrary(std::string(head) + R"(    <property name="collection_paths" type="array" class="[Ljava.lang.String;">
        <property name="[2]" type="object">null</property>
    </property>
    <property name="recently_opened" type="array" class="[Ljava.lang.String;">
        <property name="[1]" type="string">b.trk</property>
    </property>
    <property name="recently_opened_names" type="array" class="[Ljava.lang.String;">
        <property name="[0]" type="string">for a null path</property>
    </property>
</object>
)");
    ASSERT_TRUE(skipped);
    EXPECT_TRUE(skipped->library.collections.empty());
    EXPECT_EQ(skipped->library.recentlyOpened, std::vector<std::string>{"b.trk"});
    EXPECT_FALSE(library::loadLibrary(readFixture("collection-tree.xml")));
    EXPECT_FALSE(library::loadLibrary(std::string("not xml")));
    EXPECT_FALSE(library::loadLibrary(std::shared_ptr<const motrack::osp::Source>{}));
}

TEST(LibraryLibrary, SavesEditsAndKeepsEverythingElse)
{
    const auto original = readFixture("library-my.xml");
    auto document = loadLibraryFixture("library-my.xml");
    auto unedited = library::saveLibrary(document);
    ASSERT_TRUE(unedited);
    EXPECT_EQ(*unedited, original);

    library::renameCollection(document.library, "C:/Users/student/Documents/OSP/my_collection.xml", " Mine ");
    library::renameCollection(document.library, "not-present.xml", "ignored");
    library::addCollection(document.library, " https://example.org/new.xml ", " New ");
    library::addCollection(document.library, "https://example.org/new.xml", "duplicate ignored");
    library::addRecent(document.library, "C:/Users/student/Documents/experiments/z.trk", false);
    library::removeRecent(document.library, "C:/Users/student/Documents/experiments/c.trk");
    document.library.noSearchPaths.clear();
    document.library.openTabs.reset();
    document.library.chooserDirectory = "D:/elsewhere";
    document.library.cache = "C:/cache";
    document.library.subLibraryPaths.clear();
    document.library.importedLibraryPaths.emplace_back();
    document.library.name.reset();
    auto output = library::saveLibrary(document);
    ASSERT_TRUE(output) << output.error().message;
    const auto reloaded = library::loadLibrary(*output);
    ASSERT_TRUE(reloaded) << reloaded.error().message;
    const auto& library = reloaded->library;
    EXPECT_FALSE(library.name);
    ASSERT_EQ(library.collections.size(), 4U);
    EXPECT_EQ(library.collections[1].name, "Mine");
    EXPECT_EQ(library.collections[3].path, "https://example.org/new.xml");
    EXPECT_EQ(library.recentlyOpened.front(), "C:/Users/student/Documents/experiments/z.trk");
    EXPECT_EQ(library.recentlyOpened.size(), 5U) << "z.trk pushed e.trk out, then c.trk was removed";
    EXPECT_EQ(library::recentName(library, library.recentlyOpened[1]), "Mine");
    EXPECT_TRUE(library.noSearchPaths.empty());
    EXPECT_FALSE(library.openTabs);
    EXPECT_EQ(library.chooserDirectory, "D:/elsewhere");
    EXPECT_EQ(library.cache, "C:/cache");
    EXPECT_TRUE(library.subLibraryPaths.empty());
    EXPECT_EQ(library.importedLibraryPaths.size(), 2U);

    // Clearing lists removes their properties.
    auto cleared = *reloaded;
    cleared.library.collections.clear();
    cleared.library.recentlyOpened.clear();
    cleared.library.chooserDirectory.reset();
    cleared.library.openTabs = std::vector<std::optional<std::string>>{"tab.xml"};
    cleared.library.name = "Named again";
    output = library::saveLibrary(cleared);
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_EQ(output->find("collection_paths"), std::string::npos);
    EXPECT_EQ(output->find("recently_opened"), std::string::npos);
    EXPECT_NE(output->find("tab.xml"), std::string::npos);
    EXPECT_TRUE(library::sameContent(library::loadLibrary(*output)->library, library::loadLibrary(*output)->library));
}

TEST(LibraryLibrary, WriterMatchesOsp)
{
    library::Library library;
    library.name = "Generated Library";
    library::addCollection(library, "https://example.org/library/collection.xml", "Example collection");
    library::addCollection(library, "C:/Users/student/my_collection.xml", "My Collection");
    library::addRecent(library, "C:/Users/student/experiments/b.trk", false);
    library::addRecent(library, "C:/Users/student/my_collection.xml", false);
    library.noSearchPaths = {"https://example.org/alpha.xml", "https://example.org/zeta.xml"};
    library.chooserDirectory = "C:\\Users\\student";
    const auto written = library::writeLibrary(library);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written, readFixture("generated/osp-library.xml"));
    library::LibraryDocument fresh{.library = library};
    const auto saved = library::saveLibrary(fresh);
    ASSERT_TRUE(saved);
    EXPECT_EQ(*saved, *written);
    library::Library unnamed;
    const auto bare = library::writeLibrary(unnamed);
    ASSERT_TRUE(bare);
    EXPECT_EQ(bare->find("name"), std::string::npos);
    // Every optional field Library.Loader.saveObject writes.
    library::Library full = library;
    full.subLibraryPaths = {"sub.xml"};
    full.importedLibraryPaths = {"imported.xml", std::nullopt};
    full.openTabs = std::vector<std::optional<std::string>>{"tab.xml"};
    full.cache = "C:/cache";
    const auto everything = library::writeLibrary(full);
    ASSERT_TRUE(everything) << everything.error().message;
    const auto reloadedFull = library::loadLibrary(*everything);
    ASSERT_TRUE(reloadedFull);
    EXPECT_EQ(reloadedFull->library.importedLibraryPaths, full.importedLibraryPaths);
    EXPECT_EQ(reloadedFull->library.openTabs, full.openTabs);
    EXPECT_EQ(reloadedFull->library.cache, "C:/cache");
    library.name = "bad ]]></property>";
    EXPECT_FALSE(library::writeLibrary(library));
}

} // namespace
