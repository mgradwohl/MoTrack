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
/// The root of a Tracker tabset, which lists other files to open instead of holding an experiment.
inline constexpr std::string_view TFRAME_CLASS = "org.opensourcephysics.cabrillo.tracker.TFrame";
inline constexpr std::string_view STRING_ARRAY_CLASS = "[Ljava.lang.String;";
inline constexpr std::string_view STRING_MATRIX_CLASS = "[[Ljava.lang.String;";

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

/// One element of a TFrame's `tabs` array, normally a `String[]` of {absolute, relative} paths.
struct TabsetTabLoad
{
    ArrayElement element{};
    osp::PropertyType type = osp::PropertyType::Array;
    std::string className{};            ///< Array class attribute after the media remap; "" for other types.
    std::vector<StringElement> paths{}; ///< The element's own `[n]` elements in file order, when it is an array.
};

/// TFrame.Loader fields (`loadObject` reads `tabs` and `basepath`).
struct TabsetLoad
{
    osp::NodeId object{};
    std::string className{};
    /// `getString("basepath")`. XMLControlElement answers a string property of that name before the directory the
    /// document was read from, so a saved one replaces the first folder every relative tab path is resolved in.
    Read<std::string> basepath = std::string{};
    /// `getObject("tabs")`: the last property with that name, whatever its type.
    std::optional<osp::NodeId> tabs{};
    std::size_t tabsProperties = 0;           ///< Properties named `tabs`, repeats included.
    std::vector<TabsetTabLoad> tabElements{}; ///< Elements of `tabs` in file order, when it is an array.
};

/// Why OSP's loader cannot build an array or collection (#315). XMLControlElement.arrayValue resolves the component
/// type from the `class` attribute before it reads any element, and collectionValue instantiates that class, so a
/// missing class throws a NullPointerException. An indexed array (one not in the compact "array" form) is sized by its
/// last element's index, so an element whose index Integer.parseInt cannot read, or one beyond the last element's,
/// throws while the array is filled.
enum class UnloadableShape : std::uint8_t
{
    MissingClass,
    UnplacedIndex,
};

/// The shape of `container` (an array or collection property) that Java cannot load, or none.
[[nodiscard]] std::optional<UnloadableShape> unloadableShape(const osp::Source& source, osp::NodeId container);

/// Where Tracker 6.3.5's application loader meets a container it cannot load, among the ones MoTrack reads.
enum class UnloadablePlace : std::uint8_t
{
    /// The ImageVideo `paths`: the exception escapes TrackerPanel.Loader, so Tracker cannot open the file.
    ImagePaths,
    /// The ImageCoordSystem `framedata` of the panel's `coords`: Tracker cannot open the file either.
    Coordinates,
    /// The panel's `tracks` collection itself: collectionValue catches the exception and returns null, so Tracker
    /// opens the file without any of its tracks.
    TrackList,
    /// A point mass's `framedata` or `keyFrames`, or a vector's `framedata`: the exception leaves the track's loader
    /// and collectionValue drops the whole `tracks` collection, as for `TrackList`.
    TrackArray,
};

struct Unloadable
{
    UnloadablePlace place = UnloadablePlace::TrackArray;
    UnloadableShape shape = UnloadableShape::MissingClass;
    osp::NodeId container{};
    /// The container's property name: `paths`, `framedata`, `tracks` or `keyFrames`.
    std::string property{};
    /// The track's saved name (TTrack.Loader's `name`), or "" when unreadable; only for `TrackArray`.
    std::string track{};
};

/// The first container of the objects MoTrack models that Tracker's loader cannot build, in its loading order: the
/// video's `paths` (ImageVideo only), the coordinate system's `framedata` (ImageCoordSystem only), the `tracks`
/// collection, then each point mass's `framedata` and `keyFrames` and each vector's `framedata` in `tracks` order.
/// Other objects are preserved, not read, so their containers are not examined.
[[nodiscard]] std::optional<Unloadable> findUnloadable(const osp::Source& source, const PanelLoad& panel);

/// Loads the root object. The caller checks its class.
[[nodiscard]] PanelLoad loadPanel(const osp::Source& source);
/// Loads a tabset's root object. The caller checks its class and the shape of `tabs`.
[[nodiscard]] TabsetLoad loadTabset(const osp::Source& source);
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
