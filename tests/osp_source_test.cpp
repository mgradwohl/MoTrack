#include "osp/source.hpp"
#include "osp/value.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{

using motrack::osp::DecodeError;
using motrack::osp::NodeId;
using motrack::osp::ObjectView;
using motrack::osp::ParseError;
using motrack::osp::PropertyType;
using motrack::osp::PropertyView;
using motrack::osp::Source;

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string fixture(std::string_view name)
{
    return readFile(std::filesystem::path(MOTRACK_FIXTURES) / "osp" / name);
}

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

/// Quirk fixtures whose objects reach end of file load only with this; OSP loads them silently (#243).
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

std::string replaceAll(std::string text, std::string_view from, std::string_view to)
{
    for (auto position = text.find(from); position != std::string::npos; position = text.find(from, position + to.size()))
    {
        text.replace(position, from.size(), to);
    }
    return text;
}

std::string escaped(std::string_view text)
{
    return replaceAll(std::string(text), "\n", "\\n");
}

/// One line per node: objects show their class, properties "name : type" and their content or class.
void outlineObject(const Source& source, NodeId id, int depth, std::string& out);

void outlineProperty(const Source& source, NodeId id, int depth, std::string& out)
{
    const auto& node = source.node(id);
    out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + node.name.value_or("(none)") + " : " + node.typeName;
    switch (node.type)
    {
    case PropertyType::Array:
    case PropertyType::Collection:
        out += " class=" + node.className + "\n";
        for (const auto child : node.children)
        {
            outlineProperty(source, child, depth + 1, out);
        }
        break;
    case PropertyType::Object:
        if (node.children.empty())
        {
            out += " = null\n";
            break;
        }
        out += '\n';
        outlineObject(source, node.children.front(), depth + 1, out);
        break;
    default:
        out += " = " + escaped(node.content) + "\n";
        break;
    }
}

void outlineObject(const Source& source, NodeId id, int depth, std::string& out)
{
    const auto& node = source.node(id);
    out += std::string(static_cast<std::size_t>(depth) * 2, ' ') + "object " + node.className;
    if (node.version)
    {
        out += " version=" + *node.version;
    }
    out += '\n';
    for (const auto child : node.children)
    {
        outlineProperty(source, child, depth + 1, out);
    }
}

std::string outline(const Source& source)
{
    std::string out;
    outlineObject(source, source.root(), 0, out);
    return out;
}

ParseError::Code failure(std::string bytes, std::size_t* line = nullptr, const motrack::osp::ParseLimits& limits = {})
{
    const auto source = motrack::osp::parse(std::move(bytes), limits);
    if (source)
    {
        throw std::runtime_error("expected a parse failure");
    }
    if (line != nullptr)
    {
        *line = source.error().line;
    }
    return source.error().code;
}

// Expected trees below are the trees Tracker 6.3.5's XMLControlElement built for the same fixtures
// (tools/reference/XmlControlDump.java); the Linux CI oracle re-checks them on every run.

TEST(OspReader, BlankLineEndsAnArrayAndLaterElementsJoinTheObject)
{
    const auto source = parsed(fixture("blank-line-in-array.xml"));
    EXPECT_EQ(outline(*source),
              "object test.BlankLine\n"
              "  gap : array class=[Ljava.lang.Object;\n"
              "    [0] : int = 1\n"
              "  [1] : int = 2\n"
              "  after : int = 3\n"
              "  nested : collection class=java.util.ArrayList\n"
              "    item : array class=[I\n"
              "      array : string = {1,2}\n"
              "    item : string = kept\n");
    const auto& gap = source->node(source->node(source->root()).children[0]);
    EXPECT_EQ(source->lineText(require(gap.closeLine)), "");
}

TEST(OspReader, ArrayWithoutSelfClosingTagConsumesFollowingLines)
{
    const auto source = parsed(fixture("unclosed-array-line.xml"), ACCEPT_TRUNCATED);
    EXPECT_EQ(outline(*source),
              "object test.UnclosedArrayLine\n"
              "  empty : array class=[D\n"
              "  open : array class=[D\n"
              "    next : int = 4\n"
              "    items : collection class=java.util.ArrayList\n"
              "      item : string = a\n"
              "      item : object = null\n");
    // The array consumed "</object>", so the root reached end of file unclosed.
    EXPECT_TRUE(source->truncated());
    EXPECT_FALSE(source->node(source->root()).closed);
    const auto& empty = source->node(source->node(source->root()).children[0]);
    EXPECT_TRUE(empty.selfClosing);
}

