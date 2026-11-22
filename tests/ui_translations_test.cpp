#include "ui_translations.hpp"

#include "workspace_window.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QMenuBar>
#include <QSet>
#include <QStringList>
#include <QTranslator>
#include <QXmlStreamReader>
#include <gtest/gtest.h>

#include <initializer_list>
#include <utility>
#include <vector>

namespace
{

QString catalogs()
{
    return QString::fromUtf8(MOTRACK_TRANSLATIONS);
}

QString translate(const char* context, const char* source, int count = -1)
{
    return QCoreApplication::translate(context, source, nullptr, count);
}

struct ShippedLocale
{
    const char* locale;             // A system locale name that should select the catalog.
    const char* language;           // TS language of the catalog QTranslator selects.
    const char* catalog;            // translations/motrack_<catalog>.ts
    std::vector<int> pluralSamples; // One count per plural form that the catalog spells differently.
};

// Every non-English locale in Tracker.getLocales() (SwingJS branch); see tools/translations.py LOCALES.
const std::vector<ShippedLocale>& shippedLocales()
{
    static const std::vector<ShippedLocale> locales{
        // Samples avoid forms a language may spell alike: Arabic 0, 11-99 and 100+ ("%n صورة"), Ukrainian 1 and 2-4.
        {.locale = "ar_EG", .language = "ar", .catalog = "ar", .pluralSamples = {1, 2, 3, 11}},
        {.locale = "ca_ES", .language = "ca", .catalog = "ca", .pluralSamples = {1, 5}},
        {.locale = "cs_CZ", .language = "cs", .catalog = "cs", .pluralSamples = {1, 2, 5}},
        {.locale = "da_DK", .language = "da", .catalog = "da", .pluralSamples = {1, 5}},
        {.locale = "de_DE", .language = "de", .catalog = "de", .pluralSamples = {1, 5}},
        {.locale = "el_GR", .language = "el", .catalog = "el", .pluralSamples = {1, 5}},
        {.locale = "es_ES", .language = "es", .catalog = "es", .pluralSamples = {1, 5}},
        {.locale = "fi_FI", .language = "fi", .catalog = "fi", .pluralSamples = {1, 5}},
        {.locale = "fr_FR", .language = "fr", .catalog = "fr", .pluralSamples = {1, 5}},
        {.locale = "he_IL", .language = "he", .catalog = "he", .pluralSamples = {1, 5}},
        {.locale = "hu_HU", .language = "hu", .catalog = "hu", .pluralSamples = {5}},
        {.locale = "id_ID", .language = "id", .catalog = "id", .pluralSamples = {5}},
        {.locale = "it_IT", .language = "it", .catalog = "it", .pluralSamples = {1, 5}},
        {.locale = "ja_JP", .language = "ja", .catalog = "ja", .pluralSamples = {5}},
        {.locale = "ko_KR", .language = "ko", .catalog = "ko", .pluralSamples = {5}},
        {.locale = "lv_LV", .language = "lv", .catalog = "lv", .pluralSamples = {0, 1, 5}},
        {.locale = "ms_MY", .language = "ms", .catalog = "ms", .pluralSamples = {5}},
        {.locale = "nl_NL", .language = "nl", .catalog = "nl", .pluralSamples = {1, 5}},
        {.locale = "pl_PL", .language = "pl", .catalog = "pl", .pluralSamples = {1, 2, 5}},
        {.locale = "pt_PT", .language = "pt_PT", .catalog = "pt", .pluralSamples = {1, 5}},
        {.locale = "pt_BR", .language = "pt_BR", .catalog = "pt_BR", .pluralSamples = {1, 5}},
        {.locale = "ru_RU", .language = "ru", .catalog = "ru", .pluralSamples = {1, 2, 5}},
        {.locale = "sk_SK", .language = "sk", .catalog = "sk", .pluralSamples = {1, 2, 5}},
        {.locale = "sl_SI", .language = "sl", .catalog = "sl", .pluralSamples = {1, 2, 3, 5}},
        {.locale = "sv_SE", .language = "sv", .catalog = "sv", .pluralSamples = {1, 5}},
        {.locale = "th_TH", .language = "th", .catalog = "th", .pluralSamples = {5}},
        {.locale = "tr_TR", .language = "tr", .catalog = "tr", .pluralSamples = {5}},
        {.locale = "uk_UA", .language = "uk", .catalog = "uk", .pluralSamples = {2, 5}},
        {.locale = "vi_VN", .language = "vi", .catalog = "vi", .pluralSamples = {5}},
        {.locale = "zh_CN", .language = "zh_CN", .catalog = "zh_CN", .pluralSamples = {5}},
        {.locale = "zh_TW", .language = "zh_TW", .catalog = "zh_TW", .pluralSamples = {5}},
    };
    return locales;
}

TEST(UiTranslations, LoadsEveryTrackerLocaleCatalog)
{
    for (const auto& shipped : shippedLocales())
    {
        SCOPED_TRACE(shipped.locale);
        const motrack::UiTranslations translations(QLocale(QString::fromUtf8(shipped.locale)), catalogs());
        EXPECT_EQ(translations.language(), QString::fromUtf8(shipped.language));
        // Sentences and compound commands differ from English in every language (single words such as "File"
        // may legitimately stay identical, as in Italian).
        for (const auto* source : {"Open &video…", "Cancel selected open", "Opening %1…", "Close &project…"})
        {
            EXPECT_NE(translate("motrack::WorkspaceWindow", source), QString::fromUtf8(source)) << source;
        }
        for (const auto* source : {"New &point mass", "Calibration Stick", "Clip Settings", "kinetic energy", "Previous frame"})
        {
            EXPECT_NE(translate("MainWindow", source), QString::fromUtf8(source)) << source;
        }
        EXPECT_TRUE(translate("motrack::WorkspaceWindow", "Opening %1…").contains("%1"));
        // lrelease embeds the language's plural rule: each sample selects a differently spelled form. A direct
        // lookup returns an empty string instead of the English fallback when a form is missing.
        QTranslator catalog;
        ASSERT_TRUE(catalog.load(QLocale(QString::fromUtf8(shipped.locale)), "motrack", "_", catalogs()));
        QSet<QString> forms;
        for (const int count : shipped.pluralSamples)
        {
            const auto form = catalog.translate("motrack::WorkspaceWindow", "%n images", nullptr, count);
            EXPECT_FALSE(form.isEmpty()) << count;
            forms.insert(form);
            EXPECT_TRUE(translate("motrack::WorkspaceWindow", "%n images", count).contains(QString::number(count)) || !form.contains("%n"));
        }
        EXPECT_EQ(forms.size(), static_cast<qsizetype>(shipped.pluralSamples.size()));
        EXPECT_EQ(QGuiApplication::layoutDirection(), QLocale(QString::fromUtf8(shipped.language)).textDirection());
    }
    EXPECT_EQ(translate("motrack::WorkspaceWindow", "&File"), QStringLiteral("&File"));
}

struct CatalogEntry
{
    QString context;
    QString source;
    QString translation;
};

/// Non-plural entries of a committed catalog marked type="unfinished" with a translation (machine translations).
std::vector<CatalogEntry> unfinishedEntries(const QString& catalog)
{
    std::vector<CatalogEntry> entries;
    QFile file(QString::fromUtf8(MOTRACK_TS_DIRECTORY) + "/motrack_" + catalog + ".ts");
    if (!file.open(QIODevice::ReadOnly))
        return entries;
    QXmlStreamReader xml(&file);
    CatalogEntry entry;
    bool numerus = false;
    while (!xml.atEnd())
    {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        const auto name = xml.name();
        if (name == u"name")
            entry.context = xml.readElementText();
        else if (name == u"message")
            numerus = xml.attributes().value(u"numerus") == u"yes";
        else if (name == u"source")
            entry.source = xml.readElementText();
        else if (name == u"translation" && !numerus)
        {
            const bool unfinished = xml.attributes().value(u"type") == u"unfinished";
            entry.translation = xml.readElementText();
            if (unfinished && !entry.translation.isEmpty())
                entries.push_back(entry);
        }
    }
    EXPECT_FALSE(xml.hasError()) << xml.errorString().toStdString();
    return entries;
}

TEST(UiTranslations, ShowsUnfinishedMachineTranslationsAtRuntime)
{
    // Maintainer decision (2026-11-22): machine translations stay type="unfinished" for review but ship visible,
    // so lrelease must keep them in the compiled catalogs.
    for (const auto& shipped : shippedLocales())
    {
        SCOPED_TRACE(shipped.locale);
        const motrack::UiTranslations translations(QLocale(QString::fromUtf8(shipped.locale)), catalogs());
        const auto entries = unfinishedEntries(QString::fromUtf8(shipped.catalog));
        EXPECT_FALSE(entries.empty());
        for (const auto& entry : entries)
        {
            EXPECT_EQ(translate(entry.context.toUtf8().constData(), entry.source.toUtf8().constData()), entry.translation)
                << entry.source.toStdString();
        }
    }
}

TEST(UiTranslations, KeepsTrackerTerminologyForSeededStrings)
{
    struct Expected
    {
        const char* locale;
        const char* context;
        const char* source;
        const char* translation;
    };
    // Finished entries copied from Tracker's catalogs, with the source mnemonic and ellipsis applied.
    for (const auto& expected :
         {Expected{.locale = "ja_JP", .context = "motrack::WorkspaceWindow", .source = "&File", .translation = "ファイル(&F)"},
          Expected{.locale = "ar_EG", .context = "motrack::WorkspaceWindow", .source = "&File", .translation = "&ملف"},
          Expected{.locale = "zh_TW", .context = "MainWindow", .source = "Calibration Stick", .translation = "校正桿"},
          Expected{.locale = "ru_RU", .context = "MainWindow", .source = "kinetic energy", .translation = "кинетическая энергия"},
          Expected{.locale = "it_IT", .context = "MainWindow", .source = "New &point mass", .translation = "Nuovo &punto di massa"}})
    {
        SCOPED_TRACE(expected.locale);
        const motrack::UiTranslations translations(QLocale(QString::fromUtf8(expected.locale)), catalogs());
        EXPECT_EQ(translate(expected.context, expected.source), QString::fromUtf8(expected.translation));
    }
}

TEST(UiTranslations, ChoosesTheCatalogForRegionalLocales)
{
    for (const auto& [locale, language] : {std::pair{"pt_AO", "pt_PT"},
                                           std::pair{"pt_BR", "pt_BR"},
                                           std::pair{"de_AT", "de"},
                                           std::pair{"nl_BE", "nl"},
                                           std::pair{"fr_CA", "fr"},
                                           std::pair{"es_MX", "es"},
                                           std::pair{"zh_CN", "zh_CN"},
                                           std::pair{"zh_TW", "zh_TW"},
                                           std::pair{"zh_HK", "zh_TW"},
                                           std::pair{"zh_MO", "zh_TW"},
                                           std::pair{"zh_SG", "zh_CN"},
                                           std::pair{"en_GB", "en"},
                                           std::pair{"ar_SA", "ar"}})
    {
        SCOPED_TRACE(locale);
        const motrack::UiTranslations translations(QLocale(QString::fromUtf8(locale)), catalogs());
        EXPECT_EQ(translations.language(), QString::fromUtf8(language));
    }
}

TEST(UiTranslations, MirrorsTheWorkspaceForRightToLeftLanguages)
{
    for (const auto* locale : {"ar_EG", "he_IL"})
    {
        SCOPED_TRACE(locale);
        {
            const motrack::UiTranslations translations(QLocale(QString::fromUtf8(locale)), catalogs());
            EXPECT_EQ(QGuiApplication::layoutDirection(), Qt::RightToLeft);
            const motrack::WorkspaceWindow window;
            EXPECT_TRUE(window.isRightToLeft());
            const auto menus = window.menuBar()->actions();
            ASSERT_FALSE(menus.isEmpty());
            EXPECT_EQ(menus.front()->text(), translate("motrack::WorkspaceWindow", "&File"));
            EXPECT_NE(menus.front()->text(), QStringLiteral("&File"));
        }
        EXPECT_EQ(QGuiApplication::layoutDirection(), Qt::LeftToRight);
    }
}

TEST(UiTranslations, FallsBackToLeftToRightEnglish)
{
    // Persian is right-to-left, but without a MoTrack catalog the English interface stays left-to-right and Qt's
    // own Persian catalog is not mixed into it.
    for (const auto& locale : {QLocale(QLocale::Esperanto), QLocale(QStringLiteral("fa_IR"))})
    {
        SCOPED_TRACE(locale.name().toStdString());
        const motrack::UiTranslations translations(locale, catalogs());
        EXPECT_TRUE(translations.language().isEmpty());
        EXPECT_FALSE(translations.hasQtCatalog());
        EXPECT_EQ(QGuiApplication::layoutDirection(), Qt::LeftToRight);
        EXPECT_EQ(translate("motrack::WorkspaceWindow", "&File"), QStringLiteral("&File"));
    }
}

TEST(UiTranslations, UsesTheFirstSupportedLanguageOfThePreferenceList)
{
    struct Case
    {
        QStringList uiLanguages;
        const char* language;
        Qt::LayoutDirection direction;
    };
    // English has a catalog (plural forms only), so English first never falls through to a later language.
    for (const auto& [uiLanguages, language, direction] :
         {Case{.uiLanguages = {"en-US", "ja-JP"}, .language = "en", .direction = Qt::LeftToRight},
          Case{.uiLanguages = {"en-GB", "ar-EG"}, .language = "en", .direction = Qt::LeftToRight},
          Case{.uiLanguages = {"en", "he"}, .language = "en", .direction = Qt::LeftToRight},
          Case{.uiLanguages = {"eo", "fa-IR", "he-IL", "en-US"}, .language = "he", .direction = Qt::RightToLeft},
          Case{.uiLanguages = {"eo", "ja-JP", "en-US"}, .language = "ja", .direction = Qt::LeftToRight},
          Case{.uiLanguages = {"zh-HK", "en-US"}, .language = "zh_TW", .direction = Qt::LeftToRight},
          Case{.uiLanguages = {"zh-Hans-SG"}, .language = "zh_CN", .direction = Qt::LeftToRight},
          Case{.uiLanguages = {"eo", "fa"}, .language = "", .direction = Qt::LeftToRight}})
    {
        SCOPED_TRACE(uiLanguages.join(',').toStdString());
        const motrack::UiTranslations translations(uiLanguages, catalogs());
        EXPECT_EQ(translations.language(), QString::fromUtf8(language));
        EXPECT_EQ(QGuiApplication::layoutDirection(), direction);
    }
}

TEST(UiTranslations, EnglishCatalogSpellsPluralForms)
{
    const motrack::UiTranslations translations(QStringList{QStringLiteral("en-US")}, catalogs());
    ASSERT_EQ(translations.language(), QStringLiteral("en"));
    EXPECT_EQ(translate("motrack::WorkspaceWindow", "%n images", 1), QStringLiteral("1 image"));
    EXPECT_EQ(translate("motrack::WorkspaceWindow", "%n images", 2), QStringLiteral("2 images"));
    EXPECT_EQ(translate("motrack::WorkspaceWindow", "&File"), QStringLiteral("&File"));
}

TEST(UiTranslations, InstallsQtCatalogForTheSameLanguage)
{
    const auto qtCatalogs = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    const motrack::UiTranslations translations(QLocale(QStringLiteral("de_DE")), catalogs());
    EXPECT_EQ(translations.language(), QStringLiteral("de"));
    // Developer SDKs normally include Qt's catalogs; minimal Qt installations may omit them.
    if (QFile::exists(qtCatalogs + "/qtbase_de.qm"))
    {
        EXPECT_TRUE(translations.hasQtCatalog());
    }
}

TEST(UiTranslations, MissingCatalogDirectoryKeepsEnglish)
{
    const motrack::UiTranslations translations(QLocale(QStringLiteral("de_DE")), QStringLiteral(":/motrack-missing-catalogs"));
    EXPECT_TRUE(translations.language().isEmpty());
    EXPECT_EQ(translate("MainWindow", "Previous frame"), QStringLiteral("Previous frame"));
}

} // namespace
