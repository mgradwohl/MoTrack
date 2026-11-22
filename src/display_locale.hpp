#pragma once

#include <QLocale>

namespace motrack
{

/// The locale for numbers MoTrack shows in data views (the marks table, plot axes): the user's QLocale, so its
/// decimal mark and digits, without digit grouping, as Tracker's fixed DecimalFormat patterns show data. Never use
/// QString::number or the C/C++ global locale for a number the user reads (CONTRIBUTING.md, "Text encoding and
/// locale"); machine-readable text uses classic formatting instead.
[[nodiscard]] inline QLocale dataDisplayLocale()
{
    QLocale locale;
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    return locale;
}

} // namespace motrack
