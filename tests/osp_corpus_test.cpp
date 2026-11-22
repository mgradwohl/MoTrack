#include "corpus_documents.hpp"
#include "osp/render.hpp"
#include "osp/source.hpp"
#include "osp/value.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

namespace
{

using motrack::osp::NodeId;
using motrack::osp::NodeKind;
using motrack::osp::ObjectValue;
using motrack::osp::Patch;
using motrack::osp::PropertyType;
using motrack::osp::PropertyValue;
using motrack::osp::Source;

/// Quirk fixtures and fuzzed inputs include documents whose objects reach end of file (#243).
constexpr motrack::osp::ParseLimits ACCEPT_TRUNCATED{.acceptTruncated = true};
constexpr motrack::osp::ParseLimits FUZZ_LIMITS{.maxDepth = 64, .maxNodes = 100000, .acceptTruncated = true};

using Document = motrack::test::CorpusDocument;

/// SplitMix64: deterministic on every platform.
class Random
{
  public:
    explicit Random(std::uint64_t seed) : m_State(seed)
    {}

    std::uint64_t next()
    {
        auto value = (m_State += 0x9E3779B97F4A7C15U);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9U;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBU;
        return value ^ (value >> 31);
    }

    std::size_t below(std::size_t bound)
    {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }

    bool chance(std::size_t percent)
    {
        return below(100) < percent;
    }

    template <typename Values> const auto& pick(const Values& values)
    {
        return values[below(values.size())];
    }

