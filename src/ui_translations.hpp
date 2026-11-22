#pragma once

#include <QLocale>
#include <QString>
#include <QTranslator>

namespace motrack
{

/// Installs MoTrack's catalog for the first of a locale's UI languages that MoTrack ships, plus Qt's own catalog
/// for that same language. Without a matching MoTrack catalog the interface stays in English (the source text),
/// including Qt's standard dialogs. The application layout direction follows the installed MoTrack language
/// (right-to-left for Arabic and Hebrew), independent of which Qt catalogs are available.
///
/// Construct after QApplication on the GUI thread and keep it alive while translated widgets exist. Destruction
/// removes both translators and restores the layout direction that was current at construction.
class UiTranslations final
{
  public:
    /// @param catalogDirectory Directory or resource prefix containing motrack_<language>.qm files.
    explicit UiTranslations(const QLocale& locale, const QString& catalogDirectory = QStringLiteral(":/i18n"));
    ~UiTranslations();
    UiTranslations(const UiTranslations&) = delete;
    UiTranslations& operator=(const UiTranslations&) = delete;
    UiTranslations(UiTranslations&&) = delete;
    UiTranslations& operator=(UiTranslations&&) = delete;

    /// TS language of the installed MoTrack catalog (for example "pt_BR" or "he"); empty for English.
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
