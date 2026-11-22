#include "command_checks.hpp"
#include "legacy_project_dump.hpp"
#include "legacy_view.hpp"
#include "measurements.hpp"
#include "model/commands.hpp"
#include "model/document_history.hpp"
#include "model/tracker_loaders.hpp"
#include "track_edits.hpp"
#include "track_identity.hpp"
#include "utf8_path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/// Replays the scenarios of track_edits_test.cpp through ID-addressed commands and compares the legacy view of the
/// typed document with the project TrackEdits edits, after every step: refusals, track identities, the counter, the
/// revision, undo and redo, and the save heads. The façade below resolves list positions through the legacy view, as
/// the TrackEdits façade of B2.4a will; it reuses TrackEdits only for the calibration arithmetic, which B1 owns.
namespace
{

namespace model = motrack::model;
using motrack::LegacyProject;
using motrack::TrackEdits;
using motrack::test::required;

std::string lastError;

/// TrackEdits' index-based operations over a typed document and its history.
class TypedEdits
{
  public:
    explicit TypedEdits(model::TrackerDocument document) : m_Document(std::move(document))
    {}

    [[nodiscard]] const model::TrackerDocument& document() const
    {
        return m_Document;
    }

    [[nodiscard]] model::DocumentHistory& history()
    {
        return m_History;
    }

    [[nodiscard]] LegacyProject view() const
    {
        return motrack::legacyProject(m_Document);
    }

    bool createPointMass(std::string name)
    {
        auto insert = model::createTrack(m_Document, model::Family::PointMass, std::move(name));
        return insert && apply(*insert);
    }

    bool renamePointMass(std::size_t track, std::string name)
    {
        const auto project = view();
        if (track >= project.pointMasses.size() || name.empty() ||
            std::ranges::any_of(project.pointMasses, [&](const auto& mass) { return mass.name == name; }))
        {
            return false;
        }
        return set(id(project.pointMasses[track].id), "name", std::move(name));
    }

    bool setPointMassValue(std::size_t track, double mass)
    {
        const auto project = view();
        if (track >= project.pointMasses.size() || !std::isfinite(mass) || mass < motrack::MIN_POINT_MASS)
        {
            return false;
        }
        return mass == project.pointMasses[track].mass || set(id(project.pointMasses[track].id), "mass", mass);
    }

    bool setMassUnit(std::string unit)
    {
        return !unit.empty() && (unit == view().massUnit || set(panel(), "mass_unit", std::move(unit)));
    }

    bool setTimeUnit(std::string unit)
    {
        return !unit.empty() && (unit == view().timeUnit || set(panel(), "time_unit", std::move(unit)));
    }

    bool removePointMass(std::size_t track)
    {
        const auto project = view();
        return track < project.pointMasses.size() && apply(model::Remove{.object = id(project.pointMasses[track].id)});
    }

    bool markPointMass(std::size_t track, motrack::Mark mark)
    {
        const auto project = view();
        if (track >= project.pointMasses.size() || mark.frame < 0 || !std::isfinite(mark.x) || !std::isfinite(mark.y))
        {
            return false;
        }
        const auto& marks = project.pointMasses[track].marks;
        if (std::ranges::find(marks, mark) != marks.end())
        {
            return true;
        }
        auto command = model::setMark(m_Document, id(project.pointMasses[track].id), mark.frame, model::Mark{.x = mark.x, .y = mark.y});
        return command && apply(*command);
    }

    bool removeMark(std::size_t track, int frame)
    {
        const auto project = view();
        if (track >= project.pointMasses.size() ||
            std::ranges::find(project.pointMasses[track].marks, frame, &motrack::Mark::frame) == project.pointMasses[track].marks.end())
        {
            return false;
        }
        auto command = model::setMark(m_Document, id(project.pointMasses[track].id), frame, std::nullopt);
        return command && apply(*command);
    }

    bool createVector(std::string name)
    {
        if (name.empty() || std::ranges::any_of(view().vectors, [&](const auto& vector) { return vector.name == name; }))
        {
            return false;
        }
        auto insert = model::createTrack(m_Document, model::Family::Vector, std::move(name));
        return insert && apply(*insert);
    }

    bool renameVector(std::size_t track, std::string name)
    {
        const auto project = view();
        if (track >= project.vectors.size() || name.empty() ||
            std::ranges::any_of(project.vectors, [&](const auto& vector) { return vector.name == name; }))
        {
            return false;
        }
        return set(id(project.vectors[track].id), "name", std::move(name));
    }

    bool removeVector(std::size_t track)
    {
        const auto project = view();
        return track < project.vectors.size() && apply(model::Remove{.object = id(project.vectors[track].id)});
    }

    bool markVector(std::size_t track, motrack::VectorSample sample)
    {
        const auto project = view();
        const model::VectorSample value{
            .xTail = sample.tailX, .yTail = sample.tailY, .xComponent = sample.componentX, .yComponent = sample.componentY};
        if (track >= project.vectors.size() || sample.frame < 0)
        {
            return false;
        }
        const auto& samples = project.vectors[track].samples;
        if (std::ranges::find(samples, sample) != samples.end())
        {
            return true;
        }
        return setElement(id(project.vectors[track].id), sample.frame, model::FrameElement<model::VectorSample>{.value = value});
    }

    bool removeVectorSample(std::size_t track, int frame)
    {
        const auto project = view();
        if (track >= project.vectors.size() ||
            std::ranges::find(project.vectors[track].samples, frame, &motrack::VectorSample::frame) == project.vectors[track].samples.end())
        {
            return false;
        }
        return setElement(id(project.vectors[track].id), frame, std::monostate{});
    }

    bool calibrateDistance(motrack::CalibrationDistance distance)
    {
        auto target = view();
        TrackEdits arithmetic;
        return arithmetic.calibrateDistance(target, std::move(distance)) && target.calibration &&
               calibrate(*target.calibration, target.lengthUnit);
    }

    bool setCoordinateAxes(motrack::AxesPlacement axes)
    {
        auto target = view();
        TrackEdits arithmetic;
        return arithmetic.setCoordinateAxes(target, axes) && target.calibration && calibrate(*target.calibration, target.lengthUnit);
    }

    bool setClipRange(int firstFrame, int lastFrame, int stepSize)
    {
        const auto clip = view().clip;
        if (firstFrame < 0 || lastFrame < firstFrame || stepSize < 1 || (clip.sourceFrameCount && lastFrame >= *clip.sourceFrameCount))
        {
            return false;
        }
        const auto count = 1 + (static_cast<std::int64_t>(lastFrame) - firstFrame) / stepSize;
        if (!std::in_range<int>(count) ||
            motrack::ClipRange{.firstFrame = firstFrame, .stepSize = stepSize, .stepCount = static_cast<int>(count)} ==
                motrack::clipRange(clip))
        {
            return false;
        }
        const auto* slot = typedSingleton(model::Role::Clip);
        if (slot == nullptr)
        {
            auto insert = model::createSingleton(
                m_Document,
                model::Role::Clip,
                model::ClipState{.startFrame = firstFrame, .stepSize = stepSize, .stepCount = static_cast<std::int32_t>(count)});
            return insert && apply(*insert);
        }
        const auto& state = std::get<model::ClipState>(*slot->value);
        model::Batch batch;
        if (clip.firstFrame != firstFrame)
        {
            // The saved start that loads at the first frame: VideoClip.Loader subtracts the frame shift.
            const auto saved = std::int64_t{firstFrame} + model::clipFrameShift(state.frameShift);
            batch.commands.emplace_back(
                setCommand(slot->id, "startframe", std::in_range<std::int32_t>(saved) ? static_cast<std::int32_t>(saved) : firstFrame));
        }
        if (clip.stepSize != stepSize)
        {
            batch.commands.emplace_back(setCommand(slot->id, "stepsize", stepSize));
        }
        if (clip.stepCount != count)
        {
            batch.commands.emplace_back(setCommand(slot->id, "stepcount", static_cast<std::int32_t>(count)));
        }
        return apply(batch);
    }

