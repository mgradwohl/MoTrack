// Numbers in the workspace follow the user's regional format, and nothing saved or exported does (#278;
// CONTRIBUTING.md, "Text encoding and locale"). The marks table and plot axes use dataDisplayLocale() (no digit
// grouping, as Tracker's fixed patterns), read-outs QLocale() (grouped); number fields read the regional form and the
// classic one and refuse text that reads as two numbers (#66). Each case switches QLocale's default and, where the
// machine has the locale, the C and C++ locales too: the worst case configureProcessLocale() prevents.

#include "filesystem_path.hpp"
#include "legacy_project.hpp"
#include "main_window.hpp"
#include "number_input.hpp"
#include "position_plot.hpp"
#include "test_locales.hpp"
#include "track_identity.hpp"
#include "video_canvas.hpp"
#include "workspace_document.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QStatusBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <gtest/gtest.h>

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace
{

QString fixturePath(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

/// A Tracker-written image project that MoTrack can save back: frames 0 and 2 of "image mass" at image (4, 16) and
/// (12, 4), origin (2, 20), scale (2, 4) px per m, starting at -0.25 s with 0.125 s per frame.
QString linkedImages()
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/linked-images.trk";
}

QByteArray readBytes(const QString& path)
{
    QFile input(path);
    return input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray{};
}

/// The user's regional format for one scope: QLocale's default, and the C and C++ locales as well when the machine
/// has them. Restores everything on destruction.
class UserLocale
{
  public:
    UserLocale(std::string_view language, std::string_view region)
    {
        if (const auto process = motrack::test::availableLocale(language, region))
        {
            m_Process.emplace(*process);
        }
        else
        {
            QLocale::setDefault(QLocale(QString::fromUtf8(language) + '_' + QString::fromUtf8(region)));
        }
    }

    ~UserLocale()
    {
        m_Process.reset();
        QLocale::setDefault(m_Previous);
    }

    UserLocale(const UserLocale&) = delete;
    UserLocale& operator=(const UserLocale&) = delete;

  private:
    QLocale m_Previous;
    std::optional<motrack::test::ScopedProcessLocale> m_Process;
};

/// Runs @p respond on the next modal dialog, and rejects any dialog still open after five seconds so that a failed
/// expectation cannot hang the test. Both are queued; the caller then opens the dialog.
void answerDialog(QObject& context, std::function<void(QWidget&)> respond)
{
    QTimer::singleShot(0,
                       &context,
                       [respond = std::move(respond)]
                       {
                           if (auto* dialog = QApplication::activeModalWidget())
                           {
                               respond(*dialog);
                           }
                       });
    QTimer::singleShot(5000,
                       &context,
                       []
                       {
                           if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                           {
                               dialog->reject();
                           }
                       });
}

QPushButton* dialogButton(QWidget& dialog, QDialogButtonBox::StandardButton which)
{
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    return buttons ? buttons->button(which) : nullptr;
}

/// Types @p text into @p field as keystrokes, replacing what it held.
void type(QLineEdit& field, const QString& text)
{
    field.selectAll();
    field.del();
    QTest::keyClicks(&field, text);
}

/// Sets the selected point mass's value through Track > Set point-mass value, typing @p text.
bool typeMass(motrack::MainWindow& window, const QString& text)
{
    auto* action = window.findChild<QAction*>("setPointMassValue");
    if (!action || !action->isEnabled())
    {
        return false;
    }
    bool typed = false;
    answerDialog(window,
                 [&typed, &text](QWidget& widget)
                 {
                     auto* dialog = qobject_cast<QInputDialog*>(&widget);
                     auto* field = dialog ? dialog->findChild<QLineEdit*>() : nullptr;
                     if (field)
                     {
                         type(*field, text);
                         typed = true;
                         dialog->accept();
                     }
                 });
    action->trigger();
    return typed;
}

/// What the point-mass dialog shows for the selected track's value before any edit.
QString shownMass(motrack::MainWindow& window)
{
    auto* action = window.findChild<QAction*>("setPointMassValue");
    if (!action || !action->isEnabled())
    {
        return {};
    }
    QString shown;
    answerDialog(window,
                 [&shown](QWidget& widget)
                 {
                     if (auto* dialog = qobject_cast<QInputDialog*>(&widget))
                     {
                         shown = dialog->textValue();
                         dialog->reject();
                     }
                 });
    action->trigger();
    return shown;
}

/// Calibrates the current frame with the calibration stick from image (0, 1) to (2, 1), typing @p distance and the
/// unit cm. With a scale of (2, 4) px per m, as both fixtures have, a known length of 2.5 gives (0.8, 1.6).
bool typeCalibration(motrack::MainWindow& window, const QString& distance)
{
    auto* action = window.findChild<QAction*>("calibrationStick");
    auto* video = dynamic_cast<motrack::VideoCanvas*>(window.findChild<QWidget*>("videoCanvas"));
    if (!action || !video || !action->isEnabled())
    {
        return false;
    }
    action->trigger();
    if (!video->pick({0, 1}, false))
    {
        return false;
    }
    bool accepted = false;
    answerDialog(window,
                 [&accepted, &distance](QWidget& dialog)
                 {
                     auto* length = dialog.findChild<QLineEdit*>("knownDistance");
                     auto* unit = dialog.findChild<QLineEdit*>("lengthUnit");
                     auto* ok = dialogButton(dialog, QDialogButtonBox::Ok);
                     auto* cancel = dialogButton(dialog, QDialogButtonBox::Cancel);
                     if (!length || !unit || !ok || !cancel)
                     {
                         return;
                     }
                     type(*length, distance);
                     unit->setText("cm");
                     accepted = ok->isEnabled();
                     (accepted ? ok : cancel)->click();
                 });
    static_cast<void>(video->pick({2, 1}, false));
    return accepted;
}

QStringList row(const QTableWidget& table, int index)
{
    QStringList cells;
    for (int column = 0; column < table.columnCount(); ++column)
    {
        const auto* item = table.item(index, column);
        cells.push_back(item ? item->text() : QString{});
    }
    return cells;
}

QString fitSummary(motrack::MainWindow& window)
{
    auto* fit = window.findChild<QCheckBox*>("linearFit");
    auto* summary = window.findChild<QLabel*>("linearFitSummary");
    if (!fit || !summary)
    {
        return {};
    }
    fit->setChecked(true);
    const auto text = summary->text();
    fit->setChecked(false);
    return text;
}

/// Two marks of an uncalibrated, timed clip with values large enough to show digit grouping and negative y.
QString writeLargeValues(const QTemporaryDir& temporary)
{
    auto document = motrack::WorkspaceDocument{};
    document.project.clip.stepCount = 2;
    document.project.clip.frameDurationMs = 1000.0;
    document.project.clip.mediaReferences = {fixturePath("frames/frame00.ppm").toStdString(),
                                             fixturePath("frames/frame02.ppm").toStdString()};
    motrack::PointMass mass{.name = "Cart", .id = motrack::allocateTrackId(document.project)};
    mass.marks = {{.frame = 0, .x = 12345.125, .y = -2500.5}, {.frame = 1, .x = 23456.625, .y = -1500.5}};
    document.project.pointMasses.push_back(mass);
    const auto path = temporary.filePath("large-values.motrack");
    motrack::writeWorkspaceDocument(path, document);
    return path;
}

TEST(WorkspaceLocale, ShowsTheTablePlotAxesAndReadoutsInTheUserLocale)
{
    struct Case
    {
        const char* language;
        const char* region;
        QStringList firstRow;
        QStringList secondRow;
        motrack::PositionPlot::AxisLabels axes;
        QString fit;
    };
    const auto cases = {
        Case{.language = "en",
             .region = "US",
             .firstRow = {"Ball", "0", "1.5", "1.5", "0.75", "0.625"},
             .secondRow = {"Ball", "2", "2.5", "2.5", "1.25", "0.375"},
             .axes = {.top = "1.25", .bottom = "0.75", .left = "-0.5", .right = "0"},
             .fit = QString::fromUtf8("Slope 1 · intercept 1.25 · R² 1")},
        Case{.language = "de",
             .region = "DE",
             .firstRow = {"Ball", "0", "1,5", "1,5", "0,75", "0,625"},
             .secondRow = {"Ball", "2", "2,5", "2,5", "1,25", "0,375"},
             .axes = {.top = "1,25", .bottom = "0,75", .left = "-0,5", .right = "0"},
             .fit = QString::fromUtf8("Slope 1 · intercept 1,25 · R² 1")},
    };
    for (const auto& expected : cases)
    {
        SCOPED_TRACE(std::string(expected.language) + '_' + expected.region);
        const UserLocale locale(expected.language, expected.region);
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
        auto* marks = window.findChild<QTableWidget*>("marksTable");
        auto* plot = dynamic_cast<motrack::PositionPlot*>(window.findChild<QWidget*>("positionPlot"));
        auto* frame = window.findChild<QLabel*>("frameLabel");
        ASSERT_TRUE(marks && plot && frame);
        ASSERT_EQ(marks->rowCount(), 2);
        EXPECT_EQ(row(*marks, 0), expected.firstRow);
        EXPECT_EQ(row(*marks, 1), expected.secondRow);
        EXPECT_EQ(plot->accessibleName(), "x position (m) versus Time (s)");
        const auto axes = plot->axisLabels();
        EXPECT_EQ(axes.top, expected.axes.top);
        EXPECT_EQ(axes.bottom, expected.axes.bottom);
        EXPECT_EQ(axes.left, expected.axes.left);
        EXPECT_EQ(axes.right, expected.axes.right);
        EXPECT_FALSE(plot->grab().isNull());
        EXPECT_EQ(fitSummary(window), expected.fit);
        EXPECT_EQ(frame->text(), "Frame 0");
    }
}

TEST(WorkspaceLocale, GroupsDigitsInReadoutsButNotInDataViews)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = writeLargeValues(temporary);
    struct Case
    {
        const char* language;
        const char* region;
        QStringList firstRow;
        QStringList yAxis; ///< top, bottom of the y position plot
        QString fit;       ///< of the x position plot
    };
    const auto cases = {
        Case{.language = "en",
             .region = "US",
             .firstRow = {"Cart", "0", "12345.125", "-2500.5", "", ""},
             .yAxis = {"-1500.5", "-2500.5"},
             .fit = QString::fromUtf8("Slope 11,111.5 · intercept 12,345.1 · R² 1")},
        Case{.language = "de",
             .region = "DE",
             .firstRow = {"Cart", "0", "12345,125", "-2500,5", "", ""},
             .yAxis = {"-1500,5", "-2500,5"},
             .fit = QString::fromUtf8("Slope 11.111,5 · intercept 12.345,1 · R² 1")},
        // Swedish formats use U+2212 as the minus sign, which the table and axes follow.
        Case{.language = "sv",
             .region = "SE",
             .firstRow = {"Cart", "0", "12345,125", QString::fromUtf8("−2500,5"), "", ""},
             .yAxis = {QString::fromUtf8("−1500,5"), QString::fromUtf8("−2500,5")},
             .fit = QString::fromUtf8("Slope 11 111,5 · intercept 12 345,1 · R² 1")},
    };
    for (const auto& expected : cases)
    {
        SCOPED_TRACE(std::string(expected.language) + '_' + expected.region);
        const UserLocale locale(expected.language, expected.region);
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(path));
        auto* marks = window.findChild<QTableWidget*>("marksTable");
        auto* plot = dynamic_cast<motrack::PositionPlot*>(window.findChild<QWidget*>("positionPlot"));
        auto* quantities = window.findChild<QComboBox*>("plotQuantity");
        ASSERT_TRUE(marks && plot && quantities);
        EXPECT_EQ(row(*marks, 0), expected.firstRow);
        EXPECT_EQ(plot->axisLabels().top, "23457");
        EXPECT_EQ(plot->axisLabels().bottom, "12345");
        EXPECT_EQ(fitSummary(window), expected.fit);
        quantities->setCurrentIndex(1);
        EXPECT_EQ(plot->accessibleName(), "y position (px) versus Time (s)");
        EXPECT_EQ(plot->axisLabels().top, expected.yAxis.at(0));
        EXPECT_EQ(plot->axisLabels().bottom, expected.yAxis.at(1));
    }
}

