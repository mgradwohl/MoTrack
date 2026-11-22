#include "osp/render.hpp"
#include "osp/source.hpp"
#include "osp/value.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using motrack::osp::NodeId;
using motrack::osp::ObjectValue;
using motrack::osp::ObjectView;
using motrack::osp::Patch;
using motrack::osp::PropertyValue;
using motrack::osp::RenderError;
using motrack::osp::Source;

/// Returns a present optional value; tests use it where a missing value is itself a failure.
template <typename T, typename E> T require(motrack::osp::Expected<T, E> value)
{
    if (!value)
    {
        throw std::runtime_error("unexpected error value");
    }
    return *std::move(value);
}

template <typename T> T require(std::optional<T> value)
{
    if (!value)
    {
        throw std::runtime_error("missing optional value");
    }
    return *value;
}

constexpr motrack::osp::ParseLimits ACCEPT_TRUNCATED{.acceptTruncated = true};

std::shared_ptr<const Source> parsed(std::string bytes, const motrack::osp::ParseLimits& limits = {})
{
    auto source = motrack::osp::parse(std::move(bytes), limits);
    if (!source)
    {
        throw std::runtime_error("parse failed: " + source.error().message);
    }
    return *source;
}

std::string rendered(const Source& source, const Patch& patch, const motrack::osp::ParseLimits& limits = {})
{
    auto output = motrack::osp::render(source, patch, limits);
    if (!output)
    {
        throw std::runtime_error("render failed: " + output.error().message);
    }
    return *output;
}

RenderError::Code renderFailure(const Source& source, const Patch& patch, const motrack::osp::ParseLimits& limits = {})
{
    const auto output = motrack::osp::render(source, patch, limits);
    if (output)
    {
        throw std::runtime_error("expected a render failure");
    }
    return output.error().code;
}

NodeId child(const Source& source, NodeId parent, std::size_t index)
{
    return source.node(parent).children.at(index);
}

void replaceOnce(std::string& text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    if (position == std::string::npos)
    {
        throw std::runtime_error("missing text");
    }
    text.replace(position, from.size(), to);
}

/// A document in the layout Tracker writes: an object property's object shares its indentation.
constexpr std::string_view SAMPLE = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                    "<object class=\"test.Sample\" version=\"6\">\n"
                                    "    <property name=\"count\" type=\"int\">3</property>\n"
                                    "    <property name=\"label\" type=\"string\"><![CDATA[a & b]]></property> <!-- kept -->\n"
                                    "    <property name=\"scale\" type=\"double\">1.5</property>\n"
                                    "    <property name=\"shown\" type=\"boolean\">true</property>\n"
                                    "    <property name=\"clip\" type=\"object\">\n"
                                    "    <object class=\"test.Clip\"/>\n"
                                    "    </property>\n"
                                    "    <property name=\"data\" type=\"array\" class=\"[Ljava.lang.Object;\">\n"
                                    "        <property name=\"[0]\" type=\"int\">1</property>\n"
                                    "    </property>\n"
                                    "    <property name=\"none\" type=\"array\" class=\"[D\"/>\n"
                                    "</object>";

TEST(OspRender, EmptyPatchReproducesTheSourceByteForByte)
{
    const auto source = parsed(std::string(SAMPLE));
    EXPECT_EQ(rendered(*source, Patch{}), SAMPLE);
    EXPECT_TRUE(Patch{}.empty());
}

TEST(OspRender, SetsTypedValuesInPlaceAndKeepsTheRestOfTheLine)
{
    const auto source = parsed(std::string(SAMPLE));
    const auto root = source->root();
    Patch patch;
    patch.setInt(child(*source, root, 0), -7)
        .setString(child(*source, root, 1), "plain")
        .setDouble(child(*source, root, 2), 1.0e7)
        .setBoolean(child(*source, root, 3), false);
    EXPECT_FALSE(patch.empty());
    auto expected = std::string(SAMPLE);
    replaceOnce(expected, ">3<", ">-7<");
    replaceOnce(expected, "<![CDATA[a & b]]>", "plain");
    replaceOnce(expected, ">1.5<", ">1.0E7<");
    replaceOnce(expected, ">true<", ">false<");
    EXPECT_EQ(rendered(*source, patch), expected);

    // Strings that need CDATA get it; multi-line values use the file's line ending.
    const auto crlf = parsed("<object class=\"x\">\r\n<property name=\"s\" type=\"string\">old</property>\r\n</object>");
    Patch text;
    text.setString(child(*crlf, crlf->root(), 0), "first <line>\nsecond\r\nthird");
    EXPECT_EQ(
        rendered(*crlf, text),
        "<object class=\"x\">\r\n<property name=\"s\" type=\"string\"><![CDATA[first <line>\r\nsecond\r\nthird]]></property>\r\n</object>");
}