    bool setClipTiming(double frameDurationMs, double startTimeMs)
    {
        const auto project = view();
        auto candidate = project.clip;
        candidate.frameDurationMs = frameDurationMs;
        candidate.startTimeMs = startTimeMs;
        if (!motrack::supportsConstantClipTimingEdits(project) || !motrack::validConstantClipTiming(candidate) ||
            motrack::clipTiming(candidate) == motrack::clipTiming(project.clip))
        {
            return false;
        }
        model::Batch batch;
        if (project.clip.frameDurationMs != frameDurationMs)
        {
            if (const auto* control = typedSingleton(model::Role::ClipControl))
            {
                batch.commands.emplace_back(setCommand(control->id, "delta_t", frameDurationMs));
            }
            else
            {
                batch.commands.emplace_back(required(
                    model::createSingleton(m_Document, model::Role::ClipControl, model::ClipControlState{.deltaT = frameDurationMs})));
            }
        }
        if (project.clip.startTimeMs != startTimeMs)
        {
            const auto* clip = typedSingleton(model::Role::Clip);
            if (clip == nullptr)
            {
                return false; // Every document these scenarios convert has a clip.
            }
            batch.commands.emplace_back(setCommand(clip->id, "starttime", startTimeMs));
        }
        return apply(batch);
    }

    bool undo()
    {
        return record(m_History.undo(m_Document));
    }

    bool redo()
    {
        return record(m_History.redo(m_Document));
    }

    /// An update outside history, as another path changes the live document.
    void update(model::TrackerDocument next)
    {
        m_History.update(m_Document, std::move(next));
    }

    /// A replacement, as opening or Revert installs a document.
    void assign(model::TrackerDocument next)
    {
        m_History.assign(m_Document, std::move(next));
    }

  private:
    static model::ObjectId id(motrack::TrackId value)
    {
        return model::ObjectId{value};
    }

    static bool record(const std::expected<void, model::CommandError>& result)
    {
        lastError = result ? std::string() : result.error().message;
        return result.has_value();
    }

    bool apply(const model::Command& command)
    {
        return record(m_History.apply(m_Document, command));
    }

    [[nodiscard]] model::ObjectId panel() const
    {
        return m_Document.singleton(model::Role::Panel)->id;
    }

    [[nodiscard]] const model::ObjectSlot* typedSingleton(model::Role role) const
    {
        const auto* slot = m_Document.singleton(role);
        return slot != nullptr && slot->family() != model::Family::Unknown ? slot : nullptr;
    }

    [[nodiscard]] model::SetField setCommand(model::ObjectId object, std::string field, model::CommandValue after) const
    {
        auto before = required(model::fieldValue(*m_Document.object(object)->value, field));
        return {.object = object, .field = std::move(field), .before = std::move(before), .after = std::move(after)};
    }

    bool set(model::ObjectId object, std::string field, model::CommandValue after)
    {
        return apply(setCommand(object, std::move(field), std::move(after)));
    }

    bool setElement(model::ObjectId object, std::int32_t frame, model::CommandValue after)
    {
        auto before = required(model::fieldValue(*m_Document.object(object)->value, "framedata", frame));
        if (auto* element = std::get_if<model::FrameElement<model::VectorSample>>(&before))
        {
            element->node.reset();
        }
        return apply(model::SetField{
            .object = object, .field = "framedata", .frame = frame, .before = std::move(before), .after = std::move(after)});
    }

    /// The commands that turn the typed calibration and length label into the ones TrackEdits computed.
    bool calibrate(const motrack::Calibration& next, const std::string& unit)
    {
        model::Batch batch;
        const auto key = [](const motrack::CalibrationKey& item)
        {
            return model::CoordKey{
                .xOrigin = item.originX, .yOrigin = item.originY, .angle = item.angleDegrees, .xScale = item.scaleX, .yScale = item.scaleY};
        };
        if (const auto* coords = typedSingleton(model::Role::Coordinates))
        {
            const auto& state = std::get<model::CoordinatesState>(*coords->value);
            if (state.fixedOrigin.value_or(false) != next.fixedOrigin || state.fixedAngle.value_or(false) != next.fixedAngle ||
                state.fixedScale.value_or(false) != next.fixedScale)
            {
                return false; // TrackEdits sets the fixed flags only when it creates a calibration.
            }
            for (const auto& [frame, element] : state.keys.entries())
            {
                if (std::ranges::find(next.keyframes, frame, &motrack::CalibrationKey::frame) == next.keyframes.end())
                {
                    batch.commands.emplace_back(model::SetField{.object = coords->id,
                                                                .field = "framedata",
                                                                .frame = frame,
                                                                .before = model::FrameElement<model::CoordKey>{.value = element.value},
                                                                .after = std::monostate{}});
                }
            }
            for (const auto& item : next.keyframes)
            {
                const auto* current = state.keys.find(item.frame);
                if (current == nullptr || current->value != key(item))
                {
                    batch.commands.emplace_back(model::SetField{
                        .object = coords->id,
                        .field = "framedata",
                        .frame = item.frame,
                        .before = current != nullptr ? model::CommandValue(model::FrameElement<model::CoordKey>{.value = current->value})
                                                     : model::CommandValue{},
                        .after = model::FrameElement<model::CoordKey>{.value = key(item)}});
                }
            }
        }
        else
        {
            model::CoordinatesState state{.fixedOrigin = next.fixedOrigin, .fixedAngle = next.fixedAngle, .fixedScale = next.fixedScale};
            for (const auto& item : next.keyframes)
            {
                state.keys.set(item.frame, {.value = key(item)});
            }
            auto insert = model::createSingleton(m_Document, model::Role::Coordinates, std::move(state));
            if (!insert)
            {
                return false;
            }
            batch.commands.emplace_back(*std::move(insert));
        }
        if (view().lengthUnit != unit)
        {
            batch.commands.emplace_back(setCommand(panel(), "length_unit", unit));
        }
        // TrackEdits records even an unchanged calibration; a command must change something, so none is recorded.
        return batch.commands.empty() || apply(batch);
    }

    model::TrackerDocument m_Document;
    model::DocumentHistory m_History;
};

model::SaveTarget typedTarget(motrack::SaveTarget target)
{
    return target == motrack::SaveTarget::Native ? model::SaveTarget::Native : model::SaveTarget::Tracker;
}

/// A typed state change outside history: an edit of the document's parts, applied as an update.
using PartsEdit = std::function<void(model::DocumentParts&)>;

/// A Tracker file's bytes, for a Twin that starts from a baseline.
struct FromTracker
{
    std::string xml;
};

/// TrackEdits on a LegacyProject and the commands on its typed document, in lock step. Every operation runs on both and
/// must return the same result, leave the same legacy view, revision, undo and redo availability and save heads, and
/// keep the graph equal to one rebuilt from the document.
class Twin
{
  public:
    explicit Twin(LegacyProject project = {})
        : m_Project(std::move(project)), m_Typed(motrack::typedDocumentFromReduced(m_Project, std::nullopt))
    {
        check();
    }

    /// One Tracker file opened both ways: B1's import for TrackEdits, the typed import for the commands.
    explicit Twin(const FromTracker& source)
        : m_Project(motrack::parseTrackerProject(source.xml).project), m_Typed(motrack::importTypedTracker(source.xml))
    {
        check();
    }

    [[nodiscard]] const LegacyProject& project() const
    {
        return m_Project;
    }

    [[nodiscard]] const model::TrackerDocument& document() const
    {
        return m_Typed.document();
    }

    template <typename Legacy, typename Typed> bool both(Legacy legacy, Typed typed)
    {
        lastError.clear();
        const bool expected = legacy(m_Edits, m_Project);
        const bool actual = typed(m_Typed);
        EXPECT_EQ(actual, expected) << lastError;
        check();
        return expected;
    }

    void check()
    {
        EXPECT_EQ(motrack::test::legacyProjectText(m_Typed.view()), motrack::test::legacyProjectText(m_Project));
        EXPECT_EQ(m_Typed.history().revision(), m_Edits.revision());
        EXPECT_EQ(m_Typed.history().canUndo(), m_Edits.canUndo());
        EXPECT_EQ(m_Typed.history().canRedo(), m_Edits.canRedo());
        for (const auto target : {motrack::SaveTarget::Native, motrack::SaveTarget::Tracker})
        {
            EXPECT_EQ(m_Typed.history().isModified(typedTarget(target)), m_Edits.isModified(target));
        }
        EXPECT_TRUE(motrack::test::graphMatchesRebuild(m_Typed.document()));
    }

    /// Runs the same operation on both sides; `Args` come from the member signatures, never from the arguments.
    template <typename... Args>
    bool call(bool (TrackEdits::*legacy)(LegacyProject&, Args...), bool (TypedEdits::*typed)(Args...), std::type_identity_t<Args>... args)
    {
        return both([&](TrackEdits& edits, LegacyProject& project) { return (edits.*legacy)(project, args...); },
                    [&](TypedEdits& edits) { return (edits.*typed)(args...); });
    }

