#include "library/java_string.hpp"
#include "library/paths.hpp"
#include "library/resource.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace library = motrack::library;
using library::Resource;
using library::ResourceType;

TEST(LibraryJavaString, CompareFollowsUtf16CodeUnits)
{
    EXPECT_LT(library::javaCompare("a", "b"), 0);
    EXPECT_GT(library::javaCompare("b", "a"), 0);
    EXPECT_EQ(library::javaCompare("abc", "abc"), 0);
    EXPECT_LT(library::javaCompare("ab", "abc"), 0);
    EXPECT_GT(library::javaCompare("abc", "ab"), 0);
    EXPECT_LT(library::javaCompare("Z", "a"), 0);
    // U+1F600 is the surrogate pair D83D DE00, which sorts before U+FFFD in Java but after it in UTF-8.
    EXPECT_LT(library::javaCompare("\xF0\x9F\x98\x80", "\xEF\xBF\xBD"), 0);
    EXPECT_GT(library::javaCompare("\xEF\xBF\xBD", "\xF0\x9F\x98\x80"), 0);
    EXPECT_LT(library::javaCompare("\xC3\xA9", "\xF0\x9F\x98\x80"), 0);
    // Same high surrogate: low surrogates follow code point order.
    EXPECT_LT(library::javaCompare("\xF0\x9F\x98\x80", "\xF0\x9F\x98\x81"), 0);
    // A stray byte compares as itself instead of failing.
    EXPECT_NE(library::javaCompare("\xC3", "a"), 0);
    EXPECT_TRUE(library::JavaLess{}("alpha", "beta"));
}

TEST(LibraryJavaString, SplitDropsTrailingEmptyFields)
{
    using Fields = std::vector<std::string>;
    EXPECT_EQ(library::javaSplit("a / b / c", " / "), (Fields{"a", "b", "c"}));
    EXPECT_EQ(library::javaSplit("plain", " / "), Fields{"plain"});
    EXPECT_EQ(library::javaSplit("", "\n"), Fields{""});
    EXPECT_EQ(library::javaSplit("\n", "\n"), Fields{});
    EXPECT_EQ(library::javaSplit("a\n\nb\n\n", "\n"), (Fields{"a", "", "b"}));
    EXPECT_EQ(library::javaSplit("\na", "\n"), (Fields{"", "a"}));
    EXPECT_EQ(library::javaSplit("text", ""), Fields{"text"});
    EXPECT_EQ(library::asciiLower("MiXeD \xC3\x89"), "mixed \xC3\x89");
    EXPECT_EQ(library::javaTrimmed("\t  x y \n"), "x y");
}

TEST(LibraryPaths, FileNamesAndExtensions)
{
    EXPECT_EQ(library::forwardSlash(R"(C:\a\b)"), "C:/a/b");
    EXPECT_EQ(library::fileName("dir/sub/file.trk"), "file.trk");
    EXPECT_EQ(library::fileName(R"(C:\dir\file.trk)"), "file.trk");
    EXPECT_EQ(library::fileName(R"(a\b/c)"), "c");
    EXPECT_EQ(library::fileName("plain"), "plain");
    EXPECT_EQ(library::extension("dir/file.trk"), std::optional<std::string_view>("trk"));
    EXPECT_EQ(library::extension("ServeFile.cfm?ID=1&DocID=2"), std::optional<std::string_view>("cfm?ID=1&DocID=2"));
    EXPECT_FALSE(library::extension(".hidden"));
    EXPECT_FALSE(library::extension("trailing."));
    EXPECT_FALSE(library::extension("dir.d/file"));
    EXPECT_FALSE(library::extension(R"(dir.d\file)"));
    EXPECT_FALSE(library::extension("none"));
    EXPECT_EQ(library::stripExtension(R"(C:\dir\file.trk)"), "C:/dir/file");
    EXPECT_EQ(library::stripExtension("dir.d/file"), "dir.d/file");
    EXPECT_EQ(library::stripExtension("name..."), "name");
    EXPECT_EQ(library::stripExtension(".hidden"), ".hidden");
    EXPECT_EQ(library::directoryPath(R"(C:\dir\file.xml)"), "C:/dir");
    EXPECT_EQ(library::directoryPath("https://host/a/b.xml"), "https://host/a");
    EXPECT_EQ(library::directoryPath("file.xml"), "");
}

/// relative, base, expected
using ResolveCase = std::array<std::string_view, 3>;