TEST(OspRender, ReplacesMultiLineValuesAndRawContent)
{
    const auto source = parsed("<object class=\"x\">\n<property name=\"s\" type=\"string\">one\ntwo\nthree</property> tail\n"
                               "<property name=\"u\" type=\"float\">1.5</property>\n</object>");
    Patch patch;
    patch.setString(child(*source, source->root(), 0), "single").setContent(child(*source, source->root(), 1), "2.5");
    EXPECT_EQ(rendered(*source, patch),
              "<object class=\"x\">\n<property name=\"s\" type=\"string\">single</property> tail\n"
              "<property name=\"u\" type=\"float\">2.5</property>\n</object>");
}

TEST(OspRender, InsertsPropertiesWithSiblingIndentation)
{
    const auto source = parsed(std::string(SAMPLE));
    const auto root = source->root();
    Patch patch;
    patch.insert(root, 0, motrack::osp::intProperty("first", 1))
        .insert(root, 2, motrack::osp::stringProperty("between", "b"))
        .insert(root, 2, motrack::osp::stringProperty("second between", "c"))
        .append(root, motrack::osp::doubleProperty("last", 0.25));
    const auto output = rendered(*source, patch);
    EXPECT_NE(output.find("<object class=\"test.Sample\" version=\"6\">\n    <property name=\"first\" type=\"int\">1</property>\n"
                          "    <property name=\"count\""),
              std::string::npos);
    EXPECT_NE(output.find("<!-- kept -->\n    <property name=\"between\" type=\"string\">b</property>\n"
                          "    <property name=\"second between\" type=\"string\">c</property>\n    <property name=\"scale\""),
              std::string::npos);
    EXPECT_TRUE(output.ends_with("class=\"[D\"/>\n    <property name=\"last\" type=\"double\">0.25</property>\n</object>"));
}

