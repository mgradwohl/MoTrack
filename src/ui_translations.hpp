#pragma once

#include <QLocale>
#include <QString>
#include <QStringList>
#include <QTranslator>

namespace motrack
{

/// Installs MoTrack's catalog for the first language in a preference list that MoTrack ships, plus Qt's own catalog
/// for that same language. English is a shipped language (its catalog holds only plural forms), so an English-first
/// list stays English even when a later language has a catalog. Without any match the interface also stays in
/// English (the source text), including Qt's standard dialogs. The application layout direction follows the
/// selected language (right-to-left for Arabic and Hebrew), independent of which Qt catalogs are available.
///
/// Construct after QApplication on the GUI thread and keep it alive while translated widgets exist. Destruction
/// removes both translators and restores the layout direction that was current at construction.
class UiTranslations final
{
  public:
    /// Uses @p locale's UI languages; for the system locale this is the user's preferred-language list.
    /// @param catalogDirectory Directory or resource prefix containing motrack_<language>.qm files.
    explicit UiTranslations(const QLocale& locale, const QString& catalogDirectory = QStringLiteral(":/i18n"));
    /// Uses the first entry of @p uiLanguages (BCP 47 tags, most preferred first, as QLocale::uiLanguages()
    /// returns them) for which MoTrack ships a catalog.
    explicit UiTranslations(const QStringList& uiLanguages, const QString& catalogDirectory = QStringLiteral(":/i18n"));
    ~UiTranslations();
    UiTranslations(const UiTranslations&) = delete;
    UiTranslations& operator=(const UiTranslations&) = delete;
    UiTranslations(UiTranslations&&) = delete;
    UiTranslations& operator=(UiTranslations&&) = delete;

    /// TS language of the installed MoTrack catalog (for example "pt_BR", "he" or "en"); empty when none matched.
    [[nodiscard]] QString language() const;
    /// Whether Qt's catalog for the same language was found and installed.
    [[nodiscard]] bool hasQtCatalog() const;

  private:
    QTranslator m_MoTrack;
    QTranslator m_Qt;
    bool m_MoTrackInstalled{false};
    bool m_QtInstalled{false};
    Qt::LayoutDirection m_PreviousDirection;
};

} // namespace motrack
