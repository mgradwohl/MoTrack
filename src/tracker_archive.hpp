#pragma once

#include "legacy_project.hpp"

#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <atomic>

namespace motrack
{

/// Owns a temporary, validated extraction of a Tracker experiment archive.
class TrackerArchive
{
  public:
    [[nodiscard]] static TrackerArchive open(const QString& path, const std::atomic_bool* cancelled = nullptr);
    [[nodiscard]] const QStringList& projects() const;
    [[nodiscard]] QString projectPath(const QString& member) const;
    [[nodiscard]] QString root() const;
    void resolveMedia(LegacyProject& project, const QString& projectMember) const;

  private:
    QTemporaryDir m_Directory;
    QStringList m_Projects;
    QSet<QString> m_Files;
};

} // namespace motrack