TEST(OspRender, ReopensSelfClosingObjectsAndArrays)
{
    const auto source = parsed(std::string(SAMPLE));
    const auto root = source->root();
    const auto clip = child(*source, child(*source, root, 4), 0);
    const auto none = child(*source, root, 6);
    Patch patch;
    patch.append(clip, motrack::osp::intProperty("startframe", 2))
        .append(clip, motrack::osp::booleanProperty("playallsteps", true))
        .append(none, motrack::osp::doubleProperty("[0]", 3.0));
    auto expected = std::string(SAMPLE);
    replaceOnce(expected,
                "    <object class=\"test.Clip\"/>\n",
                "    <object class=\"test.Clip\">\n"
                "        <property name=\"startframe\" type=\"int\">2</property>\n"
                "        <property name=\"playallsteps\" type=\"boolean\">true</property>\n"
                "    </object>\n");
    replaceOnce(expected,
                R"(    <property name="none" type="array" class="[D"/>)",
                "    <property name=\"none\" type=\"array\" class=\"[D\">\n"
                "        <property name=\"[0]\" type=\"double\">3.0</property>\n"
                "    </property>");
    EXPECT_EQ(rendered(*source, patch), expected);

    const auto bare = parsed("\xEF\xBB\xBF<object class=\"root\"/>");
    Patch first;
    first.append(bare->root(), motrack::osp::intProperty("a", 1));
    EXPECT_EQ(rendered(*bare, first), "\xEF\xBB\xBF<object class=\"root\">\n    <property name=\"a\" type=\"int\">1</property>\n</object>");
}

TEST(OspRender, InsertsIntoEmptyNonSelfClosingParents)
{
    const auto source = parsed("<object class=\"x\">\r\n  <property name=\"list\" type=\"collection\" class=\"java.util.ArrayList\">\r\n  "
                               "</property>\r\n</object>");
    Patch patch;
    patch.append(child(*source, source->root(), 0), motrack::osp::stringProperty("item", "v"));
    patch.insert(source->root(), 1, motrack::osp::intProperty("n", 1));
    EXPECT_EQ(rendered(*source, patch),
              "<object class=\"x\">\r\n  <property name=\"list\" type=\"collection\" class=\"java.util.ArrayList\">\r\n"
              "      <property name=\"item\" type=\"string\">v</property>\r\n  </property>\r\n"
              "  <property name=\"n\" type=\"int\">1</property>\r\n</object>");
    const auto empty = parsed("<object class=\"x\">\n</object>");
    Patch fill;
    fill.append(empty->root(), motrack::osp::intProperty("n", 1));
    EXPECT_EQ(rendered(*empty, fill), "<object class=\"x\">\n    <property name=\"n\" type=\"int\">1</property>\n</object>");
}

TEST(OspRender, RemovesAndReplacesWholeProperties)
{
    const auto source = parsed(std::string(SAMPLE));
    const auto root = source->root();
    const auto data = child(*source, root, 5);
    Patch patch;
    patch.remove(child(*source, root, 1)).remove(child(*source, data, 0));
    patch.replace(child(*source, root, 4),
                  motrack::osp::objectProperty(
                      "clip", ObjectValue{.className = "test.Clip", .properties = {motrack::osp::intProperty("stepsize", 2)}}));
    const auto output = rendered(*source, patch);
    EXPECT_EQ(output.find("label"), std::string::npos);
    EXPECT_NE(output.find("    <property name=\"clip\" type=\"object\">\n    <object class=\"test.Clip\">\n"
                          "        <property name=\"stepsize\" type=\"int\">2</property>\n    </object>\n    </property>\n"),
              std::string::npos);
    EXPECT_NE(output.find("class=\"[Ljava.lang.Object;\">\n    </property>"), std::string::npos);

    // Replacing the final, unterminated line keeps the file without a final line break.
    const auto unterminated = parsed("<object class=\"x\">\n<property name=\"a\" type=\"int\">1</property>", ACCEPT_TRUNCATED);
    Patch last;
    last.replace(child(*unterminated, unterminated->root(), 0), motrack::osp::intProperty("b", 2));
    EXPECT_EQ(rendered(*unterminated, last, ACCEPT_TRUNCATED), "<object class=\"x\">\n<property name=\"b\" type=\"int\">2</property>");
}

TEST(OspRender, RejectsInvalidTargetsAndConflicts)
{
    const auto source = parsed(std::string(SAMPLE));
    const auto root = source->root();
    const auto count = child(*source, root, 0);
    const auto clipProperty = child(*source, root, 4);
    const auto clip = child(*source, clipProperty, 0);
    const auto data = child(*source, root, 5);
    const auto element = child(*source, data, 0);
    using Code = RenderError::Code;
    EXPECT_EQ(renderFailure(*source, Patch{}.remove(root)), Code::InvalidNode);
    EXPECT_EQ(renderFailure(*source, Patch{}.setInt(NodeId{999}, 1)), Code::InvalidNode);
    EXPECT_EQ(renderFailure(*source, Patch{}.insert(NodeId{999}, 0, motrack::osp::intProperty("a", 1))), Code::InvalidNode);
    EXPECT_EQ(renderFailure(*source, Patch{}.insert(count, 0, motrack::osp::intProperty("a", 1))), Code::InvalidNode);
    EXPECT_EQ(renderFailure(*source, Patch{}.insert(root, 99, motrack::osp::intProperty("a", 1))), Code::InvalidNode);
    EXPECT_EQ(renderFailure(*source, Patch{}.setDouble(count, 1.0)), Code::TypeMismatch);
    EXPECT_EQ(renderFailure(*source, Patch{}.setString(count, "1")), Code::TypeMismatch);
    EXPECT_EQ(renderFailure(*source, Patch{}.setBoolean(count, true)), Code::TypeMismatch);
    EXPECT_EQ(renderFailure(*source, Patch{}.setInt(child(*source, root, 2), 1)), Code::TypeMismatch);
    EXPECT_EQ(renderFailure(*source, Patch{}.setContent(data, "1")), Code::TypeMismatch);
    EXPECT_EQ(renderFailure(*source, Patch{}.setInt(count, 1).setInt(count, 2)), Code::ConflictingEdits);
    EXPECT_EQ(renderFailure(*source, Patch{}.remove(count).setInt(count, 2)), Code::ConflictingEdits);
    EXPECT_EQ(renderFailure(*source, Patch{}.remove(data).setInt(element, 2)), Code::ConflictingEdits);
    EXPECT_EQ(
        renderFailure(*source,
                      Patch{}.replace(clipProperty, motrack::osp::intProperty("clip", 1)).append(clip, motrack::osp::intProperty("a", 1))),
        Code::ConflictingEdits);
    EXPECT_EQ(renderFailure(*source, Patch{}.replace(clipProperty, motrack::osp::intProperty("clip", 1)).remove(clipProperty)),
              Code::ConflictingEdits);
}

TEST(OspRender, RejectsValuesOspCannotReadBack)
{
    const auto source = parsed(std::string(SAMPLE));
    const auto root = source->root();
    const auto label = child(*source, root, 1);
    const auto data = child(*source, root, 5);
    using Code = RenderError::Code;
    EXPECT_EQ(renderFailure(*source, Patch{}.setString(label, "x<![CDATA[y")), Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source, Patch{}.setString(label, "a]]></property>")), Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source, Patch{}.setContent(child(*source, root, 0), "1</property>")), Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source, Patch{}.append(root, motrack::osp::intProperty("quote\"name", 1))), Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source, Patch{}.append(root, PropertyValue{.name = "a", .typeName = "in\"t"})), Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source, Patch{}.append(root, motrack::osp::collectionProperty("c", "bad>class", {}))), Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source, Patch{}.append(root, motrack::osp::objectProperty("o", ObjectValue{.className = "a\nb"}))),
              Code::InvalidValue);
    EXPECT_EQ(
        renderFailure(*source, Patch{}.append(root, motrack::osp::objectProperty("o", ObjectValue{.className = "a", .version = "1"}))),
        Code::InvalidValue);
    // OSP's setProperty fails to read a null object directly in an object, so it is refused there.
    EXPECT_EQ(renderFailure(*source, Patch{}.append(root, motrack::osp::nullObjectProperty("video"))), Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source, Patch{}.replace(label, motrack::osp::nullObjectProperty("label"))), Code::InvalidValue);
    EXPECT_TRUE(motrack::osp::render(*source, Patch{}.append(data, motrack::osp::nullObjectProperty("[1]"))));
    // Invalid content nested in arrays and objects is found too.
    EXPECT_EQ(renderFailure(
                  *source,
                  Patch{}.append(root,
                                 motrack::osp::arrayProperty(
                                     "a", "[Ljava.lang.Object;", {motrack::osp::intProperty("", 1), PropertyValue{.typeName = "in\"t"}}))),
              Code::InvalidValue);
    EXPECT_EQ(renderFailure(*source,
                            Patch{}.append(root,
                                           motrack::osp::objectProperty(
                                               "o", ObjectValue{.className = "a", .properties = {motrack::osp::nullObjectProperty("n")}}))),
              Code::InvalidValue);
    EXPECT_EQ(renderFailure(
                  *source,
                  Patch{}.append(
                      root, motrack::osp::arrayProperty("a", "[Ljava.lang.Object;", {motrack::osp::stringProperty("s", "x<![CDATA[")}))),
              Code::InvalidValue);
}

