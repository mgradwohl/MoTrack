#pragma once

// Numbers the user types or pastes (CONTRIBUTING.md, "Text encoding and locale"; #278, #66). A field shows its
// number with editableNumber() and reads it back with parseUserNumber(): the user's regional format, or the classic
// form that CSV exports, scripts and Tracker files use. Text that two readers could take for different numbers is
// refused, never guessed, and a dialog cannot be accepted until its field holds a number.

#include <QLocale>
#include <QString>
#include <QValidator>

#include <cstdint>
#include <expected>
#include <optional>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QWidget;

namespace motrack
{

/// Why parseUserNumber() refused a text.
enum class NumberInputError : std::uint8_t
{
    Invalid,   ///< not a finite number in the regional or the classic form
    Ambiguous, ///< could be read as two different numbers
};

/// Reads a number the user typed or pasted, by these rules, in order (D is @p locale's decimal mark, G its digit
/// group separator; "." and "," are the separators readers confuse):
///
/// 1. Ambiguous: the number before any exponent has exactly one separator, a "." or "," that is not D, between a
///    digit and exactly three digits. One reader groups digits with it, another takes it for a decimal mark:
///    "1.234", "2.500", "0.125" and "1.234e3" with German, Spanish or French formats; "1,234" and "0,125" with
///    English ones.
/// 2. The regional form: D as the decimal mark; G (or, for a space or an apostrophe, a look-alike of it) between
///    groups of three digits after a first group of one to three, before D. Grouping is decided here, not by the
///    locale's minimum grouping digits, so "1.234,5" and "1.234.567" read the same with Spanish formats as with
///    German ones. Other grouping styles (Indian "12,34,567") are left to QLocale. An ASCII "e" or "E" exponent is
///    accepted where the locale writes another one ("1,5e3" with Swedish formats).
/// 3. The classic form, never grouped: "2.5" pasted from a CSV export is 2.5 with German formats, where
///    QDoubleSpinBox drops the "." as a group separator and reads 25.
/// 4. Both forms read it, differently: Ambiguous. Neither: Invalid ("1,5" with English formats, "1,234.5" with
///    German ones).
///
/// White space around the number is ignored; infinities and NaN are Invalid.
[[nodiscard]] std::expected<double, NumberInputError> parseUserNumber(const QString& text, const QLocale& locale = QLocale());

/// The hint for an Ambiguous number, naming @p locale's decimal mark.
[[nodiscard]] QString ambiguousNumberMessage(const QLocale& locale = QLocale());

/// The hint for Invalid text, with @p locale's decimal mark and an example.
[[nodiscard]] QString invalidNumberMessage(const QLocale& locale = QLocale());

/// @p value as an editable field shows it: dataDisplayLocale() with up to 15 significant digits. Its only separator is
/// D and it has no grouping, so parseUserNumber() reads it back without rule 1 applying.
[[nodiscard]] QString editableNumber(double value);

/// Acceptable when parseUserNumber() reads the text as a number from minimum to maximum, otherwise Intermediate: the
/// user can always keep typing, and the field never changes or reverts what was typed.
class NumberValidator final : public QValidator
{
  public:
    NumberValidator(double minimum, double maximum, QObject* parent = nullptr);
    State validate(QString& input, int& position) const override;

  private:
    double m_Minimum;
    double m_Maximum;
};

/// Gives @p field a NumberValidator and enables @p buttons' OK button only while the field is acceptable; @p hint
/// says why it is not: an ambiguous or invalid number, or @p rangeHint for one outside the range ("Enter a number
/// from minimum to maximum." when empty). All four must belong to the same dialog.
void requireNumber(
    QLineEdit& field, double minimum, double maximum, QDialogButtonBox& buttons, QLabel& hint, const QString& rangeHint = {});

/// Asks for a number from @p minimum to @p maximum in a modal dialog (object name "numberDialog", field
/// "numberInput", hint "numberHint") showing @p value; nullopt when cancelled or when @p parent is destroyed
/// meanwhile. @p rangeHint is as for requireNumber().
[[nodiscard]] std::optional<double> askNumber(QWidget* parent,
                                              const QString& title,
                                              const QString& label,
                                              double value,
                                              double minimum,
                                              double maximum,
                                              const QString& rangeHint = {});

} // namespace motrack
