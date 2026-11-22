#include "model/source_edits.hpp"

#include "model/tracker_loaders.hpp"
#include "osp/value.hpp"

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

/// A panel with bytes outside every node: a prolog comment, comments and blank lines between properties, text Tracker
/// ignores after an end tag, mixed line endings and an epilog (#316).
constexpr std::string_view GAPPED = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                                    "<!-- prolog -->\r\n"
                                    "<object class=\"Panel\">\r\n"
                                    "    <!-- first gap -->\r\n"
                                    "    <property name=\"label\" type=\"string\">old</property> ignored\r\n"
                                    "\r\n"
                                    "    <property name=\"gone\" type=\"int\">2</property>\n"
                                    "    <!-- after gone -->\r\n"
                                    "    <property name=\"count\" type=\"int\">1</property>\r\n"
                                    "    <property name=\"empty\" type=\"object\">\r\n"
                                    "        <object class=\"Empty\"/>\r\n"
                                    "    </property>\r\n"
                                    "    <!-- last gap -->\r\n"
                                    "</object>\r\n"
                                    "<!-- epilog -->\r\n";

/// The edits every doctoring test starts from: a value set, a removal, an insertion after a gap, an append and an
/// insertion into a self-closing object.
motrack::SourceEdits gappedEdits(const std::shared_ptr<const motrack::osp::Source>& source)
{
    motrack::SourceEdits edits(source);
    const auto root = source->root();
    edits.setString(named(*source, root, "label"), "new");
    edits.remove(named(*source, root, "gone"));
    edits.insert(root, 2, motrack::osp::intProperty("inserted", 3)); // Before "count", after the gap that precedes it.
    edits.append(root, motrack::osp::booleanProperty("appended", true));
    const auto empty = source->node(named(*source, root, "empty")).children.front();
    edits.append(empty, motrack::osp::doubleProperty("x", 0.5));
    return edits;
}

TEST(SourceEdits, KeepsEveryByteOutsideTheEditsAndFollowsTheRenderGapRule)
{
    const auto source = parsed(GAPPED);
    const auto edits = gappedEdits(source);
    const auto rendered = edits.render();
    ASSERT_TRUE(rendered) << rendered.error();
    // A removal deletes only the removed property's lines with its last line break, so the gaps on both sides stay;
    // an insertion is written before the following property, after the gap that precedes it, or before the closing
    // line when appending, in the file's line break (the first one, CRLF); a self-closing object is reopened.
    EXPECT_EQ(rendered->xml,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
              "<!-- prolog -->\r\n"
              "<object class=\"Panel\">\r\n"
              "    <!-- first gap -->\r\n"
              "    <property name=\"label\" type=\"string\">new</property> ignored\r\n"
              "\r\n"
              "    <!-- after gone -->\r\n"
              "    <property name=\"inserted\" type=\"int\">3</property>\r\n"
              "    <property name=\"count\" type=\"int\">1</property>\r\n"
              "    <property name=\"empty\" type=\"object\">\r\n"
              "        <object class=\"Empty\">\r\n"
              "            <property name=\"x\" type=\"double\">0.5</property>\r\n"
              "        </object>\r\n"
              "    </property>\r\n"
              "    <!-- last gap -->\r\n"
              "    <property name=\"appended\" type=\"boolean\">true</property>\r\n"
              "</object>\r\n"
              "<!-- epilog -->\r\n");
    // The seam accepts the render itself.
    EXPECT_TRUE(edits.verifyRendered(rendered->xml));
}

