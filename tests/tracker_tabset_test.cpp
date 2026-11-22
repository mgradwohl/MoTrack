#include "tracker_tabset.hpp"

#include "legacy_project.hpp"
#include "osp_layout.hpp"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <vector>

namespace
{

using motrack::TabsetEntry;
using motrack::TabsetItem;
using motrack::TabsetItemStatus;

constexpr auto PANEL = R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">
</object>
)";

constexpr std::string_view TFRAME_OPEN = R"(<object class="org.opensourcephysics.cabrillo.tracker.TFrame">)";

void writeFile(const QString& path, std::string_view bytes)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).path()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes.data(), static_cast<qint64>(bytes.size())), static_cast<qint64>(bytes.size()));
}

std::string readFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll().toStdString();
}

void writeTabset(const QString& path, const std::vector<TabsetEntry>& entries)
{
    const auto xml = motrack::writeTrackerTabset(entries);
    ASSERT_TRUE(xml.has_value()) << xml.error();
    writeFile(path, *xml);
}

TabsetEntry relativeEntry(std::string relative)
{
    return {.absolute = std::nullopt, .relative = std::move(relative)};
}

QString canonical(const QString& path)
{
    return QFileInfo(path).canonicalFilePath();
}

std::vector<TabsetItemStatus> statuses(const std::vector<TabsetItem>& items)
{
    auto view = items | std::views::transform(&TabsetItem::status);
    return {view.begin(), view.end()};
}

std::vector<TabsetEntry> entriesOf(const std::string& xml)
{
    auto parsed = motrack::parseTrackerTabset(xml);
    EXPECT_TRUE(parsed.has_value()) << (parsed ? std::string{} : parsed.error()) << '\n' << xml;
    return parsed ? parsed->entries : std::vector<TabsetEntry>{};
}

/// A tabset as Tracker on Windows writes it: CRLF line breaks and no final one, here with a property Tracker ignores.
constexpr std::string_view TRACKER_WRITTEN = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                                             "<object class=\"org.opensourcephysics.cabrillo.tracker.TFrame\">\r\n"
                                             "    <property name=\"note\" type=\"string\">kept as written</property>\r\n"
                                             "    <property name=\"tabs\" type=\"array\" class=\"[[Ljava.lang.String;\">\r\n"
                                             "        <property name=\"[0]\" type=\"array\" class=\"[Ljava.lang.String;\">\r\n"
                                             "            <property name=\"[0]\" type=\"string\">C:/old/a.trk</property>\r\n"
                                             "            <property name=\"[1]\" type=\"string\">a.trk</property>\r\n"
                                             "        </property>\r\n"
                                             "        <property name=\"[1]\" type=\"array\" class=\"[Ljava.lang.String;\">\r\n"
                                             "            <property name=\"[0]\" type=\"string\">C:/old/b.trz</property>\r\n"
                                             "            <property name=\"[1]\" type=\"string\">b.trz</property>\r\n"
                                             "        </property>\r\n"
                                             "    </property>\r\n"
                                             "</object>";

std::string replaced(std::string text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    EXPECT_NE(position, std::string::npos) << from;
    if (position != std::string::npos)
        text.replace(position, from.size(), to);
    return text;
}