TEST(OspReader, ClosingTagsSwallowTheFollowingLine)
{
    const auto source = parsed(fixture("self-closing.xml"));
    EXPECT_EQ(outline(*source),
              "object test.SelfClosing\n"
              "  first : object\n"
              "    object test.Empty\n"
              "  second : object\n"
              "    object test.Empty\n"
              "  kept : int = 2\n"
              "  third : object\n"
              "    object test.Inner\n"
              "      value : int = 3\n"
              "  last : int = 5\n");
    const auto& first = source->node(source->node(source->root()).children[0]);
    EXPECT_EQ(source->lineText(first.lastLine), "    <property name=\"swallowed\" type=\"int\">1</property>");
    EXPECT_FALSE(source->truncated());
}

TEST(OspReader, KeepsDuplicateNamesAndLooksUpTheLast)
{
    const auto source = parsed(fixture("duplicates.xml"));
    const ObjectView root(*source, source->root());
    ASSERT_EQ(root.properties().size(), 5U);
    EXPECT_EQ(require(root.property("value")).type(), PropertyType::String);
    EXPECT_EQ(root.getString("value"), "last");
    // The child map only receives object properties, so the earlier object is still the child control.
    EXPECT_EQ(require(root.child("value")).className(), "test.First");
    EXPECT_EQ(require(root.child("child")).getDouble("x"), 2.0);
    EXPECT_FALSE(root.child("missing"));
    EXPECT_FALSE(root.property("missing"));
    EXPECT_EQ(root.getInt("value").error(), DecodeError::Malformed);
    EXPECT_EQ(root.getBoolean("value"), false);
    EXPECT_EQ(root.getDouble("missing").error(), DecodeError::Missing);
    EXPECT_EQ(root.getInt("missing").error(), DecodeError::Missing);
    EXPECT_EQ(root.getBoolean("missing").error(), DecodeError::Missing);
    EXPECT_EQ(root.getString("missing").error(), DecodeError::Missing);
    EXPECT_EQ(root.getInt("child").error(), DecodeError::Unsupported);
    EXPECT_EQ(root.getDouble("child").error(), DecodeError::WrongType);
    EXPECT_EQ(root.getBoolean("child").error(), DecodeError::WrongType);
    EXPECT_EQ(root.getString("child").error(), DecodeError::WrongType);
}

TEST(OspReader, ReadsTextWithoutEntityDecodingAndWithCdataOnlyOnItsPath)
{
    const auto source = parsed(fixture("text-values.xml"));
    EXPECT_EQ(outline(*source),
              "object test.Text\n"
              "  cdata : string = a & b < c\n"
              "  multiline : string = line one\\nline two ]]> still\\n<property name=\"inside\" type=\"int\">9</property>\\nend\n"
              "  entities : string = &amp; &lt;tag&gt; &#233;\n"
              "  plain multiline : string = first\\nsecond\n"
              "  unknown : float = 1.5\n"
              "  empty type :  = x\n"
              "  pair : int = 1\n"
              "  trailing : int = 3\n"
              "  prefix : string = y\n"
              "  nested marker : string = a<![CDATA[b\n"
              "  empty : string = \n"
              "  spaces : string =   padded  \n"
              "  self : string = \\n    <property name=\"swallowed\" type=\"int\">4\n"
              "  last : int = 5\n");
    const ObjectView root(*source, source->root());
    EXPECT_TRUE(require(root.property("cdata")).node().cdata);
    EXPECT_FALSE(require(root.property("entities")).node().cdata);
    EXPECT_EQ(require(root.property("unknown")).type(), PropertyType::Unknown);
    EXPECT_EQ(require(root.property("nested marker")).stringValue().error(), DecodeError::Malformed);
    EXPECT_EQ(require(root.property("prefix")).stringValue(), "y");
}

TEST(OspReader, FindsAttributesByTextSearchAndMovesOldMediaClasses)
{
    const auto source = parsed(fixture("attributes.xml"));
    EXPECT_EQ(outline(*source),
              "object test.Attributes version=1.2\n"
              "  reordered : int = 1\n"
              "  (none) : int = 2\n"
              "  media : object\n"
              "    object org.opensourcephysics.media.core.VideoClip\n"
              "  list : collection class=org.opensourcephysics.media.core.Thing\n"
              "  bad class : object\n"
              "    object \n"
              "  a>b : int = b\" type=\"int\">3\n"
              "  atype= :  type= = 4\n"
              "  spaced : int = 5\n");
    const ObjectView root(*source, source->root());
    EXPECT_EQ(require(root.child("media")).node().rawClassName, "org.opensourcephysics.media.VideoClip");
    EXPECT_EQ(require(root.property("media")).className(), "org.opensourcephysics.media.core.VideoClip");
    EXPECT_FALSE(root.properties()[1].name());
    EXPECT_EQ(require(root.property("atype=")).type(), PropertyType::Unknown);
}