    bool createPointMass(const std::string& name)
    {
        return call(&TrackEdits::createPointMass, &TypedEdits::createPointMass, name);
    }

    bool renamePointMass(std::size_t track, const std::string& name)
    {
        return call(&TrackEdits::renamePointMass, &TypedEdits::renamePointMass, track, name);
    }

    bool setPointMassValue(std::size_t track, double mass)
    {
        return call(&TrackEdits::setPointMassValue, &TypedEdits::setPointMassValue, track, mass);
    }

    bool setMassUnit(const std::string& unit)
    {
        return call(&TrackEdits::setMassUnit, &TypedEdits::setMassUnit, unit);
    }

    bool setTimeUnit(const std::string& unit)
    {
        return call(&TrackEdits::setTimeUnit, &TypedEdits::setTimeUnit, unit);
    }

    bool removePointMass(std::size_t track)
    {
        return call(&TrackEdits::removePointMass, &TypedEdits::removePointMass, track);
    }

    bool markPointMass(std::size_t track, motrack::Mark mark)
    {
        return call(&TrackEdits::markPointMass, &TypedEdits::markPointMass, track, mark);
    }

    bool removeMark(std::size_t track, int frame)
    {
        return call(&TrackEdits::removeMark, &TypedEdits::removeMark, track, frame);
    }

    bool createVector(const std::string& name)
    {
        return call(&TrackEdits::createVector, &TypedEdits::createVector, name);
    }

    bool renameVector(std::size_t track, const std::string& name)
    {
        return call(&TrackEdits::renameVector, &TypedEdits::renameVector, track, name);
    }

    bool removeVector(std::size_t track)
    {
        return call(&TrackEdits::removeVector, &TypedEdits::removeVector, track);
    }

    bool markVector(std::size_t track, motrack::VectorSample sample)
    {
        return call(&TrackEdits::markVector, &TypedEdits::markVector, track, sample);
    }

    bool removeVectorSample(std::size_t track, int frame)
    {
        return call(&TrackEdits::removeVectorSample, &TypedEdits::removeVectorSample, track, frame);
    }

    bool calibrateDistance(const motrack::CalibrationDistance& distance)
    {
        return call(&TrackEdits::calibrateDistance, &TypedEdits::calibrateDistance, distance);
    }

    bool setCoordinateAxes(motrack::AxesPlacement axes)
    {
        return call(&TrackEdits::setCoordinateAxes, &TypedEdits::setCoordinateAxes, axes);
    }

    bool setClipRange(int firstFrame, int lastFrame, int stepSize)
    {
        return call(&TrackEdits::setClipRange, &TypedEdits::setClipRange, firstFrame, lastFrame, stepSize);
    }

    bool setClipTiming(double frameDurationMs, double startTimeMs)
    {
        return call(&TrackEdits::setClipTiming, &TypedEdits::setClipTiming, frameDurationMs, startTimeMs);
    }

    bool undo()
    {
        return call(&TrackEdits::undo, &TypedEdits::undo);
    }

    bool redo()
    {
        return call(&TrackEdits::redo, &TypedEdits::redo);
    }

    void clear()
    {
        m_Edits.clear();
        m_Typed.history().clear();
        check();
    }

    void markSaved(motrack::SaveTarget target = motrack::SaveTarget::Native)
    {
        m_Edits.markSaved(target);
        m_Typed.history().markSaved(typedTarget(target));
        check();
    }

    void invalidateSaved(motrack::SaveTarget target = motrack::SaveTarget::Native)
    {
        m_Edits.invalidateSaved(target);
        m_Typed.history().invalidateSaved(typedTarget(target));
        check();
    }

    [[nodiscard]] bool isModified(motrack::SaveTarget target = motrack::SaveTarget::Native) const
    {
        return m_Edits.isModified(target);
    }

    [[nodiscard]] bool canUndo() const
    {
        return m_Edits.canUndo();
    }

    [[nodiscard]] bool canRedo() const
    {
        return m_Edits.canRedo();
    }

    [[nodiscard]] std::uint64_t revision() const
    {
        return m_Edits.revision();
    }

    /// The same change made outside history to both: the legacy project directly, the typed document as an update.
    void outOfBand(const std::function<void(LegacyProject&)>& legacy, const PartsEdit& typed)
    {
        legacy(m_Project);
        auto parts = m_Typed.document().parts();
        typed(parts);
        auto next = model::TrackerDocument::assemble(std::move(parts));
        ASSERT_TRUE(next) << next.error().message;
        m_Typed.update(*std::move(next));
        check();
    }

    /// A new document for both, as opening another file or Revert installs one; the histories are cleared.
    void replace(LegacyProject project)
    {
        m_Project = std::move(project);
        m_Edits.clear();
        m_Typed.assign(motrack::typedDocumentFromReduced(m_Project, std::nullopt));
        check();
    }

  private:
    LegacyProject m_Project;
    TrackEdits m_Edits;
    TypedEdits m_Typed;
};

// Typed counterparts of the out-of-band changes that track_edits_test.cpp makes to a LegacyProject.

model::ObjectSlot& slotOf(model::DocumentParts& parts, model::ObjectId id)
{
    return *std::ranges::find(parts.objects, id, &model::ObjectSlot::id);
}

template <typename State> void editSingleton(model::DocumentParts& parts, model::Role role, const std::function<void(State&)>& edit)
{
    auto& slot = *std::ranges::find_if(
        parts.objects,
        [&](const model::ObjectSlot& object)
        { return object.role == role && object.state == model::ObjectState::Live && std::holds_alternative<State>(*object.value); });
    auto state = std::get<State>(*slot.value);
    edit(state);
    slot.value = std::make_shared<const model::TypedValue>(std::move(state));
}

PartsEdit frameDuration(std::optional<double> duration)
{
    return [duration](model::DocumentParts& parts)
    {
        editSingleton<model::ClipControlState>(parts,
                                               model::Role::ClipControl,
                                               [&](auto& control)
                                               { control.deltaT = duration ? model::Read<double>(*duration) : model::missing<double>(); });
    };
}

PartsEdit clipState(const std::function<void(model::ClipState&)>& edit)
{
    return [edit](model::DocumentParts& parts)
    {
        editSingleton<model::ClipState>(parts, model::Role::Clip, edit);
    };
}

/// Swaps the first and last point masses (or vectors) in the track list.
PartsEdit swapEnds(model::Family family)
{
    return [family](model::DocumentParts& parts)
    {
        std::vector<std::size_t> positions;
        for (std::size_t index = 0; index < parts.trackOrder.size(); ++index)
        {
            if (slotOf(parts, parts.trackOrder[index]).family() == family)
            {
                positions.push_back(index);
            }
        }
        std::swap(parts.trackOrder[positions.front()], parts.trackOrder[positions.back()]);
    };
}

/// Takes a created track out of the document, as erasing it from the project does.
PartsEdit eraseTrack(motrack::TrackId id)
{
    return [id](model::DocumentParts& parts)
    {
        std::erase(parts.trackOrder, model::ObjectId{id});
        std::erase_if(parts.objects, [&](const model::ObjectSlot& object) { return object.id == model::ObjectId{id}; });
    };
}

/// Adds a point mass at the end of the point masses, as pushing it onto the project does.
PartsEdit appendPointMass(const motrack::PointMass& added)
{
    // Shared, so that moving the edit cannot throw.
    return [shared = std::make_shared<const motrack::PointMass>(added)](model::DocumentParts& parts)
    {
        const auto& track = *shared;
        model::PointMassState state{.name = track.name, .mass = track.mass};
        std::vector<std::int32_t> keys;
        for (const auto& mark : track.marks)
        {
            state.marks.set(mark.frame, {.value = {.x = mark.x, .y = mark.y}});
            keys.push_back(mark.frame);
        }
        if (!keys.empty())
        {
            state.keyFrames = keys;
        }
        parts.objects.push_back({.id = model::ObjectId{track.id},
                                 .range = model::AllocationRange::Counter,
                                 .role = model::Role::Track,
                                 .className = std::string(motrack::tracker::POINT_MASS_CLASS),
                                 .value = std::make_shared<const model::TypedValue>(std::move(state))});
        const auto last = std::ranges::find_if(parts.trackOrder,
                                               [&](model::ObjectId id) { return slotOf(parts, id).family() != model::Family::PointMass; });
        parts.trackOrder.insert(last, model::ObjectId{track.id});
        parts.nextObjectId = std::max(parts.nextObjectId, track.id + 1);
    };
}

PartsEdit appendVector(const motrack::VectorTrack& added)
{
    return [shared = std::make_shared<const motrack::VectorTrack>(added)](model::DocumentParts& parts)
    {
        const auto& track = *shared;
        model::VectorState state{.name = track.name};
        for (const auto& sample : track.samples)
        {
            state.samples.set(
                sample.frame,
                {.value = {
                     .xTail = sample.tailX, .yTail = sample.tailY, .xComponent = sample.componentX, .yComponent = sample.componentY}});
        }
        parts.objects.push_back({.id = model::ObjectId{track.id},
                                 .range = model::AllocationRange::Counter,
                                 .role = model::Role::Track,
                                 .className = std::string(motrack::tracker::VECTOR_CLASS),
                                 .value = std::make_shared<const model::TypedValue>(std::move(state))});
        parts.trackOrder.push_back(model::ObjectId{track.id});
    };
}

/// The unsupported tracks become these classes: placeholders, as a native document without a source holds them.
PartsEdit unsupportedTracks(std::vector<std::string> classes)
{
    return [classes = std::move(classes)](model::DocumentParts& parts)
    {
        std::erase_if(parts.objects, [](const model::ObjectSlot& object) { return object.family() == model::Family::Unknown; });
        for (const auto& type : classes)
        {
            parts.objects.push_back({.id = {},
                                     .role = model::Role::Track,
                                     .className = type,
                                     .value = std::make_shared<const model::TypedValue>(model::UnknownState{})});
        }
    };
}

TEST(HistoryParity, KeepsIndependentSaveTargetsAcrossUndoRedoAndNewHistoryBranches)
{
    Twin twin;
    ASSERT_TRUE(twin.createPointMass("mass"));
    twin.markSaved();
    twin.markSaved(motrack::SaveTarget::Tracker);
    ASSERT_TRUE(twin.setPointMassValue(0, 2));
    twin.markSaved();
    EXPECT_FALSE(twin.isModified());
    EXPECT_TRUE(twin.isModified(motrack::SaveTarget::Tracker));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    twin.markSaved(motrack::SaveTarget::Tracker);
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.renamePointMass(0, "new branch"));
    EXPECT_TRUE(twin.isModified(motrack::SaveTarget::Tracker));
    twin.markSaved();
    twin.markSaved(motrack::SaveTarget::Tracker);
    ASSERT_TRUE(twin.createVector("vector"));
    ASSERT_TRUE(twin.undo());
    EXPECT_TRUE(twin.isModified());
    twin.clear();
    EXPECT_FALSE(twin.isModified(motrack::SaveTarget::Tracker));
}

