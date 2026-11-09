#pragma once

#include "workspace_document.hpp"

#include <QJsonObject>

namespace motrack::detail
{

/// Shared native metadata codec. The supplied archive is the immutable payload owned by the container.
/// Accepts standalone document versions 1–5; group-envelope dispatch belongs to the group reader.
[[nodiscard]] WorkspaceDocument decodeWorkspaceDocument(const QJsonObject& input, std::shared_ptr<const TrackerArchive> archive);
/// Produces version-5 metadata without serializing or duplicating the archive payload.
[[nodiscard]] QJsonObject encodeWorkspaceDocument(const WorkspaceDocument& document);

} // namespace motrack::detail