TEST(TrackerTabset, WritesOspXmlControlLayout)
{
    const std::vector<TabsetEntry> entries{
        {.absolute = "/data/run 1/ball.trk", .relative = "run 1/ball.trk"},
        {.absolute = "/data/R&D/cart's.trz", .relative = "R&D/cart's.trz"},
        relativeEntry("video.mov"),
    };
    const auto xml = motrack::writeTrackerTabset(entries);
    ASSERT_TRUE(xml.has_value()) << xml.error();
    // XMLControlElement.toString, which Tracker writes without a final line break.
    EXPECT_EQ(*xml,
              R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.cabrillo.tracker.TFrame">
    <property name="tabs" type="array" class="[[Ljava.lang.String;">
        <property name="[0]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string">/data/run 1/ball.trk</property>
            <property name="[1]" type="string">run 1/ball.trk</property>
        </property>
        <property name="[1]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string"><![CDATA[/data/R&D/cart's.trz]]></property>
            <property name="[1]" type="string"><![CDATA[R&D/cart's.trz]]></property>
        </property>
        <property name="[2]" type="array" class="[Ljava.lang.String;">
            <property name="[1]" type="string">video.mov</property>
        </property>
    </property>
</object>)");
    EXPECT_EQ(entriesOf(*xml), entries);
    EXPECT_TRUE(motrack::isTrackerTabset(*xml));
    EXPECT_FALSE(motrack::isTrackerTabset(PANEL));
    EXPECT_FALSE(motrack::isTrackerTabset("<object"));
}

// Deliberately lenient: OSP sizes an array from its last element, so Tracker cannot load out-of-order indices,
// but MoTrack accepts any unique dense order. MoTrack always writes indices in ascending order.
TEST(TrackerTabset, ParsesIndicesInAnyOrderUnicodeAndIgnoresUnknownProperties)
{
    const auto xml = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.cabrillo.tracker.TFrame">
    <property name="unknown" type="string">kept by Tracker's loader as unused</property>
    <property name="tabs" type="array" class="[[Ljava.lang.String;">
        <property name="[1]" type="array" class="[Ljava.lang.String;">
            <property name="[1]" type="string">b.trk</property>
            <property name="[0]" type="string">/x/b.trk</property>
        </property>
        <property name="[0]" type="array" class="[Ljava.lang.String;">
            <property name="[0]" type="string">/x/)") +
                     "\xC3\xA9t\xC3\xA9.trk" +
                     R"(</property>
            <property name="[1]" type="string">)" +
                     "\xC3\xA9t\xC3\xA9.trk" + R"(</property>
        </property>
    </property>
</object>)";
    const auto parsed = motrack::parseTrackerTabset(xml);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    ASSERT_EQ(parsed->entries.size(), 2U);
    EXPECT_EQ(parsed->entries[0].relative, "\xC3\xA9t\xC3\xA9.trk");
    EXPECT_EQ(parsed->entries[1], (TabsetEntry{.absolute = "/x/b.trk", .relative = "b.trk"}));
    EXPECT_FALSE(parsed->ordered);
    EXPECT_FALSE(parsed->nodes[1].ordered);
    EXPECT_TRUE(parsed->nodes[0].ordered);

    const auto empty = motrack::parseTrackerTabset(std::string(TFRAME_OPEN).replace(TFRAME_OPEN.size() - 1, 1, "/>"));
    ASSERT_TRUE(empty.has_value()) << empty.error();
    EXPECT_TRUE(empty->entries.empty());
    EXPECT_FALSE(empty->tabs);
}

TEST(TrackerTabset, ReadsTabsetsAsTrackersLineReaderDoes)
{
    const auto entry = [](std::string_view relative)
    {
        return std::format("        <property name=\"[0]\" type=\"array\" class=\"[Ljava.lang.String;\">\n"
                           "            <property name=\"[1]\" type=\"string\">{}</property>\n"
                           "        </property>\n",
                           relative);
    };
    const auto document = [](std::string_view before, std::string_view tabs)
    {
        return std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n{}\n{}"
                           "    <property name=\"tabs\" type=\"array\" class=\"[[Ljava.lang.String;\">\n{}    </property>\n</object>",
                           TFRAME_OPEN,
                           before,
                           tabs);
    };
    // OSP decodes no entities and reads element-like text literally; Tracker would look for exactly these files.
    EXPECT_EQ(entriesOf(document("", entry("R&amp;D/a<b/>.trk"))), std::vector{relativeEntry("R&amp;D/a<b/>.trk")});
    // A document type is inert text, so this is an empty TFrame, as in Tracker.
    EXPECT_TRUE(entriesOf(std::format("<!DOCTYPE x>{}", std::string(TFRAME_OPEN).replace(TFRAME_OPEN.size() - 1, 1, "/>"))).empty());
    // A blank line ends an array (#196): Tracker reads only the first tab, and the second becomes an unused property.
    auto second = entry("second.trk");
    second.replace(second.find("[0]"), 3, "[1]");
    EXPECT_EQ(entriesOf(document("", entry("first.trk") + "\n" + second)), std::vector{relativeEntry("first.trk")});
    // A saved basepath string replaces the tabset's folder as the base, as XMLControlElement.getString returns it.
    const auto based =
        motrack::parseTrackerTabset(document("    <property name=\"basepath\" type=\"string\">/elsewhere</property>\n", entry("a.trk")));
    ASSERT_TRUE(based.has_value()) << based.error();
    EXPECT_EQ(based->basepath, "/elsewhere");
    EXPECT_EQ(motrack::tabsetBase(*based, "/set/tabs.trk"), "/elsewhere");
    const auto unbased =
        motrack::parseTrackerTabset(document("    <property name=\"basepath\" type=\"int\">3</property>\n", entry("a.trk")));
    ASSERT_TRUE(unbased.has_value()) << unbased.error();
    EXPECT_FALSE(unbased->basepath);
    // CRLF, as Tracker writes on Windows.
    EXPECT_EQ(entriesOf(std::string(TRACKER_WRITTEN)).size(), 2U);

    // Unreadable XML is reported as Tracker import reports it.
    const auto windows1252 = motrack::parseTrackerTabset(document("", entry("caf\xE9.trk")));
    ASSERT_FALSE(windows1252.has_value());
    EXPECT_TRUE(windows1252.error().starts_with("Invalid Tracker XML at line")) << windows1252.error();
    EXPECT_TRUE(windows1252.error().contains("Windows-1252")) << windows1252.error();
    const auto truncated = motrack::parseTrackerTabset(document("", entry("a.trk")).substr(0, 200));
    ASSERT_FALSE(truncated.has_value());
    EXPECT_TRUE(truncated.error().starts_with("Invalid Tracker XML")) << truncated.error();
}