TEST(HistoryParity, PointMassCommandsFollowIdentityAfterTheTrackOrderChanges)
{
    Twin twin;
    ASSERT_TRUE(twin.createPointMass("A"));
    ASSERT_TRUE(twin.createPointMass("B"));
    const auto a = twin.project().pointMasses.front().id;
    const auto b = twin.project().pointMasses.back().id;
    const auto swap = [&]
    {
        twin.outOfBand([](auto& p) { std::swap(p.pointMasses.front(), p.pointMasses.back()); }, swapEnds(model::Family::PointMass));
    };
    ASSERT_TRUE(twin.markPointMass(1, {.frame = 3, .x = 99, .y = 99}));
    twin.clear();
    ASSERT_TRUE(twin.setPointMassValue(0, 3.5));
    ASSERT_TRUE(twin.renamePointMass(0, "Target"));
    ASSERT_TRUE(twin.markPointMass(0, {.frame = 3, .x = 7, .y = 8}));
    swap();
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    EXPECT_EQ(twin.project().pointMasses.back().id, a);
    ASSERT_TRUE(twin.removePointMass(1));
    ASSERT_TRUE(twin.undo());
    swap();
    ASSERT_TRUE(twin.redo());
    EXPECT_EQ(twin.project().pointMasses.front().id, b);
    twin.clear();
    ASSERT_TRUE(twin.createPointMass("C"));
    swap();
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
}

TEST(HistoryParity, VectorCommandsFollowIdentityAfterTheTrackOrderChanges)
{
    Twin twin;
    ASSERT_TRUE(twin.createVector("A"));
    ASSERT_TRUE(twin.createVector("B"));
    const auto b = twin.project().vectors.back().id;
    const auto swap = [&]
    {
        twin.outOfBand([](auto& p) { std::swap(p.vectors.front(), p.vectors.back()); }, swapEnds(model::Family::Vector));
    };
    ASSERT_TRUE(twin.markVector(1, {.frame = 3, .tailX = 99, .tailY = 0, .componentX = 1, .componentY = 2}));
    twin.clear();
    ASSERT_TRUE(twin.renameVector(0, "Target"));
    ASSERT_TRUE(twin.markVector(0, {.frame = 3, .tailX = 7, .tailY = 8, .componentX = 1, .componentY = 2}));
    swap();
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.removeVector(1));
    ASSERT_TRUE(twin.undo());
    swap();
    ASSERT_TRUE(twin.redo());
    EXPECT_EQ(twin.project().vectors.front().id, b);
    twin.clear();
    ASSERT_TRUE(twin.createVector("C"));
    swap();
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
}

TEST(HistoryParity, RejectsMissingAndRestoredTargetsWithoutAdvancingHistory)
{
    Twin twin;
    ASSERT_TRUE(twin.createPointMass("A"));
    ASSERT_TRUE(twin.createPointMass("B"));
    ASSERT_TRUE(twin.createVector("V"));
    twin.clear();
    ASSERT_TRUE(twin.markPointMass(0, {.frame = 2, .x = 7, .y = 8}));
    const auto revision = twin.revision();
    const auto a = twin.project().pointMasses.front().id;
    // Track A taken out of, and put back into, both documents outside history. TrackEdits' test also hides it by
    // setting its identity to zero, which a typed document cannot hold; taking it out is the typed equivalent.
    const auto take = [&]
    {
        const auto track = *std::ranges::find(twin.project().pointMasses, a, &motrack::PointMass::id);
        twin.outOfBand([&](auto& p) { std::erase_if(p.pointMasses, [&](const auto& mass) { return mass.id == a; }); }, eraseTrack(a));
        return track;
    };
    const auto put = [&](const motrack::PointMass& track)
    {
        twin.outOfBand([&](auto& p) { p.pointMasses.push_back(track); }, appendPointMass(track));
    };

    auto removed = take();
    EXPECT_FALSE(twin.undo());
    EXPECT_EQ(twin.revision(), revision);
    EXPECT_TRUE(twin.canUndo());
    put(removed);
    // TrackEdits also refuses an ambiguous target (a vector given the same identity). A typed document cannot hold
    // one: assembling it is refused.
    {
        auto parts = twin.document().parts();
        slotOf(parts, model::ObjectId{twin.project().vectors.front().id}).id = model::ObjectId{a};
        EXPECT_FALSE(model::TrackerDocument::assemble(parts));
    }
    ASSERT_TRUE(twin.undo());
    removed = take();
    EXPECT_FALSE(twin.redo());
    EXPECT_FALSE(twin.renamePointMass(1, "invalid"));
    EXPECT_TRUE(twin.canRedo());
    put(removed);
    ASSERT_TRUE(twin.redo());
    EXPECT_EQ(twin.project().pointMasses.back().marks.front().x, 7);
    const auto marked = twin.project().pointMasses.back();
    ASSERT_TRUE(twin.removePointMass(1));
    put(marked); // Another owner already restored this identity.
    const auto removedRevision = twin.revision();
    EXPECT_FALSE(twin.undo());
    EXPECT_EQ(twin.revision(), removedRevision);
    take();
    ASSERT_TRUE(twin.undo());
    twin.clear();
    // TrackEdits refuses creation when its counter collides with a track's identity; a typed document whose counter
    // does not exceed every counter identity cannot be assembled.
    {
        auto parts = twin.document().parts();
        parts.nextObjectId = twin.project().vectors.front().id;
        EXPECT_FALSE(model::TrackerDocument::assemble(parts));
    }
    ASSERT_TRUE(twin.renameVector(0, "renamed"));
    const auto vector = twin.project().vectors.front();
    twin.outOfBand([](auto& p) { p.vectors.clear(); }, eraseTrack(vector.id));
    EXPECT_FALSE(twin.undo());
    twin.outOfBand([&](auto& p) { p.vectors.push_back(vector); }, appendVector(vector));
    ASSERT_TRUE(twin.undo());
    EXPECT_EQ(twin.project().vectors.front().name, "V");
}

