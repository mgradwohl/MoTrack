#pragma once

// Comma-decimal locales for locale tests (CONTRIBUTING.md, "Text encoding and locale"). A test skips a locale the
// machine lacks, as Ubuntu images without generated locales do; CI generates de_DE.UTF-8 and fr_FR.UTF-8.

#include <QLocale>

#include <clocale>
#include <exception>
#include <locale>
#include <optional>
#include <string>
#include <string_view>

namespace motrack::test
{

/// A locale under test: its C/C++ name on this OS and the matching QLocale.
struct TestLocale
{
    std::string name; ///< "de-DE.UTF-8" on Windows, "de_DE.UTF-8" elsewhere
    QLocale display;
};

/// @p language-@p region with a UTF-8 codeset, or nullopt when the C++ runtime cannot construct that locale.
[[nodiscard]] inline std::optional<TestLocale> availableLocale(std::string_view language, std::string_view region)
{
#ifdef _WIN32
    constexpr char SEPARATOR = '-';
#else
    constexpr char SEPARATOR = '_';
#endif
    auto name = std::string(language) + SEPARATOR + std::string(region) + ".UTF-8";
    try
    {
        static_cast<void>(std::locale(name));
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    return TestLocale{.name = std::move(name),
                      .display = QLocale(QString::fromLatin1(language.data(), static_cast<qsizetype>(language.size())) + '_' +
                                         QString::fromLatin1(region.data(), static_cast<qsizetype>(region.size())))};
}

/// Puts the whole process in @p locale for one scope: the C locale (LC_ALL, so LC_NUMERIC too), the C++ global
/// locale and QLocale's default. This is the worst case configureProcessLocale() prevents; machine-readable output
/// must stay classic even then. Restores all three on destruction. Not thread-safe, like setlocale.
class ScopedProcessLocale
{
  public:
    explicit ScopedProcessLocale(const TestLocale& locale) : m_CLocale(std::setlocale(LC_ALL, nullptr))
    {
        // std::locale::global() also sets the C locale for a named locale.
        std::locale::global(std::locale(locale.name)); // text-api: allowed (test simulation)
        QLocale::setDefault(locale.display);
    }

    ~ScopedProcessLocale()
    {
        QLocale::setDefault(m_Display);
        std::locale::global(m_CppLocale);          // text-api: allowed (test simulation)
        std::setlocale(LC_ALL, m_CLocale.c_str()); // text-api: allowed (test simulation)
    }

    ScopedProcessLocale(const ScopedProcessLocale&) = delete;
    ScopedProcessLocale& operator=(const ScopedProcessLocale&) = delete;

  private:
    std::string m_CLocale;
    std::locale m_CppLocale;
    QLocale m_Display;
};

} // namespace motrack::test
