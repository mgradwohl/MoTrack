#pragma once

// Numbers the user types or pastes (CONTRIBUTING.md, "Text encoding and locale"; #278, #66). A field shows its
// number with dataDisplayLocale() and reads it back with parseUserNumber(): the user's regional format first, then
// the classic form that CSV exports, scripts and Tracker files use. Text that reads as two different numbers is
// refused, never guessed, and a dialog cannot be accepted until its field holds a number.

#include <QLocale>
#include <QString>
#include <QValidator>

#include <expected>
#include <optional>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QWidget;

namespace motrack
{

/// Why parseUserNumber() refused a text.
enum class NumberInputError
{
    Invalid,   ///< neither form reads it as a finite number
    Ambiguous, ///< both forms read it, as different numbers
};

/// Reads a number the user typed or pasted. @p locale's form comes first, with its decimal mark and, optionally, its
/// digit grouping ("1.234,5" with German formats). The classic form ("1234.5", never grouped) is also accepted, so
/// "2.5" pasted from a CSV file is 2.5 with German formats, where QDoubleSpinBox drops the "." as a group separator
/// and reads 25. When both forms read the text and disagree, as for "1.234" with German formats (1234 grouped,
/// 1.234 classic), the result is Ambiguous. White space around the number is ignored; infinities and NaN are
/// Invalid.
[[nodiscard]] std::expected<double, NumberInputError> parseUserNumber(const QString& text, const QLocale& locale = QLocale());

/// The status text for an Ambiguous number, naming @p locale's decimal mark.
[[nodiscard]] QString ambiguousNumberMessage(const QLocale& locale = QLocale());

/// @p value as an editable field shows it: dataDisplayLocale() with up to 15 significant digits. It has no digit
/// grouping, so it never reads as ambiguous, and parseUserNumber() reads it back.
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
/// says why it is not (an ambiguous number, or the accepted range). All three must belong to the same dialog.
void requireNumber(QLineEdit& field, double minimum, double maximum, QDialogButtonBox& buttons, QLabel& hint);

/// Asks for a number from @p minimum to @p maximum in a modal dialog (object name "numberDialog", field
/// "numberInput") showing @p value; nullopt when cancelled or when @p parent is destroyed meanwhile.
[[nodiscard]] std::optional<double>
askNumber(QWidget* parent, const QString& title, const QString& label, double value, double minimum, double maximum);

} // namespace motrack