TEST(TrackerTabset, RejectsMalformedOrUnsupportedStructure)
{
    const auto frame = [](std::string_view tabs)
    {
        return motrack::test::ospLayout(std::format(R"(<object class="org.opensourcephysics.cabrillo.tracker.TFrame">{}</object>)", tabs));
    };
    const auto tabs = [](std::string_view children)
    {
        return std::format(R"(<property name="tabs" type="array" class="[[Ljava.lang.String;">{}</property>)", children);
    };
    const auto matrix = [&](std::string_view children)
    {
        return frame(tabs(children));
    };
    const auto entry = [](std::string_view index, std::string_view children)
    {
        return std::format(R"(<property name="[{}]" type="array" class="[Ljava.lang.String;">{}</property>)", index, children);
    };
    const auto relative = R"(<property name="[1]" type="string">a.trk</property>)";
    const std::vector<std::string> invalid{
        "<object",
        PANEL,
        frame(R"(<property name="tabs" type="string">a.trk</property>)"),
        frame(R"(<property name="tabs" type="array" class="[Ljava.lang.String;"/>)"),
        matrix(std::format("{}{}", entry("0", relative), entry("0", relative))),
        matrix(entry("1", relative)),
        matrix(entry("01", relative)),
        matrix(entry("-1", relative)),
        matrix(entry("array", relative)),
        matrix(R"(<property name="[0]" type="array" class="[Ljava.lang.Object;"/>)"),
        matrix(R"(<property name="[0]" type="string">a.trk</property>)"),
        matrix(entry("0", R"(<property name="[0]" type="string">/a.trk</property>)")),
        matrix(entry("0", R"(<property name="[1]" type="string"></property>)")),
        matrix(entry("0", R"(<property name="[1]" type="int">1</property>)")),
        matrix(entry("0", R"(<property name="[2]" type="string">x</property>)")),
        matrix(entry("0", std::format("{}{}", relative, relative))),
        frame(tabs("") + tabs("")),
    };
    for (const auto& xml : invalid)
        EXPECT_FALSE(motrack::parseTrackerTabset(xml).has_value()) << xml;

    std::string many;
    for (std::size_t index = 0; index <= motrack::MAX_TABSET_ENTRIES; ++index)
        many += entry(std::to_string(index), relative);
    EXPECT_FALSE(motrack::parseTrackerTabset(matrix(many)).has_value());
    EXPECT_FALSE(motrack::parseTrackerTabset(std::string(motrack::MAX_TRACKER_XML_BYTES + 1, ' ')).has_value());

    EXPECT_FALSE(motrack::writeTrackerTabset(std::vector{relativeEntry("a]]>b.trk")}).has_value());
    EXPECT_FALSE(motrack::writeTrackerTabset(std::vector{TabsetEntry{.absolute = "/a]]>", .relative = "a.trk"}}).has_value());
    EXPECT_FALSE(motrack::writeTrackerTabset(std::vector{relativeEntry("")}).has_value());
    EXPECT_FALSE(motrack::writeTrackerTabset(std::vector{relativeEntry("a\nb.trk")}).has_value());
    EXPECT_FALSE(motrack::writeTrackerTabset(std::vector{relativeEntry("<![CDATA[a.trk")}).has_value());
    EXPECT_FALSE(motrack::writeTrackerTabset(std::vector(motrack::MAX_TABSET_ENTRIES + 1, relativeEntry("a.trk"))).has_value());
}

TEST(TrackerTabset, SavingEditsOnlyTheChangedEntriesAndLeavesAnUnchangedSetByteForByte)
{
    const std::string original(TRACKER_WRITTEN);
    const auto parsed = motrack::parseTrackerTabset(original);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    const auto render = [&](const std::vector<TabsetEntry>& entries)
    {
        auto rendered = motrack::renderTrackerTabset(*parsed, entries);
        EXPECT_TRUE(rendered.has_value()) << (rendered ? std::string{} : rendered.error());
        if (rendered)
            EXPECT_EQ(entriesOf(*rendered), entries);
        return rendered.value_or(std::string{});
    };
    const auto a = parsed->entries[0];
    const auto b = parsed->entries[1];
    const TabsetEntry c{.absolute = "D:/new/R&D.mov", .relative = "../new/R&D.mov"};

    EXPECT_EQ(render(parsed->entries), original);
    // A changed entry has only its paths replaced, with CDATA where XML.requiresCDATA asks for it.
    EXPECT_EQ(render({a, c}),
              replaced(replaced(original, "C:/old/b.trz", "<![CDATA[D:/new/R&D.mov]]>"), ">b.trz<", "><![CDATA[../new/R&D.mov]]><"));
    EXPECT_EQ(render({{.absolute = std::nullopt, .relative = a.relative}, b}),
              replaced(original, "            <property name=\"[0]\" type=\"string\">C:/old/a.trk</property>\r\n", ""));
    EXPECT_EQ(render({b}),
              replaced(original,
                       "        <property name=\"[0]\" type=\"array\" class=\"[Ljava.lang.String;\">\r\n"
                       "            <property name=\"[0]\" type=\"string\">C:/old/a.trk</property>\r\n"
                       "            <property name=\"[1]\" type=\"string\">a.trk</property>\r\n"
                       "        </property>\r\n"
                       "        <property name=\"[1]\" type=\"array\" class=\"[Ljava.lang.String;\">\r\n"
                       "            <property name=\"[0]\" type=\"string\">C:/old/b.trz</property>\r\n"
                       "            <property name=\"[1]\" type=\"string\">b.trz</property>\r\n",
                       "        <property name=\"[0]\" type=\"array\" class=\"[Ljava.lang.String;\">\r\n"
                       "            <property name=\"[0]\" type=\"string\">C:/old/b.trz</property>\r\n"
                       "            <property name=\"[1]\" type=\"string\">b.trz</property>\r\n"));
    // A new entry follows the others in OSP's layout and the file's own line breaks.
    EXPECT_EQ(render({a, b, relativeEntry("c.mov")}),
              replaced(original,
                       "    </property>\r\n</object>",
                       "        <property name=\"[2]\" type=\"array\" class=\"[Ljava.lang.String;\">\r\n"
                       "            <property name=\"[1]\" type=\"string\">c.mov</property>\r\n"
                       "        </property>\r\n"
                       "    </property>\r\n</object>"));

    // Entries without an absolute path receive one before their relative path.
    const auto relativeOnly = motrack::writeTrackerTabset(std::vector{relativeEntry("a.trk")});
    ASSERT_TRUE(relativeOnly.has_value());
    const auto bare = motrack::parseTrackerTabset(*relativeOnly);
    ASSERT_TRUE(bare.has_value());
    const auto withAbsolute = motrack::renderTrackerTabset(*bare, std::vector{a});
    ASSERT_TRUE(withAbsolute.has_value()) << withAbsolute.error();
    EXPECT_EQ(*withAbsolute, motrack::writeTrackerTabset(std::vector{a}).value());

    // Paths OSP cannot read back are refused as for a new tabset, here by the entry check and by the renderer.
    EXPECT_FALSE(motrack::renderTrackerTabset(*parsed, std::vector{relativeEntry("a]]>.trk")}).has_value());
    const auto refused = motrack::renderTrackerTabset(*parsed, std::vector{a, relativeEntry("<![CDATA[b.trz")});
    ASSERT_FALSE(refused.has_value());
    EXPECT_TRUE(refused.error().starts_with("Could not write the tabset:")) << refused.error();
    EXPECT_FALSE(motrack::renderTrackerTabset(motrack::TrackerTabset{}, std::vector{a}).has_value());
}

TEST(TrackerTabset, SavingAddsMissingTabsAndRewritesOrderTrackerCannotRead)
{
    // An empty, self-closing TFrame receives a tabs array.
    const auto empty = motrack::parseTrackerTabset(std::string(TFRAME_OPEN).replace(TFRAME_OPEN.size() - 1, 1, "/>"));
    ASSERT_TRUE(empty.has_value());
    const std::vector<TabsetEntry> entries{relativeEntry("a.trk"), {.absolute = "/x/b.trk", .relative = "b.trk"}};
    const auto filled = motrack::renderTrackerTabset(*empty, entries);
    ASSERT_TRUE(filled.has_value()) << filled.error();
    EXPECT_EQ(entriesOf(*filled), entries);

    const auto reversed = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.cabrillo.tracker.TFrame">
    <property name="tabs" type="array" class="[[Ljava.lang.String;">
        <property name="[1]" type="array" class="[Ljava.lang.String;">
            <property name="[1]" type="string">b.trk</property>
        </property>
        <property name="[0]" type="array" class="[Ljava.lang.String;">
            <property name="[1]" type="string">a.trk</property>
            <property name="[0]" type="string">/x/a.trk</property>
        </property>
    </property>
    <property name="after" type="string">kept</property>
</object>)");
    const auto parsed = motrack::parseTrackerTabset(reversed);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    // Unedited, the file is kept as it was read.
    EXPECT_EQ(motrack::renderTrackerTabset(*parsed, parsed->entries).value(), reversed);
    // Edited, Tracker's reading order is restored; the other property is untouched.
    const std::vector<TabsetEntry> edited{parsed->entries[0], relativeEntry("c.trk")};
    const auto rewritten = motrack::renderTrackerTabset(*parsed, edited);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error();
    const auto reread = motrack::parseTrackerTabset(*rewritten);
    ASSERT_TRUE(reread.has_value()) << reread.error();
    EXPECT_EQ(reread->entries, edited);
    EXPECT_TRUE(reread->ordered);
    EXPECT_TRUE(rewritten->ends_with("    <property name=\"after\" type=\"string\">kept</property>\n</object>")) << *rewritten;

    // An entry whose own paths are out of order is rewritten in order when it changes.
    auto inner = reversed;
    inner.replace(inner.find("[1]\" type=\"array"), 3, "[9]");
    inner = replaced(inner,
                     "        <property name=\"[9]\" type=\"array\" class=\"[Ljava.lang.String;\">\n"
                     "            <property name=\"[1]\" type=\"string\">b.trk</property>\n"
                     "        </property>\n",
                     "");
    const auto single = motrack::parseTrackerTabset(inner);
    ASSERT_TRUE(single.has_value()) << single.error();
    ASSERT_TRUE(single->ordered);
    ASSERT_FALSE(single->nodes[0].ordered);
    const auto moved = std::vector{TabsetEntry{.absolute = "/y/a.trk", .relative = "a.trk"}};
    const auto edit = motrack::renderTrackerTabset(*single, moved);
    ASSERT_TRUE(edit.has_value()) << edit.error();
    const auto again = motrack::parseTrackerTabset(*edit);
    ASSERT_TRUE(again.has_value()) << again.error();
    EXPECT_EQ(again->entries, moved);
    EXPECT_TRUE(again->nodes[0].ordered);
    // Any edit also rewrites an unchanged entry Tracker cannot read, since Tracker would refuse the whole tabset.
    const auto appended = motrack::renderTrackerTabset(*single, std::vector{single->entries[0], relativeEntry("c.trk")});
    ASSERT_TRUE(appended.has_value()) << appended.error();
    const auto repaired = motrack::parseTrackerTabset(*appended);
    ASSERT_TRUE(repaired.has_value()) << repaired.error();
    EXPECT_EQ(repaired->entries, (std::vector{single->entries[0], relativeEntry("c.trk")}));
    EXPECT_TRUE(repaired->nodes[0].ordered);

    // Tracker writes a frame without saveable tabs as a self-closing tabs array, which receives the entries.
    const std::string closed = R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.cabrillo.tracker.TFrame">
    <property name="tabs" type="array" class="[[Ljava.lang.String;"/>
</object>)";
    const auto none = motrack::parseTrackerTabset(closed);
    ASSERT_TRUE(none.has_value()) << none.error();
    ASSERT_TRUE(none->tabs);
    EXPECT_EQ(motrack::renderTrackerTabset(*none, std::vector<TabsetEntry>{}).value(), closed);
    const auto opened = motrack::renderTrackerTabset(*none, entries);
    ASSERT_TRUE(opened.has_value()) << opened.error();
    EXPECT_EQ(entriesOf(*opened), entries);
    EXPECT_TRUE(opened->starts_with(closed.substr(0, closed.find("    <property name=\"tabs\""))));

    // An entry kept as it was read need not meet the rules for new paths, here an empty absolute path.
    const auto blank = replaced(
        *motrack::writeTrackerTabset(std::vector{TabsetEntry{.absolute = "x", .relative = "a.trk"}}), ">x</property>", "></property>");
    const auto kept = motrack::parseTrackerTabset(blank);
    ASSERT_TRUE(kept.has_value()) << kept.error();
    ASSERT_EQ(kept->entries.front().absolute, "");
    EXPECT_EQ(motrack::renderTrackerTabset(*kept, kept->entries).value(), blank);
    const auto extended = motrack::renderTrackerTabset(*kept, std::vector{kept->entries.front(), relativeEntry("b.trk")});
    ASSERT_TRUE(extended.has_value()) << extended.error();
    EXPECT_EQ(entriesOf(*extended), (std::vector{kept->entries.front(), relativeEntry("b.trk")}));
    EXPECT_FALSE(motrack::writeTrackerTabset(kept->entries).has_value());
}

TEST(TrackerTabset, ComputesTrackerRelativePaths)
{
#ifdef Q_OS_WIN
    const QString root = "C:";
#else
    const QString root;
#endif
    const auto path = [&](const char* suffix)
    {
        return root + suffix;
    };
    EXPECT_EQ(motrack::tabsetRelativePath(path("/data/set/a.trk"), path("/data/set")), "a.trk");
    EXPECT_EQ(motrack::tabsetRelativePath(path("/data/set/sub/a.trk"), path("/data/set/")), "sub/a.trk");
    EXPECT_EQ(motrack::tabsetRelativePath(path("/data/other/a.trk"), path("/data/set")), "../other/a.trk");
    EXPECT_EQ(motrack::tabsetRelativePath(path("/a/x.trk"), path("/a/1/2/3/4/5")), "../../../../../x.trk");
    // OSP stops after five parent levels and stores the absolute path.
    EXPECT_EQ(motrack::tabsetRelativePath(path("/a/x.trk"), path("/a/1/2/3/4/5/6")), path("/a/x.trk"));
    // OSP's String.startsWith treats /data/a as an ancestor of /data/ab; MoTrack requires a component boundary.
    EXPECT_EQ(motrack::tabsetRelativePath(path("/data/ab/x.trk"), path("/data/a")), "../ab/x.trk");
    EXPECT_EQ(motrack::tabsetRelativePath("relative/x.trk", path("/data")), "relative/x.trk");
#ifdef Q_OS_WIN
    // A Windows drive is a usable common ancestor in OSP's climb; another drive is not.
    EXPECT_EQ(motrack::tabsetRelativePath("C:/a/b.trk", "C:/x"), "../a/b.trk");
    EXPECT_EQ(motrack::tabsetRelativePath("D:/a/b.trk", "C:/x"), "D:/a/b.trk");
    EXPECT_EQ(motrack::tabsetRelativePath("c:\\Data\\Set\\A.trk", "C:/data/set"), "A.trk");
    EXPECT_EQ(motrack::tabsetRelativePath("//server/share/a/b.trk", "//server/share/c"), "../a/b.trk");
    EXPECT_EQ(motrack::tabsetRelativePath("//server/other/b.trk", "//server/share/c"), "//server/other/b.trk");
#else
    // OSP never climbs through the POSIX root.
    EXPECT_EQ(motrack::tabsetRelativePath("/a/b.trk", "/x"), "/a/b.trk");
    // Case differs on POSIX, so only the root is shared and the absolute path is stored.
    EXPECT_EQ(motrack::tabsetRelativePath("/Data/Set/A.trk", "/data/set"), "/Data/Set/A.trk");
#endif
    const auto entry = motrack::tabsetEntry(path("/data/set/a.trk"), path("/data/set"));
    EXPECT_EQ(entry.absolute, path("/data/set/a.trk").toStdString());
    EXPECT_EQ(entry.relative, "a.trk");
}

TEST(TrackerTabset, ResolvesInTrackerOrder)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto tabsetDirectory = directory.filePath("set");
    const auto working = directory.filePath("working");
    writeFile(tabsetDirectory + "/a.trk", PANEL);
    writeFile(working + "/a.trk", PANEL);
    writeFile(working + "/b.trk", PANEL);
    writeFile(directory.filePath("absolute/c.trk"), PANEL);
    const auto absolute = canonical(directory.filePath("absolute/c.trk")).toStdString();

    EXPECT_EQ(motrack::resolveTabsetEntry(TabsetEntry{.absolute = absolute, .relative = "a.trk"}, tabsetDirectory, working),
              canonical(tabsetDirectory + "/a.trk"));
    EXPECT_EQ(motrack::resolveTabsetEntry(TabsetEntry{.absolute = absolute, .relative = "b.trk"}, tabsetDirectory, working),
              canonical(working + "/b.trk"));
    EXPECT_EQ(motrack::resolveTabsetEntry(TabsetEntry{.absolute = absolute, .relative = "missing.trk"}, tabsetDirectory, working),
              QString::fromStdString(absolute));
    EXPECT_EQ(motrack::resolveTabsetEntry(TabsetEntry{.absolute = absolute, .relative = "../absolute/c.trk"}, tabsetDirectory, {}),
              QString::fromStdString(absolute));
    EXPECT_EQ(motrack::resolveTabsetEntry(relativeEntry("missing.trk"), tabsetDirectory, working), std::nullopt);
    // An absolute relative slot is joined to its parent by java.io.File, so only the absolute slot applies.
    EXPECT_EQ(motrack::resolveTabsetEntry(relativeEntry(absolute), tabsetDirectory, working), std::nullopt);
    EXPECT_EQ(motrack::resolveTabsetEntry(TabsetEntry{.absolute = absolute, .relative = absolute}, tabsetDirectory, working),
              QString::fromStdString(absolute));
    // Directories are not files.
    EXPECT_EQ(motrack::resolveTabsetEntry(relativeEntry("."), tabsetDirectory, working), std::nullopt);
}

TEST(TrackerTabset, ExpandsNestedTabsetsInOpenOrder)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = directory.path();
    writeFile(base + "/a.trk", PANEL);
    writeFile(base + "/b.trz", "not inspected");
    writeFile(base + "/nested/c.trk", PANEL);
    writeFile(base + "/video \xC3\xA9.mov", "not inspected");
    writeFile(
        base + "/broken.trk",
        motrack::test::ospLayout(R"(<object class="org.opensourcephysics.cabrillo.tracker.TFrame"><property name="tabs"/></object>)"));
    writeTabset(base + "/nested/inner.trk",
                {
                    relativeEntry("c.trk"),
                    relativeEntry("../a.trk"),
                    relativeEntry("../top.trk"),
                    relativeEntry("inner.trk"),
                });
    writeTabset(base + "/top.trk",
                {
                    relativeEntry("a.trk"),
                    relativeEntry("nested/inner.trk"),
                    TabsetEntry{.absolute = canonical(base + "/b.trz").toStdString(), .relative = "moved/b.trz"},
                    relativeEntry("missing.trk"),
                    relativeEntry("broken.trk"),
                    relativeEntry("video \xC3\xA9.mov"),
                    relativeEntry("nested/../a.trk"),
                });
    const auto items = motrack::expandTrackerTabset(base + "/top.trk", {});
    EXPECT_EQ(statuses(items),
              (std::vector{TabsetItemStatus::Resolved,
                           TabsetItemStatus::Resolved,
                           TabsetItemStatus::Duplicate,
                           TabsetItemStatus::Cycle,
                           TabsetItemStatus::Cycle,
                           TabsetItemStatus::Resolved,
                           TabsetItemStatus::Missing,
                           TabsetItemStatus::Unreadable,
                           TabsetItemStatus::Resolved,
                           TabsetItemStatus::Duplicate}));
    ASSERT_EQ(items.size(), 10U);
    EXPECT_EQ(items[0].file, canonical(base + "/a.trk"));
    EXPECT_EQ(items[1].file, canonical(base + "/nested/c.trk"));
    EXPECT_EQ(items[1].tabset, canonical(base + "/nested/inner.trk"));
    EXPECT_EQ(items[5].file, canonical(base + "/b.trz"));
    EXPECT_EQ(items[6].entry.relative, "missing.trk");
    EXPECT_TRUE(items[6].file.isEmpty());
    EXPECT_EQ(items[7].file, canonical(base + "/broken.trk"));
    EXPECT_EQ(items[7].diagnostic, "The tabset's tabs property must be a string matrix.");
    EXPECT_EQ(items[8].file, canonical(base + "/video \xC3\xA9.mov"));
}

TEST(TrackerTabset, ResolvesAgainstASavedBasepathFirst)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = directory.path();
    writeFile(base + "/set/a.trk", PANEL);
    writeFile(base + "/elsewhere/a.trk", PANEL);
    writeFile(base + "/set/b.trk", PANEL);
    const auto xml = motrack::writeTrackerTabset(std::vector{relativeEntry("a.trk"), relativeEntry("b.trk")});
    ASSERT_TRUE(xml.has_value());
    const auto withBase = replaced(*xml,
                                   "    <property name=\"tabs\"",
                                   std::format("    <property name=\"basepath\" type=\"string\">{}</property>\n    <property name=\"tabs\"",
                                               QDir(base).filePath("elsewhere").toStdString()));
    writeFile(base + "/set/tabs.trk", withBase);
    const auto items = motrack::expandTrackerTabset(base + "/set/tabs.trk", {});
    ASSERT_EQ(items.size(), 2U);
    // Tracker looks in the basepath, then the working directory, then the absolute path; never the tabset's folder.
    EXPECT_EQ(items[0].file, canonical(base + "/elsewhere/a.trk"));
    EXPECT_EQ(items[1].status, TabsetItemStatus::Missing);
}

