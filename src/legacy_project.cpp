#include "legacy_project.hpp"

#include "track_identity.hpp"
#include "tracker_import.hpp"

#include <QChar>
#include <QFile>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace motrack
{
namespace
{

using tracker::Read;

bool present(const auto& value)
{
    return value || value.error() != osp::DecodeError::Missing;
}

/// Whether VideoClip.Loader applies a saved int. Like Java's getInt, a property of another type reads as
/// Integer.MIN_VALUE and is ignored; MoTrack still refuses unreadable int text instead of guessing.
bool savedInteger(const Read<std::int32_t>& value)
{
    return value || value.error() == osp::DecodeError::Malformed || value.error() == osp::DecodeError::OutOfRange;
}

int integer(const Read<std::int32_t>& value, int minimum)
{
    if (!value || *value < minimum)
    {
        throw ProjectError("Invalid project integer.");
    }
    return *value;
}

int arrayIndex(const tracker::ArrayElement& element)
{
    if (!element.index || *element.index < 0)
    {
        throw ProjectError("Invalid Tracker array entry.");
    }
    return *element.index;
}

std::optional<double> finiteNumber(const Read<double>& value)
{
    return value && std::isfinite(*value) ? std::optional(*value) : std::nullopt;
}

double coordinate(const Read<double>& value)
{
    const auto number = finiteNumber(value);
    if (!number)
    {
        throw ProjectError("Invalid Tracker numeric property.");
    }
    return *number;
}

std::string text(const Read<std::string>& value)
{
    return value ? *value : std::string{};
}

/// VideoClip.Loader's `frameshift` (#313), or 0 when it applies none. Java's getInt reads a missing, unreadable or
/// other-typed property as Integer.MIN_VALUE, which the loader ignores, so MoTrack ignores them too: unlike an
/// unreadable range field, an ignored shift leaves every saved value as Tracker shows it.
int clipFrameShift(const tracker::PanelLoad& panel)
{
    const auto shift = panel.videoClip ? panel.videoClip->frameShift : Read<std::int32_t>(0);
    return shift && *shift != std::numeric_limits<std::int32_t>::min() ? *shift : 0;
}

/// How many leading slots Tracker's application loader removes from a track's `framedata` before the track reads it,
/// or none when it removes the whole array (#313). TrackerIO.AsyncLoader.adjustFrameData, reached through
/// XMLControlElement.getAdjustedFrameData, drops the first `shift` slots (when positive) of every track array of Java
/// length above 1, so saved index n loads at frame n - shift. An array no longer than the shift becomes null and loads
/// no frames; one of length 1 (only `[0]`) is unchanged. The Java length is the last element's index plus one
/// (XMLControlElement.arrayValue). TrackerPanel loads the coordinate system without the adjuster: its keys never move.
template <typename Data> std::optional<int> droppedSlots(const tracker::FrameArray<Data>& array, int shift)
{
    if (shift <= 0 || array.frames.empty())
    {
        return 0;
    }
    const auto length = std::int64_t{arrayIndex(array.frames.back().element)} + 1;
    if (length <= 1)
    {
        return 0;
    }
    return shift >= length ? std::nullopt : std::optional(shift);
}

Clip readClip(const tracker::PanelLoad& panel, int frameShift)
{
    Clip clip;
    const auto* video = panel.videoClip && panel.videoClip->video ? &*panel.videoClip->video : nullptr;
    if (panel.videoClip)
    {
        const auto& loaded = *panel.videoClip;
        if (savedInteger(loaded.startFrame))
        {
            // VideoClip.Loader subtracts a saved frame shift from the start frame with Java's wrapping int arithmetic,
            // and setStartFrameNumber keeps the result at or after frame 0 (#313). Without a saved start frame Java
            // subtracts the shift from Integer.MIN_VALUE, which also wraps; MoTrack keeps frame 0 then.
            const auto start = static_cast<std::int32_t>(static_cast<std::uint32_t>(integer(loaded.startFrame, 0)) -
                                                         static_cast<std::uint32_t>(frameShift));
            clip.firstFrame = std::max<std::int32_t>(start, 0);
        }
        if (savedInteger(loaded.stepSize))
        {
            clip.stepSize = integer(loaded.stepSize, 1);
        }
        if (savedInteger(loaded.stepCount))
        {
            clip.stepCount = integer(loaded.stepCount, 0);
        }
        clip.startTimeMs = finiteNumber(loaded.startTime).value_or(0.0);
    }
    if (video)
    {
        std::vector<std::pair<int, std::string>> indexedPaths;
        for (const auto& entry : video->pathElements)
        {
            if (!entry.value)
            {
                throw ProjectError("Invalid Tracker image sequence.");
            }
            indexedPaths.emplace_back(arrayIndex(entry.element), *entry.value);
        }
        std::ranges::sort(indexedPaths, {}, &std::pair<int, std::string>::first);
        for (const auto& [index, path] : indexedPaths)
        {
            if (static_cast<std::size_t>(index) != clip.mediaReferences.size() || path.empty())
            {
                throw ProjectError("Invalid Tracker image sequence.");
            }
            clip.mediaReferences.push_back(path);
        }
        if (const auto path = text(video->path); clip.mediaReferences.empty() && !path.empty())
        {
            clip.mediaReferences.push_back(path);
        }
    }
    // Movie clip controls can save a mean interval even when frame presentation times vary.
    if (!video || video->className == tracker::IMAGE_VIDEO_CLASS)
    {
        const auto controlDuration = panel.clipControl ? finiteNumber(panel.clipControl->deltaT) : std::optional<double>{};
        clip.frameDurationMs = controlDuration;
        if (clip.frameDurationMs.value_or(0.0) <= 0.0)
        {
            clip.frameDurationMs = video ? finiteNumber(video->deltaT) : std::nullopt;
        }
        if (clip.frameDurationMs.value_or(0.0) <= 0.0)
        {
            clip.frameDurationMs.reset();
        }
        // Without media, Tracker's StepperClipControl keeps its 100 ms default unless the saved clip control
        // replaces it with a non-zero interval (#214).
        if (!video && (!panel.clipControl || controlDuration == 0.0))
        {
            clip.frameDurationMs = tracker::DEFAULT_STEPPER_FRAME_DURATION_MS;
        }
    }
    return clip;
}

/// The frame a saved track index loads at, or none when Tracker drops that slot (see `droppedSlots`).
std::optional<int> loadedFrame(const tracker::ArrayElement& element, std::optional<int> dropped)
{
    const auto index = arrayIndex(element);
    return dropped && index >= *dropped ? std::optional(index - *dropped) : std::nullopt;
}

PointMass readPointMass(const tracker::PointMassLoad& loaded, int frameShift)
{
    const auto mass = std::max(MIN_POINT_MASS, std::abs(finiteNumber(loaded.mass).value_or(1.0)));
    PointMass track{.name = text(loaded.name), .marks = {}, .mass = mass};
    if (loaded.frames)
    {
        const auto dropped = droppedSlots(*loaded.frames, frameShift);
        for (const auto& frame : loaded.frames->frames)
        {
            if (frame.element.object)
            {
                if (const auto number = loadedFrame(frame.element, dropped))
                {
                    track.marks.push_back({.frame = *number, .x = coordinate(frame.data.x), .y = coordinate(frame.data.y)});
                }
            }
        }
    }
    std::ranges::sort(track.marks, {}, &Mark::frame);
    if (std::ranges::adjacent_find(track.marks, {}, &Mark::frame) != track.marks.end())
    {
        throw ProjectError("Duplicate Tracker point mass frame.");
    }
    return track;
}

VectorTrack readVector(const tracker::VectorLoad& loaded, int frameShift)
{
    VectorTrack track{.name = text(loaded.name), .samples = {}};
    if (loaded.frames)
    {
        const auto dropped = droppedSlots(*loaded.frames, frameShift);
        for (const auto& frame : loaded.frames->frames)
        {
            if (!frame.element.object)
            {
                continue;
            }
            if (const auto number = loadedFrame(frame.element, dropped))
            {
                track.samples.push_back({.frame = *number,
                                         .tailX = coordinate(frame.data.xtail),
                                         .tailY = coordinate(frame.data.ytail),
                                         .componentX = coordinate(frame.data.xcomponent),
                                         .componentY = coordinate(frame.data.ycomponent)});
            }
        }
    }
    std::ranges::sort(track.samples, {}, &VectorSample::frame);
    if (std::ranges::adjacent_find(track.samples, {}, &VectorSample::frame) != track.samples.end())
    {
        throw ProjectError("Duplicate Tracker vector frame.");
    }
    return track;
}

using CoordFrame = tracker::Frame<tracker::CoordFrameData>;

CalibrationKey calibrationKey(int frame, const CoordFrame& data)
{
    return {.frame = frame,
            .originX = coordinate(data.data.xorigin),
            .originY = coordinate(data.data.yorigin),
            .angleDegrees = coordinate(data.data.angle),
            .scaleX = coordinate(data.data.xscale),
            .scaleY = coordinate(data.data.yscale)};
}

/// The coordinate keys as ImageCoordSystem.Loader reads `framedata`: XMLControlElement.arrayValue fills a Java array, so
/// a later element with the same index replaces an earlier one, a null one included, and Tracker keeps the last saved
/// key (#314). An unreadable or out-of-order index makes Java's loader throw; such a coordinate system, like one with
/// unreadable numbers, imports as no calibration.
std::vector<CalibrationKey> indexedKeys(const osp::Source& source, const tracker::FrameArray<tracker::CoordFrameData>& frames)
{
    const auto indexed = osp::PropertyView(source, frames.property).indexedElements();
    if (!indexed)
    {
        throw ProjectError("Invalid Tracker coordinate system.");
    }
    std::map<osp::NodeId, const CoordFrame*> decoded;
    for (const auto& frame : frames.frames)
    {
        decoded.emplace(frame.element.property, &frame);
    }
    std::vector<CalibrationKey> keys;
    for (const auto& [index, element] : indexed->elements)
    {
        if (const auto& frame = *decoded.at(element.id()); frame.element.object)
        {
            keys.push_back(calibrationKey(static_cast<int>(index), frame));
        }
    }
    return keys;
}

/// The keys of TrackerReading::SavedIndices: every object element in index order, and none at all for a duplicate index.
std::vector<CalibrationKey> savedIndexKeys(const tracker::FrameArray<tracker::CoordFrameData>& frames)
{
    std::vector<CalibrationKey> keys;
    for (const auto& frame : frames.frames)
    {
        if (frame.element.object)
        {
            keys.push_back(calibrationKey(arrayIndex(frame.element), frame));
        }
    }
    std::ranges::sort(keys, {}, &CalibrationKey::frame);
    if (std::ranges::adjacent_find(keys, {}, &CalibrationKey::frame) != keys.end())
    {
        keys.clear();
    }
    return keys;
}

std::optional<Calibration> readCalibration(const osp::Source& source, const tracker::PanelLoad& panel, TrackerReading reading)
{
    if (!panel.coords || panel.coords->className != tracker::COORD_SYSTEM_CLASS || !text(panel.referenceFrame).empty())
    {
        return std::nullopt;
    }
    const auto& coords = *panel.coords;
    Calibration calibration{.keyframes = {},
                            .fixedOrigin = coords.fixedOrigin && *coords.fixedOrigin,
                            .fixedAngle = coords.fixedAngle && *coords.fixedAngle,
                            .fixedScale = coords.fixedScale && *coords.fixedScale};
    if (coords.frames)
    {
        try
        {
            calibration.keyframes =
                reading == TrackerReading::SavedIndices ? savedIndexKeys(*coords.frames) : indexedKeys(source, *coords.frames);
        }
        catch (const ProjectError&)
        {
            return std::nullopt;
        }
    }
    if (calibration.keyframes.empty())
    {
        return std::nullopt;
    }
    return calibration;
}

/// Whether every byte has a Windows-1252 character; the code page leaves 0x81, 0x8D, 0x8F, 0x90 and 0x9D undefined.
bool decodesAsWindows1252(std::string_view bytes) noexcept
{
    return std::ranges::none_of(bytes,
                                [](char byte)
                                {
                                    const auto value = static_cast<unsigned char>(byte);
                                    return value == 0x81 || value == 0x8D || value == 0x8F || value == 0x90 || value == 0x9D;
                                });
}

} // namespace

std::shared_ptr<const osp::Source> detail::parseTrackerXml(std::string_view xml)
{
    if (xml.size() > MAX_TRACKER_XML_BYTES)
    {
        throw ProjectError("Tracker project is too large.");
    }
    auto parsed = osp::parse(std::string(xml));
    if (!parsed)
    {
        // Name what Tracker's line-oriented reader cannot read, for example a blank line ending an array (#196).
        const auto& error = parsed.error();
        auto message =
            "Invalid Tracker XML" + (error.line > 0 ? " at line " + std::to_string(error.line) : std::string{}) + ": " + error.message;
        // Tracker's File > Export > Tracker file writes the platform charset under a UTF-8 declaration (#277).
        // The reader stays strict; opening such a file with a visible warning is #302.
        if (error.code == osp::ParseError::Code::InvalidUtf8 && decodesAsWindows1252(xml))
        {
            message += " It reads as Windows-1252 text, which Tracker's File > Export > Tracker file writes on some systems. "
                       "Convert the file to UTF-8, for example in a text editor, and open it again.";
        }
        throw ProjectError(message);
    }
    return std::move(*parsed);
}

detail::ImportedTracker detail::importTracker(std::shared_ptr<const osp::Source> source, TrackerReading reading)
{
    auto panel = tracker::loadPanel(*source);
    if (panel.className != tracker::TRACKER_PANEL_CLASS)
    {
        throw ProjectError("Expected a TrackerPanel project.");
    }
    const auto frameShift = reading == TrackerReading::SavedIndices ? 0 : clipFrameShift(panel);
    ImportedTracker result{
        .source = std::move(source), .panel = std::move(panel), .project = {}, .bindings = {}, .trackFrameShift = std::max(frameShift, 0)};
    const auto& loaded = result.panel;
    const auto& xml = *result.source;
    auto& project = result.project;
    project = LegacyProject{.version = text(loaded.semanticVersion),
                            .description = text(loaded.description),
                            .clip = readClip(loaded, frameShift),
                            .calibration = readCalibration(xml, loaded, reading),
                            // TrackerPanel keeps its preferred unit unless the file saves one (#214).
                            .lengthUnit = present(loaded.lengthUnit) ? text(loaded.lengthUnit) : std::string(tracker::DEFAULT_LENGTH_UNIT),
                            .pointMasses = {},
                            .unsupportedTrackTypes = {}};
    if (auto unit = text(loaded.massUnit); !unit.empty())
    {
        project.massUnit = std::move(unit);
    }
    if (auto unit = text(loaded.timeUnit); !unit.empty())
    {
        project.timeUnit = std::move(unit);
    }
    for (const auto& item : loaded.trackItems)
    {
        if (!item.object)
        {
            continue; // A null or non-object element is not a track.
        }
        if (item.className == tracker::POINT_MASS_CLASS)
        {
            project.pointMasses.push_back(readPointMass(tracker::loadPointMass(xml, *item.object), result.trackFrameShift));
            project.pointMasses.back().id = allocateTrackId(project);
            result.bindings.push_back(
                {.trackId = project.pointMasses.back().id, .sourceIndex = item.ordinal, .kind = TrackKind::PointMass});
        }
        else if (item.className == tracker::VECTOR_CLASS)
        {
            try
            {
                project.vectors.push_back(readVector(tracker::loadVector(xml, *item.object), result.trackFrameShift));
                project.vectors.back().id = allocateTrackId(project);
                result.bindings.push_back({.trackId = project.vectors.back().id, .sourceIndex = item.ordinal, .kind = TrackKind::Vector});
            }
            catch (const ProjectError&)
            {
                project.unsupportedTrackTypes.push_back(item.className);
            }
        }
        else if (!item.className.empty())
        {
            project.unsupportedTrackTypes.push_back(item.className);
        }
    }
    return result;
}

LegacyProject parseLegacyProject(std::string_view xml)
{
    return detail::importTracker(detail::parseTrackerXml(xml)).project;
}

LegacyProject readLegacyProject(const std::filesystem::path& path)
{
    return readTrackerProject(path).project;
}

ImportedTrackerProject readTrackerProject(const std::filesystem::path& path)
{
    // Use the same Unicode/long-path capable filesystem adapter as desktop saving.
    QFile file(QString::fromStdU16String(path.u16string()));
    if (!file.open(QIODevice::ReadOnly) || file.isSequential())
    {
        throw ProjectError("Could not open the Tracker project.");
    }
    const auto length = file.size();
    if (length < 0 || std::cmp_greater(length, MAX_TRACKER_XML_BYTES))
    {
        throw ProjectError("Tracker project is too large.");
    }
    const auto xml = file.read(length + 1);
    if (file.error() != QFileDevice::NoError || xml.size() != length || !file.atEnd())
    {
        throw ProjectError("Could not read the Tracker project.");
    }
    return parseTrackerProject(xml.toStdString());
}

ImportedTrackerProject parseTrackerProject(std::string xml)
{
    auto parsed = detail::importTracker(detail::parseTrackerXml(xml));
    return {.project = std::move(parsed.project), .source = {.xml = std::move(xml), .bindings = std::move(parsed.bindings)}};
}

bool isTrackerUnitLabel(std::string_view unit)
{
    const auto label = QString::fromUtf8(unit.data(), static_cast<qsizetype>(unit.size()));
    if (label.isEmpty() || label.size() > 256 || label != label.trimmed() || label.toStdString() != unit)
    {
        return false;
    }
    return std::ranges::none_of(label.toUcs4(),
                                [](char32_t codePoint)
                                {
                                    const auto category = QChar::category(codePoint);
                                    return category == QChar::Other_Control || category == QChar::Other_Format || QChar::isDigit(codePoint);
                                });
}

void validateTrackerSource(const TrackerSource& source, const LegacyProject& project)
{
    static_cast<void>(detail::validatedTrackerSource(source, project));
}

detail::ImportedTracker detail::validatedTrackerSource(const TrackerSource& source, const LegacyProject& project)
{
    validateTrackIdentities(project);
    auto original = detail::importTracker(detail::parseTrackerXml(source.xml));
    if (source.bindings && !detail::trackerBindingsMatch(*source.bindings, project, original))
    {
        throw ProjectError("Incomplete Tracker source associations.");
    }
    return original;
}

bool detail::trackerBindingsMatch(const std::vector<TrackSourceBinding>& bindings,
                                  const LegacyProject& project,
                                  const ImportedTracker& original)
{
    std::unordered_map<TrackId, TrackKind> live;
    for (const auto& track : project.pointMasses)
    {
        live.emplace(track.id, TrackKind::PointMass);
    }
    for (const auto& track : project.vectors)
    {
        live.emplace(track.id, TrackKind::Vector);
    }
    std::unordered_set<TrackId> ids;
    for (const auto& binding : bindings)
    {
        const auto track = live.find(binding.trackId);
        if (binding.trackId == 0 || binding.trackId >= project.nextTrackId || !ids.insert(binding.trackId).second ||
            (track != live.end() && track->second != binding.kind))
        {
            throw ProjectError("Invalid Tracker source association.");
        }
    }
    if (bindings.size() != original.bindings.size())
    {
        return false;
    }
    std::unordered_map<std::size_t, TrackKind> expected;
    for (const auto& binding : original.bindings)
    {
        expected.emplace(binding.sourceIndex, binding.kind);
    }
    for (const auto& binding : bindings)
    {
        const auto object = expected.find(binding.sourceIndex);
        if (object == expected.end() || object->second != binding.kind)
        {
            return false;
        }
        expected.erase(object);
    }
    return true;
}

} // namespace motrack
