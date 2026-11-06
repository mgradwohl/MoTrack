#pragma once

#include "track_edits.hpp"
#include "workspace_document.hpp"

#include <cstdint>
#include <memory>

namespace motrack
{

enum class SaveOutcome : std::uint8_t
{
    Saved,
    Failed,
    Stale,
    Replaced
};

/// One live document, its undo history and native save checkpoint. GUI-thread owned; workers receive copies of document.
class DocumentSession
{
    struct Identity
    {};

  public:
    struct SaveCheckpoint
    {
        std::shared_ptr<const Identity> identity;
        std::uint64_t revision;
    };

    WorkspaceDocument document;
    TrackEdits history;

    /// Install a successfully prepared document, clearing only this session's old history/save target.
    void replace(WorkspaceDocument incoming, const QString& source);
    [[nodiscard]] const QString& sourcePath() const;
    [[nodiscard]] const QString& nativeSavePath() const;
    [[nodiscard]] SaveCheckpoint saveCheckpoint() const;
    /// Failed/replaced writes leave this session untouched. A successful stale write keeps its destination but stays dirty.
    [[nodiscard]] SaveOutcome completeSave(const SaveCheckpoint& checkpoint, const QString& destination, bool success);

    DocumentSession() = default;
    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;
    DocumentSession(DocumentSession&&) = delete;
    DocumentSession& operator=(DocumentSession&&) = delete;
    ~DocumentSession() = default;

  private:
    // Tokens retain the identity object, preventing reuse after replacement or destruction, even across sessions.
    std::shared_ptr<const Identity> m_Identity = std::make_shared<const Identity>();
    QString m_SourcePath;
    QString m_NativeSavePath;
};

} // namespace motrack