TEST(OspRender, VerifiesTheRenderAgainstTheIntendedTree)
{
    // The value starts after the first '>' of the line, which here is inside the name attribute.
    const auto source = parsed("<object class=\"x\">\n<property name=\"a>b\" type=\"int\">3</property>\n</object>");
    EXPECT_EQ(renderFailure(*source, Patch{}.setContent(child(*source, source->root(), 0), "9")), RenderError::Code::VerificationFailed);
    // An insertion at the end of an object that reached end of file unclosed would land in the wrong object.
    const auto unclosed = parsed("<object class=\"x\">\n<property name=\"o\" type=\"object\">\n<object class=\"y\">\n", ACCEPT_TRUNCATED);
    EXPECT_EQ(renderFailure(*unclosed, Patch{}.append(unclosed->root(), motrack::osp::intProperty("a", 1)), ACCEPT_TRUNCATED),
              RenderError::Code::UnsupportedLayout);
    // The default limits refuse to verify a render of a truncated source, as they refuse to parse one.
    EXPECT_EQ(renderFailure(*unclosed, Patch{}.insert(unclosed->root(), 0, motrack::osp::intProperty("a", 1))), RenderError::Code::VerificationFailed);
    EXPECT_EQ(rendered(*unclosed, Patch{}.insert(unclosed->root(), 0, motrack::osp::intProperty("a", 1)), ACCEPT_TRUNCATED),
              "<object class=\"x\">\n<property name=\"a\" type=\"int\">1</property>\n<property name=\"o\" type=\"object\">\n<object "
              "class=\"y\">\n");
    // Only string properties have a CDATA path, so other types store the marker literally.
    const auto unknown = parsed("<object class=\"x\">\n<property name=\"u\" type=\"float\">1</property>\n</object>");
    EXPECT_EQ(rendered(*unknown, Patch{}.setContent(child(*unknown, unknown->root(), 0), "<![CDATA[")),
              "<object class=\"x\">\n<property name=\"u\" type=\"float\"><![CDATA[</property>\n</object>");
    EXPECT_EQ(renderFailure(*unknown, Patch{}.append(unknown->root(), motrack::osp::intProperty("big", 1)), {.maxBytes = 70}),
              RenderError::Code::TooLarge);
}