TEST(OspReader, ReadsPrologEpilogAndSameLineQuirks)
{
    const auto prolog = parsed(fixture("prolog-epilog.xml"));
    EXPECT_EQ(outline(*prolog), "object test.Prolog\n  value : int = 1\n");
    EXPECT_EQ(prolog->prolog(),
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- A comment before the root -->\n<!DOCTYPE object SYSTEM \"osp10.dtd\">\n");
    EXPECT_EQ(prolog->epilog(), "<object class=\"test.Ignored\"/>\ntrailing text\n");
    EXPECT_EQ(prolog->rawText(prolog->root()).substr(0, 26), "<object class=\"test.Prolog");

    const auto sameLine = parsed(fixture("same-line.xml"), ACCEPT_TRUNCATED);
    // The version comes from the XML declaration on the root's line; the property's next line is taken as
    // the object's opening tag.
    EXPECT_EQ(outline(*sameLine),
              "object test.SameLine version=1.0\n"
              "  inline : object\n"
              "    object \n"
              "      after : int = 2\n");
    EXPECT_TRUE(sameLine->truncated());

    const auto line10 = parsed(fixture("root-line-10.xml"));
    EXPECT_EQ(outline(*line10), "object test.Line10\n  value : int = 10\n");
    const auto unclosed = parsed(fixture("unclosed-root.xml"), ACCEPT_TRUNCATED);
    EXPECT_EQ(outline(*unclosed),
              "object test.UnclosedRoot\n  value : int = 1\n  child : object\n    object test.Child\n      x : int = 2\n");
    EXPECT_TRUE(unclosed->truncated());
}

TEST(OspReader, ReadsUnicodeAndOnlyBreaksLinesAtCrAndLf)
{
    const auto source = parsed(fixture("unicode.xml"));
    const ObjectView root(*source, source->root());
    EXPECT_EQ(root.getString("\xC3\xA9l\xC3\xA8ve \xCE\xB2"), "P\xC3\xA9ndulo \xF0\x9F\x98\x80 \xE4\xB8\xAD\xE6\x96\x87");
    EXPECT_EQ(root.getString("separators"),
              "a\xE2\x80\xA8"
              "b\xE2\x80\xA9"
              "c\xC2\x85"
              "d");
    EXPECT_EQ(root.getString("cdata"), "< \xC3\xBC > \xF0\x9D\x84\x9E");
    EXPECT_EQ(root.getInt("\xCE\xA9"), 7);
}

TEST(OspReader, LineEndingsAndByteOrderMarkDoNotChangeTheTree)
{
    for (const auto* name : {"blank-line-in-array.xml", "self-closing.xml", "text-values.xml", "same-line.xml", "unicode.xml"})
    {
        const auto lf = fixture(name);
        const auto expected = motrack::osp::objectValue(*parsed(lf, ACCEPT_TRUNCATED), NodeId{0});
        const auto crlf = replaceAll(lf, "\n", "\r\n");
        const auto cr = replaceAll(lf, "\n", "\r");
        const auto crlfSource = parsed(crlf, ACCEPT_TRUNCATED);
        const auto crSource = parsed(cr, ACCEPT_TRUNCATED);
        const auto bomSource = parsed("\xEF\xBB\xBF" + crlf, ACCEPT_TRUNCATED);
        EXPECT_EQ(motrack::osp::objectValue(*crlfSource, NodeId{0}), expected) << name;
        EXPECT_EQ(motrack::osp::objectValue(*crSource, NodeId{0}), expected) << name;
        EXPECT_EQ(motrack::osp::objectValue(*bomSource, NodeId{0}), expected) << name;
        EXPECT_EQ(parsed(lf, ACCEPT_TRUNCATED)->lineEnding(), motrack::osp::LineEnding::Lf);
        EXPECT_EQ(crlfSource->lineEnding(), motrack::osp::LineEnding::CrLf);
        EXPECT_EQ(crSource->lineEnding(), motrack::osp::LineEnding::Cr);
    }
    EXPECT_EQ(parsed("<object class=\"x\"/>")->lineEnding(), motrack::osp::LineEnding::Lf);
    // "\r\r\n" is a CR line break followed by an empty CRLF line.
    const auto mixed = parsed("<object class=\"x\">\r\r\n</object>");
    EXPECT_EQ(mixed->lines().size(), 3U);
    EXPECT_EQ(mixed->lineText(1), "");
}