TEST(WorkspaceLocale, ExportsAndSavesTheSameBytesInEveryLocale)
{
    // The same edits, typed in each locale's own form, through the same paths: every exported and saved byte is
    // the same, with classic numbers.
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto directory = temporary.filePath("save");
    const auto source = linkedImages();

    std::map<std::string, std::map<QString, QByteArray>> saved;
    for (const auto* region : {"US", "DE"})
    {
        const std::string language = region == std::string_view("US") ? "en" : "de";
        SCOPED_TRACE(language + '_' + region);
        ASSERT_TRUE(QDir().mkpath(directory));
        {
            const UserLocale locale(language, region);
            motrack::MainWindow window;
            window.show();
            ASSERT_TRUE(window.loadProject(source));
            ASSERT_TRUE(typeMass(window, QLocale().toString(2.5)));
            ASSERT_TRUE(typeCalibration(window, QLocale().toString(2.5)));
            auto* plot = dynamic_cast<motrack::PositionPlot*>(window.findChild<QWidget*>("positionPlot"));
            ASSERT_NE(plot, nullptr);
            ASSERT_TRUE(window.saveMeasurements(directory + "/measurements.csv"));
            const auto series = plot->csv();
            ASSERT_TRUE(window.saveTrackerProject(directory + "/experiment.trk")) << window.statusBar()->currentMessage().toStdString();
            ASSERT_TRUE(window.saveWorkspace(directory + "/native.motrack"));
            auto& files = saved[language];
            files["plot.csv"] = QByteArray::fromStdString(series);
            for (const auto& name : QDir(directory).entryList(QDir::Files))
            {
                files[name] = readBytes(directory + '/' + name);
            }
        }
        ASSERT_TRUE(QDir(directory).removeRecursively());
    }
    const auto& english = saved["en"];
    const auto& german = saved["de"];
    ASSERT_TRUE(english.contains("measurements.csv") && english.contains("experiment.trk") && english.contains("experiment.motrack") &&
                english.contains("native.motrack"));
    EXPECT_EQ(german.size(), english.size());
    for (const auto& [name, bytes] : english)
    {
        SCOPED_TRACE(name.toStdString());
        ASSERT_TRUE(german.contains(name));
        EXPECT_EQ(german.at(name), bytes);
    }
    EXPECT_TRUE(english.at("measurements.csv").contains("image mass,0,4,16,2.5,2.5,cm\r\n"))
        << english.at("measurements.csv").toStdString();
    EXPECT_EQ(english.at("plot.csv"), "frame,time_s,x position (cm)\r\n0,-0.25,2.5\r\n2,0,12.5\r\n");
    const auto& xml = english.at("experiment.trk");
    EXPECT_TRUE(xml.contains(R"(<property name="mass" type="double">2.5</property>)"));
    EXPECT_TRUE(xml.contains(R"(<property name="xscale" type="double">0.8</property>)"));
    EXPECT_TRUE(xml.contains(R"(<property name="yscale" type="double">1.6</property>)"));
    for (const auto& [name, bytes] : english)
    {
        EXPECT_FALSE(bytes.contains("2,5") || bytes.contains("12,5") || bytes.contains("0,8")) << name.toStdString();
    }
}