TEST(LibraryPaths, ResolvedPathMatchesOsp)
{
    const std::vector<ResolveCase> cases = {
        {"a.trk", "https://host/dir/", "https://host/dir/a.trk"},
        {"a.trk", "https://host/dir", "https://host/dir/a.trk"},
        {"../a.trk", "https://host/dir/sub", "https://host/dir/a.trk"},
        // Climbing past the host keeps cutting at the last '/': OSP produces a broken URL.
        {"../../a.trk", "https://host/dir", "https:/a.trk"},
        {"../../../a.trk", "https://host/dir", "https:/a.trk"},
        {"../../../../a.trk", "https://host/dir", "a.trk"},
        {"./a.trk", "/base", "/base/a.trk"},
        {"./../a.trk", "/base/sub", "/base/sub/../a.trk"},
        {".", "/base", "/base/"},
        {"a/./b.trk", "/base", "/base/a/./b.trk"},
        {"../x.trk", "dir", "x.trk"},
        {"x.trk", "", "x.trk"},
        {"../x.trk", "", "../x.trk"},
        {"/abs/x.trk", "https://host", "/abs/x.trk"},
        {R"(C:\data\run 1.trk)", "https://host", "C:/data/run 1.trk"},
        {R"(\\server\share\lab.trz)", "C:/base", "//server/share/lab.trz"},
        {"file:/Users/x.trk", "C:/base", "file:/Users/x.trk"},
        {"C:x.trk", "/base", "/base/C:x.trk"},
        {"..\\Exp\xC3\xA9riences\\chute libre.trk",
         R"(C:\Users\Student\Documents\Tracker Library)",
         "C:/Users/Student/Documents/Exp\xC3\xA9riences/chute libre.trk"},
        {"&OSPSubject=223",
         "https://www.compadre.org/osp/services/REST/osp_tracker.cfm?OSPType=Tracker",
         "https://www.compadre.org/osp/services/REST/osp_tracker.cfm?OSPType=Tracker&OSPSubject=223"},
        {"pendulum.trz!/html/info.html", "https://host/exp/", "https://host/exp/pendulum.trz!/html/info.html"},
        {"a.trk", R"(C:\base\)", "C:/base/a.trk"},
        {"a%20b.trk", "/base", "/base/a%20b.trk"},
    };
    for (const auto& item : cases)
    {
        EXPECT_EQ(library::resolvedPath(item[0], item[1]), item[2]) << item[0] << " | " << item[1];
    }
}

TEST(LibraryPaths, RelativePathMatchesWholeSegments)
{
    EXPECT_EQ(library::relativePath("C:/lib/exp/a.trk", "C:/lib"), "exp/a.trk");
    EXPECT_EQ(library::relativePath(R"(C:\lib\exp\a.trk)", R"(C:\lib\)"), "exp/a.trk");
    // OSP would return "rary/a.html", which resolves to a missing file.
    EXPECT_EQ(library::relativePath("C:/library/a.html", "C:/lib"), "../library/a.html");
    EXPECT_EQ(library::relativePath("/home/u/a.trk", "/home/u/lib/sub"), "../../a.trk");
    EXPECT_EQ(library::relativePath("/a/b", "/a/b"), "");
    EXPECT_EQ(library::relativePath("relative/a.trk", "/base"), "relative/a.trk");
    EXPECT_EQ(library::relativePath("/abs/a.trk", "relative"), "/abs/a.trk");
    EXPECT_EQ(library::relativePath("D:/other/a.trk", "C:/base"), "D:/other/a.trk");
    EXPECT_EQ(library::relativePath("/x/a.trk", "/1/2/3/4/5/6"), "/x/a.trk");
    EXPECT_EQ(library::relativePath("/1/a.trk", "/1/2/3/4/5"), "../../../../a.trk");
    EXPECT_EQ(library::relativePath("C:a.trk", "C:"), "C:a.trk");
}