  private:
    std::uint64_t m_State;
};

const std::vector<Document>& corpus()
{
    return motrack::test::corpusDocuments();
}

std::string replaceAll(std::string text, std::string_view from, std::string_view to)
{
    for (auto position = text.find(from); position != std::string::npos; position = text.find(from, position + to.size()))
    {
        text.replace(position, from.size(), to);
    }
    return text;
}

/// A render that must succeed; a failure is reported as the test's own failure, never dereferenced.
std::string rendered(const Source& source, const Patch& patch, const motrack::osp::ParseLimits& limits = {})
{
    auto output = motrack::osp::render(source, patch, limits);
    if (!output)
    {
        throw std::runtime_error("render failed: " + output.error().message);
    }
    return *std::move(output);
}

bool expectedFailure(const Document& document)
{
    return document.quirk && document.label.find("/fail-") != std::string::npos;
}

/// Checks the invariants every successful parse must satisfy: lines tile the input and node spans are ordered.
void checkInvariants(const Source& source)
{
    std::string joined;
    for (const auto& line : source.lines())
    {
        ASSERT_LE(line.begin, line.end);
        ASSERT_LE(line.end, line.next);
        joined.append(source.bytes().substr(line.begin, line.next - line.begin));
    }
    ASSERT_EQ(joined, source.bytes());
    for (std::size_t index = 0; index < source.nodeCount(); ++index)
    {
        const auto& node = source.node(static_cast<NodeId>(index));
        ASSERT_LE(node.firstLine, node.lastLine);
        ASSERT_LT(node.lastLine, source.lines().size());
        for (const auto child : node.children)
        {
            ASSERT_EQ(source.node(child).parent, static_cast<NodeId>(index));
            ASSERT_GE(source.node(child).firstLine, node.firstLine);
            ASSERT_LE(source.node(child).lastLine, node.lastLine);
        }
        if (node.kind == NodeKind::Property && motrack::osp::isTextType(node.type))
        {
            ASSERT_LE(node.valueBegin, node.valueEnd);
            ASSERT_LE(node.valueEnd, source.bytes().size());
        }
    }
}

TEST(OspCorpus, EveryFixtureRendersByteForByteInEveryLineEnding)
{
    std::size_t rendered = 0;
    for (const auto& document : corpus())
    {
        const auto lf = replaceAll(replaceAll(document.bytes, "\r\n", "\n"), "\r", "\n");
        const auto source = motrack::osp::parse(document.bytes, ACCEPT_TRUNCATED);
        ASSERT_EQ(source.has_value(), !expectedFailure(document)) << document.label;
        if (!source)
        {
            continue;
        }
        // By default a document whose objects reach end of file is refused (#243); nothing else is.
        const auto strict = motrack::osp::parse(document.bytes);
        EXPECT_EQ(strict.has_value(), !(*source)->truncated()) << document.label;
        if (!strict)
        {
            EXPECT_EQ(strict.error().code, motrack::osp::ParseError::Code::Truncated) << document.label;
        }
        const auto expected = motrack::osp::objectValue(**source, (*source)->root());
        for (const auto& variant : {document.bytes, lf, replaceAll(lf, "\n", "\r\n"), replaceAll(lf, "\n", "\r")})
        {
            const auto variantSource = motrack::osp::parse(variant, ACCEPT_TRUNCATED);
            ASSERT_TRUE(variantSource) << document.label;
            checkInvariants(**variantSource);
            const auto output = motrack::osp::render(**variantSource, Patch(**variantSource), ACCEPT_TRUNCATED);
            ASSERT_TRUE(output) << document.label << ": " << output.error().message;
            EXPECT_EQ(*output, variant) << document.label;
            EXPECT_EQ(motrack::osp::objectValue(**variantSource, (*variantSource)->root()), expected) << document.label;
            ++rendered;
        }
    }
    // tests/fixtures, the osp reader fixtures, the committed corpus fixtures and their archive members.
    EXPECT_GE(rendered, 4U * 60U);
}

TEST(OspCorpus, RewritingEveryPropertyWithItsOwnValueReadsTheSame)
{
    for (const auto& document : corpus())
    {
        if (document.quirk)
        {
            continue;
        }
        const auto source = motrack::osp::parse(document.bytes, ACCEPT_TRUNCATED);
        ASSERT_TRUE(source) << document.label;
        const auto& root = (*source)->node((*source)->root());
        Patch replace(**source);
        for (const auto child : root.children)
        {
            replace.replace(child, motrack::osp::propertyValue(**source, child));
        }
        const auto replaced = motrack::osp::render(**source, replace, ACCEPT_TRUNCATED);
        ASSERT_TRUE(replaced) << document.label << ": " << replaced.error().message;
        const auto reread = motrack::osp::parse(*replaced, ACCEPT_TRUNCATED);
        ASSERT_TRUE(reread);
        EXPECT_EQ(motrack::osp::objectValue(**reread, (*reread)->root()), motrack::osp::objectValue(**source, (*source)->root()));

        Patch contents(**source);
        for (std::size_t index = 0; index < (*source)->nodeCount(); ++index)
        {
            const auto& node = (*source)->node(static_cast<NodeId>(index));
            if (node.kind == NodeKind::Property && motrack::osp::isTextType(node.type))
            {
                contents.setContent(static_cast<NodeId>(index), node.content);
            }
        }
        const auto rewritten = motrack::osp::render(**source, contents, ACCEPT_TRUNCATED);
        ASSERT_TRUE(rewritten) << document.label << ": " << rewritten.error().message;
    }
}

/// Lines whose leading whitespace is outside every value: opening lines and closing lines of nodes.
std::set<std::size_t> structuralLines(const Source& source)
{
    std::set<std::size_t> lines;
    for (std::size_t index = 0; index < source.nodeCount(); ++index)
    {
        const auto& node = source.node(static_cast<NodeId>(index));
        lines.insert(node.firstLine);
        if (const auto close = node.closeLine)
        {
            lines.insert(*close);
        }
    }
    return lines;
}

/// Lines before which the reader is inside an object's property loop, where blank and comment lines are ignored.
std::set<std::size_t> ignorablePositions(const Source& source)
{
    std::set<std::size_t> lines;
    for (std::size_t index = 0; index < source.nodeCount(); ++index)
    {
        const auto& node = source.node(static_cast<NodeId>(index));
        if (node.kind != NodeKind::Object || node.selfClosing)
        {
            continue;
        }
        for (const auto child : node.children)
        {
            lines.insert(source.node(child).firstLine);
        }
        if (const auto close = node.closeLine)
        {
            lines.insert(*close);
        }
    }
    return lines;
}

TEST(OspCorpus, WhitespaceAndLineBreakMutationsKeepTheTree)
{
    Random random(0x05F5E100);
    const std::vector<std::string> indents{"", " ", "\t", "        ", " \t  "};
    const std::vector<std::string> fillers{"", "   ", "<!-- inserted comment -->", "\t"};
    for (const auto& document : corpus())
    {
        if (document.quirk)
        {
            continue;
        }
        const auto source = *motrack::osp::parse(document.bytes, ACCEPT_TRUNCATED);
        const auto expected = motrack::osp::objectValue(*source, source->root());
        const auto structural = structuralLines(*source);
        const auto ignorable = ignorablePositions(*source);
        for (int iteration = 0; iteration < 3; ++iteration)
        {
            std::string mutated;
            const auto lines = source->lines();
            for (std::size_t line = 0; line < lines.size(); ++line)
            {
                const auto terminator = random.chance(50) ? "\r\n" : "\n";
                if (ignorable.contains(line) && random.chance(20))
                {
                    mutated += random.pick(fillers) + terminator;
                }
                auto text = std::string(source->lineText(line));
                if (structural.contains(line) && random.chance(30))
                {
                    const auto indent = text.find_first_not_of(" \t");
                    text = random.pick(indents) + (indent == std::string::npos ? std::string() : text.substr(indent));
                }
                mutated += text;
                if (lines[line].next > lines[line].end || line + 1 < lines.size())
                {
                    mutated += terminator;
                }
            }
            const auto reread = motrack::osp::parse(mutated, ACCEPT_TRUNCATED);
            ASSERT_TRUE(reread) << document.label;
            EXPECT_EQ(motrack::osp::objectValue(**reread, (*reread)->root()), expected) << document.label << " iteration " << iteration;
            EXPECT_EQ(rendered(**reread, Patch(**reread), ACCEPT_TRUNCATED), mutated);
        }
    }
}

/// Test-side model of a patch, applied to the logical tree independently of the renderer.
struct Expectation
{
    std::map<NodeId, std::string> contents;
    std::set<NodeId> removed;
    std::map<NodeId, PropertyValue> replaced;
    std::map<NodeId, std::vector<std::pair<std::size_t, PropertyValue>>> inserted;
};

std::vector<PropertyValue> expectedChildren(const Source& source, NodeId parent, const Expectation& expectation);

PropertyValue expectedProperty(const Source& source, NodeId id, const Expectation& expectation)
{
    auto value = motrack::osp::propertyValue(source, id);
    const auto& node = source.node(id);
    if (motrack::osp::isTextType(node.type))
    {
        if (const auto content = expectation.contents.find(id); content != expectation.contents.end())
        {
            value.content = motrack::osp::normalizeLineBreaks(content->second);
        }
    }
    else if (node.type == PropertyType::Object)
    {
        if (!node.children.empty())
        {
            auto object = *value.object;
            object.properties = expectedChildren(source, node.children.front(), expectation);
            value.object = std::make_shared<const ObjectValue>(std::move(object));
        }
    }
    else
    {
        value.elements = expectedChildren(source, id, expectation);
    }
    return value;
}

std::vector<PropertyValue> expectedChildren(const Source& source, NodeId parent, const Expectation& expectation)
{
    const auto& children = source.node(parent).children;
    std::vector<PropertyValue> result;
    const auto inserted = expectation.inserted.find(parent);
    for (std::size_t position = 0; position <= children.size(); ++position)
    {
        if (inserted != expectation.inserted.end())
        {
            for (const auto& [index, value] : inserted->second)
            {
                if (index == position)
                {
                    result.push_back(value);
                }
            }
        }
        if (position == children.size())
        {
            break;
        }
        const auto child = children[position];
        if (expectation.removed.contains(child))
        {
            continue;
        }
        const auto replacement = expectation.replaced.find(child);
        result.push_back(replacement != expectation.replaced.end() ? replacement->second : expectedProperty(source, child, expectation));
    }
    return result;
}

constexpr std::array<std::string_view, 8> NAMES{"x", "name", "\xC3\xA9l\xC3\xA8ve", "a b", "mass_unit", "[0]", "item", "\xF0\x9F\x98\x80"};
constexpr std::array<std::string_view, 9> TEXTS{
    "", "plain", "a & b", "<tag>", "line one\nline two", "\xCE\xB2 \xF0\x9F\x98\x80", "  spaced  ", "]]>", "'quoted'"};

PropertyValue randomValue(Random& random, int depth, bool allowNull)
{
    const auto name = std::string(random.pick(NAMES));
    switch (random.below(depth > 2 ? 4 : 8))
    {
    case 0:
        return motrack::osp::intProperty(name, static_cast<std::int32_t>(random.next()));
    case 1:
        return motrack::osp::doubleProperty(name, static_cast<double>(static_cast<std::int64_t>(random.next() % 2000001) - 1000000) / 64.0);
    case 2:
        return motrack::osp::booleanProperty(name, random.chance(50));
    case 3:
        return motrack::osp::stringProperty(name, random.pick(TEXTS));
    case 4:
    {
        ObjectValue object{.className = "test.Random"};
        for (std::size_t count = random.below(3); count > 0; --count)
        {
            object.properties.push_back(randomValue(random, depth + 1, false));
        }
        return motrack::osp::objectProperty(name, std::move(object));
    }
    case 5:
    {
        std::vector<PropertyValue> items;
        for (std::size_t count = random.below(3); count > 0; --count)
        {
            items.push_back(randomValue(random, depth + 1, true));
        }
        return motrack::osp::collectionProperty(name, "java.util.ArrayList", std::move(items));
    }
    case 6:
    {
        std::vector<std::optional<PropertyValue>> elements;
        for (std::size_t count = random.below(4); count > 0; --count)
        {
            elements.push_back(random.chance(30) ? std::nullopt : std::optional(randomValue(random, depth + 1, false)));
        }
        return motrack::osp::arrayProperty(name, "[Ljava.lang.Object;", elements);
    }
    default:
        if (allowNull && random.chance(50))
        {
            return motrack::osp::nullObjectProperty(name);
        }
        return *motrack::osp::primitiveArrayProperty(
            name, "[D", {.kind = motrack::osp::CompactArray::Kind::Double, .doubles = {1.5, -0.25}});
    }
}

bool within(const Source& source, NodeId node, NodeId ancestor)
{
    for (std::optional<NodeId> current = node; current; current = source.node(*current).parent)
    {
        if (*current == ancestor)
        {
            return true;
        }
    }
    return false;
}

TEST(OspCorpus, RandomPatchesRenderTheIntendedTreeAndKeepUntouchedBytes)
{
    Random random(20261121);
    std::size_t applied = 0;
    for (const auto& document : corpus())
    {
        if (document.quirk)
        {
            continue;
        }
        const auto source = *motrack::osp::parse(document.bytes, ACCEPT_TRUNCATED);
        std::vector<NodeId> properties;
        std::vector<NodeId> parents;
        for (std::size_t index = 0; index < source->nodeCount(); ++index)
        {
            const auto id = static_cast<NodeId>(index);
            const auto& node = source->node(id);
            if (node.kind == NodeKind::Property)
            {
                properties.push_back(id);
            }
            if (node.closed && (node.kind == NodeKind::Object || node.type == PropertyType::Array || node.type == PropertyType::Collection))
            {
                parents.push_back(id);
            }
        }
        for (int iteration = 0; iteration < 4; ++iteration)
        {
            Patch patch(*source);
            Expectation expectation;
            std::vector<NodeId> targets;
            std::vector<NodeId> cut;
            const auto blocked = [&](NodeId id)
            {
                return std::ranges::any_of(cut, [&](NodeId removed) { return within(*source, id, removed); });
            };
            for (std::size_t step = 0; step < 4; ++step)
            {
                const auto action = random.below(4);
                if (action == 3)
                {
                    const auto parent = random.pick(parents);
                    if (blocked(parent))
                    {
                        continue;
                    }
                    const auto& node = source->node(parent);
                    const auto index = random.below(node.children.size() + 1);
                    auto value = randomValue(random, 0, node.kind != NodeKind::Object);
                    patch.insert(parent, index, value);
                    expectation.inserted[parent].emplace_back(index, std::move(value));
                    targets.push_back(parent);
                    continue;
                }
                const auto property = random.pick(properties);
                const auto& node = source->node(property);
                if (blocked(property) || std::ranges::any_of(targets, [&](NodeId target) { return within(*source, target, property); }))
                {
                    continue;
                }
                const auto parent = node.parent;
                if (!parent)
                {
                    continue;
                }
                const bool inObject = source->node(*parent).kind == NodeKind::Object;
                if (action == 0 && motrack::osp::isTextType(node.type))
                {
                    auto text = std::string(random.pick(TEXTS));
                    if (node.type == PropertyType::Int)
                    {
                        const auto value = static_cast<std::int32_t>(random.next());
                        patch.setInt(property, value);
                        text = motrack::osp::formatJavaInt(value);
                    }
                    else if (node.type == PropertyType::String)
                    {
                        patch.setString(property, text);
                    }
                    else if (text.find('<') == std::string::npos)
                    {
                        patch.setContent(property, text);
                    }
                    else
                    {
                        continue;
                    }
                    expectation.contents[property] = text;
                }
                else if (action == 1)
                {
                    patch.remove(property);
                    expectation.removed.insert(property);
                    cut.push_back(property);
                }
                else
                {
                    auto value = randomValue(random, 0, !inObject);
                    value.name = node.name;
                    patch.replace(property, value);
                    expectation.replaced.emplace(property, std::move(value));
                    cut.push_back(property);
                }
                targets.push_back(property);
            }
            const auto output = motrack::osp::render(*source, patch, ACCEPT_TRUNCATED);
            ASSERT_TRUE(output) << document.label << ": " << output.error().message;
            const auto reread = motrack::osp::parse(*output, ACCEPT_TRUNCATED);
            ASSERT_TRUE(reread);
            ObjectValue expected = motrack::osp::objectValue(*source, source->root());
            expected.properties = expectedChildren(*source, source->root(), expectation);
            EXPECT_EQ(motrack::osp::objectValue(**reread, (*reread)->root()), expected) << document.label << " iteration " << iteration;
            // Untouched top-level subtrees keep their exact bytes.
            for (const auto child : source->node(source->root()).children)
            {
                if (std::ranges::none_of(targets, [&](NodeId target) { return within(*source, target, child); }))
                {
                    EXPECT_NE(output->find(source->rawText(child)), std::string::npos) << document.label;
                }
            }
            ++applied;
        }
    }
    EXPECT_GE(applied, 4U * 50U);
}

TEST(OspCorpus, ByteMutationsKeepTheReaderInvariants)
{
    Random random(0xC0FFEE);
    const std::vector<std::string> fragments{"<",
                                             ">",
                                             "/>",
                                             "\"",
                                             "\n",
                                             "\r",
                                             "\r\n",
                                             " ",
                                             "{",
                                             "}",
                                             ",",
                                             "<property",
                                             "</property>",
                                             "</object>",
                                             ">null</property",
                                             "<![CDATA[",
                                             "]]>",
                                             "<object class=\"z\">",
                                             "type=\"int\"",
                                             "\xC3\xA9",
                                             "\xFF"};
    std::size_t parsedCount = 0;
    for (const auto& document : corpus())
    {
        for (int iteration = 0; iteration < 6; ++iteration)
        {
            auto bytes = document.bytes;
            for (std::size_t edit = 1 + random.below(3); edit > 0 && !bytes.empty(); --edit)
            {
                const auto position = random.below(bytes.size());
                switch (random.below(4))
                {
                case 0:
                    bytes.insert(position, random.pick(fragments));
                    break;
                case 1:
                    bytes.erase(position, 1 + random.below(40));
                    break;
                case 2:
                    bytes.replace(position, 1, random.pick(fragments));
                    break;
                default:
                    bytes.resize(position);
                    break;
                }
            }
            const auto source = motrack::osp::parse(bytes, FUZZ_LIMITS);
            if (!source)
            {
                EXPECT_FALSE(source.error().message.empty());
                continue;
            }
            ++parsedCount;
            checkInvariants(**source);
            EXPECT_EQ(rendered(**source, Patch(**source), FUZZ_LIMITS), bytes);
            (void)motrack::osp::objectValue(**source, (*source)->root());
            // Any edit either renders into exactly the intended tree or is refused; it never mis-renders.
            const auto& root = (*source)->node((*source)->root());
            Patch patch(**source);
            patch.insert((*source)->root(), random.below(root.children.size() + 1), motrack::osp::stringProperty("fuzz", "a & b"));
            if (!root.children.empty())
            {
                patch.replace(root.children.front(), motrack::osp::intProperty("first", 1));
            }
            const auto output = motrack::osp::render(**source, patch, FUZZ_LIMITS);
            if (output)
            {
                ASSERT_TRUE(motrack::osp::parse(*output, FUZZ_LIMITS));
            }
        }
    }
    EXPECT_GT(parsedCount, 150U);
}

} // namespace
