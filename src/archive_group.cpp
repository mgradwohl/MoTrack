#include "archive_group.hpp"

#include "tracker_archive.hpp"
#include "workspace_document_codec.hpp"
#include "workspace_storage.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <utility>

namespace motrack
{
namespace
{

constexpr int GROUP_VERSION = 6;

void require(bool valid)
{
    if (!valid)
        throw ProjectError("Invalid MoTrack archive group.");
}

void checkCancellation(const std::atomic_bool* cancelled)
{
    if (cancelled && *cancelled)
        throw ProjectError("Archive group operation canceled.");
}

QUuid readId(const QJsonValue& value)
{
    require(value.isString());
    const auto text = value.toString();
    const QUuid id(text);
    require(!id.isNull() && id.toString(QUuid::WithoutBraces) == text);
    return id;
}

void validateMember(const ArchiveGroupMember& member, const std::shared_ptr<const TrackerArchive>& archive)
{
    require(archive && !member.id.isNull() && !member.member.empty());
    const auto& document = member.document;
    require(document.archivePayload == archive && document.archiveReference.empty() && document.archiveMember &&
            *document.archiveMember == member.member && document.trackerSource);
    // Exact member lookup rejects foreign names and paths; do not normalize basenames or case.
    (void)archive->projectPath(QString::fromStdString(member.member));
}

void validateMembership(const ArchiveGroup& group, const std::atomic_bool* cancelled)
{
    checkCancellation(cancelled);
    require(!group.id.isNull() && group.archive && !group.members.empty());
    require(std::cmp_less_equal(group.members.size(), group.archive->projects().size()));
    QSet<QUuid> ids;
    QSet<QString> members;
    for (const auto& member : group.members)
    {
        checkCancellation(cancelled);
        validateMember(member, group.archive);
        const auto name = QString::fromStdString(member.member);
        require(!ids.contains(member.id) && !members.contains(name));
        ids.insert(member.id);
        members.insert(name);
    }
}

} // namespace

ArchiveGroup makeArchiveGroup(WorkspaceDocument document)
{
    if (!document.archivePayload || !document.archiveMember)
        throw ProjectError("Invalid MoTrack archive group.");
    ArchiveGroup result{.id = QUuid::createUuid(), .archive = document.archivePayload, .members = {}};
    ArchiveGroupMember member{.id = QUuid::createUuid(), .member = *document.archiveMember, .document = std::move(document)};
    validateMember(member, result.archive);
    result.members.push_back(std::move(member));
    return result;
}

QUuid appendArchiveGroupDocument(ArchiveGroup& group, WorkspaceDocument document)
{
    validateMembership(group, nullptr);
    if (!document.archiveMember || !std::cmp_less(group.members.size(), group.archive->projects().size()))
        throw ProjectError("Invalid MoTrack archive group.");
    ArchiveGroupMember member{.id = QUuid::createUuid(), .member = *document.archiveMember, .document = std::move(document)};
    validateMember(member, group.archive);
    for (const auto& existing : group.members)
        require(existing.member != member.member && existing.id != member.id);
    const auto id = member.id;
    group.members.push_back(std::move(member));
    return id;
}

ArchiveGroup readArchiveGroup(const QString& path, const std::atomic_bool* cancelled)
{
    auto storage = readWorkspaceStorage(path, cancelled);
    checkCancellation(cancelled);
    const auto input = QJsonDocument::fromJson(storage.json).object();
    require(input.value("format") == "MoTrack" && input.value("version").toInt() == GROUP_VERSION &&
            input.value("kind") == "archive-group" && storage.archive);
    require(input.value("sourceDigest").isString() &&
            input.value("sourceDigest").toString() == QString::fromLatin1(storage.archive->sourceDigest().toHex()));
    ArchiveGroup result{.id = readId(input.value("groupId")), .archive = std::move(storage.archive), .members = {}};
    require(input.value("members").isArray());
    const auto members = input.value("members").toArray();
    require(!members.isEmpty() && members.size() <= result.archive->projects().size());
    result.members.reserve(static_cast<std::size_t>(members.size()));
    for (const auto& entry : members)
    {
        checkCancellation(cancelled);
        require(entry.isObject());
        const auto member = entry.toObject();
        require(member.value("member").isString() && member.value("document").isObject());
        const auto document = member.value("document").toObject();
        require(document.value("version").toInt() == 5);
        result.members.push_back({.id = readId(member.value("documentId")),
                                  .member = member.value("member").toString().toStdString(),
                                  .document = detail::decodeWorkspaceDocument(document, result.archive)});
    }
    validateMembership(result, cancelled);
    return result;
}

void writeArchiveGroup(const QString& path, const ArchiveGroup& group, const std::atomic_bool* cancelled)
{
    validateMembership(group, cancelled);
    QJsonObject output{{"format", "MoTrack"},
                       {"version", GROUP_VERSION},
                       {"kind", "archive-group"},
                       {"groupId", group.id.toString(QUuid::WithoutBraces)},
                       {"sourceDigest", QString::fromLatin1(group.archive->sourceDigest().toHex())},
                       {"members", QJsonArray{}}};
    qint64 metadataSize = QJsonDocument(output).toJson(QJsonDocument::Compact).size();
    QJsonArray members;
    for (const auto& member : group.members)
    {
        checkCancellation(cancelled);
        const bool sourceIntact = group.archive->memberMatchesSource(QString::fromStdString(member.member), cancelled);
        checkCancellation(cancelled);
        require(sourceIntact);
        const auto document = detail::encodeWorkspaceDocument(member.document);
        // Verify readability before opening the destination or committing any group member.
        (void)detail::decodeWorkspaceDocument(document, group.archive);
        const QJsonObject encoded{{"documentId", member.id.toString(QUuid::WithoutBraces)},
                                  {"member", QString::fromStdString(member.member)},
                                  {"document", document}};
        const auto size = QJsonDocument(encoded).toJson(QJsonDocument::Compact).size();
        const qint64 separator = members.isEmpty() ? 0 : 1;
        require(size <= MAX_WORKSPACE_JSON_BYTES - metadataSize - separator);
        metadataSize += size + separator;
        members.append(encoded);
    }
    checkCancellation(cancelled);
    output.insert("members", members);
    writeWorkspaceStorage(path, QJsonDocument(output).toJson(QJsonDocument::Compact), group.archive, cancelled);
}

} // namespace motrack