TEST(LibraryPaths, UrlAndArchiveForms)
{
    EXPECT_TRUE(library::isHttp("http://x"));
    EXPECT_TRUE(library::isHttp("https://x"));
    EXPECT_FALSE(library::isHttp("HTTP://x"));
    EXPECT_FALSE(library::isHttp("file:/x"));
    EXPECT_TRUE(library::isJarZipTrz("a.trz"));
    EXPECT_TRUE(library::isJarZipTrz("a.zip"));
    EXPECT_TRUE(library::isJarZipTrz("a.jar"));
    EXPECT_FALSE(library::isJarZipTrz("a.TRZ"));
    const auto member = library::splitArchiveMember("C:/x/a.trz!/html/info.html");
    ASSERT_TRUE(member);
    EXPECT_EQ(member->archive, "C:/x/a.trz");
    EXPECT_EQ(member->member, "html/info.html");
    EXPECT_FALSE(library::splitArchiveMember("a.trz!info.html"));
    EXPECT_EQ(library::nonUriPath("jar:file:/C:/Tracker%20Lib/a.trz!/b.html"), "C:/Tracker Lib/a.trz!/b.html");
    EXPECT_EQ(library::nonUriPath("file:/home/u/a&b?.trk"), "/home/u/a_b_.trk");
    EXPECT_EQ(library::uriPath(R"( C:\Tracker Lib\a.trk )", library::UriPlatform::Windows), "file:/C:/Tracker Lib/a.trk");
    EXPECT_EQ(library::uriPath("home/u/a.trk", library::UriPlatform::Posix), "file://home/u/a.trk");
    EXPECT_EQ(library::uriPath("/home/u/dir", library::UriPlatform::Posix), "file:/home/u/dir/");
    EXPECT_EQ(library::uriPath("https://host/a b.trk", library::UriPlatform::Posix), "https://host/a%20b.trk");
    EXPECT_EQ(library::uriPath("jar:file:/a.jar!/b.png", library::UriPlatform::Windows), "jar:file:/a.jar!/b.png");
    EXPECT_EQ(library::uriPath("file:/a/b.png", library::UriPlatform::Windows), "file:/a/b.png");
    EXPECT_EQ(library::uriPath("", library::UriPlatform::Windows), "");
}

TEST(LibraryResource, TypeNames)
{
    for (const auto type : {ResourceType::Unknown,
                            ResourceType::Collection,
                            ResourceType::Tracker,
                            ResourceType::Ejs,
                            ResourceType::Video,
                            ResourceType::Image,
                            ResourceType::Html,
                            ResourceType::Pdf,
                            ResourceType::Data,
                            ResourceType::Url})
    {
        EXPECT_EQ(library::resourceTypeFromName(library::resourceTypeName(type)), type);
    }
    EXPECT_EQ(library::resourceTypeName(ResourceType::Ejs), "EJS");
    EXPECT_FALSE(library::resourceTypeFromName("video"));
    EXPECT_FALSE(library::resourceTypeFromName("Videos"));
}

TEST(LibraryResource, SettersTrimAndReportChanges)
{
    auto resource = library::makeResource("  Name  ");
    EXPECT_EQ(resource.name, "Name");
    EXPECT_FALSE(library::setName(resource, "Name "));
    EXPECT_TRUE(library::setDescription(resource, " <p>x</p> "));
    EXPECT_EQ(resource.description, "<p>x</p>");
    EXPECT_FALSE(library::setDescription(resource, "<p>x</p>"));
    EXPECT_TRUE(library::setBasePath(resource, " /base "));
    EXPECT_FALSE(library::setBasePath(resource, "/base"));
    EXPECT_EQ(library::effectiveType(resource), ResourceType::Unknown);
    EXPECT_FALSE(library::targetOf(resource));
    const auto collection = library::makeCollection("C");
    EXPECT_EQ(library::effectiveType(collection), ResourceType::Collection);
}