TEST(OspWriter, WritesNewDocumentsInOspLayout)
{
    ObjectValue root{.className = "org.opensourcephysics.cabrillo.tracker.TrackerPanel", .version = "6.3.5"};
    root.properties.push_back(motrack::osp::stringProperty("semantic_version", "6.3.5"));
    root.properties.push_back(motrack::osp::stringProperty("description", "Ball & \"toss\""));
    root.properties.push_back(motrack::osp::objectProperty(
        "videoclip",
        ObjectValue{.className = "org.opensourcephysics.media.core.VideoClip",
                    .properties = {motrack::osp::intProperty("startframe", 0), motrack::osp::doubleProperty("starttime", 0.0)}}));
    root.properties.push_back(motrack::osp::collectionProperty(
        "tracks",
        "java.util.ArrayList",
        {motrack::osp::objectProperty("ignored", ObjectValue{.className = "org.opensourcephysics.cabrillo.tracker.CoordAxes"})}));
    root.properties.push_back(motrack::osp::arrayProperty(
        "framedata",
        "[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;",
        {std::nullopt,
         motrack::osp::objectProperty("", ObjectValue{.className = "F", .properties = {motrack::osp::doubleProperty("x", 1.5)}}),
         std::nullopt,
         std::nullopt}));
    root.properties.push_back(
        *motrack::osp::primitiveArrayProperty("keys", "[I", {.kind = motrack::osp::CompactArray::Kind::Int, .ints = {0, 3}}));
    root.properties.push_back(motrack::osp::booleanProperty("locked", false));
    const auto written = motrack::osp::write(root);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(*written,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<object class=\"org.opensourcephysics.cabrillo.tracker.TrackerPanel\" version=\"6.3.5\">\n"
              "    <property name=\"semantic_version\" type=\"string\">6.3.5</property>\n"
              "    <property name=\"description\" type=\"string\"><![CDATA[Ball & \"toss\"]]></property>\n"
              "    <property name=\"videoclip\" type=\"object\">\n"
              "    <object class=\"org.opensourcephysics.media.core.VideoClip\">\n"
              "        <property name=\"startframe\" type=\"int\">0</property>\n"
              "        <property name=\"starttime\" type=\"double\">0.0</property>\n"
              "    </object>\n"
              "    </property>\n"
              "    <property name=\"tracks\" type=\"collection\" class=\"java.util.ArrayList\">\n"
              "        <property name=\"item\" type=\"object\">\n"
              "        <object class=\"org.opensourcephysics.cabrillo.tracker.CoordAxes\"/>\n"
              "        </property>\n"
              "    </property>\n"
              "    <property name=\"framedata\" type=\"array\" class=\"[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;\">\n"
              "        <property name=\"[1]\" type=\"object\">\n"
              "        <object class=\"F\">\n"
              "            <property name=\"x\" type=\"double\">1.5</property>\n"
              "        </object>\n"
              "        </property>\n"
              "        <property name=\"[3]\" type=\"object\">null</property>\n"
              "    </property>\n"
              "    <property name=\"keys\" type=\"array\" class=\"[I\">\n"
              "        <property name=\"array\" type=\"string\">{0,3}</property>\n"
              "    </property>\n"
              "    <property name=\"locked\" type=\"boolean\">false</property>\n"
              "</object>");
    const auto source = motrack::osp::parse(*written);
    ASSERT_TRUE(source);
    EXPECT_EQ(motrack::osp::objectValue(**source, (*source)->root()), root);

    const auto crlf = motrack::osp::write(ObjectValue{.className = "x", .properties = {motrack::osp::stringProperty("s", "a\nb")}},
                                          {.lineEnding = motrack::osp::LineEnding::CrLf});
    EXPECT_EQ(*crlf,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<object class=\"x\">\r\n    <property name=\"s\" "
              "type=\"string\">a\r\nb</property>\r\n</object>");
    EXPECT_EQ(*motrack::osp::write(ObjectValue{.className = "x"}, {.lineEnding = motrack::osp::LineEnding::Cr}),
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r<object class=\"x\"/>");
    EXPECT_EQ(motrack::osp::write(ObjectValue{.className = "org.opensourcephysics.controls.Cryptic"}).error().code,
              RenderError::Code::VerificationFailed);
    EXPECT_EQ(motrack::osp::write(ObjectValue{.className = "x", .properties = {motrack::osp::nullObjectProperty("n")}}).error().code,
              RenderError::Code::InvalidValue);
    EXPECT_EQ(motrack::osp::write(ObjectValue{.className = "x"}, {.limits = {.maxBytes = 10}}).error().code, RenderError::Code::TooLarge);
}