TEST(HistoryParity, KeepsDistinctIdentitiesAcrossRenameDeleteUndoRedoAndNewHistory)
{
    Twin twin;
    ASSERT_TRUE(twin.createPointMass("same"));
    ASSERT_TRUE(twin.createPointMass("same"));
    ASSERT_TRUE(twin.createVector("same"));
    const auto vector = twin.project().vectors.at(0).id;
    ASSERT_TRUE(twin.renamePointMass(1, "renamed"));
    ASSERT_TRUE(twin.removePointMass(0));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    const auto counter = twin.project().nextTrackId;
    ASSERT_TRUE(twin.redo());
    EXPECT_EQ(twin.project().vectors.front().id, vector);
    EXPECT_EQ(twin.project().nextTrackId, counter);
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.createVector("replacement"));
    EXPECT_GT(twin.project().vectors.front().id, vector);
    EXPECT_FALSE(twin.canRedo());
    twin.clear();
    EXPECT_NO_THROW(motrack::validateTrackIdentities(twin.project()));
}

TEST(HistoryParity, ExhaustedIdentitySpaceRejectsCreationWithoutChangingHistory)
{
    // TrackEdits exhausts at 2^64 - 1; the typed counter stops below the countdown low-water mark, so its capacity is
    // smaller by the number of countdown identities (docs/tracker-document.md, "Identities"). Both are replayed at their
    // own last identity.
    LegacyProject project;
    project.nextTrackId = std::numeric_limits<motrack::TrackId>::max() - 1;
    TrackEdits edits;
    auto typedParts = motrack::typedDocumentFromReduced(LegacyProject{}, std::nullopt).parts();
    typedParts.nextObjectId = typedParts.countdownLowWater - 1;
    TypedEdits typed(*model::TrackerDocument::assemble(typedParts));
    ASSERT_TRUE(edits.createPointMass(project, "last"));
    ASSERT_TRUE(typed.createPointMass("last"));
    const auto revision = edits.revision();
    EXPECT_FALSE(edits.createPointMass(project, "overflow"));
    EXPECT_FALSE(typed.createPointMass("overflow"));
    EXPECT_FALSE(edits.createVector(project, "overflow"));
    EXPECT_FALSE(typed.createVector("overflow"));
    EXPECT_EQ(edits.revision(), revision);
    EXPECT_EQ(typed.history().revision(), revision);
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(typed.undo());
    EXPECT_FALSE(edits.createVector(project, "reused"));
    EXPECT_FALSE(typed.createVector("reused"));
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(typed.redo());
    EXPECT_EQ(project.pointMasses.front().id, std::numeric_limits<motrack::TrackId>::max() - 1);
    EXPECT_EQ(typed.view().pointMasses.front().id, typedParts.nextObjectId);
    EXPECT_FALSE(typed.document().hasCounterCapacity());
}

TEST(HistoryParity, UndoingCreationStillNeedsToSaveTheAdvancedIdentityCounter)
{
    for (const bool vector : {false, true})
    {
        Twin twin;
        twin.markSaved();
        ASSERT_TRUE(vector ? twin.createVector("A") : twin.createPointMass("A"));
        ASSERT_TRUE(twin.undo());
        EXPECT_TRUE(twin.isModified());
        twin.markSaved();
        ASSERT_TRUE(twin.redo());
        ASSERT_TRUE(twin.undo());
        EXPECT_FALSE(twin.isModified());
    }
}

TEST(HistoryParity, ChangesRevisionForEditsUndoRedoAndDocumentReset)
{
    Twin twin;
    twin.createPointMass("A");
    twin.markSaved();
    EXPECT_FALSE(twin.removePointMass(5));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    twin.clear();
    twin.invalidateSaved(motrack::SaveTarget::Tracker);
}

TEST(HistoryParity, InvalidatesASavedCheckpointWithoutChangingTheProjectRevision)
{
    Twin twin;
    twin.createPointMass("A");
    twin.markSaved();
    twin.createPointMass("B");
    twin.invalidateSaved();
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    twin.markSaved();
    EXPECT_FALSE(twin.isModified());
}

TEST(HistoryParity, EditsVectorsAndRestoresSamplesAndTrackOrder)
{
    Twin twin;
    EXPECT_FALSE(twin.createVector(""));
    ASSERT_TRUE(twin.createVector("A"));
    ASSERT_TRUE(twin.createVector("B"));
    EXPECT_FALSE(twin.createVector("A"));
    EXPECT_FALSE(twin.renameVector(4, "C"));
    EXPECT_FALSE(twin.renameVector(0, ""));
    EXPECT_FALSE(twin.renameVector(0, "B"));
    ASSERT_TRUE(twin.renameVector(1, "C"));
    ASSERT_TRUE(twin.markVector(0, {.frame = 2, .tailX = 10, .tailY = 20, .componentX = 3, .componentY = -4}));
    ASSERT_TRUE(twin.markVector(0, {.frame = 0, .tailX = 1, .tailY = 2, .componentX = 0, .componentY = 0}));
    twin.markSaved();
    ASSERT_TRUE(twin.markVector(0, {.frame = 0, .tailX = 1, .tailY = 2, .componentX = 0, .componentY = 0}));
    EXPECT_FALSE(twin.isModified());
    ASSERT_TRUE(twin.markVector(0, {.frame = 0, .tailX = 2, .tailY = 3, .componentX = -5, .componentY = 6}));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    EXPECT_FALSE(twin.removeVectorSample(8, 0));
    EXPECT_FALSE(twin.removeVectorSample(0, 1));
    ASSERT_TRUE(twin.removeVectorSample(0, 0));
    ASSERT_TRUE(twin.undo());
    EXPECT_FALSE(twin.removeVector(8));
    ASSERT_TRUE(twin.removeVector(0));
    ASSERT_TRUE(twin.markVector(0, {.frame = 9, .tailX = 0, .tailY = 0, .componentX = 1, .componentY = 1}));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    ASSERT_EQ(twin.project().vectors.size(), 2u);
    while (twin.undo())
    {
    }
    EXPECT_TRUE(twin.project().vectors.empty());
    while (twin.redo())
    {
    }
    EXPECT_EQ(twin.project().vectors.front().samples.front().frame, 9);
}

TEST(HistoryParity, RejectsNonfiniteOrOverflowingVectorEndpointsWithoutAddingHistory)
{
    LegacyProject project;
    project.vectors = {{.name = "Vector", .samples = {}, .id = motrack::allocateTrackId(project)}};
    Twin twin(project);
    const motrack::VectorSample valid{.frame = 0, .tailX = 1, .tailY = 2, .componentX = 3, .componentY = 4};
    EXPECT_FALSE(twin.markVector(1, valid));
    auto invalid = valid;
    invalid.frame = -1;
    EXPECT_FALSE(twin.markVector(0, invalid));
    for (auto member : {&motrack::VectorSample::tailX,
                        &motrack::VectorSample::tailY,
                        &motrack::VectorSample::componentX,
                        &motrack::VectorSample::componentY})
    {
        invalid = valid;
        invalid.*member = std::numeric_limits<double>::infinity();
        EXPECT_FALSE(twin.markVector(0, invalid));
        invalid.*member = std::numeric_limits<double>::quiet_NaN();
        EXPECT_FALSE(twin.markVector(0, invalid));
    }
    invalid = valid;
    invalid.tailX = invalid.componentX = std::numeric_limits<double>::max();
    EXPECT_FALSE(twin.markVector(0, invalid));
    invalid = valid;
    invalid.tailY = invalid.componentY = std::numeric_limits<double>::max();
    EXPECT_FALSE(twin.markVector(0, invalid));
    EXPECT_FALSE(twin.canUndo());
    EXPECT_FALSE(twin.isModified());
}