TEST(WorkspaceLocale, RoundTripsEditedValuesThroughTheUiInGerman)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("edited.trk");
    const auto source = linkedImages();

    const UserLocale locale("de", "DE");
    const QStringList firstRow{"image mass", "0", "4", "16", "2,5", "2,5"};
    {
        motrack::MainWindow window;
        window.show();
        ASSERT_TRUE(window.loadProject(source));
        EXPECT_EQ(shownMass(window), "2");
        ASSERT_TRUE(typeMass(window, "0,125"));
        EXPECT_EQ(shownMass(window), "0,125");
        // A classic number pasted from a CSV export: 2.5, not the 25 a QDoubleSpinBox would read.
        ASSERT_TRUE(typeCalibration(window, "2.5"));
        auto* marks = window.findChild<QTableWidget*>("marksTable");
        ASSERT_NE(marks, nullptr);
        EXPECT_EQ(row(*marks, 0), firstRow);
        ASSERT_TRUE(window.saveTrackerProject(path)) << window.statusBar()->currentMessage().toStdString();
    }
    const auto xml = readBytes(path);
    EXPECT_TRUE(xml.contains(R"(<property name="mass" type="double">0.125</property>)"));
    EXPECT_TRUE(xml.contains(R"(<property name="xscale" type="double">0.8</property>)"));
    EXPECT_FALSE(xml.contains("0,125"));
    const auto reopened = motrack::readTrackerProject(motrack::toFilesystemPath(path));
    ASSERT_EQ(reopened.project.pointMasses.size(), 1u);
    EXPECT_EQ(reopened.project.pointMasses.front().mass, 0.125);

    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(path));
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    ASSERT_NE(marks, nullptr);
    EXPECT_EQ(row(*marks, 0), firstRow);
    EXPECT_EQ(shownMass(window), "0,125");
}

