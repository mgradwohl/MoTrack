#include "tracker_tabset.hpp"

#include "legacy_project.hpp"

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

TEST(TrackerTabset, WritesOspXmlControlLayout)
{
    const std::vector<TabsetEntry> entries{
        {.absolute = "/data/run 1/ball.trk", .relative = "run 1/ball.trk"},
        {.absolute = "/data/R&D/cart's.trz", .relative = "R&D/cart's.trz"},
        relativeEntry("video.mov"),
    };
    const auto xml = motrack::writeTrackerTabset(entries);
    ASSERT_TRUE(xml.has_value());
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
</object>
)");
    const auto parsed = motrack::parseTrackerTabset(*xml);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, entries);
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
    ASSERT_EQ(parsed->size(), 2U);
    EXPECT_EQ((*parsed)[0].relative, "\xC3\xA9t\xC3\xA9.trk");
    EXPECT_EQ((*parsed)[1], (TabsetEntry{.absolute = "/x/b.trk", .relative = "b.trk"}));

    const auto empty = motrack::parseTrackerTabset(R"(<object class="org.opensourcephysics.cabrillo.tracker.TFrame"/>)");
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
}

TEST(TrackerTabset, RejectsMalformedOrUnsupportedStructure)
{
    const auto frame = [](std::string_view tabs)
    {
        return std::format(R"(<object class="org.opensourcephysics.cabrillo.tracker.TFrame">{}</object>)", tabs);
    };
    const auto matrix = [&](std::string_view children)
    {
        return frame(std::format(R"(<property name="tabs" type="array" class="[[Ljava.lang.String;">{}</property>)", children));
    };
    const auto entry = [](std::string_view index, std::string_view children)
    {
        return std::format(R"(<property name="[{}]" type="array" class="[Ljava.lang.String;">{}</property>)", index, children);
    };
    const auto relative = R"(<property name="[1]" type="string">a.trk</property>)";
    const std::vector<std::string> invalid{
        "<object",
        PANEL,
        R"(<!DOCTYPE x><object class="org.opensourcephysics.cabrillo.tracker.TFrame"/>)",
        frame(R"(<property name="tabs" type="string">a.trk</property>)"),
        frame(R"(<property name="tabs" type="array" class="[Ljava.lang.String;"/>)"),
        matrix(std::format("{}{}", entry("0", relative), entry("0", relative))),
        matrix(entry("1", relative)),
        matrix(entry("01", relative)),
        matrix(entry("-1", relative)),
        matrix(R"(<property name="[0]" type="array" class="[Ljava.lang.Object;"/>)"),
        matrix(entry("0", R"(<property name="[0]" type="string">/a.trk</property>)")),
        matrix(entry("0", R"(<property name="[1]" type="string"></property>)")),
        matrix(entry("0", R"(<property name="[1]" type="int">1</property>)")),
        matrix(entry("0", R"(<property name="[2]" type="string">x</property>)")),
        matrix(entry("0", std::format("{}{}", relative, relative))),
        matrix(entry("0", R"(<property name="[1]" type="string">a<b/></property>)")),
        matrix(R"(<item name="[0]"/>)"),
        frame(
            R"(<property name="tabs" type="array" class="[[Ljava.lang.String;"/><property name="tabs" type="array" class="[[Ljava.lang.String;"/>)"),
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
    EXPECT_FALSE(motrack::writeTrackerTabset(std::vector(motrack::MAX_TABSET_ENTRIES + 1, relativeEntry("a.trk"))).has_value());
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
    writeFile(base + "/broken.trk", R"(<object class="org.opensourcephysics.cabrillo.tracker.TFrame"><property name="tabs"/></object>)");
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
    EXPECT_FALSE(items[7].diagnostic.empty());
    EXPECT_EQ(items[8].file, canonical(base + "/video \xC3\xA9.mov"));
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
}

TEST(TrackerTabset, SavesAtomicallyAndPreservesExistingFilesOnFailure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("\xE6\xB5\x8B\xE8\xAF\x95 set.trk");
    const std::vector<TabsetEntry> entries{relativeEntry("a.trk")};
    ASSERT_TRUE(motrack::saveTrackerTabset(path, entries).has_value());
    const auto saved = readFile(path);
    EXPECT_EQ(motrack::parseTrackerTabset(saved).value(), entries);

    EXPECT_FALSE(motrack::saveTrackerTabset(path, std::vector{relativeEntry("a]]>.trk")}).has_value());
    EXPECT_EQ(readFile(path), saved);
    EXPECT_FALSE(motrack::saveTrackerTabset(directory.filePath("missing/set.trk"), entries).has_value());
    EXPECT_EQ(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

} // namespace
