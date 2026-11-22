#include "library/collection.hpp"
#include "library/compadre.hpp"
#include "library/resource.hpp"
#include "library/xml_dom.hpp"

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
namespace xml = motrack::library::xml;
using library::ComPadreMode;
using library::Resource;
using library::ResourceType;

std::string readFixture(std::string_view name)
{
    std::ifstream stream(std::filesystem::path(MOTRACK_FIXTURES) / "library" / "compadre" / name, std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error("missing fixture " + std::string(name));
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

constexpr std::string_view RELOAD = "https://www.compadre.org/osp/services/REST/osp_tracker.cfm?OSPType=Tracker&OSPSubject=1";

Resource loading()
{
    auto collection = library::makeCollection("records");
    library::setDescription(collection, "<p>Loading</p>");
    library::setTarget(collection, "&OSPSubject=1", {});
    return collection;
}

library::ComPadreOptions mode(ComPadreMode value)
{
    library::ComPadreOptions options;
    options.mode = value;
    return options;
}

std::vector<std::string> names(const Resource& collection)
{
    std::vector<std::string> result;
    result.reserve(collection.children.size());
    for (const auto& child : collection.children)
    {
        result.push_back(child.name);
    }
    return result;
}

TEST(LibraryXml, ParsesTheDomThatLibraryComPadreReads)
{
    const auto document = xml::parse("\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<!-- prolog -->\r\n<?pi data?>\r\n"
                                     "<root a=\"x&amp;y&#65;&#x42;&lt;&gt;&quot;&apos;\" b='multi\nline'>\r\n"
                                     "  <t>one &amp; two<!-- split -->three</t>\r\n"
                                     "  <c><![CDATA[<raw>]]>after</c>\r\n"
                                     "  <e/><e x=\"1\" />\r\n"
                                     "  <ns:q>\xC3\xA9</ns:q>\r\n"
                                     "  <?inner instruction?>\r"
                                     "</root>\r\n<!-- epilog -->\n");
    ASSERT_TRUE(document) << document.error().message;
    EXPECT_EQ(document->name, "root");
    EXPECT_EQ(document->attribute("a"), R"(x&yAB<>"')");
    EXPECT_EQ(document->attribute("b"), "multi line");
    EXPECT_EQ(document->attribute("missing"), "");
    EXPECT_EQ(xml::childValue(document.operator->(), "t"), std::optional<std::string_view>("one & two"));
    EXPECT_EQ(xml::childValue(document.operator->(), "c"), std::optional<std::string_view>("after")) << "CDATA sections are not Text nodes";
    EXPECT_EQ(xml::childValue(document.operator->(), "e"), std::nullopt);
    EXPECT_EQ(xml::childValue(document.operator->(), "ns:q"), std::optional<std::string_view>("\xC3\xA9"));
    EXPECT_EQ(xml::childValue(nullptr, "t"), std::nullopt);
    EXPECT_EQ(document->childrenNamed("e").size(), 2U);
    EXPECT_EQ(document->firstChild("missing"), nullptr);
    EXPECT_EQ(xml::elementsByTagName(*document, "e").size(), 2U);
    EXPECT_EQ(xml::elementsByTagName(*document, "root").size(), 1U);
    EXPECT_EQ(document->textValue(), std::optional<std::string_view>("\n  ")) << "line ends are normalized";
}

TEST(LibraryXml, RejectsMalformedDocuments)
{
    const std::vector<std::string_view> invalid = {
        "",
        "text",
        "<a>",
        "<a></b>",
        "<a><b></a>",
        R"(<a x="1" x="2"/>)",
        "<a x=1/>",
        "<a x/>",
        R"(<a x="<"/>)",
        R"(<a x="&unknown;"/>)",
        R"(<a x="unterminated/>)",
        R"(<a x="1"y="2"/>)",
        "<a>&#xZZ;</a>",
        "<a>&#0;</a>",
        "<a>&#;</a>",
        "<a>&#123456789;</a>",
        "<a>&amp</a>",
        "<a>]]></a>",
        "<a><![CDATA[x</a>",
        "<a><!-- x -- y --></a>",
        "<a><!-- x</a>",
        "<a><?pi x</a>",
        R"(<a><?xml version="1.0"?></a>)",
        "<a><!ELEMENT x></a>",
        "<!DOCTYPE a><a/>",
        R"(<?xml version="1.0" encoding="ISO-8859-1"?><a/>)",
        R"(<?xml version="1.0" encoding="UTF-8"<a/>)",
        R"(<?xml version="1.0" encoding="UTF-8><a/>)",
        "<a/><b/>",
        "<a/>text",
        "<a / >",
        "<1a/>",
        "<a></>",
        "<a></a >x",
        "<a>\x01</a>",
        "<a>\xC3</a>",
        "<a>\xC0\x80</a>",
        "<a>\xED\xA0\x80</a>",
        "<a>\xF8\x88\x80\x80\x80</a>",
        "<a>\xE2\x82</a>",
        "<a>\xE2\x28\xA1</a>",
        "<a></a",
    };
    for (const auto text : invalid)
    {
        EXPECT_FALSE(xml::parse(text)) << text;
    }
    EXPECT_TRUE(xml::parse("<a>&#x10FFFF;&#65;\xF0\x9F\x98\x80</a>"));
    const auto references = xml::parse("<a>&#233;&#x20ac;&#x1F600;</a>");
    ASSERT_TRUE(references);
    EXPECT_EQ(references->textValue(), std::optional<std::string_view>("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));
    EXPECT_TRUE(xml::parse(R"(<?xml version="1.0" encoding='utf8'?><a/>)"));
    EXPECT_TRUE(xml::parse("<a></a   >"));
    EXPECT_FALSE(xml::parse("<a><b><c/></b></a>", xml::Limits{.maxDepth = 2}));
    EXPECT_FALSE(xml::parse("<a><b/><c/></a>", xml::Limits{.maxNodes = 2}));
    EXPECT_FALSE(xml::parse("<a/>", xml::Limits{.maxBytes = 2}));
    EXPECT_FALSE(xml::parse("<a>text<b/></a>", xml::Limits{.maxNodes = 2}));
    EXPECT_FALSE(xml::parse("<a><!--c--></a>", xml::Limits{.maxNodes = 1}));
    EXPECT_FALSE(xml::parse("<a><?p?></a>", xml::Limits{.maxNodes = 1}));
    EXPECT_FALSE(xml::parse("<a><![CDATA[x]]></a>", xml::Limits{.maxNodes = 1}));
    EXPECT_FALSE(xml::parse("<a>x</a>", xml::Limits{.maxNodes = 1}));
}

TEST(LibraryComPadre, QueryHelpers)
{
    const std::string primary = std::string(library::COMPADRE_TRACKER_TREE) + std::string(library::COMPADRE_PRIMARY_ONLY);
    EXPECT_TRUE(library::isComPadrePath(primary));
    EXPECT_FALSE(library::isComPadrePath("https://example.org/osp"));
    EXPECT_TRUE(library::isPrimarySubjectOnly(primary));
    EXPECT_EQ(library::comPadreCollectionName(primary), library::COMPADRE_TRACKER_COLLECTION_NAME);
    EXPECT_EQ(library::comPadreCollectionName(library::COMPADRE_EJS_TREE), library::COMPADRE_EJS_COLLECTION_NAME);
    EXPECT_EQ(library::comPadreCollectionName(library::COMPADRE_TRACKER_RECORDS), library::COMPADRE_GENERIC_COLLECTION_NAME);
    EXPECT_EQ(library::comPadreCollectionPath(primary, true), primary);
    EXPECT_EQ(library::comPadreCollectionPath(primary, false), library::COMPADRE_TRACKER_TREE);
    EXPECT_EQ(library::comPadreCollectionPath(library::COMPADRE_TRACKER_TREE, true), primary);
    EXPECT_EQ(library::comPadreCollectionPath(library::COMPADRE_TRACKER_TREE, false), library::COMPADRE_TRACKER_TREE);
}

TEST(LibraryComPadre, MapsTheSubjectTree)
{
    auto collection = library::makeCollection("tree");
    ASSERT_TRUE(library::loadComPadreTree(collection, readFixture("tracker-tree.rest")));
    // Only the osp-subject set is read; its Unclassified sub-tree becomes the collection's own query.
    EXPECT_EQ(names(collection), (std::vector<std::string>{"Classical Mechanics", "Optics", ""}));
    EXPECT_EQ(collection.target, "&OSPSubject=0");
    const auto& mechanics = collection.children[0];
    EXPECT_EQ(names(mechanics),
              (std::vector<std::string>{"Applications of Newton's Laws", "Motion in One Dimension", "Padded name", "Wrong detail type"}));
    const auto& newton = mechanics.children[0];
    EXPECT_EQ(newton.target, "&OSPSubject=223&OSPSubjectDetail=241&OSPSubjectDetailDetail=0_Extra=x_y");
    ASSERT_EQ(newton.children.size(), 1U);
    EXPECT_EQ(newton.children[0].target, "&OSPSubject=223&OSPSubjectDetail=241&OSPSubjectDetailDetail=1001");
    EXPECT_EQ(newton.children[0].description, "<h2>Friction</h2><blockquote>...</blockquote>");
    EXPECT_EQ(mechanics.children[2].target, "&OSPSubject=223&OSPSubjectDetail=251 b");
    EXPECT_EQ(mechanics.children[2].description, "<h2>  Padded name  </h2><blockquote>...</blockquote>");
    EXPECT_TRUE(mechanics.children[3].children.empty()) << "a detail set of another type is not read";
    EXPECT_EQ(mechanics.children[3].target, "");
    EXPECT_EQ(collection.children[1].target, "&OSPSubject=231");

    auto untouched = library::makeCollection("x");
    EXPECT_FALSE(library::loadComPadreTree(untouched, "<not xml"));
    EXPECT_FALSE(library::loadComPadreTree(untouched, "<Identify/>"));
    EXPECT_TRUE(untouched.children.empty());
}

TEST(LibraryComPadre, BuildsTheCollectionsTrackerOffers)
{
    const auto tree = readFixture("tracker-tree.rest");
    const auto tracker =
        library::comPadreCollection(std::string(library::COMPADRE_TRACKER_TREE) + std::string(library::COMPADRE_PRIMARY_ONLY), tree);
    EXPECT_EQ(tracker.name, library::COMPADRE_TRACKER_COLLECTION_NAME);
    EXPECT_EQ(tracker.htmlPath, library::COMPADRE_TRACKER_INFO_URL);
    EXPECT_EQ(tracker.basePath, std::string(library::COMPADRE_TRACKER_RECORDS) + std::string(library::COMPADRE_PRIMARY_ONLY));
    ASSERT_EQ(tracker.children.size(), 4U);
    EXPECT_EQ(tracker.children[0].name, library::COMPADRE_ABOUT_OSP);
    EXPECT_EQ(tracker.children[0].type, ResourceType::Html);
    std::vector<const Resource*> lineage{&tracker, &tracker.children[1], &tracker.children[1].children[1]};
    EXPECT_EQ(library::browserAbsoluteTarget(lineage, ""),
              std::string(library::COMPADRE_TRACKER_RECORDS) + "&OSPPrimary=Subject&OSPSubject=223&OSPSubjectDetail=250");
    const auto ejs = library::comPadreCollection(library::COMPADRE_EJS_TREE, "<Identify/>");
    EXPECT_EQ(ejs.htmlPath, library::COMPADRE_EJS_INFO_URL);
    EXPECT_EQ(ejs.basePath, library::COMPADRE_EJS_RECORDS);
    const auto generic = library::comPadreCollection("https://www.compadre.org/osp/services/REST/osp_other.cfm", "");
    EXPECT_EQ(generic.htmlPath, "");
    EXPECT_EQ(generic.basePath, library::COMPADRE_EJS_RECORDS) << "OSP gives other collections the EJS record query";
    // The collection round-trips through the collection writer.
    const auto written = library::writeCollection(tracker);
    ASSERT_TRUE(written);
    EXPECT_TRUE(library::sameContent(library::loadCollection(*written)->root, library::reloadedForm(tracker)));
}

TEST(LibraryComPadre, MapsRecordsAsTracker635Does)
{
    auto collection = loading();
    const auto result =
        library::loadComPadreRecords(collection, readFixture("tracker-records.rest"), RELOAD, mode(ComPadreMode::Tracker635));
    EXPECT_EQ(result.records, 7U);
    EXPECT_EQ(result.added, 4U);
    EXPECT_TRUE(result.completed);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(collection.description, "");
    EXPECT_EQ(collection.target, "");
    EXPECT_EQ(names(collection),
              (std::vector<std::string>{"Rolling Ball TRZ", "Rolling ball video", "Record without contributors", "Paragraph description"}));
    const auto& first = collection.children[0];
    EXPECT_EQ(first.target, "https://www.compadre.org/osp/document/ServeFile.cfm?ID=90001&DocID=1&TrackerSet=1");
    EXPECT_EQ(first.type, ResourceType::Tracker);
    EXPECT_EQ(first.thumbnail, "https://www.compadre.org/profiles/thumbnail.jpg");
    EXPECT_EQ(first.properties.at(library::DOWNLOAD_FILENAME_PROPERTY), "rolling-ball.trz");
    EXPECT_EQ(first.properties.at(library::RELOAD_URL_PROPERTY), RELOAD);
    const std::vector<library::Metadata> metadata = {
        {.key = "Author", .value = "A. Author, B. Author"},
        {.key = "Keywords", .value = "Classical Mechanics, Motion in One Dimension, Velocity, Acceleration"}};
    EXPECT_EQ(first.metadata, metadata);
    EXPECT_EQ(first.description,
              "<h2>Rolling Ball TRZ</h2>\n      <blockquote>\n        <b>Tracker Experiment</b>\n"
              "        <p><img src=\"https://www.compadre.org/profiles/thumbnail.jpg\" alt=\"Rolling Ball TRZ\"></p>\n"
              "        <p>A synthetic ball rolling on a track.<br>Second line.</p>\n"
              "        <p><b>Author:  </b>A. Author, B. Author</p>\n"
              "        <p><b>Resource:  </b>rolling-ball.trz (1.1MB)</p>\n"
              R"(        <p><b>More information at:  </b><a href="https://www.compadre.org/osp/items/detail.cfm?ID=90001">)"
              "https://www.compadre.org/osp/items/detail.cfm?ID=90001</a></p>\n      </blockquote>");
    // #213: Tracker opens the item's main TRZ for the video record.
    EXPECT_EQ(collection.children[1].properties.at(library::DOWNLOAD_FILENAME_PROPERTY), "rolling-ball.trz");
    // Missing contributors end setRecord early: no description, metadata, thumbnail or reload URL.
    const auto& partial = collection.children[2];
    EXPECT_EQ(partial.target, "https://www.compadre.org/osp/document/ServeFile.cfm?ID=90003&DocID=4");
    EXPECT_EQ(partial.description, "");
    EXPECT_TRUE(partial.metadata.empty());
    EXPECT_FALSE(partial.thumbnail);
    EXPECT_FALSE(partial.properties.contains(library::RELOAD_URL_PROPERTY));
    // A later Main attachment with the record's file identifier replaces the first match.
    EXPECT_EQ(collection.children[3].properties.at(library::DOWNLOAD_FILENAME_PROPERTY), "second.trz");
    EXPECT_NE(collection.children[3].description.find("<p>Already an HTML paragraph.</p>"), std::string::npos);
    EXPECT_EQ(collection.children[3].description.find("More information"), std::string::npos) << "a URL without protocol is omitted";
}

TEST(LibraryComPadre, OpensEachRecordsOwnFile)
{
    auto collection = loading();
    const auto result = library::loadComPadreRecords(collection, readFixture("tracker-records.rest"), RELOAD);
    EXPECT_EQ(result.added, 5U);
    EXPECT_EQ(names(collection),
              (std::vector<std::string>{"Rolling Ball TRZ",
                                        "Rolling ball video",
                                        "Experiment attached as source code",
                                        "Record without contributors",
                                        "Paragraph description"}));
    EXPECT_EQ(collection.children[1].properties.at(library::DOWNLOAD_FILENAME_PROPERTY), "rolling-ball.mp4");
    EXPECT_EQ(collection.children[1].target, "https://www.compadre.org/osp/document/ServeFile.cfm?ID=90001&DocID=2&TrackerSet=1");
    EXPECT_EQ(collection.children[2].properties.at(library::DOWNLOAD_FILENAME_PROPERTY), "harmonic.trz");
    EXPECT_NE(collection.children[2].description.find("harmonic.trz (0.0MB)"), std::string::npos);
    const auto& complete = collection.children[3];
    EXPECT_NE(complete.description.find("optics.zip (0.2MB)"), std::string::npos) << "262144 bytes round half to even";
    EXPECT_EQ(complete.thumbnail, "https://www.compadre.org/profiles/other.jpg");
    EXPECT_EQ(complete.metadata.size(), 1U);
    EXPECT_EQ(complete.properties.at(library::RELOAD_URL_PROPERTY), RELOAD);
}

TEST(LibraryComPadre, AMalformedRecordEndsTrackersList)
{
    const auto text = readFixture("records-stop.rest");
    auto tracker = loading();
    const auto stopped = library::loadComPadreRecords(tracker, text, RELOAD, mode(ComPadreMode::Tracker635));
    EXPECT_FALSE(stopped.completed);
    EXPECT_EQ(stopped.added, 1U);
    EXPECT_EQ(tracker.description, "<p>Loading</p>") << "the clean-up after loading never runs";
    EXPECT_EQ(tracker.target, "&OSPSubject=1");

    auto motrack = loading();
    const auto all = library::loadComPadreRecords(motrack, text, RELOAD);
    EXPECT_TRUE(all.completed);
    EXPECT_EQ(names(motrack),
              (std::vector<std::string>{
                  "Before the malformed record", "Missing file identifier: Tracker stops here", "After the malformed record"}));
    EXPECT_NE(motrack.children[1].description.find("missing-size.trz (0.0MB)"), std::string::npos);
    EXPECT_EQ(motrack.target, "");
}

TEST(LibraryComPadre, HandlesOtherDesiredTypesAndInputs)
{
    const std::string records = R"(<records>
  <record><file-identifier>1</file-identifier><title>EJS model</title><contributors/>
    <osp-type>EJS Model</osp-type>
    <attached-document><file-identifier>9</file-identifier><file-type>Main</file-type><download-url>main.jar</download-url></attached-document>
    <attached-document><file-identifier>8</file-identifier><file-type>Source Code</file-type><file-name file-size="10">model.ejs</file-name>
      <download-url>source.zip</download-url></attached-document></record>
  <record><file-identifier>2</file-identifier><title>Tracker model</title><contributors/>
    <osp-type>A tracking model</osp-type>
    <attached-document><file-identifier>2</file-identifier><file-type>Main</file-type><download-url>t.trz</download-url></attached-document></record>
  <record><file-identifier>3</file-identifier><title>Other</title><contributors/>
    <osp-type>Something else</osp-type>
    <attached-document><file-identifier>3</file-identifier><file-type>Main</file-type><download-url>o.pdf</download-url></attached-document></record>
  <record><file-identifier>4</file-identifier><title>No text</title><contributors/>
    <osp-type/>
    <attached-document><file-identifier>4</file-identifier><file-type>Main</file-type><download-url>n.trz</download-url></attached-document></record>
</records>)";
    library::ComPadreOptions ejs;
    ejs.desiredOspType = "EJS";
    auto models = library::makeCollection("ejs");
    library::loadComPadreRecords(models, records, RELOAD, ejs);
    ASSERT_GE(models.children.size(), 1U);
    EXPECT_EQ(models.children[0].type, ResourceType::Ejs);
    EXPECT_EQ(models.children[0].target, "source.zip");
    EXPECT_NE(models.children[0].description.find("<b>EJS Model</b>"), std::string::npos);

    library::ComPadreOptions any;
    any.desiredOspType.clear();
    auto all = library::makeCollection("any");
    library::loadComPadreRecords(all, records, RELOAD, any);
    ASSERT_EQ(all.children.size(), 4U);
    EXPECT_EQ(all.children[0].type, ResourceType::Ejs);
    EXPECT_EQ(all.children[1].type, ResourceType::Tracker);
    EXPECT_EQ(all.children[2].type, ResourceType::Unknown);
    EXPECT_NE(all.children[2].description.find("<b>Something else</b>"), std::string::npos);
    EXPECT_EQ(all.children[3].type, ResourceType::Unknown);

    auto strictAny = library::makeCollection("strict");
    any.mode = ComPadreMode::Tracker635;
    library::loadComPadreRecords(strictAny, records, RELOAD, any);
    EXPECT_EQ(strictAny.children.size(), 4U);

    library::ComPadreOptions other;
    other.desiredOspType = "Model";
    other.mode = ComPadreMode::Tracker635;
    auto tracking = library::makeCollection("model");
    library::loadComPadreRecords(tracking, records, RELOAD, other);
    EXPECT_EQ(tracking.children.size(), 1U) << "the osp-type without text makes Tracker stop";

    auto broken = loading();
    const auto failed = library::loadComPadreRecords(broken, "<records", RELOAD);
    EXPECT_TRUE(failed.error);
    EXPECT_FALSE(failed.success);
    EXPECT_EQ(broken.description, "");
    auto none = loading();
    const auto empty = library::loadComPadreRecords(none, "<records/>", RELOAD);
    EXPECT_EQ(empty.records, 0U);
    EXPECT_TRUE(empty.completed);
    EXPECT_FALSE(empty.success);
    auto notCollection = library::makeResource("r");
    EXPECT_EQ(library::loadComPadreRecords(notCollection, records, RELOAD).records, 0U);
}

TEST(LibraryComPadre, RecordsWithMissingFieldsInTracker635)
{
    const std::string records = R"(<records>
  <record><file-identifier>1</file-identifier><title>No URL text</title><contributors/>
    <osp-type>Tracker</osp-type>
    <attached-document><file-identifier>1</file-identifier><file-type>Main</file-type><download-url/></attached-document></record>
  <record><file-identifier>2</file-identifier><title>Null subject and author</title>
    <contributors><contributor role="Author"/></contributors>
    <osp-type>Tracker</osp-type><osp-subject/>
    <attached-document><file-identifier>2</file-identifier><file-type>Main</file-type><download-url>x.trz</download-url></attached-document></record>
  <record><file-identifier>3</file-identifier><title>Bad size</title><contributors/>
    <osp-type>Tracker</osp-type>
    <attached-document><file-identifier>3</file-identifier><file-type>Main</file-type><file-name file-size="big">x</file-name>
      <download-url>y.trz</download-url></attached-document></record>
</records>)";
    auto tracker = library::makeCollection("t");
    const auto result = library::loadComPadreRecords(tracker, records, RELOAD, mode(ComPadreMode::Tracker635));
    EXPECT_FALSE(result.completed);
    ASSERT_EQ(tracker.children.size(), 2U);
    EXPECT_EQ(tracker.children[0].target, "") << "a download URL without text makes setRecord fail";
    EXPECT_EQ(tracker.children[1].target, "x.trz");
    EXPECT_TRUE(tracker.children[1].metadata.empty());
    EXPECT_NE(tracker.children[1].description.find("null"), std::string::npos) << R"(Java appends "null" for an author without text)";

    auto motrack = library::makeCollection("m");
    const auto robust = library::loadComPadreRecords(motrack, records, RELOAD);
    EXPECT_TRUE(robust.completed);
    EXPECT_EQ(names(motrack), (std::vector<std::string>{"Null subject and author", "Bad size"}));
    EXPECT_EQ(motrack.children[0].description.find("null"), std::string::npos);
}

TEST(LibraryComPadre, HtmlBodyAndSizes)
{
    library::ComPadreLabels labels;
    labels.decimalSeparator = ',';
    const library::ComPadreAttachment attachment{.type = "Main", .url = "u", .fileName = "f.trz", .size = 786432};
    EXPECT_EQ(library::comPadreHtmlBody("", "", std::nullopt, std::nullopt, "", std::nullopt, std::nullopt, labels),
              "\n      <blockquote>\n      </blockquote>");
    constexpr auto TRACKER = ComPadreMode::Tracker635;
    const auto body =
        library::comPadreHtmlBody("", "Custom", "thumb.png", "a\n\n", "", "mailto:someone@example.org", attachment, labels, TRACKER);
    EXPECT_EQ(body,
              "\n      <blockquote>\n        <b>Custom</b>\n        <p><img src=\"thumb.png\"></p>\n        <p>a</p>\n"
              "        <p><b>Resource:  </b>f.trz (0,8MB)</p>\n"
              "        <p><b>More information at:  </b><a href=\"mailto:someone@example.org\">mailto:someone@example.org</a></p>\n"
              "      </blockquote>");
    for (const auto url : {"url:http://example.org", "jar:file:/a.jar!/b", "ftp://host/x", " https://host ", "jmod:/x"})
    {
        EXPECT_NE(library::comPadreHtmlBody("", "", std::nullopt, std::nullopt, "", url, std::nullopt, labels, TRACKER).find("<a href"),
                  std::string::npos)
            << url;
    }
    for (const auto url : {"jar:file:/a.jar", "1http://x", "noscheme", ":x", "/path:x", "gopher://x", "ht~tp://x"})
    {
        EXPECT_EQ(library::comPadreHtmlBody("", "", std::nullopt, std::nullopt, "", url, std::nullopt, labels, TRACKER).find("<a href"),
                  std::string::npos)
            << url;
    }
    // MoTrack escapes the record's text and links only to the web.
    const library::ComPadreAttachment hostile{.type = "Main", .url = "u", .fileName = "<b>x</b>.trz", .size = 0};
    const auto escaped = library::comPadreHtmlBody(R"(T" onmouseover="x)",
                                                   "<i>type</i>",
                                                   "file:///etc/passwd",
                                                   "<p>kept HTML</p>",
                                                   "A & <script>",
                                                   "javascript:alert(1)",
                                                   hostile,
                                                   labels);
    EXPECT_EQ(escaped,
              "\n      <h2>T&quot; onmouseover=&quot;x</h2>\n      <blockquote>\n        <b>&lt;i&gt;type&lt;/i&gt;</b>\n"
              "        <p>kept HTML</p>\n        <p><b>Author:  </b>A &amp; &lt;script&gt;</p>\n"
              "        <p><b>Resource:  </b>&lt;b&gt;x&lt;/b&gt;.trz (0,0MB)</p>\n      </blockquote>");
    const auto linked = library::comPadreHtmlBody(
        "It's", "", "https://host/t.jpg?a=1&b=2", std::nullopt, "", "https://host/i?x=\"y\"", std::nullopt, labels);
    EXPECT_NE(linked.find(R"(<img src="https://host/t.jpg?a=1&amp;b=2" alt="It&#39;s">)"), std::string::npos);
    EXPECT_NE(linked.find(R"(<a href="https://host/i?x=&quot;y&quot;">)"), std::string::npos);
    const auto raw =
        library::comPadreHtmlBody(R"(T" onmouseover="x)", "", std::nullopt, std::nullopt, "", std::nullopt, std::nullopt, labels, TRACKER);
    EXPECT_NE(raw.find(R"(<h2>T" onmouseover="x</h2>)"), std::string::npos) << "Tracker inserts the title unchanged";
    EXPECT_EQ(library::comPadreMegabytes(1165166, '.'), "1.1");
    EXPECT_EQ(library::comPadreMegabytes(262144, '.'), "0.2");
    EXPECT_EQ(library::comPadreMegabytes(786432, '.'), "0.8");
    EXPECT_EQ(library::comPadreMegabytes(0, '.'), "0.0");
    EXPECT_EQ(library::comPadreMegabytes(-5, '.'), "-0.0");
    EXPECT_EQ(library::comPadreMegabytes(2147483647, '.'), "2048.0");
}

} // namespace