TEST(SourceEdits, RefusesEveryDoctoredOutput)
{
    const auto source = parsed(GAPPED);
    const auto edits = gappedEdits(source);
    const auto rendered = edits.render();
    ASSERT_TRUE(rendered) << rendered.error();
    const auto& xml = rendered->xml;
    const auto doctored = [&](std::string_view from, std::string_view to)
    {
        const auto at = xml.find(from);
        if (at == std::string::npos)
        {
            throw std::runtime_error("missing doctoring target");
        }
        auto copy = xml;
        copy.replace(at, from.size(), to);
        return copy;
    };
    const std::vector<std::pair<std::string, std::string>> cases{
        {"prolog byte", doctored("<!-- prolog -->", "<!-- prolog!-->")},
        {"prolog line break", doctored("<!-- prolog -->\r\n", "<!-- prolog -->\n")},
        {"epilog byte", doctored("<!-- epilog -->", "<!-- epilog!-->")},
        {"root line break", doctored("</object>\r\n<!--", "</object>\n<!--")},
        {"first gap byte", doctored("first gap", "first gap!")},
        {"gap after a removal", doctored("after gone", "after gone!")},
        {"blank gap line removed", doctored("ignored\r\n\r\n", "ignored\r\n")},
        {"gap line added", doctored("    <property name=\"count\"", "\r\n    <property name=\"count\"")},
        {"last gap byte", doctored("last gap", "last gap!")},
        {"unedited node byte", doctored(">1</property>", ">1 </property>")},
        {"unedited node line break", doctored(">1</property>\r\n", ">1</property>\n")},
        {"text after an edited value", doctored("</property> ignored", "</property> changed")},
        {"edited container's opening line", doctored("<object class=\"Panel\">", "<object class=\"Panel\"> ")},
        {"edited container's closing line", doctored("</object>\r\n<!-- epilog", "</object> \r\n<!-- epilog")},
        {"reopened object's closing line", doctored("        </object>", "      </object>")},
        {"inserted property's line break", doctored(">3</property>\r\n", ">3</property>\n")},
        {"edited value", doctored(">new<", ">newer<")},
    };
    for (const auto& [label, output] : cases)
    {
        SCOPED_TRACE(label);
        const auto checked = edits.verifyRendered(output);
        ASSERT_FALSE(checked);
        EXPECT_TRUE(checked.error().starts_with("O8: ") || checked.error().starts_with("The rendered Tracker XML does not parse"))
            << checked.error();
    }
    EXPECT_FALSE(edits.verifyRendered("not osp"));
}

TEST(SourceEdits, RenamedAndRemovedPropertiesKeepTheGapsAroundThem)
{
    const auto source = parsed("<object class=\"Panel\">\n"
                               "    <property name=\"values\" type=\"array\" class=\"[Ljava.lang.String;\">\n"
                               "        <property name=\"[0]\" type=\"string\">a</property>\n"
                               "        <property name=\"[1]\" type=\"string\">b</property>\n"
                               "        <property name=\"[2]\" type=\"string\">c</property>\n"
                               "    </property>\n"
                               "    <!-- between -->\n"
                               "    <property name=\"tail\" type=\"int\">1</property>\n"
                               "</object>");
    motrack::SourceEdits edits(source);
    const auto values = named(*source, source->root(), "values");
    edits.remove(named(*source, values, "[0]"));
    edits.rename(named(*source, values, "[1]"), "[0]");
    edits.rename(named(*source, values, "[2]"), "[1]");
    edits.remove(named(*source, source->root(), "tail"));
    const auto rendered = edits.render();
    ASSERT_TRUE(rendered) << rendered.error();
    // Removing the last property keeps the gap before it; the renamed elements are rewritten in place.
    EXPECT_EQ(rendered->xml,
              "<object class=\"Panel\">\n"
              "    <property name=\"values\" type=\"array\" class=\"[Ljava.lang.String;\">\n"
              "        <property name=\"[0]\" type=\"string\">b</property>\n"
              "        <property name=\"[1]\" type=\"string\">c</property>\n"
              "    </property>\n"
              "    <!-- between -->\n"
              "</object>");
    auto doctored = rendered->xml;
    doctored.replace(doctored.find("between"), 7, "betwixt");
    EXPECT_FALSE(edits.verifyRendered(doctored));
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
        if (!mass.frames)
        {
            FAIL() << "The framedata array was not read.";
        }
        const auto& frames = mass.frames->frames;
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
