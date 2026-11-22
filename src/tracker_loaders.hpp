#pragma once

#include "osp/expected.hpp"
#include "osp/source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Typed loaders for the Tracker classes MoTrack models: the TrackerPanel subset, VideoClip and its video,
/// ClipControl, ImageCoordSystem, PointMass and Vector. They read the lossless OSP XMLControl tree
/// (`motrack_osp`) the way Tracker's `XML.ObjectLoader`s read it, with OSP's property names, accessor
/// conversions (`getInt`, `getDouble`, `getBoolean`, `getString`, array `getObject`) and class-name remaps.
/// Every loaded object keeps its node, whose line span lets a writer patch it with `osp::Patch` and
/// `osp::render` while unedited bytes stay identical. Values are Java accessor results; MoTrack's import
/// policy (`LegacyProject`) and the writer's guards are applied by their callers. No Qt dependency.
namespace motrack::tracker
{

// Effective class names (after osp::effectiveClassName).
inline constexpr std::string_view TRACKER_PANEL_CLASS = "org.opensourcephysics.cabrillo.tracker.TrackerPanel";
inline constexpr std::string_view POINT_MASS_CLASS = "org.opensourcephysics.cabrillo.tracker.PointMass";
inline constexpr std::string_view POINT_MASS_FRAME_CLASS = "org.opensourcephysics.cabrillo.tracker.PointMass$FrameData";
inline constexpr std::string_view VECTOR_CLASS = "org.opensourcephysics.cabrillo.tracker.Vector";
inline constexpr std::string_view VECTOR_FRAME_CLASS = "org.opensourcephysics.cabrillo.tracker.Vector$FrameData";
inline constexpr std::string_view COORD_AXES_CLASS = "org.opensourcephysics.cabrillo.tracker.CoordAxes";
inline constexpr std::string_view VIDEO_CLIP_CLASS = "org.opensourcephysics.media.core.VideoClip";
inline constexpr std::string_view IMAGE_VIDEO_CLASS = "org.opensourcephysics.media.core.ImageVideo";
inline constexpr std::string_view XUGGLE_VIDEO_CLASS = "org.opensourcephysics.media.xuggle.XuggleVideo";
inline constexpr std::string_view STEPPER_CLIP_CONTROL_CLASS = "org.opensourcephysics.media.core.StepperClipControl";
inline constexpr std::string_view VIDEO_CLIP_CONTROL_CLASS = "org.opensourcephysics.media.core.VideoClipControl";
inline constexpr std::string_view COORD_SYSTEM_CLASS = "org.opensourcephysics.media.core.ImageCoordSystem";
inline constexpr std::string_view COORD_FRAME_CLASS = "org.opensourcephysics.media.core.ImageCoordSystem$FrameData";

/// Loader defaults of pinned Tracker 6.3.5 for properties a file does not save (#214).
/// TrackerPanel starts with `Tracker.preferredLengthUnit` (SI "m" without a preference file) and only replaces it
/// when the file contains `length_unit`. A media-free panel's StepperClipControl starts with a 100 ms frame duration;
/// ClipControl.Loader only replaces it with a saved non-zero `delta_t` (setFrameDuration ignores zero).
inline constexpr std::string_view DEFAULT_LENGTH_UNIT = "m";
inline constexpr double DEFAULT_STEPPER_FRAME_DURATION_MS = 100.0;

/// A Java accessor result: the decoded value, `Missing`, or the reason the conversion fails.
template <typename T> using Read = osp::Expected<T, osp::DecodeError>;

/// An `[n]` element of an object or string array, in file order.
struct ArrayElement
{
    osp::NodeId property{};
    /// Integer.parseInt of the text between '[' and the first ']' (XMLControlElement.arrayValue).
    Read<std::int32_t> index = 0;
    /// The element's object; absent for a null element or a text element.
    std::optional<osp::NodeId> object{};
};

/// PointMass$FrameData: `x`, `y` in image pixels.
struct PointMassFrameData
{
    Read<double> x = 0.0;
    Read<double> y = 0.0;
};

/// Vector$FrameData: tail position and components in image pixels.
struct VectorFrameData
{
    Read<double> xtail = 0.0;
    Read<double> ytail = 0.0;
    Read<double> xcomponent = 0.0;
    Read<double> ycomponent = 0.0;
};

/// ImageCoordSystem$FrameData: image origin, angle in degrees and pixels per world unit.
struct CoordFrameData
{
    Read<double> xorigin = 0.0;
    Read<double> yorigin = 0.0;
    Read<double> angle = 0.0;
    Read<double> xscale = 0.0;
    Read<double> yscale = 0.0;
};

template <typename Data> struct Frame
{
    ArrayElement element{};
    Data data{}; ///< Decoded only when `element.object` is present.
};

/// An object array such as `framedata`, with every element in file order (including nulls).
template <typename Data> struct FrameArray
{
    osp::NodeId property{};
    std::string className{}; ///< Array class attribute after the media remap.
    std::vector<Frame<Data>> frames{};
};

/// A string array element such as an ImageVideo `paths` entry.
struct StringElement
{
    ArrayElement element{};
    Read<std::string> value = std::string{};
};

/// The VideoClip's `video` child (ImageVideo, XuggleVideo or another Video class).
struct VideoLoad
{
    osp::NodeId object{};
    std::string className{};
    Read<std::string> path = std::string{};
    Read<double> deltaT = 0.0;
    std::optional<osp::NodeId> paths{}; ///< The `paths` property.
    std::vector<StringElement> pathElements{};
};

/// VideoClip.Loader fields.
struct VideoClipLoad
{
    osp::NodeId object{};
    std::string className{};
    Read<std::int32_t> startFrame = 0;
    Read<std::int32_t> stepSize = 0;
    Read<std::int32_t> stepCount = 0;
    Read<std::int32_t> frameCount = 0; ///< `video_framecount`
    Read<std::int32_t> frameShift = 0;
    Read<double> startTime = 0.0;
    std::optional<VideoLoad> video{};
};

/// ClipControl.Loader fields.
struct ClipControlLoad
{
    osp::NodeId object{};
    std::string className{};
    Read<double> rate = 0.0;
    Read<double> deltaT = 0.0;
    Read<std::int32_t> frame = 0;
    Read<bool> looping = false;
};

/// ImageCoordSystem.Loader fields.
struct CoordSystemLoad
{
    osp::NodeId object{};
    std::string className{};
    Read<bool> fixedOrigin = false;
    Read<bool> fixedAngle = false;
    Read<bool> fixedScale = false;
    Read<bool> locked = false;
    std::optional<FrameArray<CoordFrameData>> frames{};
};

/// PointMass.Loader (with TTrack.Loader's name) fields.
struct PointMassLoad
{
    osp::NodeId object{};
    Read<std::string> name = std::string{};
    Read<double> mass = 0.0;
    std::optional<FrameArray<PointMassFrameData>> frames{};
    std::optional<osp::NodeId> keyFramesProperty{};
    /// `(int[]) getObject("keyFrames")`, compact or indexed.
    Read<std::vector<std::int32_t>> keyFrames = std::vector<std::int32_t>{};
};

/// Vector.Loader (with TTrack.Loader's name) fields.
struct VectorLoad
{
    osp::NodeId object{};
    Read<std::string> name = std::string{};
    std::optional<FrameArray<VectorFrameData>> frames{};
};

/// One element of the panel's `tracks` collection.
struct TrackItem
{
    std::size_t ordinal = 0; ///< Position among the collection's elements.
    osp::NodeId property{};
    std::optional<osp::NodeId> object{}; ///< Absent for a null or non-object element.
    std::string className{};             ///< Effective class, or "" without an object.
};

/// TrackerPanel.Loader fields MoTrack models.
struct PanelLoad
{
    osp::NodeId object{};
    std::string className{};
    Read<std::string> semanticVersion = std::string{};
    Read<std::string> description = std::string{};
    Read<std::string> lengthUnit = std::string{};
    Read<std::string> massUnit = std::string{};
    Read<std::string> timeUnit = std::string{};
    Read<std::string> referenceFrame = std::string{};
    Read<std::string> selectedTrack = std::string{};
    std::optional<VideoClipLoad> videoClip{};
    std::optional<ClipControlLoad> clipControl{};
    std::optional<CoordSystemLoad> coords{};
    std::optional<osp::NodeId> tracks{}; ///< The `tracks` property.
    std::vector<TrackItem> trackItems{};
};

/// Loads the root object. The caller checks its class.
[[nodiscard]] PanelLoad loadPanel(const osp::Source& source);
[[nodiscard]] PointMassLoad loadPointMass(const osp::Source& source, osp::NodeId object);
[[nodiscard]] VectorLoad loadVector(const osp::Source& source, osp::NodeId object);
[[nodiscard]] CoordSystemLoad loadCoordSystem(const osp::Source& source, osp::NodeId object);

/// The object of the last object-typed property with this name (XMLControlElement.getChildControl).
[[nodiscard]] std::optional<osp::NodeId> childObject(const osp::Source& source, osp::NodeId object, std::string_view name);
/// The last property with this name (XMLControlElement's lookup map).
[[nodiscard]] std::optional<osp::NodeId> propertyNode(const osp::Source& source, osp::NodeId object, std::string_view name);
/// Elements of an object array in file order, with indices and object nodes.
[[nodiscard]] std::vector<ArrayElement> arrayElements(const osp::Source& source, osp::NodeId array);

} // namespace motrack::tracker
