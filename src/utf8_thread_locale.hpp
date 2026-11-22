#pragma once

// On POSIX, libarchive converts ZIP member names through the calling thread's C character locale, even for the
// archive_entry_*_utf8 functions and with the zip:hdrcharset=UTF-8 option. MoTrack's libarchive is built without
// iconv, so in the "C" locale (every process before configureProcessLocale(), and every test binary) reading
// refuses Unicode names and the writers' hdrcharset option fails outright. ScopedUtf8ThreadLocale gives the
// calling thread a UTF-8 LC_CTYPE for one archive operation and restores the caller's thread locale afterwards,
// on success or failure; the process's global locale is never changed. On Windows libarchive converts through
// UTF-16 and the class does nothing. See the Apple thread-locale contract:
// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/uselocale.3.html

#include "legacy_project.hpp"

#ifndef _WIN32
#include <clocale>

#include <locale.h> // NOLINT(modernize-deprecated-headers) - newlocale/uselocale/freelocale are POSIX, not in <clocale>
#ifdef __APPLE__
#include <xlocale.h>
#endif
#endif

namespace motrack
{

/// A UTF-8 LC_CTYPE on the calling thread for the lifetime of the object (POSIX); nothing on Windows.
/// Throws ProjectError when no UTF-8 locale exists. Construct it on the thread that calls libarchive.
class ScopedUtf8ThreadLocale
{
  public:
    ScopedUtf8ThreadLocale()
    {
#ifndef _WIN32
        for (const char* name : {"C.UTF-8", "UTF-8", "en_US.UTF-8"})
        {
            m_Locale = newlocale(LC_CTYPE_MASK, name, static_cast<locale_t>(nullptr));
            if (m_Locale != static_cast<locale_t>(nullptr))
            {
                break;
            }
        }
        if (m_Locale == static_cast<locale_t>(nullptr))
        {
            throw ProjectError("Could not create a UTF-8 locale for archive member names.");
        }
        m_Previous = uselocale(m_Locale);
        if (m_Previous == static_cast<locale_t>(nullptr))
        {
            freelocale(m_Locale);
            throw ProjectError("Could not select a UTF-8 locale for archive member names.");
        }
#endif
    }

    ~ScopedUtf8ThreadLocale()
    {
#ifndef _WIN32
        static_cast<void>(uselocale(m_Previous));
        freelocale(m_Locale);
#endif
    }

    ScopedUtf8ThreadLocale(const ScopedUtf8ThreadLocale&) = delete;
    ScopedUtf8ThreadLocale& operator=(const ScopedUtf8ThreadLocale&) = delete;
    ScopedUtf8ThreadLocale(ScopedUtf8ThreadLocale&&) = delete;
    ScopedUtf8ThreadLocale& operator=(ScopedUtf8ThreadLocale&&) = delete;

  private:
#ifndef _WIN32
    locale_t m_Locale = static_cast<locale_t>(nullptr);
    locale_t m_Previous = static_cast<locale_t>(nullptr);
#endif
};

} // namespace motrack
