#include "source_edits.hpp"

#include "osp/value.hpp"
#include "tracker_loaders.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr std::string_view DOCUMENT = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                      "<object class=\"Panel\">\n"
                                      "    <property name=\"label\" type=\"string\">old</property>\n"
                                      "    <!-- kept -->\n"
                                      "    <property name=\"count\" type=\"int\">1</property>\n"
                                      "    <property name=\"values\" type=\"array\" class=\"[Ljava.lang.String;\">\n"
                                      "        <property name=\"[0]\" type=\"string\">a</property>\n"
                                      "        <property name=\"[2]\" type=\"string\">c</property>\n"
                                      "    </property>\n"
                                      "    <property name=\"child\" type=\"object\">\n"
                                      "        <object class=\"Child\">\n"
                                      "            <property name=\"x\" type=\"double\">1.5</property>\n"
                                      "        </object>\n"
                                      "    </property>\n"
                                      "</object>\n";

std::shared_ptr<const motrack::osp::Source> parsed(std::string_view xml)
{
    auto source = motrack::osp::parse(std::string(xml));
    if (!source)
    {
        throw std::runtime_error(source.error().message);
    }
    return std::move(*source);
}

motrack::osp::NodeId named(const motrack::osp::Source& source, motrack::osp::NodeId owner, std::string_view name)
{
    for (const auto child : source.node(owner).children)
    {
        if (source.node(child).name == name)
        {
            return child;
        }
    }
    throw std::runtime_error("missing test property");
}

TEST(SourceEdits, LaterEditsRefineEarlierOnesAndUneditedBytesStay)
{
    const auto source = parsed(DOCUMENT);
    motrack::SourceEdits edits(source);
    EXPECT_TRUE(edits.empty());
    const auto root = source->root();
    const auto label = named(*source, root, "label");
    edits.setString(label, "first");
    edits.setString(label, "a & b");
    EXPECT_EQ(edits.content(label), "a & b");
    EXPECT_EQ(edits.text(label), "a & b");
    const auto values = named(*source, root, "values");
    const auto child = named(*source, root, "child");
    const auto x = named(*source, source->node(child).children.front(), "x");
    edits.setDouble(x, 2.0);
    edits.remove(child); // Removal wins over the earlier edit inside the subtree.
    EXPECT_TRUE(edits.removed(x));
    edits.append(child, motrack::osp::intProperty("ignored", 1));
    edits.insert(values, 1, motrack::osp::stringProperty("[1]", "b"));
    EXPECT_TRUE(edits.hasInsertions(values));
    EXPECT_EQ(edits.insertions(values).size(), 1u);
    EXPECT_EQ(edits.children(root).size(), 3u);
    const auto rendered = edits.render();
    ASSERT_TRUE(rendered) << rendered.error();
    EXPECT_EQ(rendered->xml,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<object class=\"Panel\">\n"
              "    <property name=\"label\" type=\"string\"><![CDATA[a & b]]></property>\n"
              "    <!-- kept -->\n"
              "    <property name=\"count\" type=\"int\">1</property>\n"
              "    <property name=\"values\" type=\"array\" class=\"[Ljava.lang.String;\">\n"
              "        <property name=\"[0]\" type=\"string\">a</property>\n"
              "        <property name=\"[1]\" type=\"string\">b</property>\n"
              "        <property name=\"[2]\" type=\"string\">c</property>\n"
              "    </property>\n"
              "</object>\n");
}

TEST(SourceEdits, RenamesCarryEditsInsideTheReplacedSubtree)
{
    const auto source = parsed(DOCUMENT);
    motrack::SourceEdits edits(source);
    const auto root = source->root();
    const auto values = named(*source, root, "values");
    const auto third = named(*source, values, "[2]");
    edits.setString(third, "renamed text");
    edits.rename(third, "[1]");
    EXPECT_EQ(edits.name(third), "[1]");
    EXPECT_EQ(edits.value(third).content, "renamed text");
    const auto child = named(*source, root, "child");
    edits.rename(child, "moved");
    edits.setDouble(named(*source, source->node(child).children.front(), "x"), -0.25);
    edits.setInt(named(*source, root, "count"), 7);
    edits.setBoolean(named(*source, root, "count"), true); // Rendered as a type mismatch below.
    EXPECT_FALSE(edits.render());
    motrack::SourceEdits valid(source);
    valid.setString(third, "renamed text");
    valid.rename(third, "[1]");
    valid.rename(child, "moved");
    valid.setDouble(named(*source, source->node(child).children.front(), "x"), -0.25);
    valid.setInt(named(*source, root, "count"), 7);
    const auto rendered = valid.render();
    ASSERT_TRUE(rendered) << rendered.error();
    EXPECT_NE(rendered->xml.find("        <property name=\"[1]\" type=\"string\">renamed text</property>\n"), std::string::npos);
    // OSP's writer gives an object its property's indentation.
    EXPECT_NE(rendered->xml.find("    <property name=\"moved\" type=\"object\">\n    <object class=\"Child\">\n"
                                 "        <property name=\"x\" type=\"double\">-0.25</property>\n"),
              std::string::npos);
    EXPECT_NE(rendered->xml.find("    <!-- kept -->\n    <property name=\"count\" type=\"int\">7</property>\n"), std::string::npos);
    ASSERT_TRUE(rendered->source);
    EXPECT_EQ(rendered->source->bytes(), rendered->xml);
}