TEST(WorkspaceLocale, ReadsRegionalAndClassicNumbersButRefusesAmbiguousOnes)
{
    using motrack::NumberInputError;
    using motrack::parseUserNumber;
    const QLocale german(QLocale::German, QLocale::Germany);
    EXPECT_EQ(parseUserNumber("1,5", german), 1.5);
    EXPECT_EQ(parseUserNumber("2.5", german), 2.5) << "a classic number pasted from a CSV file";
    EXPECT_EQ(parseUserNumber("1.234,5", german), 1234.5);
    EXPECT_EQ(parseUserNumber("1.234.567", german), 1234567.0);
    EXPECT_EQ(parseUserNumber(" -0,25 ", german), -0.25);
    EXPECT_EQ(parseUserNumber("1234", german), 1234.0);
    EXPECT_EQ(parseUserNumber("1e3", german), 1000.0);
    EXPECT_EQ(parseUserNumber("1.234", german), std::unexpected(NumberInputError::Ambiguous)) << "1234 grouped or 1.234 classic";
    EXPECT_EQ(parseUserNumber("-1.000", german), std::unexpected(NumberInputError::Ambiguous));
    for (const auto* invalid : {"", "abc", "1,5x", "1,2,3", "inf", "nan"})
    {
        EXPECT_EQ(parseUserNumber(QString::fromUtf8(invalid), german), std::unexpected(NumberInputError::Invalid)) << invalid;
    }

    const QLocale english(QLocale::English, QLocale::UnitedStates);
    EXPECT_EQ(parseUserNumber("1,234", english), 1234.0);
    EXPECT_EQ(parseUserNumber("1,234.5", english), 1234.5);
    EXPECT_EQ(parseUserNumber("2.5", english), 2.5);
    EXPECT_EQ(parseUserNumber("1,5", english), std::unexpected(NumberInputError::Invalid));

    const QLocale french(QLocale::French, QLocale::France);
    EXPECT_EQ(parseUserNumber("1,5", french), 1.5);
    EXPECT_EQ(parseUserNumber("1.5", french), 1.5);
    EXPECT_EQ(parseUserNumber("1" + french.groupSeparator() + "234,5", french), 1234.5);

    const QLocale swedish(QLocale::Swedish, QLocale::Sweden);
    EXPECT_EQ(parseUserNumber(QString::fromUtf8("−0,5"), swedish), -0.5);
    EXPECT_EQ(parseUserNumber("-0.5", swedish), -0.5);

    // Fields show numbers without grouping, so their own text never reads as ambiguous.
    const UserLocale locale("de", "DE");
    EXPECT_EQ(motrack::editableNumber(1234.5), "1234,5");
    EXPECT_EQ(parseUserNumber(motrack::editableNumber(1000.0)), 1000.0);
    EXPECT_EQ(parseUserNumber(motrack::editableNumber(0.1)), 0.1);
}

