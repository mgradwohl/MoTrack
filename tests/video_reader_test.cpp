#include "video_reader.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <gtest/gtest.h>

#include <array>
#include <chrono>

namespace
{

using namespace std::chrono_literals;

QString fixturePath(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + '/' + name;
}

TEST(VideoReader, DecodesMovieInPresentationOrderWithVariableTiming)
{
    motrack::VideoReader reader(fixturePath("vfr.mp4"));
    constexpr std::array EXPECTED_TIMES{40ms, 168ms, 288ms, 368ms};
    for (std::size_t ordinal = 0; ordinal < EXPECTED_TIMES.size(); ++ordinal)
    {
        const auto frame = reader.readNext();
        if (!frame || !frame->presentationTime)
        {
            FAIL() << "Missing frame or presentation timestamp at ordinal " << ordinal;
        }
        EXPECT_EQ(frame->ordinal, ordinal);
        EXPECT_EQ(*frame->presentationTime, EXPECTED_TIMES.at(ordinal));
        EXPECT_EQ(frame->image.size(), QSize(32, 24));
        EXPECT_EQ(frame->image.format(), QImage::Format_RGBA8888);
    }
    EXPECT_FALSE(reader.readNext().has_value());
    EXPECT_FALSE(reader.readNext().has_value());
}

TEST(VideoReader, RejectsMissingFilesAndUnknownFormats)
{
    EXPECT_THROW(motrack::VideoReader{fixturePath("missing.mp4")}, motrack::VideoError);
    EXPECT_THROW(motrack::VideoReader{fixturePath("point-mass.trk")}, motrack::VideoError);
}

TEST(VideoReader, AppliesOrthogonalDisplayMetadataWithoutChangingTimestamps)
{
    QFile original(fixturePath("vfr.mp4"));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    auto bytes = original.readAll();
    const auto header = bytes.indexOf("tkhd");
    ASSERT_GE(header, 0);
    ASSERT_EQ(bytes.at(header + 4), '\0');
    constexpr qsizetype MATRIX_OFFSET = 44;
    constexpr qsizetype INTEGER_BYTES = 4;
    constexpr std::int32_t UNIT = 1 << 16;
    constexpr std::int32_t HOMOGENEOUS_UNIT = 1 << 30;
    constexpr std::array<std::int32_t, 9> ROTATION{0, UNIT, 0, -UNIT, 0, 0, 0, 0, HOMOGENEOUS_UNIT};
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("rotated.mp4");
    for (std::size_t index = 0; index < ROTATION.size(); ++index)
    {
        qToBigEndian(ROTATION.at(index), bytes.data() + header + MATRIX_OFFSET + static_cast<qsizetype>(index) * INTEGER_BYTES);
    }
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();

    motrack::VideoReader baseline(fixturePath("vfr.mp4"));
    motrack::VideoReader rotated(path);
    motrack::VideoReader encoded(path, motrack::VideoOrientation::Encoded);
    const auto source = baseline.readNext();
    const auto result = rotated.readNext();
    const auto unchanged = encoded.readNext();
    if (!source || !result || !unchanged)
    {
        FAIL() << "The baseline or rotated frame could not be decoded.";
    }
    EXPECT_EQ(result->presentationTime, source->presentationTime);
    EXPECT_EQ(result->image.size(), QSize(24, 32));
    EXPECT_EQ(result->image.pixelColor(23, 0), source->image.pixelColor(0, 0));
    EXPECT_EQ(result->image.pixelColor(0, 31), source->image.pixelColor(31, 23));
    EXPECT_EQ(unchanged->image, source->image);
}

TEST(VideoReader, RejectsAudioOnlyFiles)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QFile file(temporary.filePath("audio.wav"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const auto bytes = QByteArray::fromHex("524946462600000057415645666d74201000000001000100401f0000803e000002001000"
                                           "64617461020000000000");
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
    EXPECT_THROW(motrack::VideoReader{file.fileName()}, motrack::VideoError);
}

TEST(VideoReader, DoesNotOpenSecondaryInputsFromAPlaylist)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(QFile::copy(fixturePath("vfr.mp4"), temporary.filePath("segment.mp4")));
    QFile file(temporary.filePath("playlist.ffconcat"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    const QByteArray playlist = "ffconcat version 1.0\nfile segment.mp4\n";
    ASSERT_EQ(file.write(playlist), playlist.size());
    file.close();
    EXPECT_THROW(motrack::VideoReader{file.fileName()}, motrack::VideoError);
}

TEST(VideoReader, SeeksPastMetadataLargerThanTheIoBuffer)
{
    QFile original(fixturePath("vfr.mp4"));
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    const auto movie = original.readAll();
    const auto movieHeader = movie.lastIndexOf("moov") - 4;
    ASSERT_GT(movieHeader, 0);

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    QFile file(temporary.filePath("padded.mp4"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    constexpr quint32 PADDING_SIZE = 128U * 1024;
    QByteArray padding(PADDING_SIZE, '\0');
    qToBigEndian<quint32>(PADDING_SIZE, padding.data());
    padding.replace(4, 4, "free");
    const auto padded = movie.first(movieHeader) + padding + movie.sliced(movieHeader);
    ASSERT_EQ(file.write(padded), padded.size());
    file.close();

    motrack::VideoReader reader(file.fileName());
    const auto frame = reader.readNext();
    EXPECT_TRUE(frame.has_value());
}

} // namespace
