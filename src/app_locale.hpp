#pragma once

// The process's C locale, set once in each executable right after its Q*Application is constructed
// (CONTRIBUTING.md, "Text encoding and locale"):
//
//   Windows  setlocale(LC_ALL, "<GetUserDefaultLocaleName()>.UTF-8"), else ".UTF-8", else "C.UTF-8"
//   POSIX    setlocale(LC_ALL, "") as QCoreApplication already did; when LC_CTYPE's codeset is not UTF-8,
//            LC_CTYPE becomes the same language with ".UTF-8", else "C.UTF-8", "UTF-8" or "en_US.UTF-8"
//   Both     then setlocale(LC_NUMERIC, "C")
//
// On Unix QCoreApplication calls setlocale(LC_ALL, ""), so until this runs the C runtime parses and prints
// numbers with the user's decimal mark: strtod, printf("%f"), std::to_string(double) and the C libraries
// MoTrack links (FFmpeg parses numbers with strtod). LC_NUMERIC "C" keeps all of them classic. The C++ global
// locale stays the classic locale and is never replaced: MoTrack takes display formats from QLocale, and a
// default-constructed stream must stay locale-independent.
//
// Nothing else may call setlocale (except to query it) or std::locale::global; tools/check-text-apis.py fails
// CI otherwise. The selection takes its C runtime calls through CRuntime so tests can check the fallback
// order without changing the test process's locale.

#include <string>
#include <string_view>
#include <vector>

namespace motrack
{

/// The C runtime calls the C-locale selection makes.
struct CRuntime
{
    /// setlocale(): the locale's name on success, nullptr when @p name is unavailable or when querying fails.
    const char* (*setLocale)(int category, const char* name) = nullptr;
    /// nl_langinfo(CODESET) for the current LC_CTYPE, for example "UTF-8". POSIX only.
    std::string (*codeset)() = nullptr;
    /// GetUserDefaultLocaleName() as UTF-8, for example "de-DE"; empty when unknown. Windows only.
    std::string (*userLocaleName)() = nullptr;
};

/// What a C-locale selection did: the name setlocale accepted (empty when none was) and anything worth a warning.
struct CLocaleOutcome
{
    std::string chosen;
    std::vector<std::string> warnings;
};

/// Whether @p codeset names UTF-8 ("UTF-8", "utf8", in any case).
[[nodiscard]] bool isUtf8Codeset(std::string_view codeset) noexcept;

/// The LC_ALL names to try on Windows, in order: "<userLocaleName>.UTF-8" (when not empty), ".UTF-8", "C.UTF-8".
[[nodiscard]] std::vector<std::string> windowsLocaleCandidates(std::string_view userLocaleName);

/// The LC_CTYPE names to try on POSIX when @p current (setlocale's LC_CTYPE name, such as "de_DE.ISO8859-1") is
/// not UTF-8: the same language with ".UTF-8" (not for "C"/"POSIX"), then "C.UTF-8", "UTF-8" and "en_US.UTF-8".
[[nodiscard]] std::vector<std::string> posixCtypeCandidates(std::string_view current);

/// Sets the Windows C locale through @p runtime: LC_ALL from windowsLocaleCandidates(), then LC_NUMERIC "C".
[[nodiscard]] CLocaleOutcome applyWindowsCLocale(const CRuntime& runtime);

/// Sets the POSIX C locale through @p runtime: LC_ALL "", LC_CTYPE from posixCtypeCandidates() when its codeset
/// is not UTF-8, then LC_NUMERIC "C". chosen is the resulting LC_CTYPE name.
[[nodiscard]] CLocaleOutcome applyPosixCLocale(const CRuntime& runtime);

/// The locale state after configureProcessLocale(), for the diagnostic log line.
struct ProcessLocale
{
    std::string cLocale;         ///< setlocale(LC_ALL, nullptr)
    std::string numericLocale;   ///< setlocale(LC_NUMERIC, nullptr); "C" on success
    std::string cppLocale;       ///< std::locale().name(); "C", never replaced
    std::string displayLocale;   ///< QLocale().name(), which displayed numbers follow
    unsigned activeCodePage = 0; ///< GetACP() on Windows (65001 with the application manifest); 0 elsewhere
    std::vector<std::string> warnings;
};

/// Sets the C locale as described above and logs the result to the "motrack.locale" category (its warnings at
/// QtWarningMsg; the summary at QtInfoMsg, enabled with QT_LOGGING_RULES="motrack.locale.info=true"). Call it on
/// the GUI thread immediately after constructing the Q*Application, before starting any other thread: setlocale is
/// not thread-safe. Calling it again re-applies the same settings.
ProcessLocale configureProcessLocale();

/// The one-line summary configureProcessLocale() logs.
[[nodiscard]] std::string describe(const ProcessLocale& locale);

} // namespace motrack