TEST(LibraryResource, TypeFromPath)
{
    const library::MediaTypeLookup media = [](std::string_view extension) -> std::optional<ResourceType>
    {
        if (extension == "mp4")
            return ResourceType::Video;
        if (extension == "png")
            return ResourceType::Image;
        if (extension == "txt")
            return ResourceType::Data;
        return std::nullopt;
    };
    EXPECT_EQ(library::typeFromPath("a.b", media), ResourceType::Html);
    EXPECT_EQ(library::typeFromPath("", media), ResourceType::Html);
    // Four bytes but two UTF-16 units: still "short".
    EXPECT_EQ(library::typeFromPath("\xC3\xA9\xC3\xA9", media), ResourceType::Html);
    EXPECT_EQ(library::typeFromPath("page.HTML", media), ResourceType::Html);
    EXPECT_EQ(library::typeFromPath("x.trk", media), ResourceType::Tracker);
    EXPECT_EQ(library::typeFromPath("x.TRZ", media), ResourceType::Tracker);
    EXPECT_EQ(library::typeFromPath("ServeFile.cfm?ID=1&TrackerSet=1", media), ResourceType::Tracker);
    EXPECT_EQ(library::typeFromPath("x.zip", media), ResourceType::Unknown);
    EXPECT_EQ(library::typeFromPath("x.pdf", media), ResourceType::Pdf);
    EXPECT_EQ(library::typeFromPath("x.ejs", media), ResourceType::Ejs);
    EXPECT_EQ(library::typeFromPath("x.mp4", media), ResourceType::Video);
    EXPECT_EQ(library::typeFromPath("x.png", media), ResourceType::Image);
    EXPECT_EQ(library::typeFromPath("x.txt", media), ResourceType::Unknown);
    EXPECT_EQ(library::typeFromPath("x.mov", media), ResourceType::Unknown);
    EXPECT_EQ(library::typeFromPath("noextension", media), ResourceType::Unknown);
    EXPECT_EQ(library::typeFromPath("x.mp4", {}), ResourceType::Unknown);
}

TEST(LibraryResource, SetTargetAndHtmlPathSideEffects)
{
    auto resource = library::makeResource("r");
    resource.thumbnail = "cached.png";
    EXPECT_TRUE(library::setTarget(resource, " run.trz ", {}));
    EXPECT_EQ(resource.target, "run.trz");
    EXPECT_FALSE(resource.thumbnail);
    EXPECT_EQ(resource.type, ResourceType::Tracker);
    EXPECT_FALSE(library::setTarget(resource, "run.trz", {}));
    EXPECT_TRUE(library::setTarget(resource, "archive.zip", {}));
    EXPECT_EQ(resource.type, ResourceType::Tracker) << "an Unknown type from the path does not replace the type";
    EXPECT_TRUE(library::setTarget(resource, "", {}));
    EXPECT_EQ(resource.type, ResourceType::Html) << "OSP classifies an empty target as HTML";

    auto page = library::makeResource("page");
    EXPECT_TRUE(library::setHtmlPath(page, " info.html "));
    EXPECT_EQ(page.htmlPath, "info.html");
    EXPECT_EQ(page.type, ResourceType::Html);
    EXPECT_FALSE(library::setHtmlPath(page, "info.html"));
    EXPECT_TRUE(library::setHtmlPath(page, ""));
    EXPECT_EQ(page.type, ResourceType::Unknown);
    page.target = "x.trk";
    library::setHtmlPath(page, "info.html");
    EXPECT_EQ(page.type, ResourceType::Unknown) << "a resource with a target keeps its type";

    auto collection = library::makeCollection("c");
    EXPECT_TRUE(library::setTarget(collection, " &OSPSubject=1 ", {}));
    EXPECT_EQ(collection.target, "&OSPSubject=1");
    EXPECT_TRUE(library::setHtmlPath(collection, "c.html"));
    EXPECT_FALSE(library::setType(collection, ResourceType::Tracker));
    EXPECT_EQ(collection.type, ResourceType::Unknown);
    EXPECT_TRUE(library::setType(page, ResourceType::Video));
    EXPECT_FALSE(library::setType(page, ResourceType::Video));
}

TEST(LibraryResource, MetadataAndProperties)
{
    auto resource = library::makeResource("r");
    EXPECT_TRUE(library::addMetadata(resource, {.key = "keywords", .value = "b"}));
    EXPECT_TRUE(library::addMetadata(resource, {.key = "author", .value = "z"}));
    EXPECT_TRUE(library::addMetadata(resource, {.key = "AUTHOR", .value = "y"}));
    EXPECT_FALSE(library::addMetadata(resource, {.key = "Author", .value = "z"}));
    EXPECT_FALSE(library::addMetadata(resource, {.key = "Viewport", .value = "width"}));
    EXPECT_FALSE(library::addMetadata(resource, {.key = "robots", .value = "none"}));
    EXPECT_TRUE(library::addMetadata(resource, {.key = "contact", .value = "c"}));
    const std::vector<library::Metadata> expected = {{.key = "AUTHOR", .value = "y"},
                                                     {.key = "Author", .value = "z"},
                                                     {.key = "Contact", .value = "c"},
                                                     {.key = "Keywords", .value = "b"}};
    EXPECT_EQ(resource.metadata, expected);
    EXPECT_TRUE(library::removeMetadata(resource, {.key = "Contact", .value = "c"}));
    EXPECT_FALSE(library::removeMetadata(resource, {.key = "Contact", .value = "c"}));
    library::setProperty(resource, "zeta", "1");
    library::setProperty(resource, "alpha", std::nullopt);
    library::setProperty(resource, "zeta", "2");
    ASSERT_EQ(resource.properties.size(), 2U);
    EXPECT_EQ(resource.properties.begin()->first, "alpha");
    EXPECT_EQ(resource.properties.at("zeta"), "2");
}

