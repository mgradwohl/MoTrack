#pragma once

#include "workspace_document.hpp"

#include <QUuid>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace motrack
{

struct ArchiveGroupMember
{
    QUuid id;
    std::string member;
    WorkspaceDocument document;
};

/// Persistence snapshot, independent of which experiments have visible tabs.
/// Records share one immutable archive owner and retain exact original member bindings.
/// Persistent IDs do not replace live session/job identities or save checkpoints.
struct ArchiveGroup
{
    QUuid id;
    std::shared_ptr<const TrackerArchive> archive;
    std::vector<ArchiveGroupMember> members;
};

/// Promote a known-member archive document, retaining its source owner and current edits.
[[nodiscard]] ArchiveGroup makeArchiveGroup(WorkspaceDocument document);
/// Add a prepared document from this exact owner; equal digests do not join independent groups.
/// Duplicate members and invalid ownership leave the group unchanged.
[[nodiscard]] QUuid appendArchiveGroupDocument(ArchiveGroup& group, WorkspaceDocument document);
/// Check persistent IDs, uniqueness and exact source-owner/member bindings without reading or writing files.
/// Full metadata, source-integrity and output checks remain the persistence worker's responsibility.
void validateArchiveGroupMembership(const ArchiveGroup& group);
/// Read a version-6 group with one original archive; standalone formats are handled separately.
[[nodiscard]] ArchiveGroup readArchiveGroup(const QString& path, const std::atomic_bool* cancelled = nullptr);
/// Atomically write every retained member, including records without a visible tab.
/// Low-level snapshot storage only; coordinated live-session saving is a separate layer.
void writeArchiveGroup(const QString& path, const ArchiveGroup& group, const std::atomic_bool* cancelled = nullptr);

} // namespace motrack
