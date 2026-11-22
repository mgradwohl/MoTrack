#include "app_locale.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <QLoggingCategory>
#include <QString>

#include <clocale>
#include <locale>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <array>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <langinfo.h>
#endif

namespace motrack
{
namespace
{

Q_LOGGING_CATEGORY(localeLog, "motrack.locale", QtWarningMsg)

const char* realSetLocale(int category, const char* name)
{
    // text-api: allowed (the process's single locale setup)
    return std::setlocale(category, name);
}

// NOLINTNEXTLINE(modernize-use-string-view) - the CRuntime signature; nl_langinfo reuses its buffer
std::string realCodeset()
{
#ifdef _WIN32
    return {};
#else
    const char* codeset = nl_langinfo(CODESET);
    return codeset != nullptr ? std::string(codeset) : std::string();
#endif
}

// NOLINTNEXTLINE(modernize-use-string-view) - the CRuntime signature; Windows returns a converted string
std::string realUserLocaleName()
{
#ifdef _WIN32
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> name{};
    const auto length = GetUserDefaultLocaleName(name.data(), static_cast<int>(name.size()));
    return length > 1 ? QString::fromWCharArray(name.data(), length - 1).toStdString() : std::string();
#else
    return {};
#endif
}

std::string quoted(const std::vector<std::string>& names)
{
    std::string text;
    for (const auto& name : names)
    {
        text += text.empty() ? "\"" : ", \"";
        text += name;
        text += '"';
    }
    return text;
}

std::string current(const CRuntime& runtime, int category)
{
    const char* name = runtime.setLocale(category, nullptr);
    return name != nullptr ? std::string(name) : std::string();
}

/// The first of @p names setlocale(@p category) accepts, or empty.
std::string setFirst(const CRuntime& runtime, int category, const std::vector<std::string>& names)
{
    for (const auto& name : names)
    {
        if (runtime.setLocale(category, name.c_str()) != nullptr)
        {
            return name;
        }
    }
    return {};
}

void forceClassicNumeric(const CRuntime& runtime, std::vector<std::string>& warnings)
{
    if (runtime.setLocale(LC_NUMERIC, "C") == nullptr)
    {
        warnings.emplace_back(R"(setlocale(LC_NUMERIC, "C") failed)");
    }
}

} // namespace

bool isUtf8Codeset(std::string_view codeset) noexcept
{
    constexpr std::string_view EXPECTED = "utf8";
    std::size_t matched = 0;
    for (auto character : codeset)
    {
        if (character == '-' || character == '_')
        {
            continue;
        }
        if (character >= 'A' && character <= 'Z')
        {
            character = static_cast<char>(character - 'A' + 'a');
        }
        if (matched == EXPECTED.size() || character != EXPECTED[matched])
        {
            return false;
        }
        ++matched;
    }
    return matched == EXPECTED.size();
}

std::vector<std::string> windowsLocaleCandidates(std::string_view userLocaleName)
{
    std::vector<std::string> names;
    if (!userLocaleName.empty())
    {
        names.push_back(std::string(userLocaleName) + ".UTF-8");
    }
    names.emplace_back(".UTF-8");
    names.emplace_back("C.UTF-8");
    return names;
}

std::vector<std::string> posixCtypeCandidates(std::string_view current)
{
    std::vector<std::string> names;
    // "language_TERRITORY.codeset@modifier": keep language_TERRITORY.
    const auto language = current.substr(0, current.find_first_of(".@"));
    if (!language.empty() && language != "C" && language != "POSIX")
    {
        names.push_back(std::string(language) + ".UTF-8");
    }
    names.emplace_back("C.UTF-8");
    names.emplace_back("UTF-8"); // macOS's codeset-only LC_CTYPE locale
    names.emplace_back("en_US.UTF-8");
    return names;
}

CLocaleOutcome applyWindowsCLocale(const CRuntime& runtime)
{
    CLocaleOutcome outcome;
    const auto names = windowsLocaleCandidates(runtime.userLocaleName());
    outcome.chosen = setFirst(runtime, LC_ALL, names);
    if (outcome.chosen.empty())
    {
        outcome.warnings.push_back("no UTF-8 C locale is available (tried " + quoted(names) + ")");
    }
    else if (outcome.chosen != names.front())
    {
        outcome.warnings.push_back("C locale \"" + names.front() + "\" is not available; using \"" + outcome.chosen + "\"");
    }
    forceClassicNumeric(runtime, outcome.warnings);
    return outcome;
}

CLocaleOutcome applyPosixCLocale(const CRuntime& runtime)
{
    CLocaleOutcome outcome;
    if (runtime.setLocale(LC_ALL, "") == nullptr)
    {
        outcome.warnings.emplace_back(R"(setlocale(LC_ALL, "") failed: the environment names a locale that is not installed)");
    }
    const auto ctype = current(runtime, LC_CTYPE);
    if (isUtf8Codeset(runtime.codeset()))
    {
        outcome.chosen = ctype;
    }
    else
    {
        const auto names = posixCtypeCandidates(ctype);
        outcome.chosen = setFirst(runtime, LC_CTYPE, names);
        if (outcome.chosen.empty())
        {
            outcome.warnings.push_back("LC_CTYPE \"" + ctype + "\" is not UTF-8 and no UTF-8 locale is available (tried " + quoted(names) +
                                       ")");
        }
        else if (ctype != "C" && ctype != "POSIX")
        {
            // Replacing the default "C" (no locale configured, as for an app started from Finder) is routine.
            outcome.warnings.push_back("LC_CTYPE \"" + ctype + "\" is not UTF-8; using \"" + outcome.chosen + "\"");
        }
    }
    forceClassicNumeric(runtime, outcome.warnings);
    return outcome;
}

ProcessLocale configureProcessLocale()
{
    const CRuntime runtime{.setLocale = &realSetLocale, .codeset = &realCodeset, .userLocaleName = &realUserLocaleName};
    ProcessLocale result;
    if (QCoreApplication::instance() == nullptr)
    {
        result.warnings.emplace_back("the locale was configured before the Q*Application, which resets it on Unix");
    }
#ifdef _WIN32
    auto outcome = applyWindowsCLocale(runtime);
    result.activeCodePage = GetACP();
    if (result.activeCodePage != CP_UTF8)
    {
        result.warnings.push_back("the process code page is " + std::to_string(result.activeCodePage) +
                                  ", not UTF-8 (65001): the manifest's activeCodePage is missing or Windows is older than 10 "
                                  "version 1903; narrow text APIs use that code page");
    }
#else
    auto outcome = applyPosixCLocale(runtime);
#endif
    result.warnings.insert(result.warnings.begin(), outcome.warnings.begin(), outcome.warnings.end());
    result.cLocale = current(runtime, LC_ALL);
    result.numericLocale = current(runtime, LC_NUMERIC);
    result.cppLocale = std::locale().name();
    if (result.cppLocale != "C")
    {
        result.warnings.push_back("the C++ global locale is \"" + result.cppLocale + "\", not the classic locale");
    }
    result.displayLocale = QLocale().name().toStdString();
    for (const auto& warning : result.warnings)
    {
        qCWarning(localeLog).noquote() << QString::fromStdString(warning);
    }
    qCInfo(localeLog).noquote() << QString::fromStdString(describe(result));
    return result;
}

std::string describe(const ProcessLocale& locale)
{
    return "Locale: GetACP()=" + (locale.activeCodePage != 0 ? std::to_string(locale.activeCodePage) : std::string("n/a")) + " C=\"" +
           locale.cLocale + "\" LC_NUMERIC=\"" + locale.numericLocale + "\" C++ global=\"" + locale.cppLocale + "\" display=\"" +
           locale.displayLocale + "\"";
}

} // namespace motrack