TEST(OspReader, ReportsTheFailuresJavaReportsAsReadFailures)
{
    using Case = std::tuple<const char*, ParseError::Code, std::size_t>;
    for (const auto& [name, code, line] : {Case{"fail-root-line-11.xml", ParseError::Code::MissingRoot, 10},
                                           Case{"fail-no-root.xml", ParseError::Code::MissingRoot, 0},
                                           Case{"fail-missing-type.xml", ParseError::Code::MissingType, 3},
                                           Case{"fail-null-in-object.xml", ParseError::Code::NullObjectProperty, 3},
                                           Case{"fail-unterminated-array.xml", ParseError::Code::UnterminatedArray, 3},
                                           Case{"fail-unterminated-name.xml", ParseError::Code::MalformedAttribute, 3},
                                           Case{"fail-missing-object.xml", ParseError::Code::MissingObject, 3},
                                           Case{"fail-unterminated-version.xml", ParseError::Code::MalformedAttribute, 2}})
    {
        std::size_t reported = 99;
        EXPECT_EQ(failure(fixture(name), &reported), code) << name;
        EXPECT_EQ(reported, line) << name;
    }
    // Java appends "null" forever at end of file inside a value; MoTrack reports it instead.
    std::size_t line = 0;
    EXPECT_EQ(failure("<object class=\"x\">\n<property name=\"a\" type=\"string\"><![CDATA[open\n", &line),
              ParseError::Code::UnterminatedValue);
    EXPECT_EQ(line, 2U);
    EXPECT_EQ(failure("<object class=\"x\">\n<property name=\"a\" type=\"int\">1"), ParseError::Code::UnterminatedValue);
    EXPECT_EQ(failure(""), ParseError::Code::MissingRoot);
    EXPECT_EQ(failure("<object class=\"org.opensourcephysics.controls.Cryptic\">\n<property name=\"cryptic\" "
                      "type=\"string\">abc</property>\n</object>"),
              ParseError::Code::Encrypted);
}

TEST(OspReader, RefusesDocumentsThatEndBeforeTheirObjectsClose)
{
    // OSP's readObject treats end of file as a closing tag and loads the partial tree with failedToRead()
    // false (#243); MoTrack reports the truncation unless explicitly asked to accept it.
    for (const auto* name : {"unclosed-root.xml", "unclosed-array-line.xml", "same-line.xml"})
    {
        const auto bytes = fixture(name);
        std::size_t line = 0;
        EXPECT_EQ(failure(bytes, &line), ParseError::Code::Truncated) << name;
        EXPECT_EQ(line, parsed(bytes, ACCEPT_TRUNCATED)->lines().size()) << name;
        EXPECT_TRUE(parsed(bytes, ACCEPT_TRUNCATED)->truncated()) << name;
    }
    EXPECT_EQ(failure("<object class=\"x\">\n<property name=\"a\" type=\"int\">1</property>\n"), ParseError::Code::Truncated);
    EXPECT_FALSE(parsed("<object class=\"x\">\n<property name=\"a\" type=\"int\">1</property>\n</object>")->truncated());
    EXPECT_FALSE(parsed("<object class=\"x\"/>")->truncated());
}