TEST(TrackerTabset, BoundsDepthAndTotalEntries)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = directory.path();
    for (int level = 0; level < motrack::MAX_TABSET_DEPTH + 1; ++level)
        writeTabset(base + std::format("/level{}.trk", level).c_str(), {relativeEntry(std::format("level{}.trk", level + 1))});
    writeFile(base + std::format("/level{}.trk", motrack::MAX_TABSET_DEPTH + 1).c_str(), PANEL);
    const auto deep = motrack::expandTrackerTabset(base + "/level0.trk", {});
    ASSERT_EQ(deep.size(), 1U);
    EXPECT_EQ(deep.front().status, TabsetItemStatus::TooDeep);
    EXPECT_EQ(deep.front().file, canonical(base + std::format("/level{}.trk", motrack::MAX_TABSET_DEPTH).c_str()));

    // Two nested tabsets of 200 entries exceed the total bound once, without a report per nesting level.
    for (const auto* name : {"left", "right"})
    {
        std::vector<TabsetEntry> entries;
        for (int index = 0; index < 200; ++index)
        {
            const auto file = std::format("{}-{}.trk", name, index);
            writeFile(base + "/" + file.c_str(), PANEL);
            entries.push_back(relativeEntry(file));
        }
        writeTabset(base + "/" + name + ".trk", entries);
    }
    writeTabset(base + "/many.trk", {relativeEntry("left.trk"), relativeEntry("right.trk")});
    const auto many = motrack::expandTrackerTabset(base + "/many.trk", {});
    ASSERT_EQ(many.size(), motrack::MAX_TABSET_ENTRIES + 1);
    EXPECT_EQ(std::ranges::count(many, TabsetItemStatus::TooMany, &TabsetItem::status), 1);
    EXPECT_EQ(many.back().status, TabsetItemStatus::TooMany);
}