TEST(SourceEdits, RefusesContentOspCannotReadBack)
{
    const auto source = parsed(DOCUMENT);
    motrack::SourceEdits edits(source);
    edits.setString(named(*source, source->root(), "label"), "<![CDATA[nested");
    const auto rendered = edits.render();
    ASSERT_FALSE(rendered);
    EXPECT_NE(rendered.error().find("could not be rendered"), std::string::npos);
    motrack::SourceEdits empty(source);
    const auto unchanged = empty.render();
    ASSERT_TRUE(unchanged) << unchanged.error();
    EXPECT_EQ(unchanged->xml, DOCUMENT);
}

TEST(TrackerLoaders, ReadPointMassKeysAndFramesLikeTrackersLoaders)
{
    const auto document = [](std::string_view keys)
    {
        return "<object class=\"org.opensourcephysics.cabrillo.tracker.PointMass\">\n"
               "    <property name=\"name\" type=\"string\">mass</property>\n"
               "    <property name=\"mass\" type=\"double\">2.5</property>\n"
               "    <property name=\"framedata\" type=\"array\" class=\"[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;\">\n"
               "        <property name=\"[1]\" type=\"object\">\n"
               "        <object class=\"org.opensourcephysics.cabrillo.tracker.PointMass$FrameData\">\n"
               "            <property name=\"x\" type=\"double\">3.0</property>\n"
               "        </object>\n"
               "        </property>\n"
               "        <property name=\"[2]\" type=\"object\">null</property>\n"
               "    </property>\n" +
               std::string(keys) + "</object>\n";
    };
    const auto load = [&](std::string_view keys)
    {
        const auto source = parsed(document(keys));
        return std::pair(source, motrack::tracker::loadPointMass(*source, source->root()));
    };
    {
        const auto [source, mass] = load("    <property name=\"keyFrames\" type=\"array\" class=\"[I\">\n"
                                         "        <property name=\"array\" type=\"string\">{1,4}</property>\n"
                                         "    </property>\n");
        EXPECT_EQ(*mass.name, "mass");
        EXPECT_EQ(*mass.mass, 2.5);
        ASSERT_TRUE(mass.frames);
        const auto& frames = mass.frames.value().frames;
        ASSERT_EQ(frames.size(), 2u);
        EXPECT_EQ(*frames[0].element.index, 1);
        EXPECT_EQ(*frames[0].data.x, 3.0);
        EXPECT_EQ(frames[0].data.y.error(), motrack::osp::DecodeError::Missing);
        EXPECT_FALSE(frames[1].element.object);
        EXPECT_EQ(*mass.keyFrames, (std::vector<std::int32_t>{1, 4}));
    }
    {
        // Indexed int elements fill a Java int[] whose absent slots stay 0.
        const auto [source, mass] = load("    <property name=\"keyFrames\" type=\"array\" class=\"[I\">\n"
                                         "        <property name=\"[0]\" type=\"int\">3</property>\n"
                                         "        <property name=\"[2]\" type=\"int\">5</property>\n"
                                         "    </property>\n");
        EXPECT_EQ(*mass.keyFrames, (std::vector<std::int32_t>{3, 0, 5}));
    }
    EXPECT_TRUE(load("    <property name=\"keyFrames\" type=\"array\" class=\"[I\"/>\n").second.keyFrames->empty());
    EXPECT_FALSE(load("    <property name=\"keyFrames\" type=\"array\" class=\"[D\"/>\n").second.keyFrames);
    EXPECT_FALSE(load("    <property name=\"keyFrames\" type=\"array\" class=\"[I\">\n"
                      "        <property name=\"[1000000]\" type=\"int\">3</property>\n"
                      "    </property>\n")
                     .second.keyFrames);
    EXPECT_FALSE(load("    <property name=\"keyFrames\" type=\"array\" class=\"[I\">\n"
                      "        <property name=\"[0]\" type=\"string\">x</property>\n"
                      "    </property>\n")
                     .second.keyFrames);
    EXPECT_FALSE(load("    <property name=\"keyFrames\" type=\"array\" class=\"[I\">\n"
                      "        <property name=\"array\" type=\"string\">{}</property>\n"
                      "    </property>\n")
                     .second.keyFrames);
    EXPECT_EQ(load("").second.keyFrames.error(), motrack::osp::DecodeError::Missing);
}

} // namespace
