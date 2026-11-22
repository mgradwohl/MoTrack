#include "ui_translations.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibraryInfo>

namespace motrack
{
namespace
{

/// Loads MoTrack's catalog for the first language tag that has one. QTranslator tries each tag and its shorter
/// forms (de-AT, then de). uiLanguages() lists zh-HK, zh-MO and zh-SG without their script, so Chinese regions then
/// use the catalog for their script: Traditional (Hong Kong, Macao) zh_TW, Simplified (Singapore) zh_CN.
bool loadFirstCatalog(QTranslator& translator, const QStringList& uiLanguages, const QString& directory)
{
    for (const auto& tag : uiLanguages)
    {
        const QLocale locale(tag);
        if (locale.language() == QLocale::C)
            continue;
        if (translator.load(locale, "motrack", "_", directory))
            return true;
        if (locale.language() == QLocale::Chinese)
        {
            const auto catalog =
                locale.script() == QLocale::TraditionalHanScript ? QStringLiteral("motrack_zh_TW") : QStringLiteral("motrack_zh_CN");
            if (translator.load(catalog, directory))
                return true;
        }
    }
    return false;
}

} // namespace

UiTranslations::UiTranslations(const QLocale& locale, const QString& catalogDirectory)
    : UiTranslations(locale.uiLanguages(), catalogDirectory)
{}

UiTranslations::UiTranslations(const QStringList& uiLanguages, const QString& catalogDirectory)
    : m_PreviousDirection(QGuiApplication::layoutDirection())
{
    m_MoTrackInstalled = loadFirstCatalog(m_MoTrack, uiLanguages, catalogDirectory) && QCoreApplication::installTranslator(&m_MoTrack);
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
