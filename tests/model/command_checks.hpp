#pragma once

#include "model/dependencies/name_binding.hpp"
#include "model/descriptors.hpp"
#include "model/tracker_document.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// Checks shared by the command tests: the incrementally maintained graph against one rebuilt from the document, and
/// what every reference renders as at save.
namespace motrack::test
{

/// The value of an optional a test requires; throws, failing the test, when there is none.
template <typename T> T required(std::optional<T> value)
{
    if (!value)
    {
        throw std::runtime_error("A required value is missing.");
    }
    return *std::move(value);
}

inline std::string_view facetLabel(model::Facet facet)
{
    switch (facet)
    {
    case model::Facet::Definition:
        return "definition";
    case model::Facet::Geometry:
        return "geometry";
    case model::Facet::Data:
        break;
    }
    return "data";
}

inline std::string dependencyText(const model::Dependency& dependency)
{
    auto text = std::string(model::dependencyKindName(dependency.kind)) + " " + std::to_string(dependency.slot) + " -> " +
                (dependency.target ? std::to_string(dependency.target->value) : std::string("unbound")) + " " +
                std::string(facetLabel(dependency.targetFacet)) + " '" + dependency.savedName + "' binding " +
                std::to_string(static_cast<int>(dependency.binding)) + " missing " +
                std::to_string(static_cast<int>(dependency.onMissing)) + " candidates";
    for (const auto candidate : dependency.candidates)
    {
        text += " " + std::to_string(candidate.value);
    }
    return text;
}

/// The graph state of every identity the document holds, and the document's reference issues. Retired identities that
/// left the document (created objects whose creation was undone) are the graph's own record and are not compared.
inline std::string graphText(const model::TrackerDocument& document)
{
    const auto& graph = document.graph();
    std::string text;
    for (const auto& object : document.objects())
    {
        text +=
            std::to_string(object.id.value) + (graph.contains(object.id) ? " live" : "") + (graph.wasRemoved(object.id) ? " removed" : "");
        if (const auto* info = graph.object(object.id))
        {
            text += " '" + info->label.value_or("<none>") + "' " + info->className;
        }
        text += '\n';
        for (const auto facet : {model::Facet::Definition, model::Facet::Geometry, model::Facet::Data})
        {
            for (const auto& dependency : graph.dependencies({.object = object.id, .facet = facet}))
            {
                text += "  " + std::string(facetLabel(facet)) + " " + dependencyText(dependency) + "\n";
            }
        }
        for (const auto& reference : graph.referencesTo(object.id))
        {
            text += "  referenced by " + std::to_string(reference.owner.object.value) + "\n";
        }
    }
    for (const auto& issue : document.referenceIssues())
    {
        text += "issue " + std::to_string(issue.owner.value) + " " + issue.field + " " + issue.reason + "\n";
    }
    return text;
}

/// The graph a document holds after commands equals the one assembling its parts derives.
inline ::testing::AssertionResult graphMatchesRebuild(const model::TrackerDocument& document)
{
    auto rebuilt = model::TrackerDocument::assemble(document.parts());
    if (!rebuilt)
    {
        return ::testing::AssertionFailure() << "The document does not assemble: " << rebuilt.error().message;
    }
    const auto incremental = graphText(document);
    const auto derived = graphText(*rebuilt);
    if (incremental != derived)
    {
        return ::testing::AssertionFailure() << "Incremental graph:\n" << incremental << "Rebuilt graph:\n" << derived;
    }
    return ::testing::AssertionSuccess();
}

/// The reference-only descriptor that a dependency of an owner of this class came from.
inline const model::ReferenceDescriptor* referenceField(const std::string& ownerClass, const model::Dependency& dependency)
{
    for (const auto& field : model::referenceDescriptors())
    {
        if (std::ranges::contains(field.ownerClasses, std::string_view(ownerClass)) && field.kind == dependency.kind &&
            dependency.slot >= field.slotBase && dependency.slot - field.slotBase < field.maxSlots)
        {
            return &field;
        }
    }
    return nullptr;
}

/// What each reference of the document renders as at save (`renderName` with its field's filter against the current
/// labels), one line per dependency in document order.
inline std::vector<std::string> renderedReferences(const model::TrackerDocument& document)
{
    const auto& graph = document.graph();
    auto current = model::NameIndex::fromGraph(graph);
    std::vector<std::string> lines;
    for (const auto& object : document.objects())
    {
        for (const auto facet : {model::Facet::Definition, model::Facet::Geometry, model::Facet::Data})
        {
            for (const auto& dependency : graph.dependencies({.object = object.id, .facet = facet}))
            {
                const auto* field = referenceField(object.className, dependency);
                auto line = std::to_string(object.id.value) + " " + dependencyText(dependency) + ": ";
                if (field == nullptr)
                {
                    lines.push_back(line + "no descriptor");
                    continue;
                }
                const auto rendered = model::renderName(graph, dependency, current, field->filter);
                lines.push_back(line + (rendered ? "'" + rendered->name + "' status " + std::to_string(static_cast<int>(rendered->status))
                                                 : std::string("budget")));
            }
        }
    }
    return lines;
}

} // namespace motrack::test