TEST(HistoryParity, RestoresDeletedTracksAndMarksInTheirOriginalOrder)
{
    LegacyProject project;
    project.pointMasses = {{.name = "First", .marks = {{.frame = 0, .x = 3, .y = 4}}, .id = motrack::allocateTrackId(project)},
                           {.name = "Second", .marks = {{.frame = 2, .x = 7, .y = 8}}, .id = motrack::allocateTrackId(project)}};
    Twin twin(project);
    EXPECT_FALSE(twin.renamePointMass(0, "Second"));
    EXPECT_FALSE(twin.renamePointMass(0, ""));
    EXPECT_FALSE(twin.removePointMass(2));
    EXPECT_FALSE(twin.removeMark(2, 0));
    EXPECT_FALSE(twin.removeMark(0, 3));
    ASSERT_TRUE(twin.removeMark(0, 0));
    ASSERT_TRUE(twin.renamePointMass(1, "Target"));
    ASSERT_TRUE(twin.removePointMass(0));
    ASSERT_TRUE(twin.markPointMass(0, {.frame = 3, .x = 10, .y = 10}));
    for (int step = 0; step < 4; ++step)
    {
        ASSERT_TRUE(twin.undo());
    }
    EXPECT_EQ(twin.project().pointMasses.front().name, "First");
    for (int step = 0; step < 4; ++step)
    {
        ASSERT_TRUE(twin.redo());
    }
    EXPECT_EQ(twin.project().pointMasses.front().marks.back().frame, 3);
}