std::string generatedFixture(std::string_view name)
{
    std::ifstream stream(std::filesystem::path(MOTRACK_FIXTURES) / "osp" / "generated" / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

TEST(OspWriter, ReproducesDocumentsWrittenByOspItself)
{
    using Kind = motrack::osp::CompactArray::Kind;
    using motrack::osp::CompactArray;
    // The document of OSP's src/test/SetXMLControlTest.java, written by XMLControlElement.toXML
    // (tools/reference/XmlControlDump.java --generate).
    const auto row = [](double first, double second)
    {
        return CompactArray{.kind = Kind::Double, .doubles = {first, second}};
    };
    const ObjectValue demo{
        .className = "java.lang.Object",
        .properties = {motrack::osp::stringProperty("name", "myName"),
                       motrack::osp::doubleProperty("pi", 3.14),
                       require(motrack::osp::primitiveArrayProperty("data", "[D", {.kind = Kind::Double, .doubles = {10.0, 20.0, 30.0}})),
                       require(motrack::osp::primitiveArrayProperty(
                           "data2", "[[D", {.kind = Kind::Nested, .rows = {row(1, 10), row(2, 20), row(3, 30)}}))}};
    EXPECT_EQ(motrack::osp::write(demo), generatedFixture("set-xml-control-test.xml"));

    const ObjectValue values{
        .className = "java.lang.Object",
        .version = "6.3.5",
        .properties = {
            motrack::osp::stringProperty("text", "Ball & \"toss\" <1>"),
            motrack::osp::stringProperty("empty", ""),
            motrack::osp::stringProperty("multiline", "first\nsecond"),
            motrack::osp::intProperty("count", -7),
            motrack::osp::doubleProperty("small", 1.0e-5),
            motrack::osp::doubleProperty("large", 1.0e7),
            motrack::osp::booleanProperty("shown", true),
            require(motrack::osp::primitiveArrayProperty("no ints", "[I", {.kind = Kind::Int})),
            require(motrack::osp::primitiveArrayProperty("flags", "[Z", {.kind = Kind::Boolean, .booleans = {true, false}})),
            require(motrack::osp::primitiveArrayProperty(
                "rows", "[[D", {.kind = Kind::Nested, .rows = {CompactArray{}, CompactArray{.kind = Kind::Double, .doubles = {0.5}}}})),
            motrack::osp::arrayProperty(
                "labels",
                "[Ljava.lang.String;",
                {motrack::osp::stringProperty("", "a"), std::nullopt, motrack::osp::stringProperty("", "c"), std::nullopt}),
            motrack::osp::collectionProperty(
                "list",
                "java.util.ArrayList",
                {motrack::osp::stringProperty("", "x"), motrack::osp::doubleProperty("", 2.5), motrack::osp::intProperty("", 3)}),
            motrack::osp::objectProperty("point",
                                         ObjectValue{.className = "java.awt.Point",
                                                     .properties = {require(motrack::osp::primitiveArrayProperty(
                                                         "location", "[I", {.kind = Kind::Int, .ints = {3, 4}}))}}),
        }};
    EXPECT_EQ(motrack::osp::write(values), generatedFixture("osp-writer-values.xml"));
}

TEST(OspWriter, AvoidsCompactArraysOspCannotReadBack)
{
    using Kind = motrack::osp::CompactArray::Kind;
    using motrack::osp::CompactArray;
    // OSP writes a later empty row as "{}" (osp-jagged-arrays.xml, written by XMLControlElement.toXML), and its
    // reader, mirrored here, then fails for numbers and adds a false element for booleans.
    const auto java = parsed(generatedFixture("osp-jagged-arrays.xml"));
    const ObjectView document(*java, java->root());
    EXPECT_EQ(require(document.property("ints")).compactArray().error(), motrack::osp::DecodeError::Malformed);
    EXPECT_EQ(require(document.property("doubles")).compactArray().error(), motrack::osp::DecodeError::Malformed);
    EXPECT_EQ(require(require(document.property("booleans")).compactArray()).rows.at(1).booleans, std::vector<bool>{false});

    // MoTrack writes the "[j]" element form for those values, which OSP reads back exactly.
    const CompactArray jagged{.kind = Kind::Nested,
                              .rows = {CompactArray{.kind = Kind::Int, .ints = {1, 2}}, CompactArray{.kind = Kind::Int}}};
    const CompactArray deep{.kind = Kind::Nested,
                            .rows = {CompactArray{.kind = Kind::Nested, .rows = {CompactArray{.kind = Kind::Boolean, .booleans = {true}}}},
                                     CompactArray{.kind = Kind::Nested, .rows = {CompactArray{.kind = Kind::Boolean}}}}};
    const auto ints = require(motrack::osp::primitiveArrayProperty("ints", "[[I", jagged));
    const auto booleans = require(motrack::osp::primitiveArrayProperty("booleans", "[[[Z", deep));
    const auto written = require(motrack::osp::write(ObjectValue{.className = "java.lang.Object", .properties = {ints, booleans}}));
    EXPECT_EQ(written,
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<object class=\"java.lang.Object\">\n"
              "    <property name=\"ints\" type=\"array\" class=\"[[I\">\n"
              "        <property name=\"[0]\" type=\"array\" class=\"[I\">\n"
              "            <property name=\"array\" type=\"string\">{1,2}</property>\n"
              "        </property>\n"
              "        <property name=\"[1]\" type=\"array\" class=\"[I\"/>\n"
              "    </property>\n"
              "    <property name=\"booleans\" type=\"array\" class=\"[[[Z\">\n"
              "        <property name=\"[0]\" type=\"array\" class=\"[[Z\">\n"
              "            <property name=\"array\" type=\"string\">{{true}}</property>\n"
              "        </property>\n"
              "        <property name=\"[1]\" type=\"array\" class=\"[[Z\">\n"
              "            <property name=\"[0]\" type=\"array\" class=\"[Z\"/>\n"
              "        </property>\n"
              "    </property>\n"
              "</object>");
    const auto reread = parsed(written);
    const auto rows = require(require(ObjectView(*reread, reread->root()).property("ints")).indexedElements());
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(require(require(rows[0]).compactArray()).ints, (std::vector<std::int32_t>{1, 2}));
    EXPECT_TRUE(require(rows[1]).elements().empty());
}

TEST(OspWriter, KeepsTrailingNullRowsOspWouldDrop)
{
    using Kind = motrack::osp::CompactArray::Kind;
    using motrack::osp::CompactArray;
    // OSP writes {{1,2},null}, and its decoder never reaches the final null row: it reads [[1,2]]
    // (OpenSourcePhysics/osp#20).
    EXPECT_EQ(require(motrack::osp::parseCompactArray("{{1,2},null}", "[[I")).rows.size(), 1U);
    const CompactArray values{.kind = Kind::Nested, .rows = {CompactArray{.kind = Kind::Int, .ints = {1, 2}}, CompactArray{}}};
    const auto property = require(motrack::osp::primitiveArrayProperty("rows", "[[I", values));
    ASSERT_EQ(property.elements.size(), 2U);
    EXPECT_EQ(property.elements[0].name, "[0]");
    EXPECT_EQ(property.elements[1].name, "[1]");
    EXPECT_EQ(property.elements[1].type(), motrack::osp::PropertyType::Object);
    const auto reread = parsed(require(motrack::osp::write(ObjectValue{.className = "x", .properties = {property}})));
    EXPECT_EQ(require(require(ObjectView(*reread, reread->root()).property("rows")).indexedElements()).size(), 2U);
    // A null row before the last one is read back, so the compact form stays.
    const CompactArray leading{.kind = Kind::Nested, .rows = {CompactArray{}, CompactArray{.kind = Kind::Int, .ints = {1}}}};
    EXPECT_EQ(require(motrack::osp::primitiveArrayProperty("rows", "[[I", leading)).elements.at(0).content, "{null,{1}}");
    // NaN and -0.0 survive the compact form.
    const CompactArray special{.kind = Kind::Double, .doubles = {std::numeric_limits<double>::quiet_NaN(), -0.0}};
    EXPECT_EQ(require(motrack::osp::primitiveArrayProperty("d", "[D", special)).elements.at(0).content, "{NaN,-0.0}");
}

TEST(OspWriter, RefusesStringsThatWouldInjectProperties)
{
    // A CDATA value ending in "]]></property>" would let the following line become a real property in OSP
    // (getInt("injected") returned 7 in Tracker 6.3.5; OpenSourcePhysics/osp#21).
    const std::string injection = "x]]></property>\n<property name=\"injected\" type=\"int\">7</property>\n<property name=\"rest\" type=\"string\"><![CDATA[y";
    const auto source = parsed(std::string(SAMPLE));
    EXPECT_EQ(renderFailure(*source, Patch{}.setString(child(*source, source->root(), 1), injection)), RenderError::Code::InvalidValue);
    EXPECT_EQ(motrack::osp::write(ObjectValue{.className = "x", .properties = {motrack::osp::stringProperty("s", injection)}}).error().code,
              RenderError::Code::InvalidValue);
    // Without the CDATA terminator, multi-line text cannot leave its property.
    const auto multiline = rendered(*source, Patch{}.setString(child(*source, source->root(), 1), "a\n<property name=\"injected\" type=\"int\">7"));
    const auto reread = parsed(multiline);
    const ObjectView root(*reread, reread->root());
    EXPECT_FALSE(root.property("injected"));
    EXPECT_EQ(root.getString("label"), "a\n<property name=\"injected\" type=\"int\">7");
}

TEST(OspWriter, RewritesLoadedCdataStringsWithCdata)
{
    // OSP keeps a loaded string without its CDATA markers and XMLControlElement.toXML then writes it bare
    // (`a<b`, OpenSourcePhysics/osp#21). MoTrack decides CDATA from the logical value whenever it writes one.
    const auto source = parsed("<object class=\"x\">\n<property name=\"s\" type=\"string\"><![CDATA[a<b]]></property>\n</object>");
    const auto property = child(*source, source->root(), 0);
    const auto replaced = rendered(*source, Patch{}.replace(property, motrack::osp::propertyValue(*source, property)));
    EXPECT_EQ(replaced, "<object class=\"x\">\n<property name=\"s\" type=\"string\"><![CDATA[a<b]]></property>\n</object>");
    const auto copied = require(motrack::osp::write(ObjectValue{.className = "x", .properties = {motrack::osp::propertyValue(*source, property)}}));
    EXPECT_NE(copied.find("<![CDATA[a<b]]>"), std::string::npos);
    EXPECT_EQ(rendered(*source, Patch{}.setContent(property, "a<b")), replaced);
}

TEST(OspWriter, WritesPrimitiveArraysAsOspDoes)
{
    using Kind = motrack::osp::CompactArray::Kind;
    using motrack::osp::CompactArray;
    const auto doubles = motrack::osp::primitiveArrayProperty("d", "[D", {.kind = Kind::Double, .doubles = {1.0, 0.1, -2.5e-8}});
    ASSERT_TRUE(doubles);
    EXPECT_EQ(doubles->elements.at(0).content, "{1.0,0.1,-2.5E-8}");
    const auto empty = motrack::osp::primitiveArrayProperty("d", "[D", {.kind = Kind::Double});
    EXPECT_TRUE(empty->elements.empty());
    // A first row of length zero makes Java's element count zero, so rows are written as "[j]" elements.
    const CompactArray rows{.kind = Kind::Nested,
                            .rows = {CompactArray{.kind = Kind::Int}, CompactArray{.kind = Kind::Int, .ints = {1}}, CompactArray{}}};
    const auto nested = motrack::osp::primitiveArrayProperty("n", "[[I", rows);
    ASSERT_TRUE(nested);
    ASSERT_EQ(nested->elements.size(), 3U);
    EXPECT_EQ(nested->elements[0].name, "[0]");
    EXPECT_TRUE(nested->elements[0].elements.empty());
    EXPECT_EQ(nested->elements[1].elements.at(0).content, "{1}");
    EXPECT_EQ(nested->elements[2].type(), motrack::osp::PropertyType::Object);
    EXPECT_EQ(motrack::osp::primitiveArrayProperty("n", "[[I", rows, false)->elements.size(), 2U);
    // A null first row stops the count early; two levels down Java no longer sees a primitive base type.
    const CompactArray nullFirst{.kind = Kind::Nested, .rows = {CompactArray{}, CompactArray{.kind = Kind::Boolean, .booleans = {true}}}};
    EXPECT_EQ(motrack::osp::primitiveArrayProperty("b", "[[Z", nullFirst)->elements.at(0).content, "{null,{true}}");
    const CompactArray deep{
        .kind = Kind::Nested,
        .rows = {CompactArray{}, CompactArray{.kind = Kind::Nested, .rows = {CompactArray{.kind = Kind::Int, .ints = {2}}}}}};
    const auto deepProperty = motrack::osp::primitiveArrayProperty("t", "[[[I", deep);
    ASSERT_TRUE(deepProperty);
    ASSERT_EQ(deepProperty->elements.size(), 1U);
    EXPECT_EQ(deepProperty->elements[0].name, "[1]");
    EXPECT_EQ(deepProperty->elements[0].elements.at(0).content, "{{2}}");
    EXPECT_EQ(motrack::osp::primitiveArrayProperty("x", "[D", {.kind = Kind::Int}).error(), motrack::osp::DecodeError::WrongType);
    EXPECT_EQ(motrack::osp::primitiveArrayProperty("x", "[[D", {.kind = Kind::Double}).error(), motrack::osp::DecodeError::WrongType);
    EXPECT_EQ(
        motrack::osp::primitiveArrayProperty("x", "[[D", CompactArray{.kind = Kind::Nested, .rows = {CompactArray{.kind = Kind::Int}}})
            .error(),
        motrack::osp::DecodeError::WrongType);
    EXPECT_EQ(motrack::osp::primitiveArrayProperty("x", "[Ljava.lang.Object;", {.kind = Kind::Int}).error(),
              motrack::osp::DecodeError::WrongType);
    // Each written form reads back as the same values.
    const auto document = motrack::osp::write(ObjectValue{.className = "x", .properties = {*doubles, *nested}});
    ASSERT_TRUE(document);
    const auto source = motrack::osp::parse(*document);
    const motrack::osp::ObjectView view(**source, (*source)->root());
    EXPECT_EQ(require(view.property("d")).compactArray()->doubles, (std::vector<double>{1.0, 0.1, -2.5e-8}));
}

TEST(OspValues, CompareLogicalTreesAndNormalizeText)
{
    const auto source = parsed(std::string(SAMPLE));
    const auto value = motrack::osp::objectValue(*source, source->root());
    EXPECT_EQ(value.className, "test.Sample");
    EXPECT_EQ(value.version, "6");
    ASSERT_EQ(value.properties.size(), 7U);
    EXPECT_EQ(value.properties[1].content, "a & b");
    EXPECT_TRUE(value.properties[4].object);
    EXPECT_EQ(value.properties[5].className, "[Ljava.lang.Object;");
    auto changed = value;
    EXPECT_EQ(changed, value);
    changed.properties[4].object = std::make_shared<const ObjectValue>(ObjectValue{.className = "other"});
    EXPECT_NE(changed, value);
    changed.properties[4].object.reset();
    EXPECT_NE(changed, value);
    EXPECT_EQ(motrack::osp::propertyValue(*source, child(*source, source->root(), 0)), motrack::osp::intProperty("count", 3));
    EXPECT_EQ(motrack::osp::normalizeLineBreaks("a\r\nb\rc\n"), "a\nb\nc\n");
    EXPECT_EQ(motrack::osp::stringProperty("s", "x\r\ny").content, "x\ny");
    EXPECT_EQ(motrack::osp::doubleProperty("d", std::numeric_limits<double>::infinity()).content, "Infinity");
}

} // namespace