TEST(LibraryResource, ExternalHtmlUsesTheLowerCasedTarget)
{
    auto resource = library::makeResource("r");
    resource.htmlPath = "a.trz!/a_info.html";
    EXPECT_TRUE(library::hasExternalHtml(resource));
    resource.target = "a.trz";
    EXPECT_FALSE(library::hasExternalHtml(resource));
    resource.target = "A.TRZ";
    resource.htmlPath = "A.TRZ!/a_info.html";
    EXPECT_TRUE(library::hasExternalHtml(resource));
    resource.htmlPath = "info.html";
    EXPECT_TRUE(library::hasExternalHtml(resource));
}

TEST(LibraryResource, PathsResolveThroughTheLineage)
{
    auto root = library::makeCollection("root");
    root.basePath = "https://host/lib/";
    auto middle = library::makeCollection("middle");
    auto leaf = library::makeResource("leaf");
    leaf.target = "../a.trk";
    leaf.htmlPath = "a.html";
    std::vector<const Resource*> lineage{&root, &middle, &leaf};
    EXPECT_EQ(library::inheritedBasePath(lineage), "https://host/lib/");
    EXPECT_EQ(library::absoluteTarget(lineage), "https://host/a.trk");
    EXPECT_EQ(library::absoluteHtmlPath(lineage), "https://host/lib/a.html");
    middle.basePath = "/local/dir";
    EXPECT_EQ(library::absoluteTarget(lineage), "/local/a.trk");
    leaf.target.clear();
    leaf.htmlPath.clear();
    EXPECT_EQ(library::absoluteTarget(lineage), "");
    EXPECT_EQ(library::absoluteHtmlPath(lineage), "");
    EXPECT_FALSE(library::browserAbsoluteTarget(lineage, "C:/docs/collection.xml"));
    EXPECT_FALSE(library::browserHtmlPath(lineage, "C:/docs/collection.xml"));

    // Without base paths the browser falls back to the collection file's directory.
    auto alone = library::makeResource("alone");
    alone.target = "exp/a.trk";
    alone.htmlPath = "exp/a.html";
    std::vector<const Resource*> single{&alone};
    EXPECT_EQ(library::browserBasePath(single, R"(C:\docs\collection.xml)"), "C:/docs");
    EXPECT_EQ(library::browserAbsoluteTarget(single, R"(C:\docs\collection.xml)"), "C:/docs/exp/a.trk");
    EXPECT_EQ(library::browserHtmlPath(single, "https://host/c/collection.xml"), "https://host/c/exp/a.html");
    EXPECT_EQ(library::absoluteTarget(single), "exp/a.trk");

    // A collection that has children appends its target to the base path (ComPADRE query parameters).
    auto subject = library::makeCollection("subject");
    subject.basePath = "https://www.compadre.org/q?OSPType=Tracker";
    subject.target = "&OSPSubject=1";
    std::vector<const Resource*> query{&subject};
    EXPECT_EQ(library::browserAbsoluteTarget(query, ""), "https://www.compadre.org/q?OSPType=Tracker&OSPSubject=1");
    subject.children.push_back(library::makeResource("record"));
    subject.target = "/x";
    EXPECT_EQ(library::browserAbsoluteTarget(query, ""), "https://www.compadre.org/q?OSPType=Tracker/x");
}

TEST(LibraryResource, SameContentIgnoresOriginAndThumbnail)
{
    auto left = library::makeCollection("c");
    left.children.push_back(library::makeResource("a"));
    auto right = left;
    right.thumbnail = "t.png";
    EXPECT_TRUE(library::sameContent(left, right));
    right.children.front().name = "b";
    EXPECT_FALSE(library::sameContent(left, right));
    right = left;
    right.children.push_back(library::makeResource("extra"));
    EXPECT_FALSE(library::sameContent(left, right));
    right = left;
    right.kind = library::ResourceKind::Resource;
    EXPECT_FALSE(library::sameContent(left, right));
}

} // namespace
