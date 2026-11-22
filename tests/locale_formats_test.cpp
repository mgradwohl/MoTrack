// Machine-readable text stays classic and displayed numbers follow QLocale, with the whole process in a
// comma-decimal locale (CONTRIBUTING.md, "Text encoding and locale"). Each case runs under de-DE and fr-FR and
// skips a locale the machine lacks.

#include "display_locale.hpp"
#include "filesystem_path.hpp"
#include "legacy_project.hpp"
#include "measurements.hpp"
#include "test_locales.hpp"
#include "track_edits.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <clocale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace
{

using motrack::test::availableLocale;
using motrack::test::ScopedProcessLocale;
using motrack::test::TestLocale;

QString corpusFixture(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + name;
}

std::string readBytes(const QString& path)
{
    QFile input(path);
    return input.open(QIODevice::ReadOnly) ? input.readAll().toStdString() : std::string{};
}

class CommaLocale : public testing::TestWithParam<std::pair<const char*, const char*>>
{
  protected:
    void SetUp() override
    {
        m_Locale = availableLocale(GetParam().first, GetParam().second);
        if (!m_Locale)
        {
            GTEST_SKIP() << "The " << GetParam().first << '-' << GetParam().second << " UTF-8 locale is not installed";
        }
        m_Scope.emplace(*m_Locale);
        // The simulation must really change the C runtime and the global C++ locale, or the cases prove nothing.
        ASSERT_EQ(std::string(std::localeconv()->decimal_point), ",");
        std::ostringstream stream;
        stream << 1.5;
        ASSERT_EQ(stream.str(), "1,5");
    }

    void TearDown() override
    {
        m_Scope.reset();
    }

    [[nodiscard]] const TestLocale& locale() const
    {
        return *m_Locale;
    }

  private:
    std::optional<TestLocale> m_Locale;
    std::optional<ScopedProcessLocale> m_Scope;
};

TEST_P(CommaLocale, TrackerXmlIsParsedAndWrittenWithJavaNumbers)
{
    // OSP writes doubles with Double.toString in every locale; MoTrack must read and write them the same way.
    const auto imported = motrack::readTrackerProject(motrack::toFilesystemPath(corpusFixture("keyed-calibration.trk")));
    ASSERT_EQ(imported.project.pointMasses.size(), 1u);
    EXPECT_EQ(imported.project.pointMasses.front().mass, 2.5);
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(project, 0, 1234.5));
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 1, .x = 10.25, .y = 0.125}));
    const auto xml = motrack::renderTrackerProject(project, imported.source);
    EXPECT_NE(xml.find(R"(<property name="mass" type="double">1234.5</property>)"), std::string::npos);
    EXPECT_NE(xml.find(">10.25<"), std::string::npos);
    EXPECT_NE(xml.find(">0.125<"), std::string::npos);
    EXPECT_EQ(xml.find("1234,5"), std::string::npos);
    EXPECT_EQ(motrack::parseTrackerProject(xml).project, project);
}

TEST_P(CommaLocale, CsvExportsUseClassicNumbers)
{
    const auto imported = motrack::readTrackerProject(motrack::toFilesystemPath(corpusFixture("keyed-calibration.trk")));
    auto project = imported.project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 1, .x = 10.25, .y = 0.125}));
    const auto csv = motrack::measurementsCsv(project);
    EXPECT_NE(csv.find(",1,10.25,0.125,"), std::string::npos) << csv;
    const auto plot =
        motrack::plotCsv({.samples = {{.frame = 1, .horizontal = 0.5, .vertical = 1234.5}}, .timed = true, .timeUnit = "s"}, "x");
    EXPECT_EQ(plot, "frame,time_s,x\r\n1,0.5,1234.5\r\n");
}

TEST_P(CommaLocale, NativeWorkspaceRoundTripsExactNumbers)
{
    auto document = motrack::importTrackerDocument(corpusFixture("keyed-calibration.trk"));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 0.1));
    ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 1.0 / 3.0, .y = 1e-300}));
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("comma.motrack");
    motrack::writeWorkspaceDocument(path, document);
    const auto json = QJsonDocument::fromJson(QByteArray::fromStdString(readBytes(path)));
    ASSERT_TRUE(json.isObject());
    EXPECT_EQ(motrack::readWorkspaceDocument(path).project, document.project);
}

TEST_P(CommaLocale, DataViewsShowTheUserDecimalMarkWithoutGrouping)
{
    // QLocale's default now is the locale under test: data views use its decimal mark, never QString::number.
    const auto display = motrack::dataDisplayLocale();
    EXPECT_EQ(display.decimalPoint(), ",");
    EXPECT_EQ(display.toString(1234.5, 'g', 12), "1234,5");
    EXPECT_EQ(display.toString(-0.0625, 'g', 5), "-0,0625");
    EXPECT_EQ(display.toString(12345), "12345");
    EXPECT_EQ(QString::number(1234.5, 'g', 12), "1234.5"); // the machine form, for comparison
    EXPECT_EQ(display.toDouble(display.toString(0.75, 'g', 12)), 0.75);
}

INSTANTIATE_TEST_SUITE_P(Locales,
                         CommaLocale,
                         testing::Values(std::pair{"de", "DE"}, std::pair{"fr", "FR"}),
                         [](const testing::TestParamInfo<std::pair<const char*, const char*>>& info)
                         { return std::string(info.param.first) + info.param.second; });

TEST(DataDisplayLocale, OmitsGroupingInADotLocale)
{
    const auto previous = QLocale();
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    EXPECT_EQ(motrack::dataDisplayLocale().toString(1234567.25, 'g', 12), "1234567.25");
    EXPECT_EQ(QLocale().toString(1234567.25, 'g', 12), "1,234,567.25");
    QLocale::setDefault(previous);
}

} // namespace
