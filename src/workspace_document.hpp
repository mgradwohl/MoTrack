#pragma once

#include "legacy_project.hpp"

#include <QString>

namespace motrack
{

struct WorkspaceDocument
{
    LegacyProject project;
    std::vector<double> movieFrameTimes;
    bool movie = false;
    std::string archiveReference = {};
};

/// Reads a versioned MoTrack document. Media paths are relative to the document.
[[nodiscard]] WorkspaceDocument readWorkspaceDocument(const QString& path);
/// Writes atomically. The caller supplies media paths relative to the destination.
void writeWorkspaceDocument(const QString& path, const WorkspaceDocument& document);

} // namespace motrack
