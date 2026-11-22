#include "model/codecs.hpp"

#include <QChar>
#include <QList>
#include <QString>

#include <algorithm>
#include <optional>

namespace motrack::model
{
namespace
{

/// The UTF-16 form of valid UTF-8 text, or nothing when `text` is not valid UTF-8 (the round trip must be exact).
std::optional<QString> utf16(std::string_view text)
{
    auto converted = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    if (converted.toStdString() != text)
    {
        return std::nullopt;
    }
    return converted;
}

bool isControlOrFormat(char32_t codePoint)
{
    const auto category = QChar::category(codePoint);
    return category == QChar::Other_Control || category == QChar::Other_Format;
}

} // namespace

bool isTrackerUnitLabel(std::string_view unit)
{
    const auto label = utf16(unit);
    if (!label || label->isEmpty() || label->size() > 256 || *label != label->trimmed())
    {
        return false;
    }
    return std::ranges::none_of(label->toUcs4(),
                                [](char32_t codePoint) { return isControlOrFormat(codePoint) || QChar::isDigit(codePoint); });
}

bool isTrackerTrackName(std::string_view name)
{
    const auto text = utf16(name);
    if (!text || text->isEmpty() || text->size() > 4096 || *text != text->trimmed() || *text == QStringLiteral("null") ||
        text->contains(u',') || text->contains(u';') || text->contains(QStringLiteral("]]>")))
    {
        return false;
    }
    return std::ranges::none_of(text->toUcs4(), isControlOrFormat);
}

} // namespace motrack::model