TEST(OspReader, RequiresUtf8)
{
    const std::string prefix = "<object class=\"x\">\n<property name=\"a\" type=\"string\">";
    for (const std::string_view bad : {std::string_view("\xC0\xAF"),
                                       std::string_view("\xE0\x80\xAF"),
                                       std::string_view("\xED\xA0\x80"),
                                       std::string_view("\xF4\x90\x80\x80"),
                                       std::string_view("\xF5\x80\x80\x80"),
                                       std::string_view("\x80"),
                                       std::string_view("\xC3"),
                                       std::string_view("\xE2\x82"),
                                       std::string_view("\xC3\x28")})
    {
        std::size_t line = 0;
        EXPECT_EQ(failure(prefix + std::string(bad) + "</property>\n</object>", &line), ParseError::Code::InvalidUtf8);
        EXPECT_EQ(line, 2U);
    }
    // OSP decodes every document as UTF-8 whatever it declares; only non-ASCII text could have been meant
    // differently, so only that is refused under another declared encoding.
    const std::string latin =
        "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n<object class=\"x\">\n<property name=\"a\" type=\"string\">";
    EXPECT_EQ(failure(latin + "\xC3\xA9</property>\n</object>"), ParseError::Code::UnsupportedEncoding);
    EXPECT_TRUE(motrack::osp::parse(latin + "e</property>\n</object>"));
    EXPECT_EQ(failure("<?xml version='1.0' encoding = 'windows-1252' ?>\n<object class=\"\xC3\xA9\"/>"),
              ParseError::Code::UnsupportedEncoding);
    // The scan stays inside the declaration: here "encoding" only occurs in the root's class name.
    EXPECT_TRUE(motrack::osp::parse("<?xml version=\"1.0\"<object class=\"org.example.encodingTool\"/>\n"));
    EXPECT_TRUE(motrack::osp::parse("<?xml version=\"1.0\"\n<object class=\"org.example.encodingTool\">\n<property name=\"\xC3\xA9\" "
                                    "type=\"int\">1</property>\n</object>"));
    // Malformed encoding attributes are ignored, as OSP ignores the declaration.
    for (const auto* declaration : {"<?xml encoding?>",
                                    "<?xml encoding=UTF-8?>",
                                    "<?xml encoding=\"UTF-8?>",
                                    "<?xml xencoding=\"latin1\"?>",
                                    R"(<?xml version="1.0" encoding="utf-8"?>)",
                                    "<?xml encoding='UTF8'?>",
                                    R"(<?xml version="1.0"?>)",
                                    "<!-- no declaration -->"})
    {
        EXPECT_TRUE(motrack::osp::parse(std::string(declaration) + "\n<object class=\"\xC3\xA9\"/>")) << declaration;
    }
}

TEST(OspReader, EnforcesSizeDepthAndNodeLimits)
{
    const auto document = fixture("duplicates.xml");
    EXPECT_EQ(failure(document, nullptr, {.maxBytes = document.size() - 1}), ParseError::Code::TooLarge);
    EXPECT_TRUE(motrack::osp::parse(document, {.maxBytes = document.size()}));
    EXPECT_EQ(failure(document, nullptr, {.maxDepth = 2}), ParseError::Code::TooDeep);
    EXPECT_TRUE(motrack::osp::parse(document, {.maxDepth = 3}));
    EXPECT_EQ(failure(document, nullptr, {.maxNodes = 7}), ParseError::Code::TooManyNodes);
    EXPECT_EQ(parsed(document)->nodeCount(), 11U);
    std::string deep = "<object class=\"x\">\n";
    for (int level = 0; level < 300; ++level)
    {
        deep += "<property name=\"p\" type=\"object\">\n<object class=\"x\">\n";
    }
    EXPECT_EQ(failure(deep), ParseError::Code::TooDeep);
}

TEST(OspReader, DecodesTypedValuesLikeTheJavaAccessors)
{
    const auto source = parsed(fixture("numbers.xml"));
    const ObjectView root(*source, source->root());
    EXPECT_EQ(require(root.property("int plus")).intValue(), 5);
    EXPECT_EQ(require(root.property("int overflow")).intValue().error(), DecodeError::OutOfRange);
    EXPECT_EQ(require(root.property("int space")).intValue().error(), DecodeError::Malformed);
    EXPECT_EQ(require(root.property("double suffix")).doubleValue(), 1.5);
    EXPECT_TRUE(std::isnan(*require(root.property("double nan")).doubleValue()));
    EXPECT_EQ(require(root.property("double comma")).doubleValue().error(), DecodeError::Malformed);
    EXPECT_EQ(require(root.property("bool true")).booleanValue(), true);
    EXPECT_EQ(require(root.property("bool upper")).booleanValue(), false);
    EXPECT_EQ(require(root.property("string cdata")).stringValue(), "x");
    EXPECT_EQ(require(root.property("int plus")).doubleValue().error(), DecodeError::WrongType);
    EXPECT_EQ(require(root.property("double d")).intValue().error(), DecodeError::WrongType);
    EXPECT_EQ(require(root.property("int plus")).booleanValue().error(), DecodeError::WrongType);
    EXPECT_EQ(require(root.property("int plus")).stringValue().error(), DecodeError::WrongType);
    // getDouble also converts int and string properties; getInt converts string properties.
    EXPECT_EQ(root.getDouble("int plus"), 5.0);
    EXPECT_EQ(root.getInt("string cdata").error(), DecodeError::Malformed);
    EXPECT_EQ(root.getInt("double d").error(), DecodeError::WrongType);
    EXPECT_EQ(root.getBoolean("bool true"), true);
    EXPECT_EQ(root.getBoolean("int plus").error(), DecodeError::WrongType);
    EXPECT_EQ(root.getDouble("bool true").error(), DecodeError::WrongType);
}

