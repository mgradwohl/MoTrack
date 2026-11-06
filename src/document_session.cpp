#include "document_session.hpp"

#include <QFileInfo>

#include <utility>

namespace motrack
{

void DocumentSession::replace(WorkspaceDocument incoming, const QString& source)
{
    auto identity = std::make_shared<const Identity>();
    const auto path = QFileInfo(source).absoluteFilePath();
    const auto native = source.endsWith(".motrack", Qt::CaseInsensitive) ? path : QString{};
    document = std::move(incoming);
    m_Identity = std::move(identity);
    m_SourcePath = path;
    m_NativeSavePath = native;
    history.clear();
}

const QString& DocumentSession::sourcePath() const
{
    return m_SourcePath;
}

const QString& DocumentSession::nativeSavePath() const
{
    return m_NativeSavePath;
}

DocumentSession::SaveCheckpoint DocumentSession::saveCheckpoint() const
{
    return {.identity = m_Identity, .revision = history.revision()};
}

SaveOutcome DocumentSession::completeSave(const SaveCheckpoint& checkpoint, const QString& destination, bool success)
{
    if (checkpoint.identity != m_Identity)
    {
        return SaveOutcome::Replaced;
    }
    if (!success)
    {
        return SaveOutcome::Failed;
    }
    m_NativeSavePath = QFileInfo(destination).absoluteFilePath();
    if (checkpoint.revision != history.revision())
    {
        history.invalidateSaved();
        return SaveOutcome::Stale;
    }
    history.markSaved();
    return SaveOutcome::Saved;
}

} // namespace motrack
