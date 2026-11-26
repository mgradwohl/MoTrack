#include "number_input.hpp"

#include "display_locale.hpp"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>

#include <algorithm>
#include <cmath>

namespace motrack
{
namespace
{

class NumberInput
{
    Q_DECLARE_TR_FUNCTIONS(NumberInput)
};

bool isSpace(QChar c)
{
    return c == u' ' || c == QChar(0x00A0) || c == QChar(0x202F);
}

bool isApostrophe(QChar c)
{
    return c == u'\'' || c == QChar(0x2019);
}

/// Whether @p c separates digit groups for a locale whose separator is @p group: the separator itself or, for a space
/// or an apostrophe, a look-alike that is easier to type (U+00A0 or a space for French formats' U+202F, ' for Swiss
/// formats' U+2019).
bool isGroupMark(QChar c, QChar group)
{
    return c == group || (isSpace(group) && isSpace(c)) || (isApostrophe(group) && isApostrophe(c));
}

bool allDigits(QStringView text)
{
    return !text.isEmpty() && std::ranges::all_of(text, [](QChar c) { return c.isDigit(); });
}

std::optional<double> finiteNumber(const QLocale& locale, const QString& text)
{
    bool read = false;
    const auto value = locale.toDouble(text, &read);
    return read && std::isfinite(value) ? std::optional(value) : std::nullopt;
}

/// Where the exponent of @p text begins: its first ASCII e or E, or @p exponent (the locale's symbol), after the first
/// character; text.size() when it has none.
qsizetype exponentStart(const QString& text, const QString& exponent)
{
    for (qsizetype at = 1; at < text.size(); ++at)
    {
        if (text.at(at) == u'e' || text.at(at) == u'E' ||
            (!exponent.isEmpty() && QStringView(text).sliced(at).startsWith(exponent, Qt::CaseInsensitive)))
        {
            return at;
        }
    }
    return text.size();
}

/// Rule 1 of parseUserNumber(): @p mantissa's only separator is a "." or "," other than @p decimal, between a digit and
/// exactly three digits.
bool readsTwoWays(QStringView mantissa, QChar decimal, QChar group)
{
    qsizetype found = -1;
    for (qsizetype at = 0; at < mantissa.size(); ++at)
    {
        const auto c = mantissa.at(at);
        if (c == u'.' || c == u',' || c == decimal || isGroupMark(c, group))
        {
            if (found != -1)
            {
                return false;
            }
            found = at;
        }
    }
    if (found <= 0)
    {
        return false;
    }
    const auto mark = mantissa.at(found);
    return (mark == u'.' || mark == u',') && mark != decimal && mantissa.at(found - 1).isDigit() && mantissa.size() - found - 1 == 3 &&
           allDigits(mantissa.sliced(found + 1));
}

/// Rule 2's grouping: @p text without its group marks when they are well formed in the integer part of its first
/// @p mantissaLength characters (groups of three after a first group of one to three, before @p decimal), otherwise
/// nullopt. A lone "." or "," without a decimal mark is not grouping: rule 1 or the classic form reads it.
std::optional<QString> withoutGrouping(const QString& text, qsizetype mantissaLength, QChar decimal, QChar group)
{
    const auto mantissa = QStringView(text).first(mantissaLength);
    const auto decimalAt = mantissa.indexOf(decimal);
    const auto integerEnd = decimalAt == -1 ? mantissaLength : decimalAt;
    qsizetype marks = 0;
    for (qsizetype at = 0; at < mantissaLength; ++at)
    {
        if (isGroupMark(mantissa.at(at), group))
        {
            if (at >= integerEnd)
            {
                return std::nullopt;
            }
            ++marks;
        }
    }
    if (marks == 1 && decimalAt == -1 && (group == u'.' || group == u','))
    {
        return std::nullopt;
    }
    qsizetype start = 0; // after any sign
    while (start < integerEnd && !mantissa.at(start).isDigit() && !isGroupMark(mantissa.at(start), group))
    {
        ++start;
    }
    qsizetype groupStart = start;
    for (qsizetype at = start; at <= integerEnd; ++at)
    {
        if (at == integerEnd || isGroupMark(mantissa.at(at), group))
        {
            const auto digits = mantissa.sliced(groupStart, at - groupStart);
            if (!allDigits(digits) || (groupStart == start ? digits.size() > 3 : digits.size() != 3))
            {
                return std::nullopt;
            }
            groupStart = at + 1;
        }
    }
    QString plain;
    plain.reserve(text.size());
    for (qsizetype at = 0; at < text.size(); ++at)
    {
        if (at >= integerEnd || !isGroupMark(text.at(at), group))
        {
            plain.append(text.at(at));
        }
    }
    return plain;
}

/// Rule 2 of parseUserNumber(), after rule 1 found @p text unambiguous.
std::optional<double> regionalNumber(const QString& text, qsizetype exponentAt, const QLocale& locale, QChar decimal, QChar group)
{
    auto ungrouped = locale;
    ungrouped.setNumberOptions(locale.numberOptions() | QLocale::RejectGroupSeparator);
    auto local = text;
    const auto exponent = locale.exponential();
    if (exponentAt < text.size() && (text.at(exponentAt) == u'e' || text.at(exponentAt) == u'E') &&
        exponent.compare(u"e", Qt::CaseInsensitive) != 0)
    {
        local = text.first(exponentAt) + exponent + text.sliced(exponentAt + 1);
    }
    const auto mantissa = QStringView(local).first(exponentAt);
    if (!std::ranges::any_of(mantissa, [group](QChar c) { return isGroupMark(c, group); }))
    {
        return finiteNumber(ungrouped, local);
    }
    if (const auto plain = withoutGrouping(local, exponentAt, decimal, group))
    {
        return finiteNumber(ungrouped, *plain);
    }
    return finiteNumber(locale, local); // another grouping style, such as Indian 12,34,567
}

} // namespace

std::expected<double, NumberInputError> parseUserNumber(const QString& text, const QLocale& locale)
{
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty())
    {
        return std::unexpected(NumberInputError::Invalid);
    }
    const auto decimalMark = locale.decimalPoint();
    const auto groupMark = locale.groupSeparator();
    std::optional<double> regional;
    if (decimalMark.size() == 1 && groupMark.size() <= 1)
    {
        const auto decimal = decimalMark.front();
        const auto group = groupMark.isEmpty() ? QChar() : groupMark.front();
        const auto exponentAt = exponentStart(trimmed, locale.exponential());
        if (readsTwoWays(QStringView(trimmed).first(exponentAt), decimal, group))
        {
            return std::unexpected(NumberInputError::Ambiguous);
        }
        regional = regionalNumber(trimmed, exponentAt, locale, decimal, group);
    }
    else
    {
        regional = finiteNumber(locale, trimmed); // marks of several characters: QLocale's own reading
    }
    // Rule 3. QString::toDouble reads the classic form without digit grouping, whatever the process locale.
    bool read = false;
    const auto classicValue = trimmed.toDouble(&read);
    const auto classic = read && std::isfinite(classicValue) ? std::optional(classicValue) : std::nullopt;
    if (regional && classic && *regional != *classic)
    {
        return std::unexpected(NumberInputError::Ambiguous);
    }
    if (regional)
    {
        return *regional;
    }
    if (classic)
    {
        return *classic;
    }
    return std::unexpected(NumberInputError::Invalid);
}