TEST(OspReader, DecodesCompactArraysLikeArrayValue)
{
    using Kind = motrack::osp::CompactArray::Kind;
    const auto source = parsed(fixture("compact-arrays.xml"));
    const ObjectView root(*source, source->root());
    const auto ints = require(root.property("ints")).compactArray();
    ASSERT_TRUE(ints);
    EXPECT_EQ(ints->kind, Kind::Int);
    EXPECT_EQ(ints->ints, (std::vector<std::int32_t>{1, -2, 3}));
    const auto doubles = require(root.property("doubles")).compactArray();
    ASSERT_TRUE(doubles);
    ASSERT_EQ(doubles->doubles.size(), 6U);
    EXPECT_EQ(doubles->doubles[1], 2.5);
    EXPECT_TRUE(std::isnan(doubles->doubles[2]));
    EXPECT_TRUE(std::signbit(doubles->doubles[5]));
    EXPECT_EQ(require(root.property("booleans")).compactArray()->booleans, (std::vector<bool>{true, true, false, false}));
    const auto nested = require(root.property("nested")).compactArray();
    ASSERT_TRUE(nested);
    ASSERT_EQ(nested->rows.size(), 3U);
    EXPECT_EQ(nested->rows[1].kind, Kind::Null);
    EXPECT_EQ(nested->rows[2].doubles, (std::vector<double>{3.0}));
    EXPECT_EQ(require(root.property("nested trailing")).compactArray()->rows.size(), 1U);
    EXPECT_EQ(require(root.property("nested leading")).compactArray()->rows[0].kind, Kind::Null);
    EXPECT_EQ(require(root.property("nested empty")).compactArray()->rows.size(), 0U);
    EXPECT_EQ(require(root.property("boolean empty")).compactArray()->booleans, std::vector<bool>{false});
    EXPECT_EQ(require(root.property("int comma")).compactArray()->ints.size(), 0U);
    EXPECT_EQ(require(root.property("deep")).compactArray()->rows[1].rows[0].ints, std::vector<std::int32_t>{3});
    for (const auto* name : {"nested empty row", "int empty", "int gap", "int spaced", "not braces", "unbalanced"})
    {
        EXPECT_FALSE(require(root.property(name)).compactArray()) << name;
    }
    EXPECT_EQ(require(root.property("typed holder")).compactArray()->doubles, std::vector<double>{1.0});
    EXPECT_EQ(require(root.property("cdata holder")).compactArray()->ints, std::vector<std::int32_t>{4});
    EXPECT_EQ(require(root.property("object class")).compactArray().error(), DecodeError::Unsupported);
    EXPECT_EQ(require(root.property("indexed")).compactArray().error(), DecodeError::WrongType);
    EXPECT_EQ(motrack::osp::formatCompactArray(*require(root.property("media class")).compactArray()), "{{true},{false,true}}");
    EXPECT_EQ(motrack::osp::formatCompactArray(*nested), "{{1.0,2.0},null,{3.0}}");
    EXPECT_EQ(motrack::osp::formatCompactArray(*ints), "{1,-2,3}");
    EXPECT_EQ(motrack::osp::parseCompactArray("{1}", "[Ljava.lang.Integer;").error(), DecodeError::Unsupported);
    EXPECT_FALSE(motrack::osp::parseCompactArray("{", "[I"));
    EXPECT_FALSE(motrack::osp::parseCompactArray("{{1}", "[[I"));
    EXPECT_FALSE(motrack::osp::parseCompactArray("{x}", "[D"));
}

TEST(OspReader, BoundsCompactArrayNesting)
{
    // The JVM caps arrays at 255 dimensions (Class.forName fails beyond), so OSP cannot load deeper classes.
    const auto deepClass = std::string(256, '[') + "I";
    EXPECT_FALSE(motrack::osp::isPrimitiveArrayClass(deepClass));
    EXPECT_EQ(motrack::osp::parseCompactArray(std::string(256, '{') + "1" + std::string(256, '}'), deepClass).error(),
              DecodeError::Unsupported);
    // 255 dimensions decode, recursing once per dimension.
    const auto maximum = std::string(255, '[') + "I";
    EXPECT_TRUE(motrack::osp::isPrimitiveArrayClass(maximum));
    auto value = require(motrack::osp::parseCompactArray(std::string(255, '{') + "7" + std::string(255, '}'), maximum));
    for (int level = 0; level < 254; ++level)
    {
        ASSERT_EQ(value.rows.size(), 1U);
        auto row = std::move(value.rows.front());
        value = std::move(row);
    }
    EXPECT_EQ(value.ints, std::vector<std::int32_t>{7});
    // Many sibling rows are matched in one linear pass (each row once, not once per enclosing level).
    std::string wide = "{";
    for (int row = 0; row < 20000; ++row)
    {
        wide += row == 0 ? "{1,2}" : ",{1,2}";
    }
    wide += '}';
    EXPECT_EQ(require(motrack::osp::parseCompactArray(wide, "[[I")).rows.size(), 20000U);
    // An unbalanced row still fails as in Java, which searches only before the enclosing closing brace.
    EXPECT_EQ(motrack::osp::parseCompactArray("{{1,{2}", "[[[I").error(), DecodeError::Malformed);
    // A surplus closing brace is skipped like a separator, as Java's scan does.
    EXPECT_EQ(require(motrack::osp::parseCompactArray("{{1}}}", "[[I")).rows.size(), 1U);
}

