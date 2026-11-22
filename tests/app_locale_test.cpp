// The process locale setup (src/app_locale.hpp): its fallback order through a fake C runtime, and the real setup
// after a QCoreApplication started in a comma-decimal environment (CONTRIBUTING.md, "Text encoding and locale").

#include "app_locale.hpp"

#include "test_locales.hpp"

#include <QCoreApplication>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <locale>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <langinfo.h>
#endif

namespace
{

/// What main() saw: the comma-decimal locale it put in LC_ALL (POSIX only), and LC_NUMERIC once the
/// QCoreApplication was constructed but before configureProcessLocale() ran.
std::optional<motrack::test::TestLocale> environmentLocale;
std::string numericAfterQt;
motrack::ProcessLocale configured;

/// A C runtime that knows a fixed set of locale names. "" stands for the environment's locale.
struct FakeRuntime
{
    std::vector<std::string> installed;
    std::string environment = "C";
    std::string user;
    std::map<int, std::string> current{{LC_ALL, "C"}, {LC_CTYPE, "C"}, {LC_NUMERIC, "C"}};
    std::vector<std::pair<int, std::string>> calls;
};

FakeRuntime& fake()
{
    static FakeRuntime instance;
    return instance;
}

const char* fakeSetLocale(int category, const char* name)
{
    if (name == nullptr)
    {
        return fake().current[category].c_str();
    }
    fake().calls.emplace_back(category, name);
    const std::string value = *name == '\0' ? fake().environment : std::string(name);
    if (value != "C" && std::ranges::find(fake().installed, value) == fake().installed.end())
    {
        return nullptr;
    }
    if (category == LC_ALL)
    {
        for (auto& entry : fake().current)
        {
            entry.second = value;
        }
    }
    else
    {
        fake().current[category] = value;
        fake().current[LC_ALL] = "mixed";
    }
    return fake().current[category].c_str();
}

std::string fakeCodeset()
{
    const auto& ctype = fake().current[LC_CTYPE];
    const auto dot = ctype.find('.');
    if (dot != std::string::npos)
    {
        return ctype.substr(dot + 1);
    }
    return ctype == "UTF-8" ? ctype : "ANSI_X3.4-1968";
}

std::string fakeUser()
{
    return fake().user;
}

constexpr motrack::CRuntime FAKE{.setLocale = &fakeSetLocale, .codeset = &fakeCodeset, .userLocaleName = &fakeUser};

class LocaleSelection : public testing::Test
{
  protected:
    void SetUp() override
    {
        fake() = {};
    }
};

TEST_F(LocaleSelection, RecognizesUtf8CodesetSpellings)
{
    for (const auto* codeset : {"UTF-8", "utf8", "UTF_8", "Utf-8"})
    {
        EXPECT_TRUE(motrack::isUtf8Codeset(codeset)) << codeset;
    }
    for (const auto* codeset : {"", "ISO-8859-1", "UTF-16", "utf8x", "ANSI_X3.4-1968", "UTF"})
    {
        EXPECT_FALSE(motrack::isUtf8Codeset(codeset)) << codeset;
    }
}

TEST_F(LocaleSelection, ListsWindowsAndPosixCandidatesInOrder)
{
    EXPECT_EQ(motrack::windowsLocaleCandidates("de-DE"), (std::vector<std::string>{"de-DE.UTF-8", ".UTF-8", "C.UTF-8"}));
    EXPECT_EQ(motrack::windowsLocaleCandidates(""), (std::vector<std::string>{".UTF-8", "C.UTF-8"}));
    const std::vector<std::string> fallbacks{"C.UTF-8", "UTF-8", "en_US.UTF-8"};
    auto german = fallbacks;
    german.insert(german.begin(), "de_DE.UTF-8");
    EXPECT_EQ(motrack::posixCtypeCandidates("de_DE.ISO8859-1"), german);
    EXPECT_EQ(motrack::posixCtypeCandidates("de_DE@euro"), german);
    EXPECT_EQ(motrack::posixCtypeCandidates("C"), fallbacks);
    EXPECT_EQ(motrack::posixCtypeCandidates("POSIX"), fallbacks);
    EXPECT_EQ(motrack::posixCtypeCandidates(""), fallbacks);
}

TEST_F(LocaleSelection, WindowsUsesTheUserLocaleWithUtf8AndClassicNumbers)
{
    fake().user = "de-DE";
    fake().installed = {"de-DE.UTF-8"};
    const auto outcome = motrack::applyWindowsCLocale(FAKE);
    EXPECT_EQ(outcome.chosen, "de-DE.UTF-8");
    EXPECT_TRUE(outcome.warnings.empty());
    EXPECT_EQ(fake().current[LC_CTYPE], "de-DE.UTF-8");
    EXPECT_EQ(fake().current[LC_NUMERIC], "C");
    EXPECT_EQ(fake().calls.back(), (std::pair<int, std::string>{LC_NUMERIC, "C"}));
}

TEST_F(LocaleSelection, WindowsFallsBackAndWarns)
{
    fake().user = "xx-YY";
    fake().installed = {".UTF-8"};
    auto outcome = motrack::applyWindowsCLocale(FAKE);
    EXPECT_EQ(outcome.chosen, ".UTF-8");
    ASSERT_EQ(outcome.warnings.size(), 1u);
    EXPECT_NE(outcome.warnings.front().find("xx-YY.UTF-8"), std::string::npos);
    EXPECT_EQ(fake().current[LC_NUMERIC], "C");

    fake() = {};
    outcome = motrack::applyWindowsCLocale(FAKE);
    EXPECT_TRUE(outcome.chosen.empty());
    ASSERT_EQ(outcome.warnings.size(), 1u);
    EXPECT_NE(outcome.warnings.front().find("no UTF-8 C locale"), std::string::npos);
    EXPECT_EQ(fake().current[LC_NUMERIC], "C");
}

TEST_F(LocaleSelection, PosixKeepsAUtf8EnvironmentButNotItsNumbers)
{
    fake().environment = "de_DE.UTF-8";
    fake().installed = {"de_DE.UTF-8"};
    const auto outcome = motrack::applyPosixCLocale(FAKE);
    EXPECT_EQ(outcome.chosen, "de_DE.UTF-8");
    EXPECT_TRUE(outcome.warnings.empty());
    EXPECT_EQ(fake().current[LC_CTYPE], "de_DE.UTF-8");
    EXPECT_EQ(fake().current[LC_NUMERIC], "C");
}

TEST_F(LocaleSelection, PosixReplacesALegacyCtypeWithUtf8)
{
    fake().environment = "de_DE.ISO8859-1";
    fake().installed = {"de_DE.ISO8859-1", "C.UTF-8"};
    const auto outcome = motrack::applyPosixCLocale(FAKE);
    EXPECT_EQ(outcome.chosen, "C.UTF-8");
    ASSERT_EQ(outcome.warnings.size(), 1u);
    EXPECT_NE(outcome.warnings.front().find("de_DE.ISO8859-1"), std::string::npos);
    EXPECT_EQ(fake().current[LC_CTYPE], "C.UTF-8");
    EXPECT_EQ(fake().current[LC_NUMERIC], "C");
}

TEST_F(LocaleSelection, PosixQuietlyGivesTheDefaultLocaleAUtf8Ctype)
{
    // No LANG or LC_* at all, as for an application started from the macOS Finder.
    fake().installed = {"C.UTF-8"};
    const auto outcome = motrack::applyPosixCLocale(FAKE);
    EXPECT_EQ(outcome.chosen, "C.UTF-8");
    EXPECT_TRUE(outcome.warnings.empty());
    EXPECT_EQ(fake().current[LC_CTYPE], "C.UTF-8");
    EXPECT_EQ(fake().current[LC_NUMERIC], "C");
}

TEST_F(LocaleSelection, PosixWarnsAboutAMissingEnvironmentLocale)
{
    fake().environment = "xx_XX.UTF-8";
    fake().installed = {"UTF-8"};
    const auto outcome = motrack::applyPosixCLocale(FAKE);
    EXPECT_EQ(outcome.chosen, "UTF-8");
    ASSERT_EQ(outcome.warnings.size(), 1u); // replacing the resulting default "C" LC_CTYPE is routine
    EXPECT_NE(outcome.warnings.front().find("not installed"), std::string::npos);
    EXPECT_EQ(fake().current[LC_CTYPE], "UTF-8");

    fake() = {};
    fake().environment = "C";
    const auto none = motrack::applyPosixCLocale(FAKE);
    EXPECT_TRUE(none.chosen.empty());
    ASSERT_EQ(none.warnings.size(), 1u);
    EXPECT_NE(none.warnings.front().find("no UTF-8 locale"), std::string::npos);
    EXPECT_EQ(fake().current[LC_NUMERIC], "C");
}

TEST(ProcessLocale, ResetsTheNumericLocaleTheApplicationObjectSet)
{
#ifndef _WIN32
    // QCoreApplication calls setlocale(LC_ALL, "") on Unix, so LC_NUMERIC followed the comma-decimal environment.
    if (environmentLocale)
    {
        EXPECT_EQ(numericAfterQt, environmentLocale->name);
    }
#endif
    EXPECT_EQ(configured.numericLocale, "C");
    EXPECT_EQ(std::string(std::setlocale(LC_NUMERIC, nullptr)), "C");
    EXPECT_EQ(std::string(std::localeconv()->decimal_point), ".");
    // text-api: allowed (checks the C runtime state that printf("%f") underneath depends on)
    EXPECT_EQ(std::to_string(1.5), "1.500000");
    EXPECT_EQ(configured.cppLocale, "C");
    EXPECT_EQ(std::locale().name(), "C");
    const auto summary = motrack::describe(configured);
    EXPECT_NE(summary.find(R"(LC_NUMERIC="C")"), std::string::npos) << summary;
    EXPECT_NE(summary.find(R"(C++ global="C")"), std::string::npos) << summary;
}

TEST(ProcessLocale, SelectsAUtf8CharacterLocale)
{
#ifdef _WIN32
    const std::string all = std::setlocale(LC_ALL, nullptr);
    const std::string ctype = std::setlocale(LC_CTYPE, nullptr);
    EXPECT_TRUE(ctype.find('.') != std::string::npos && motrack::isUtf8Codeset(ctype.substr(ctype.find('.') + 1))) << ctype;
    EXPECT_NE(configured.activeCodePage, 0u);
    EXPECT_FALSE(all.empty());
#else
    EXPECT_TRUE(motrack::isUtf8Codeset(nl_langinfo(CODESET))) << nl_langinfo(CODESET);
#ifdef __linux__
    // QLocale::system() follows the environment on Linux (macOS reads the user's preferences instead).
    if (environmentLocale)
    {
        EXPECT_EQ(configured.displayLocale, environmentLocale->display.name().toStdString());
    }
#endif
#endif
    EXPECT_EQ(configured.cLocale, std::string(std::setlocale(LC_ALL, nullptr)));
}

TEST(ProcessLocale, ReappliesTheSameSettings)
{
    const auto again = motrack::configureProcessLocale();
    EXPECT_EQ(again.cLocale, configured.cLocale);
    EXPECT_EQ(again.numericLocale, "C");
    EXPECT_EQ(again.warnings, configured.warnings);
}

} // namespace

int main(int argc, char** argv)
try
{
#ifndef _WIN32
    // Start as a German or French desktop session would, so the test sees what QCoreApplication does to the C
    // locale. Without either locale installed the test still checks the setup, but not Qt's side of it.
    for (const auto& [language, region] : std::array{std::pair{"de", "DE"}, std::pair{"fr", "FR"}})
    {
        environmentLocale = motrack::test::availableLocale(language, region);
        if (environmentLocale)
        {
            setenv("LC_ALL", environmentLocale->name.c_str(), 1);
            break;
        }
    }
#endif
    const QCoreApplication application(argc, argv);
    numericAfterQt = std::setlocale(LC_NUMERIC, nullptr);
    configured = motrack::configureProcessLocale();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
catch (const std::exception& error)
{
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
}