TEST(TrackerTabset, ExpansionFailuresThrowOnlyForTheRequestedTabset)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = directory.path();
    EXPECT_THROW((void)motrack::expandTrackerTabset(base + "/missing.trk", {}), motrack::ProjectError);
    writeFile(base + "/panel.trk", PANEL);
    EXPECT_THROW((void)motrack::expandTrackerTabset(base + "/panel.trk", {}), motrack::ProjectError);
    writeTabset(base + "/set.trk", {relativeEntry("panel.trk")});
    const std::atomic_bool cancelled{true};
    EXPECT_THROW((void)motrack::expandTrackerTabset(base + "/set.trk", {}, &cancelled), motrack::ProjectError);
    const std::atomic_bool running{false};
    EXPECT_EQ(motrack::expandTrackerTabset(base + "/set.trk", {}, &running).size(), 1U);
    EXPECT_FALSE(motrack::readTrackerTabset(base + "/missing.trk").has_value());
    EXPECT_FALSE(motrack::isTrackerTabsetFile(base + "/missing.trk"));
}

TEST(TrackerTabset, KeepsEntriesThatStillNameTheirFiles)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    // Canonical, so that relative paths compare equal where the temporary folder is a link (macOS /var).
    const auto base = canonical(directory.path());
    writeFile(base + "/a.trk", PANEL);
    writeFile(base + "/b.trz", "not inspected");
    writeFile(base + "/c.mov", "not inspected");
    const auto a = canonical(base + "/a.trk");
    const auto b = canonical(base + "/b.trz");
    const auto c = canonical(base + "/c.mov");
    const auto set = base + "/set.trk";
    // As Tracker writes it: stale absolute paths from before the folder moved, and the archive once per tab.
    const std::vector<TabsetEntry> saved{
        {.absolute = "/moved/a.trk", .relative = "a.trk"},
        {.absolute = "/moved/b.trz", .relative = "b.trz"},
        {.absolute = "/moved/b.trz", .relative = "b.trz"},
    };
    writeTabset(set, saved);
    const auto bytes = readFile(set);
    const auto existing = motrack::readTrackerTabset(set);
    ASSERT_TRUE(existing.has_value()) << existing.error();
    const auto entries = [&](std::vector<QString> files)
    {
        return motrack::tabsetEntries(files, set, {}, &*existing);
    };

    // Saving the same files changes nothing, byte for byte.
    EXPECT_EQ(entries({a, b}), saved);
    ASSERT_TRUE(motrack::saveTrackerTabset(set, entries({a, b}), &*existing).has_value());
    EXPECT_EQ(readFile(set), bytes);
    // Reordered and added files keep the existing entries they still match; new entries follow.
    EXPECT_EQ(entries({b, a}), (std::vector{saved[1], saved[2], saved[0]}));
    EXPECT_EQ(entries({a, b, c}), (std::vector{saved[0], saved[1], saved[2], motrack::tabsetEntry(c, base)}));
    EXPECT_EQ(entries({c, a, b}), (std::vector{motrack::tabsetEntry(c, base), saved[0], saved[1], saved[2]}));
    // A file that is not listed keeps its entry unless the user closed it; either way the change is reported.
    motrack::TabsetChanges changes;
    EXPECT_EQ(motrack::tabsetEntries(std::vector{b}, set, {}, &*existing, {}, &changes), saved);
    EXPECT_EQ(changes.unopened, std::vector{saved[0]});
    EXPECT_TRUE(changes.closed.empty() && changes.repeated.empty());
    const auto closed = std::vector{motrack::tabsetIdentityKey(a)};
    changes = {};
    EXPECT_EQ(motrack::tabsetEntries(std::vector{b}, set, {}, &*existing, closed, &changes), (std::vector{saved[1], saved[2]}));
    EXPECT_EQ(changes.closed, std::vector{saved[0]});
    EXPECT_TRUE(changes.unopened.empty() && changes.repeated.empty());
    // Without an existing tabset every entry is new.
    EXPECT_EQ(motrack::tabsetEntries(std::vector{a}, set, {}), std::vector{motrack::tabsetEntry(a, base)});
    // An entry naming no file stays where it is, with new entries after it.
    writeTabset(set, {saved[0], relativeEntry("gone.trk"), saved[1]});
    const auto withMissing = motrack::readTrackerTabset(set);
    ASSERT_TRUE(withMissing.has_value()) << withMissing.error();
    changes = {};
    EXPECT_EQ(motrack::tabsetEntries(std::vector{a, c, b}, set, {}, &*withMissing, {}, &changes),
              (std::vector{saved[0], relativeEntry("gone.trk"), motrack::tabsetEntry(c, base), saved[1]}));
    EXPECT_EQ(changes.unopened, std::vector{relativeEntry("gone.trk")});
    // MoTrack opens each file once, so a repeat that does not follow the file's entry is dropped, and reported.
    writeTabset(set, {saved[0], saved[1], saved[0]});
    const auto repeated = motrack::readTrackerTabset(set);
    ASSERT_TRUE(repeated.has_value()) << repeated.error();
    changes = {};
    EXPECT_EQ(motrack::tabsetEntries(std::vector{a, b}, set, {}, &*repeated, {}, &changes), (std::vector{saved[0], saved[1]}));
    EXPECT_EQ(changes.repeated, std::vector{saved[0]});
    // After the tabset moves alone, an entry whose relative path no longer finds its file is rewritten.
    writeFile(base + "/sets/moved.trk",
              motrack::writeTrackerTabset(std::vector{TabsetEntry{.absolute = a.toStdString(), .relative = "a.trk"}}).value());
    const auto alone = motrack::readTrackerTabset(base + "/sets/moved.trk");
    ASSERT_TRUE(alone.has_value()) << alone.error();
    EXPECT_EQ(motrack::tabsetEntries(std::vector{a}, base + "/sets/moved.trk", {}, &*alone),
              std::vector{motrack::tabsetEntry(a, base + "/sets")});
    writeFile(set, bytes);

    // A saved basepath stays the base for new entries, so Tracker finds them where MoTrack wrote them.
    const auto based = replaced(bytes,
                                "    <property name=\"tabs\"",
                                std::format("    <property name=\"basepath\" type=\"string\">{}</property>\n    <property name=\"tabs\"",
                                            QDir(base).filePath("sub").toStdString()));
    writeFile(base + "/sub/a.trk", PANEL);
    writeFile(set, based);
    const auto rebased = motrack::readTrackerTabset(set);
    ASSERT_TRUE(rebased.has_value()) << rebased.error();
    const auto inSub = canonical(base + "/sub/a.trk");
    const auto withBase = motrack::tabsetEntries(std::vector{inSub, c}, set, {}, &*rebased);
    ASSERT_EQ(withBase.size(), 4U);
    EXPECT_EQ(withBase[0], saved[0]); // "a.trk" now names sub/a.trk through the basepath.
    EXPECT_EQ(withBase[3].relative, "../c.mov");
}