TEST(OspReader, ReadsIndexedArraysLikeArrayValue)
{
    const auto source = parsed(readFile(std::filesystem::path(MOTRACK_FIXTURES) / "point-mass.trk"));
    const ObjectView root(*source, source->root());
    const auto tracks = require(root.property("tracks")).elements();
    ASSERT_EQ(tracks.size(), 2U);
    const auto mass = tracks[1].object();
    ASSERT_TRUE(mass);
    const auto slots = require(require(mass).property("framedata")).indexedElements();
    ASSERT_TRUE(slots);
    ASSERT_EQ(slots->length, 7U);
    ASSERT_EQ(slots->elements.size(), 3U);
    EXPECT_TRUE(require(slots->at(2)).object());
    EXPECT_TRUE(require(slots->at(4)).isNull());
    EXPECT_FALSE(slots->at(5));
    EXPECT_FALSE(slots->at(99));
    EXPECT_EQ(require(slots->at(6)).elementIndex(), 6);
    EXPECT_EQ(require(mass).getDouble("mass"), 125.5);
    EXPECT_TRUE(require(root.property("tracks")).indexedElements().error() == DecodeError::WrongType);

    const auto quirks = parsed("<object class=\"x\">\n"
                               "<property name=\"later index\" type=\"array\" class=\"[D\">\n"
                               "<property name=\"[3]\" type=\"double\">1.0</property>\n"
                               "<property name=\"[1]\" type=\"double\">2.0</property>\n"
                               "</property>\n"
                               "<property name=\"bad name\" type=\"array\" class=\"[D\">\n"
                               "<property name=\"[x]\" type=\"double\">1.0</property>\n"
                               "</property>\n"
                               "<property name=\"negative\" type=\"array\" class=\"[D\">\n"
                               "<property name=\"[-1]\" type=\"double\">1.0</property>\n"
                               "</property>\n"
                               "<property name=\"duplicate\" type=\"array\" class=\"[D\">\n"
                               "<property name=\"[0]\" type=\"double\">1.0</property>\n"
                               "<property name=\"[0]\" type=\"double\">2.0</property>\n"
                               "</property>\n"
                               "<property name=\"unnamed\" type=\"array\" class=\"[D\">\n"
                               "<property type=\"double\">1.0</property>\n"
                               "</property>\n"
                               "<property name=\"bad first\" type=\"array\" class=\"[D\">\n"
                               "<property name=\"[]\" type=\"double\">1.0</property>\n"
                               "<property name=\"[0]\" type=\"double\">1.0</property>\n"
                               "</property>\n"
                               "<property name=\"empty\" type=\"array\" class=\"[D\"/>\n"
                               "</object>");
    const ObjectView object(*quirks, quirks->root());
    // The last element sets the length; an earlier, larger index is outside it, as in Java.
    EXPECT_EQ(require(object.property("later index")).indexedElements().error(), DecodeError::OutOfRange);
    EXPECT_EQ(require(object.property("bad name")).indexedElements().error(), DecodeError::Malformed);
    EXPECT_EQ(require(object.property("negative")).indexedElements().error(), DecodeError::OutOfRange);
    EXPECT_EQ(require(require(require(object.property("duplicate")).indexedElements()).at(0)).doubleValue(), 2.0);
    EXPECT_EQ(require(object.property("unnamed")).indexedElements().error(), DecodeError::Missing);
    EXPECT_EQ(require(object.property("bad first")).indexedElements().error(), DecodeError::Malformed);
    EXPECT_EQ(require(object.property("empty")).indexedElements()->length, 0U);
    // A huge index allocates nothing: only present elements are stored (Java would allocate the whole array).
    const auto huge = parsed("<object class=\"x\">\n<property name=\"a\" type=\"array\" class=\"[D\">\n"
                             "<property name=\"[2147483646]\" type=\"double\">1.0</property>\n</property>\n</object>");
    const auto sparse = require(require(ObjectView(*huge, huge->root()).property("a")).indexedElements());
    EXPECT_EQ(sparse.length, std::size_t{2147483647});
    EXPECT_EQ(sparse.elements.size(), 1U);
    EXPECT_EQ(require(object.property("empty")).compactArray().error(), DecodeError::Missing);
    EXPECT_EQ(require(object.property("empty")).elements().size(), 0U);
    EXPECT_FALSE(require(object.property("empty")).object());
}