TEST(HistoryParity, ChangesMassWithUndoAndRejectsInvalidValues)
{
    Twin twin;
    EXPECT_FALSE(twin.setPointMassValue(0, 2.0));
    twin.createPointMass("Cart");
    EXPECT_FALSE(twin.setPointMassValue(0, 0.0));
    EXPECT_FALSE(twin.setPointMassValue(0, std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(twin.setPointMassValue(0, 2.5));
    EXPECT_TRUE(twin.setPointMassValue(0, 2.5));
    EXPECT_TRUE(twin.undo());
    EXPECT_TRUE(twin.redo());
}

TEST(HistoryParity, ChangesTheClipWithoutRenumberingMarks)
{
    Twin twin;
    EXPECT_FALSE(twin.setClipRange(0, std::numeric_limits<int>::max(), 1));
    const motrack::PointMass ball{.name = "Ball", .marks = {{.frame = 4, .x = 2, .y = 3}}, .id = twin.project().nextTrackId};
    twin.outOfBand(
        [&](auto& p)
        {
            p.clip.sourceFrameCount = 12;
            p.pointMasses.push_back(ball);
            ++p.nextTrackId;
        },
        [&](model::DocumentParts& parts)
        {
            parts.media.sourceFrameCount = 12;
            appendPointMass(ball)(parts);
        });
    EXPECT_FALSE(twin.setClipRange(7, 6, 1));
    EXPECT_FALSE(twin.setClipRange(1, 12, 1));
    EXPECT_TRUE(twin.setClipRange(1, 10, 3));
    EXPECT_FALSE(twin.setClipRange(1, 10, 3));
    EXPECT_TRUE(twin.undo());
    EXPECT_FALSE(twin.project().clip.stepCount.has_value());
    EXPECT_TRUE(twin.redo());
}

LegacyProject timedProject()
{
    LegacyProject project;
    project.clip.firstFrame = 2;
    project.clip.stepSize = 2;
    project.clip.stepCount = 5;
    project.pointMasses.push_back({.name = "Cart",
                                   .marks = {{.frame = 2, .x = 0, .y = 0},
                                             {.frame = 4, .x = 2, .y = 0},
                                             {.frame = 6, .x = 8, .y = 0},
                                             {.frame = 8, .x = 18, .y = 0},
                                             {.frame = 10, .x = 32, .y = 0}},
                                   .mass = 3,
                                   .id = motrack::allocateTrackId(project)});
    return project;
}

TEST(HistoryParity, ChangesTimingWithHistoryAndRescalesDerivedMotionAtOriginalFrames)
{
    Twin twin(timedProject());
    ASSERT_TRUE(twin.setClipTiming(500, -500));
    twin.markSaved();
    EXPECT_FALSE(twin.setClipTiming(500, -500));
    ASSERT_TRUE(twin.setClipTiming(250, -500));
    EXPECT_DOUBLE_EQ(
        motrack::measurementPlot(motrack::legacyProject(twin.document()), 0, motrack::PlotQuantity::VelocityX).samples.at(0).vertical, 8.0);
    ASSERT_TRUE(twin.undo());
    EXPECT_FALSE(twin.isModified());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    const auto media = [&](std::vector<std::string> references)
    {
        twin.outOfBand([&](auto& p) { p.clip.mediaReferences = references; },
                       [&](model::DocumentParts& parts) { parts.media.references = references; });
    };
    media({"movie.mp4"});
    EXPECT_FALSE(twin.undo());
    EXPECT_TRUE(twin.canUndo());
    media({});
    ASSERT_TRUE(twin.undo());
    EXPECT_EQ(twin.project().clip.frameDurationMs, std::optional<double>(500));
}

TEST(HistoryParity, TimeOriginDoesNotDistortConstantIntervalDerivatives)
{
    LegacyProject project;
    project.clip.stepCount = 5;
    motrack::PointMass linear{.name = "linear", .mass = 3, .id = motrack::allocateTrackId(project)};
    for (int frame = 0; frame < 5; ++frame)
    {
        linear.marks.push_back({.frame = frame, .x = static_cast<double>(frame), .y = 2.0 * frame});
    }
    project.pointMasses.push_back(linear);
    Twin twin(project);
    ASSERT_TRUE(twin.setClipTiming(3, 0));
    for (const auto origin : {0.0, -1e16, 1e16})
    {
        twin.outOfBand([&](auto& p) { p.clip.startTimeMs = origin; }, clipState([&](auto& clip) { clip.startTime = origin; }));
        const auto plot = motrack::measurementPlot(motrack::legacyProject(twin.document()), 0, motrack::PlotQuantity::VelocityX);
        ASSERT_EQ(plot.samples.size(), 3u);
        EXPECT_NEAR(plot.samples.at(1).vertical, 1000.0 / 3.0, 1e-8);
    }
}

TEST(HistoryParity, RangeChangesRefuseOverflowingTimingWithoutLosingRedo)
{
    LegacyProject project;
    project.clip.stepCount = 1;
    Twin twin(project);
    ASSERT_TRUE(twin.setClipTiming(1e308, 0));
    twin.markSaved();
    ASSERT_TRUE(twin.setTimeUnit("ms"));
    ASSERT_TRUE(twin.undo());
    EXPECT_FALSE(twin.setClipRange(0, 2, 1));
    EXPECT_FALSE(twin.isModified());
    EXPECT_TRUE(twin.canRedo());
}

TEST(HistoryParity, TimingAndRangeHistoryValidateChangedCounterpartsButRestoreLegacyStates)
{
    LegacyProject legacy;
    legacy.clip.stepCount = 3;
    legacy.clip.frameDurationMs = 1e308; // A legacy overflowing timebase can be repaired and restored.
    Twin twin(legacy);
    const auto duration = [&](double value)
    {
        twin.outOfBand([&](auto& p) { p.clip.frameDurationMs = value; }, frameDuration(value));
    };
    const auto steps = [&](int count)
    {
        twin.outOfBand([&](auto& p) { p.clip.stepCount = count; }, clipState([&](auto& clip) { clip.stepCount = count; }));
    };
    ASSERT_TRUE(twin.setClipRange(0, 0, 1));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    duration(9e307); // Out-of-band timing change must be checked during range undo.
    EXPECT_FALSE(twin.undo());
    duration(1e308);
    ASSERT_TRUE(twin.undo());
    twin.outOfBand(
        [](auto& p)
        {
            p.clip.frameDurationMs = 1e308;
            p.clip.startTimeMs = 1e308;
        },
        clipState([](auto& clip) { clip.startTime = 1e308; }));
    EXPECT_FALSE(twin.redo());

    LegacyProject single;
    single.clip.stepCount = 1;
    twin.replace(single);
    ASSERT_TRUE(twin.setClipTiming(1e308, 0));
    ASSERT_TRUE(twin.undo());
    steps(3);
    EXPECT_FALSE(twin.redo());
    steps(1);
    ASSERT_TRUE(twin.redo());
    steps(3);
    ASSERT_TRUE(twin.undo()); // Omitted legacy timing remains exactly omitted.
    EXPECT_FALSE(twin.project().clip.frameDurationMs);

    twin.replace(legacy);
    ASSERT_TRUE(twin.setClipTiming(100, 0));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    steps(4);
    EXPECT_FALSE(twin.undo());
}

TEST(HistoryParity, RefusesInvalidOrUnrepresentableTimingWithoutHistoryChanges)
{
    LegacyProject original;
    original.clip.stepCount = 10;
    Twin twin(original);
    for (const auto duration : {0.0,
                                -1.0,
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::denorm_min()})
    {
        EXPECT_FALSE(twin.setClipTiming(duration, 0));
    }
    for (const auto origin : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1e20})
    {
        EXPECT_FALSE(twin.setClipTiming(1, origin));
    }
    EXPECT_FALSE(twin.setClipTiming(1e308, 1e308));
    EXPECT_FALSE(twin.setClipTiming(3, -1e16));
    EXPECT_FALSE(twin.setClipTiming(3, 1e16));
    EXPECT_FALSE(twin.canUndo());
    // Each change to the original clip, outside history. A negative first frame has no typed form (Tracker's loader
    // keeps the clip at or after frame 0), so it is not replayed.
    struct Change
    {
        std::function<void(motrack::Clip&)> legacy;
        PartsEdit typed;
    };
    const std::vector<Change> changes{
        {.legacy = [](auto& clip) { clip.stepCount.reset(); },
         .typed = clipState([](auto& clip) { clip.stepCount = model::missing<std::int32_t>(); })},
        {.legacy = [](auto& clip) { clip.stepCount = 0; }, .typed = clipState([](auto& clip) { clip.stepCount = 0; })},
        {.legacy = [](auto& clip) { clip.stepSize = 0; }, .typed = clipState([](auto& clip) { clip.stepSize = 0; })},
        {.legacy = [](auto& clip) { clip.stepCount = 500002; }, .typed = clipState([](auto& clip) { clip.stepCount = 500002; })},
        {.legacy =
             [](auto& clip)
         {
             clip.firstFrame = 2;
             clip.stepCount = 1;
             clip.stepSize = std::numeric_limits<int>::max();
         },
         .typed = clipState(
             [](auto& clip)
             {
                 clip.startFrame = 2;
                 clip.stepCount = 1;
                 clip.stepSize = std::numeric_limits<std::int32_t>::max();
             })},
        {.legacy = [](auto& clip) { clip.sourceFrameCount = 3; },
         .typed = [](model::DocumentParts& parts) { parts.media.sourceFrameCount = 3; }},
        {.legacy = [](auto& clip) { clip.mediaReferences = {"frames/one.png"}; },
         .typed = [](model::DocumentParts& parts) { parts.media.references = {"frames/one.png"}; }}};
    for (const auto& change : changes)
    {
        twin.replace(original);
        twin.outOfBand([&](auto& p) { change.legacy(p.clip); }, change.typed);
        EXPECT_FALSE(twin.setClipTiming(250, -500));
        EXPECT_FALSE(twin.canUndo());
    }
    twin.replace(original);
    twin.outOfBand(
        [](auto& p)
        {
            p.clip.firstFrame = 500000;
            p.clip.stepCount = 1;
        },
        clipState(
            [](auto& clip)
            {
                clip.startFrame = 500000;
                clip.stepCount = 1;
            }));
    EXPECT_TRUE(twin.setClipTiming(250, -500));
    twin.outOfBand(
        [](auto& p)
        {
            p.clip.firstFrame = 0;
            p.clip.stepCount = 500001;
        },
        clipState(
            [](auto& clip)
            {
                clip.startFrame = 0;
                clip.stepCount = 500001;
            }));
    EXPECT_TRUE(twin.setClipTiming(1000.0 / 29.97, -500)); // Ordinary long clips retain their intervals.
}

TEST(HistoryParity, RefusesUnsupportedTimingProvidersWithoutDiscardingHistory)
{
    for (const auto* type :
         {"DynamicParticle", "DynamicParticlePolar", "DynamicSystem", "AnalyticParticle", "ParticleDataTrack", "UnknownTrack"})
    {
        SCOPED_TRACE(type);
        LegacyProject project;
        project.clip.stepCount = 10;
        project.unsupportedTrackTypes = {std::string("org.opensourcephysics.cabrillo.tracker.") + type};
        Twin twin(project);
        ASSERT_TRUE(twin.setMassUnit("g"));
        twin.markSaved();
        ASSERT_TRUE(twin.setTimeUnit("ms"));
        ASSERT_TRUE(twin.undo());
        EXPECT_FALSE(twin.setClipTiming(250, -500));
        EXPECT_FALSE(twin.isModified());
        ASSERT_TRUE(twin.redo());
        EXPECT_EQ(twin.project().timeUnit, "ms");
    }
}

TEST(HistoryParity, TimingHistoryRefusesNewUnsupportedProvidersAndRetainsItsPosition)
{
    LegacyProject project;
    project.clip.stepCount = 10;
    const std::vector<std::string> allowed{"org.opensourcephysics.cabrillo.tracker.CoordAxes",
                                           "org.opensourcephysics.cabrillo.tracker.Protractor"};
    project.unsupportedTrackTypes = allowed;
    Twin twin(project);
    const auto providers = [&](std::vector<std::string> classes)
    {
        twin.outOfBand([&](auto& p) { p.unsupportedTrackTypes = classes; }, unsupportedTracks(classes));
    };
    ASSERT_TRUE(twin.setClipTiming(250, -500));
    twin.markSaved();
    auto withData = allowed;
    withData.emplace_back("org.opensourcephysics.cabrillo.tracker.ParticleDataTrack");
    providers(withData);
    EXPECT_FALSE(twin.undo());
    EXPECT_FALSE(twin.isModified());
    providers(allowed);
    ASSERT_TRUE(twin.undo());
    auto withModel = allowed;
    withModel.emplace_back("org.opensourcephysics.cabrillo.tracker.DynamicParticle");
    providers(withModel);
    EXPECT_FALSE(twin.redo());
    EXPECT_TRUE(twin.isModified());
    providers(allowed);
    ASSERT_TRUE(twin.redo());
    EXPECT_FALSE(twin.isModified());
}

TEST(HistoryParity, PlacesAxesWithoutChangingAnisotropicScale)
{
    LegacyProject project;
    project.lengthUnit = "m";
    project.calibration =
        motrack::Calibration{.keyframes = {{.frame = 0, .originX = 0, .originY = 0, .angleDegrees = 0, .scaleX = 2, .scaleY = 4}},
                             .fixedOrigin = false,
                             .fixedAngle = true,
                             .fixedScale = true};
    Twin twin(project);
    EXPECT_FALSE(twin.setCoordinateAxes({.frame = 5, .originX = 10, .originY = 20, .positiveX = 10, .positiveY = 20}));
    ASSERT_TRUE(twin.setCoordinateAxes({.frame = 5, .originX = 10, .originY = 20, .positiveX = 12, .positiveY = 16}));
    EXPECT_EQ(twin.project().calibration.value_or(motrack::Calibration{}).keyframes.size(), 2u);
    EXPECT_TRUE(twin.undo());
    twin.outOfBand(
        [](auto& p) { p.calibration.reset(); },
        [](model::DocumentParts& parts)
        { std::erase_if(parts.objects, [](const model::ObjectSlot& object) { return object.role == model::Role::Coordinates; }); });
    ASSERT_TRUE(twin.setCoordinateAxes({.frame = 0, .originX = 2, .originY = 2, .positiveX = 2, .positiveY = 1}));
    EXPECT_EQ(twin.project().lengthUnit, "px");
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
}

TEST(HistoryParity, InsertsReplacesAndRestoresMarksInFrameOrder)
{
    Twin twin;
    EXPECT_FALSE(twin.undo());
    EXPECT_FALSE(twin.redo());
    EXPECT_FALSE(twin.markPointMass(0, {.frame = 0, .x = 1, .y = 2}));
    twin.createPointMass("Ball");
    EXPECT_FALSE(twin.markPointMass(0, {.frame = -1, .x = 1, .y = 2}));
    EXPECT_FALSE(twin.markPointMass(0, {.frame = 0, .x = std::numeric_limits<double>::infinity(), .y = 2}));
    EXPECT_TRUE(twin.markPointMass(0, {.frame = 2, .x = 3, .y = 4}));
    EXPECT_TRUE(twin.markPointMass(0, {.frame = 0, .x = 1, .y = 2}));
    EXPECT_TRUE(twin.markPointMass(0, {.frame = 0, .x = 7, .y = 8}));
    EXPECT_TRUE(twin.markPointMass(0, {.frame = 0, .x = 7, .y = 8}));
    for (int step = 0; step < 4; ++step)
    {
        EXPECT_TRUE(twin.undo());
    }
    EXPECT_FALSE(twin.canUndo());
    EXPECT_TRUE(twin.redo());
    EXPECT_TRUE(twin.redo());
    EXPECT_TRUE(twin.markPointMass(0, {.frame = 4, .x = 9, .y = 10}));
    EXPECT_FALSE(twin.canRedo());
    twin.clear();
}

TEST(HistoryParity, MassUnitHistoryInterleavesWithMassValuesWithoutRescaling)
{
    Twin twin;
    ASSERT_TRUE(twin.createPointMass("A"));
    ASSERT_TRUE(twin.setPointMassValue(0, 2.5));
    twin.clear();
    EXPECT_FALSE(twin.setMassUnit(""));
    EXPECT_TRUE(twin.setMassUnit(twin.project().massUnit));
    ASSERT_TRUE(twin.setMassUnit("g"));
    twin.markSaved();
    ASSERT_TRUE(twin.setPointMassValue(0, 4));
    ASSERT_TRUE(twin.setMassUnit("mg"));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    EXPECT_FALSE(twin.isModified());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.setMassUnit("tonne"));
    EXPECT_FALSE(twin.canRedo());
}