TEST(WorkspaceLocale, NumberDialogsAcceptOnlyANumberInRange)
{
    const UserLocale locale("de", "DE");
    QObject context;
    const auto ask = [&context](std::function<void(QWidget&)> respond)
    {
        answerDialog(context, std::move(respond));
        return motrack::askNumber(nullptr, "Image sequence timing", "Frames per second (0 for unknown)", 30, 0, 1'000'000);
    };
    const auto submit = [](const QString& text)
    {
        return [text](QWidget& dialog)
        {
            auto* field = dialog.findChild<QLineEdit*>("numberInput");
            auto* ok = dialogButton(dialog, QDialogButtonBox::Ok);
            if (field && ok)
            {
                EXPECT_EQ(field->text(), "30");
                type(*field, text);
                EXPECT_TRUE(ok->isEnabled()) << text.toStdString();
                ok->click();
            }
        };
    };
    EXPECT_EQ(ask(submit("29,97")), 29.97);
    EXPECT_EQ(ask(submit("29.97")), 29.97);
    EXPECT_EQ(ask(submit("1.000.000")), 1'000'000.0);

    // Ambiguous or out-of-range text keeps OK disabled and says why; Enter does not accept it either.
    const auto refuse = [](const QString& text, const QString& reason)
    {
        return [text, reason](QWidget& dialog)
        {
            auto* field = dialog.findChild<QLineEdit*>("numberInput");
            auto* hint = dialog.findChild<QLabel*>("numberHint");
            auto* ok = dialogButton(dialog, QDialogButtonBox::Ok);
            auto* cancel = dialogButton(dialog, QDialogButtonBox::Cancel);
            if (!field || !hint || !ok || !cancel)
            {
                return;
            }
            EXPECT_TRUE(hint->isHidden());
            type(*field, text);
            EXPECT_FALSE(ok->isEnabled()) << text.toStdString();
            EXPECT_FALSE(hint->isHidden());
            EXPECT_EQ(hint->text(), reason);
            QTest::keyClick(field, Qt::Key_Return);
            EXPECT_TRUE(dialog.isVisible());
            EXPECT_EQ(field->text(), text) << "the field keeps what was typed";
            cancel->click();
        };
    };
    EXPECT_EQ(ask(refuse("1.000", motrack::ambiguousNumberMessage())), std::nullopt);
    EXPECT_EQ(motrack::ambiguousNumberMessage(),
              QString::fromUtf8("This number can be read two ways. Type it without digit grouping, with “,” as the decimal mark."));
    EXPECT_EQ(ask(refuse("2000000", "Enter a number from 0 to 1000000.")), std::nullopt);
    EXPECT_EQ(ask(refuse("x", "Enter a number from 0 to 1000000.")), std::nullopt);
    // Accepting programmatically bypasses the disabled button; the text is still checked.
    EXPECT_EQ(ask(
                  [](QWidget& dialog)
                  {
                      if (auto* field = dialog.findChild<QLineEdit*>("numberInput"))
                      {
                          field->setText("1.234");
                      }
                      if (auto* window = qobject_cast<QDialog*>(&dialog))
                      {
                          window->accept();
                      }
                  }),
              std::nullopt);
}

TEST(WorkspaceLocale, RefusesAnAmbiguousMassOrCalibration)
{
    const UserLocale locale("de", "DE");
    motrack::MainWindow window;
    window.show();
    ASSERT_TRUE(window.loadProject(fixturePath("image-sequence.trk")));
    auto* undo = window.findChild<QAction*>("undoEdit");
    ASSERT_NE(undo, nullptr);
    ASSERT_TRUE(typeMass(window, "1.500"));
    EXPECT_EQ(window.statusBar()->currentMessage(), motrack::ambiguousNumberMessage());
    EXPECT_EQ(shownMass(window), "1");
    EXPECT_FALSE(typeCalibration(window, "1.500")) << "OK stays disabled";
    EXPECT_FALSE(undo->isEnabled()) << "neither value changed the project";
    auto* marks = window.findChild<QTableWidget*>("marksTable");
    ASSERT_NE(marks, nullptr);
    EXPECT_EQ(row(*marks, 0).at(4), "0,75");
}

} // namespace