TEST(TrackerTabset, SavingKeepsNestedTabsetsRefusesOtherFilesAndUsesTrackersExtension)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto base = canonical(directory.path());
    for (const auto* name : {"/a.trk", "/c.trk", "/d.trk", "/panel.trk"})
        writeFile(base + name, PANEL);
    writeTabset(base + "/inner.trk", {relativeEntry("c.trk"), relativeEntry("d.trk")});
    const auto top = base + "/top.trk";
    writeTabset(top, {relativeEntry("a.trk"), relativeEntry("inner.trk"), relativeEntry("top.trk")});
    const auto bytes = readFile(top);
    const std::vector files{canonical(base + "/a.trk"), canonical(base + "/c.trk"), canonical(base + "/d.trk")};
    using Status = motrack::TabsetSave::Status;

    // The workspace holds the files the nested tabset opened; its entry stays for them, and the cycle stays too.
    auto saved = motrack::saveTrackerTabsetFiles(top, files, {});
    EXPECT_EQ(saved.status, Status::Saved) << saved.diagnostic;
    EXPECT_EQ(saved.changes.unopened, std::vector{relativeEntry("top.trk")});
    EXPECT_EQ(readFile(top), bytes);
    // A listed file keeps an entry of its own even when a nested tabset opens it as well.
    writeTabset(top, {relativeEntry("inner.trk"), relativeEntry("c.trk")});
    EXPECT_EQ(motrack::saveTrackerTabsetFiles(top, files, {}).status, Status::Saved);
    EXPECT_EQ(entriesOf(readFile(top)),
              (std::vector{relativeEntry("inner.trk"), motrack::tabsetEntry(files[0], base), relativeEntry("c.trk")}));

    // Other files are refused and left unchanged; a new tabset lists every file.
    EXPECT_EQ(motrack::saveTrackerTabsetFiles(base + "/panel.trk", files, {}).status, Status::NotTabset);
    EXPECT_EQ(readFile(base + "/panel.trk"), PANEL);
    const auto broken = motrack::test::ospLayout(
        R"(<object class="org.opensourcephysics.cabrillo.tracker.TFrame"><property name="tabs" type="string">a.trk</property></object>)");
    writeFile(base + "/broken.trk", broken);
    saved = motrack::saveTrackerTabsetFiles(base + "/broken.trk", files, {});
    EXPECT_EQ(saved.status, Status::Unreadable);
    EXPECT_FALSE(saved.diagnostic.empty());
    EXPECT_EQ(readFile(base + "/broken.trk"), broken);
    EXPECT_EQ(motrack::saveTrackerTabsetFiles(base + "/missing/new.trk", files, {}).status, Status::Failed);
    EXPECT_EQ(motrack::saveTrackerTabsetFiles(base + "/new.trk", files, {}).status, Status::Saved);
    EXPECT_EQ(entriesOf(readFile(base + "/new.trk")).size(), 3U);

    // TrackerIO.fixXML: the extension becomes .trk.
    EXPECT_EQ(motrack::trackerTabsetPath("/x/set.trz"), "/x/set.trk");
    EXPECT_EQ(motrack::trackerTabsetPath("/x/Lab 3.5"), "/x/Lab 3.trk");
    EXPECT_EQ(motrack::trackerTabsetPath("/x.y/lesson"), "/x.y/lesson.trk");
    EXPECT_EQ(motrack::trackerTabsetPath("/x/lesson.TRK"), "/x/lesson.TRK");
    EXPECT_EQ(motrack::trackerTabsetPath("lesson"), "lesson.trk");
}

TEST(TrackerTabset, SavesAtomicallyAndPreservesExistingFilesOnFailure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("\xE6\xB5\x8B\xE8\xAF\x95 set.trk");
    const std::vector<TabsetEntry> entries{relativeEntry("a.trk")};
    ASSERT_TRUE(motrack::saveTrackerTabset(path, entries).has_value());
    const auto saved = readFile(path);
    EXPECT_EQ(entriesOf(saved), entries);

    EXPECT_FALSE(motrack::saveTrackerTabset(path, std::vector{relativeEntry("a]]>.trk")}).has_value());
    EXPECT_EQ(readFile(path), saved);
    EXPECT_FALSE(motrack::saveTrackerTabset(directory.filePath("missing/set.trk"), entries).has_value());
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

} // namespace
