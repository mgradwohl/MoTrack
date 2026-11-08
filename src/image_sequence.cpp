#include "image_sequence.hpp"

#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <limits>

namespace motrack
{

std::optional<ImageSequence>
ImageSequence::open(const QString& projectPath, const Clip& clip, const std::map<std::string, QString>& mediaLocations)
{
    if (clip.firstFrame < 0 || clip.stepSize < 1 || clip.mediaReferences.empty() ||
        clip.mediaReferences.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return std::nullopt;
    }

    const auto count = static_cast<int>(clip.mediaReferences.size());
    if (clip.firstFrame >= count || clip.stepCount.value_or(1) <= 0)
    {
        return std::nullopt;
    }

    const auto mediaPath = [&](const std::string& reference)
    {
        const auto retained = mediaLocations.find(reference);
        return retained == mediaLocations.end() ? resolveLocalMediaPath(projectPath, reference) : retained->second;
    };
    const auto firstPath = QString::fromStdString(clip.mediaReferences.front());
    const auto suffix = QFileInfo(firstPath).suffix().toLatin1().toLower();
    if (count == 1 && !QImageReader::supportedImageFormats().contains(suffix) &&
        !QImageReader(mediaPath(clip.mediaReferences.front())).canRead())
    {
        return std::nullopt;
    }

    ImageSequence sequence;
    for (const auto& reference : clip.mediaReferences)
    {
        sequence.m_Paths.append(mediaPath(reference));
    }
    sequence.m_FirstFrame = clip.firstFrame;
    sequence.m_StepSize = clip.stepSize;
    const int available = 1 + (count - 1 - clip.firstFrame) / clip.stepSize;
    sequence.m_StepCount = std::min(available, clip.stepCount.value_or(available));
    return sequence;
}

int ImageSequence::stepCount() const
{
    return m_StepCount;
}

std::optional<ImageFrame> ImageSequence::readStep(int step) const
{
    if (step < 0 || step >= m_StepCount)
    {
        return std::nullopt;
    }
    const auto number = m_FirstFrame + step * m_StepSize;
    const auto& path = m_Paths.at(number);
    return ImageFrame{.number = number, .image = path.isEmpty() ? QImage() : QImageReader(path).read()};
}

} // namespace motrack