TEST(OspReader, ExposesNodesLinesAndHelpers)
{
    const std::string text = "<object class=\"x\">\r\n    <property name=\"a\" type=\"int\">1</property>\r\n</object>";
    const auto source = parsed(text);
    EXPECT_EQ(source->bytes(), text);
    EXPECT_EQ(source->lines().size(), 3U);
    EXPECT_EQ(source->lineText(1), "    <property name=\"a\" type=\"int\">1</property>");
    const auto property = source->node(source->root()).children.front();
    EXPECT_EQ(source->rawText(property), source->lineText(1));
    EXPECT_TRUE(source->contains(property));
    EXPECT_FALSE(source->contains(NodeId{99}));
    EXPECT_THROW((void)source->node(NodeId{99}), std::out_of_range);
    EXPECT_THROW(PropertyView(*source, source->root()), std::invalid_argument);
    EXPECT_THROW(ObjectView(*source, property), std::invalid_argument);
    const PropertyView view(*source, property);
    EXPECT_EQ(view.id(), property);
    EXPECT_EQ(view.name(), "a");
    EXPECT_EQ(view.content(), "1");
    EXPECT_FALSE(view.isNull());
    EXPECT_TRUE(view.elements().empty());
    EXPECT_EQ(ObjectView(*source, source->root()).id(), source->root());
    EXPECT_EQ(source->epilog(), "");

    EXPECT_EQ(motrack::osp::effectiveClassName("org.opensourcephysics.media.VideoClip"), "org.opensourcephysics.media.core.VideoClip");
    EXPECT_EQ(motrack::osp::effectiveClassName("xorg.opensourcephysics.media.Video"), "xorg.opensourcephysics.media.core.Video");
    EXPECT_EQ(motrack::osp::effectiveClassName("org.opensourcephysics.media.core.VideoClip"), "org.opensourcephysics.media.core.VideoClip");
    EXPECT_EQ(motrack::osp::effectiveClassName("Plain"), "Plain");
    for (const auto type : {PropertyType::Int,
                            PropertyType::Double,
                            PropertyType::Boolean,
                            PropertyType::String,
                            PropertyType::Array,
                            PropertyType::Collection,
                            PropertyType::Object})
    {
        EXPECT_EQ(motrack::osp::propertyTypeFromName(motrack::osp::propertyTypeName(type)), type);
    }
    EXPECT_EQ(motrack::osp::propertyTypeName(PropertyType::Unknown), "unknown");
    EXPECT_EQ(motrack::osp::propertyTypeFromName("Int"), PropertyType::Unknown);
    EXPECT_TRUE(motrack::osp::isTextType(PropertyType::Unknown));
    EXPECT_FALSE(motrack::osp::isTextType(PropertyType::Collection));
    for (const auto* name : {"[I", "[D", "[Z", "[[[D"})
    {
        EXPECT_TRUE(motrack::osp::isPrimitiveArrayClass(name)) << name;
    }
    for (const auto* name : {"", "[", "I", "[[", "[J", "[Ljava.lang.Object;", "[I[I"})
    {
        EXPECT_FALSE(motrack::osp::isPrimitiveArrayClass(name)) << name;
    }
    EXPECT_EQ(motrack::osp::removeCdata("plain"), "plain");
    EXPECT_EQ(motrack::osp::removeCdata("a<![CDATA[b]]>c"), "b");
    EXPECT_EQ(motrack::osp::removeCdata("]]><![CDATA[b").error(), DecodeError::Malformed);
    EXPECT_EQ(motrack::osp::lineEndingText(motrack::osp::LineEnding::CrLf), "\r\n");
    EXPECT_EQ(motrack::osp::lineEndingText(motrack::osp::LineEnding::Cr), "\r");
    EXPECT_EQ(motrack::osp::lineEndingText(motrack::osp::LineEnding::Lf), "\n");
}

} // namespace
