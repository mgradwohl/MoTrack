#include "ui_translations.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibraryInfo>

namespace motrack
{

UiTranslations::UiTranslations(const QLocale& locale, const QString& catalogDirectory)
    : m_PreviousDirection(QGuiApplication::layoutDirection())
{
    // QTranslator tries each of the locale's UI languages in order, then shorter forms (de_AT, then de).
    m_MoTrackInstalled = m_MoTrack.load(locale, "motrack", "_", catalogDirectory) && QCoreApplication::installTranslator(&m_MoTrack);
    if (!m_MoTrackInstalled)
    {
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
        return;
    }
    // Load Qt's catalog for the language MoTrack uses, not for an earlier UI language MoTrack lacks.
    const QLocale language(m_MoTrack.language());
    const auto loadQtCatalog = [&](const QString& directory)
    {
        return m_Qt.load(language, "qtbase", "_", directory) || m_Qt.load(language, "qt", "_", directory);
    };
    auto qtLoaded = loadQtCatalog(QLibraryInfo::path(QLibraryInfo::TranslationsPath));
#ifdef Q_OS_MACOS
    qtLoaded = qtLoaded || loadQtCatalog(QCoreApplication::applicationDirPath() + "/../Resources/translations");
#endif
    m_QtInstalled = qtLoaded && QCoreApplication::installTranslator(&m_Qt);
    // Set explicitly so the direction does not depend on Qt's own catalog shipping QT_LAYOUT_DIRECTION.
    QGuiApplication::setLayoutDirection(language.textDirection());
}

UiTranslations::~UiTranslations()
{
    if (m_QtInstalled)
        QCoreApplication::removeTranslator(&m_Qt);
    if (m_MoTrackInstalled)
        QCoreApplication::removeTranslator(&m_MoTrack);
    QGuiApplication::setLayoutDirection(m_PreviousDirection);
}

QString UiTranslations::language() const
{
    return m_MoTrackInstalled ? m_MoTrack.language() : QString();
}

bool UiTranslations::hasQtCatalog() const
{
    return m_QtInstalled;
}

} // namespace motrack