QString ambiguousNumberMessage(const QLocale& locale)
{
    return NumberInput::tr("This number can be read two ways. Type it without digit grouping, with “%1” as the decimal mark.")
        .arg(locale.decimalPoint());
}

QString invalidNumberMessage(const QLocale& locale)
{
    auto plain = locale;
    plain.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    return NumberInput::tr("This is not a number. Type digits with “%1” as the decimal mark, such as %2.")
        .arg(locale.decimalPoint(), plain.toString(1234.5));
}
QString editableNumber(double value)
{
    return dataDisplayLocale().toString(value, 'g', 15);
}

NumberValidator::NumberValidator(double minimum, double maximum, QObject* parent)
    : QValidator(parent), m_Minimum(minimum), m_Maximum(maximum)
{}

QValidator::State NumberValidator::validate(QString& input, int& /*position*/) const
{
    const auto number = parseUserNumber(input, locale());
    return number && *number >= m_Minimum && *number <= m_Maximum ? Acceptable : Intermediate;
}

void requireNumber(QLineEdit& field, double minimum, double maximum, QDialogButtonBox& buttons, QLabel& hint, const QString& rangeHint)
{
    auto* validator = new NumberValidator(minimum, maximum, &field);
    validator->setLocale(field.locale());
    field.setValidator(validator);
    hint.setWordWrap(true);
    const auto outside = rangeHint.isEmpty()
                             ? NumberInput::tr("Enter a number from %1 to %2.").arg(editableNumber(minimum), editableNumber(maximum))
                             : rangeHint;
    const auto update = [&field, &buttons, &hint, outside]
    {
        const bool acceptable = field.hasAcceptableInput();
        if (auto* accept = buttons.button(QDialogButtonBox::Ok))
        {
            accept->setEnabled(acceptable);
        }
        QString problem;
        if (!acceptable && !field.text().trimmed().isEmpty())
        {
            const auto number = parseUserNumber(field.text(), field.locale());
            problem = number                                          ? outside
                      : number.error() == NumberInputError::Ambiguous ? ambiguousNumberMessage(field.locale())
                                                                      : invalidNumberMessage(field.locale());
        }
        hint.setText(problem);
        hint.setVisible(!problem.isEmpty());
    };
    QObject::connect(&field, &QLineEdit::textChanged, &hint, update);
    update();
}

std::optional<double> askNumber(
    QWidget* parent, const QString& title, const QString& label, double value, double minimum, double maximum, const QString& rangeHint)
{
    // Owned here; a parent destroyed while the dialog runs deletes it first, and the pointer then reads null.
    auto* window = new QDialog(parent);
    const QPointer<QDialog> dialog = window;
    const auto cleanup = qScopeGuard([&dialog] { delete dialog.data(); });
    window->setObjectName("numberDialog");
    window->setWindowTitle(title);
    auto* layout = new QFormLayout(window);
    auto* field = new QLineEdit(editableNumber(value));
    field->setObjectName("numberInput");
    auto* hint = new QLabel;
    hint->setObjectName("numberHint");
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(label, field);
    layout->addRow(hint);
    layout->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, window, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, window, &QDialog::reject);
    requireNumber(*field, minimum, maximum, *buttons, *hint, rangeHint);
    field->selectAll();
    const auto result = window->exec();
    if (!dialog || result != QDialog::Accepted)
    {
        return std::nullopt;
    }
    // Accepting bypasses the disabled OK button when done programmatically, so check the text again.
    const auto number = parseUserNumber(field->text(), field->locale());
    if (!number || *number < minimum || *number > maximum)
    {
        return std::nullopt;
    }
    return *number;
}

} // namespace motrack
