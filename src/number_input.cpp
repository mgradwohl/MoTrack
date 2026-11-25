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

#include <cmath>

namespace motrack
{
namespace
{

class NumberInput
{
    Q_DECLARE_TR_FUNCTIONS(NumberInput)
};

} // namespace

std::expected<double, NumberInputError> parseUserNumber(const QString& text, const QLocale& locale)
{
    const auto trimmed = text.trimmed();
    bool regional = false;
    const auto regionalValue = locale.toDouble(trimmed, &regional);
    // QString::toDouble reads the classic form without digit grouping, whatever the process locale.
    bool classic = false;
    const auto classicValue = trimmed.toDouble(&classic);
    regional = regional && std::isfinite(regionalValue);
    classic = classic && std::isfinite(classicValue);
    if (regional && classic && regionalValue != classicValue)
    {
        return std::unexpected(NumberInputError::Ambiguous);
    }
    if (regional)
    {
        return regionalValue;
    }
    if (classic)
    {
        return classicValue;
    }
    return std::unexpected(NumberInputError::Invalid);
}

QString ambiguousNumberMessage(const QLocale& locale)
{
    return NumberInput::tr("This number can be read two ways. Type it without digit grouping, with “%1” as the decimal mark.")
        .arg(locale.decimalPoint());
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

void requireNumber(QLineEdit& field, double minimum, double maximum, QDialogButtonBox& buttons, QLabel& hint)
{
    auto* validator = new NumberValidator(minimum, maximum, &field);
    validator->setLocale(field.locale());
    field.setValidator(validator);
    hint.setWordWrap(true);
    const auto update = [&field, &buttons, &hint, minimum, maximum]
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
            problem = !number && number.error() == NumberInputError::Ambiguous
                          ? ambiguousNumberMessage(field.locale())
                          : NumberInput::tr("Enter a number from %1 to %2.").arg(editableNumber(minimum), editableNumber(maximum));
        }
        hint.setText(problem);
        hint.setVisible(!problem.isEmpty());
    };
    QObject::connect(&field, &QLineEdit::textChanged, &hint, update);
    update();
}

std::optional<double> askNumber(QWidget* parent, const QString& title, const QString& label, double value, double minimum, double maximum)
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
    requireNumber(*field, minimum, maximum, *buttons, *hint);
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