TEST(HistoryParity, TimeLabelHistoryPreservesTimingMarksAndMassEdits)
{
    LegacyProject project;
    project.clip.frameDurationMs = 125;
    project.clip.startTimeMs = -750;
    Twin twin(project);
    ASSERT_TRUE(twin.createPointMass("A"));
    ASSERT_TRUE(twin.markPointMass(0, {.frame = 2, .x = 4, .y = 5}));
    twin.clear();
    EXPECT_FALSE(twin.setTimeUnit(""));
    EXPECT_TRUE(twin.setTimeUnit("s"));
    ASSERT_TRUE(twin.setTimeUnit("tick"));
    twin.markSaved();
    ASSERT_TRUE(twin.setPointMassValue(0, 2.5));
    ASSERT_TRUE(twin.setTimeUnit("beat"));
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.redo());
    ASSERT_TRUE(twin.undo());
    ASSERT_TRUE(twin.setTimeUnit("cycle"));
    EXPECT_FALSE(twin.canRedo());
}

TEST(HistoryParity, CreatesDistanceCalibrationAndRestoresThePriorUnit)
{
    LegacyProject project;
    project.lengthUnit = "m";
    Twin twin(project);
    motrack::CalibrationDistance distance{
        .frame = 0, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 2, .defaultOriginX = 10, .defaultOriginY = 20, .unit = "cm"};
    EXPECT_TRUE(twin.calibrateDistance(distance));
    EXPECT_TRUE(twin.undo());
    EXPECT_FALSE(twin.project().calibration.has_value());
    EXPECT_TRUE(twin.redo());
    // Standard export refuses a label Tracker cannot store, so the edit refuses it too.
    for (const auto* unit : {"", " cm", "cm\t", "m2", "\xE2\x80\x8Bm", "\xFF"})
    {
        SCOPED_TRACE(unit);
        distance.unit = unit;
        EXPECT_FALSE(twin.calibrateDistance(distance));
    }
    distance.unit = "\xC2\xB5m";
    EXPECT_TRUE(twin.calibrateDistance(distance));
    EXPECT_TRUE(twin.undo());
    EXPECT_TRUE(twin.undo());
    EXPECT_TRUE(twin.redo());
}

TEST(HistoryParity, ChangesScaleAtTheSelectedKeyframeAndPreservesAnisotropy)
{
    LegacyProject project;
    project.calibration =
        motrack::Calibration{.keyframes = {{.frame = 0, .originX = 10, .originY = 20, .angleDegrees = 15, .scaleX = 2, .scaleY = 4},
                                           {.frame = 10, .originX = 10, .originY = 20, .angleDegrees = 15, .scaleX = 8, .scaleY = 16}}};
    Twin twin(project);
    motrack::CalibrationDistance distance{
        .frame = 5, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 2, .defaultOriginX = 0, .defaultOriginY = 0, .unit = "m"};
    EXPECT_TRUE(twin.calibrateDistance(distance));
    distance.knownLength = 1;
    EXPECT_TRUE(twin.calibrateDistance(distance));
    ASSERT_EQ(twin.project().calibration.value_or(motrack::Calibration{}).keyframes.size(), 3u);
    EXPECT_TRUE(twin.undo());
    EXPECT_TRUE(twin.undo());
    EXPECT_TRUE(twin.redo());
}

TEST(HistoryParity, RejectsDegenerateCalibrationDistances)
{
    Twin twin;
    motrack::CalibrationDistance distance{
        .frame = 0, .imageDeltaX = 0, .imageDeltaY = 0, .knownLength = 1, .defaultOriginX = 0, .defaultOriginY = 0, .unit = "m"};
    EXPECT_FALSE(twin.calibrateDistance(distance));
    distance.knownLength = 0;
    EXPECT_FALSE(twin.calibrateDistance(distance));
    EXPECT_FALSE(twin.canUndo());
}

/// A committed Tracker file of the reference corpus.
std::string corpusFile(std::string_view name)
{
    std::ifstream stream(motrack::utf8Path(MOTRACK_CORPUS_FIXTURES) / motrack::utf8Path(name), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// The reorder and delete scenarios on tracks bound to a baseline: TrackEdits on B1's import, commands on the typed import.
/// Bound tracks keep their slots while removed, and every restoration returns where its history entry recorded it.
TEST(HistoryParity, BoundTracksFollowIdentityAndReturnWhereTheirEntriesRecordedThem)
{
    for (const auto* name : {"center-of-mass.trk", "independent-vectors.trk"})
    {
        SCOPED_TRACE(name);
        Twin twin(FromTracker{.xml = corpusFile(name)});
        ASSERT_EQ(twin.document().associations(), model::Associations::Known);
        const bool masses = !twin.project().pointMasses.empty();
        const auto family = masses ? model::Family::PointMass : model::Family::Vector;
        const auto count = [&]
        {
            return masses ? twin.project().pointMasses.size() : twin.project().vectors.size();
        };
        ASSERT_GE(count(), 2u);
        const auto id = [&](std::size_t index)
        {
            return masses ? twin.project().pointMasses.at(index).id : twin.project().vectors.at(index).id;
        };
        const auto swap = [&]
        {
            twin.outOfBand(
                [&](auto& p)
                {
                    if (masses)
                    {
                        std::swap(p.pointMasses.front(), p.pointMasses.back());
                    }
                    else
                    {
                        std::swap(p.vectors.front(), p.vectors.back());
                    }
                },
                swapEnds(family));
        };
        const auto remove = [&](std::size_t index)
        {
            return masses ? twin.removePointMass(index) : twin.removeVector(index);
        };
        const auto rename = [&](std::size_t index, const std::string& label)
        {
            return masses ? twin.renamePointMass(index, label) : twin.renameVector(index, label);
        };
        const auto mark = [&](std::size_t index, int frame, double x)
        {
            return masses ? twin.markPointMass(index, {.frame = frame, .x = x, .y = 2})
                          : twin.markVector(index, {.frame = frame, .tailX = x, .tailY = 2, .componentX = 3, .componentY = 4});
        };
        const auto first = id(0);

        // Removed at position 0, the track returns there after a reorder between its undo and redo.
        ASSERT_TRUE(remove(0));
        ASSERT_TRUE(twin.undo());
        swap();
        ASSERT_TRUE(twin.redo());
        ASSERT_TRUE(twin.undo());
        EXPECT_EQ(id(0), first);
        const auto* restored = twin.document().object(model::ObjectId{first});
        ASSERT_NE(restored, nullptr);
        EXPECT_TRUE(restored->binding);

        // Edits follow identity across a reorder.
        twin.clear();
        ASSERT_TRUE(rename(0, "Renamed"));
        ASSERT_TRUE(mark(0, 7, 11));
        swap();
        ASSERT_TRUE(twin.undo());
        ASSERT_TRUE(twin.undo());
        ASSERT_TRUE(twin.redo());
        ASSERT_TRUE(twin.redo());
        ASSERT_TRUE(remove(count() - 1));
        swap();
        ASSERT_TRUE(twin.undo());
        ASSERT_TRUE(twin.redo());

        // Every track deleted, then every step undone and redone.
        while (count() > 0)
        {
            ASSERT_TRUE(remove(0));
        }
        while (twin.canUndo())
        {
            ASSERT_TRUE(twin.undo());
        }
        while (twin.canRedo())
        {
            ASSERT_TRUE(twin.redo());
        }
    }
}

} // namespace
