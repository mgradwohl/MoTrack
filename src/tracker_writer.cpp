#include "tracker_writer.hpp"

#include "local_media_path.hpp"
#include "measurements.hpp"
#include "tracker_archive.hpp"
#include "tracker_archive_writer.hpp"
#include "tracker_rename.hpp"
#include "workspace_document.hpp"

#include <QByteArray>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace motrack
{
namespace
{

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("This Tracker export cannot preserve all edits or source associations.");
    }
}

QDomElement uniqueProperty(const QDomElement& object, const QString& name)
{
    QDomElement result;
    for (auto child = object.firstChildElement("property"); !child.isNull(); child = child.nextSiblingElement("property"))
    {
        if (child.attribute("name") == name)
        {
            require(result.isNull());
            result = child;
        }
    }
    return result;
}

void patchScalar(QDomDocument& xml, QDomElement object, const QString& name, const QString& type, const QString& value)
{
    auto property = uniqueProperty(object, name);
    if (property.isNull())
    {
        property = xml.createElement("property");
        property.setAttribute("name", name);
        property.setAttribute("type", type);
        // Added fields need their own lines for OSP's reader; leave every existing text node alone.
        object.appendChild(xml.createTextNode("\n"));
        object.appendChild(property);
        object.appendChild(xml.createTextNode("\n"));
    }
    require(property.attribute("type") == type && property.firstChildElement().isNull());
    // OSP requires the CDATA terminator immediately before </property>. It does not decode
    // entities in ordinary strings, and preserving adjacent comments can make its reader hang.
    if (type == "string")
    {
        for (auto child = property.firstChild(); !child.isNull(); child = child.nextSibling())
        {
            require(child.isText() || child.isCDATASection());
        }
    }
    const auto scalar = [&]() -> QDomNode
    {
        if (type == "string")
        {
            require(!value.contains("]]>"));
            return xml.createCDATASection(value);
        }
        return xml.createTextNode(value);
    };
    // Preserve comments and processing instructions within numeric scalar properties.
    bool written = false;
    for (auto child = property.firstChild(); !child.isNull();)
    {
        const auto next = child.nextSibling();
        if (child.isText() || child.isCDATASection())
        {
            if (!written)
            {
                property.replaceChild(scalar(), child);
                written = true;
            }
            else
            {
                property.removeChild(child);
            }
        }
        child = next;
    }
    if (!written)
    {
        property.appendChild(scalar());
    }
}

void patchDouble(QDomDocument& xml, const QDomElement& object, const QString& name, double value)
{
    patchScalar(xml, object, name, "double", QString::number(value, 'g', 17));
}

void patchUnit(QDomDocument& xml, const QString& name, const std::string& unit)
{
    const auto label = QString::fromStdString(unit);
    require(!label.isEmpty() && label.size() <= 256 && label == label.trimmed() && label.toStdString() == unit);
    for (const auto codePoint : label.toUcs4())
    {
        const auto category = QChar::category(codePoint);
        require(category != QChar::Other_Control && category != QChar::Other_Format && !QChar::isDigit(codePoint));
    }
    patchScalar(xml, xml.documentElement(), name, "string", label);
}

enum class LinkedMedia : std::uint8_t
{
    None,
    Images,
    Movie
};

void patchClipRange(QDomDocument& xml, const Clip& clip, LinkedMedia media)
{
    if (clip.frameDurationMs)
    {
        auto timing = clip;
        // Audited media use the same constant interval arithmetic. This private
        // numeric check does not change the saved source inventory or enable timing edits.
        if (media != LinkedMedia::None)
            timing.mediaReferences.clear();
        require(validConstantClipTiming(timing));
    }
    const auto count = clip.stepCount.value_or(0);
    require(clip.firstFrame >= 0 && clip.stepSize > 0 && count > 0);
    const auto end = static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(count - 1) * clip.stepSize;
    // Tracker 6.3.5 clamps media-free clips at frame 500000 and loads a temporary frame count
    // of start + count * stride. Reject values its Java int arithmetic cannot retain.
    require(end <= 500000 && end + clip.stepSize <= std::numeric_limits<int>::max());
    require(!clip.sourceFrameCount || end < *clip.sourceFrameCount);
    const auto property = uniqueProperty(xml.documentElement(), "videoclip");
    const auto object = property.firstChildElement("object");
    require(property.attribute("type") == "object" && object.attribute("class") == "org.opensourcephysics.media.core.VideoClip" &&
            object.nextSiblingElement().isNull());
    require(uniqueProperty(object, "frameshift").isNull());
    if (media != LinkedMedia::None)
    {
        // The media schema, complete frame inventory/index and optional saved count were
        // checked before patching. Trimming changes only the clip, never the source media.
        require(uniqueProperty(object, "video").firstChildElement("object").attribute("class") ==
                (media == LinkedMedia::Images ? "org.opensourcephysics.media.core.ImageVideo"
                                              : "org.opensourcephysics.media.xuggle.XuggleVideo"));
    }
    else
        require(uniqueProperty(object, "video").isNull() && uniqueProperty(object, "video_framecount").isNull());
    patchScalar(xml, object, "startframe", "int", QString::number(clip.firstFrame));
    patchScalar(xml, object, "stepsize", "int", QString::number(clip.stepSize));
    patchScalar(xml, object, "stepcount", "int", QString::number(count));
}

void patchClipTiming(QDomDocument& xml, const LegacyProject& project)
{
    const auto& clip = project.clip;
    require(validConstantClipTiming(clip));
    // Data tracks can supply their own time source; models require independent trajectory checks.
    require(supportsConstantClipTimingEdits(project));
    const auto clipProperty = uniqueProperty(xml.documentElement(), "videoclip");
    const auto clipObject = clipProperty.firstChildElement("object");
    require(clipProperty.attribute("type") == "object" && clipObject.attribute("class") == "org.opensourcephysics.media.core.VideoClip" &&
            clipObject.nextSiblingElement().isNull());
    const QStringList clipFields{"startframe", "stepsize", "stepcount", "starttime", "readout", "playallsteps"};
    for (auto field = clipObject.firstChildElement(); !field.isNull(); field = field.nextSiblingElement())
    {
        const auto name = field.attribute("name");
        require(field.tagName() == "property" && clipFields.contains(name) && field.firstChildElement().isNull());
        require(!uniqueProperty(clipObject, name).isNull());
        if (name == "starttime")
        {
            bool valid = false;
            const auto value = field.text().toDouble(&valid);
            require(field.attribute("type") == "double" && valid && std::isfinite(value));
        }
        else if (name == "readout")
        {
            require(field.attribute("type") == "string");
        }
        else if (name == "playallsteps")
        {
            require(field.attribute("type") == "boolean" && (field.text() == "true" || field.text() == "false"));
        }
        else
        {
            bool valid = false;
            const auto value = field.text().toInt(&valid);
            require(field.attribute("type") == "int" && valid && value >= (name == "startframe" ? 0 : 1));
        }
    }
    const auto controlProperty = uniqueProperty(xml.documentElement(), "clipcontrol");
    const auto control = controlProperty.firstChildElement("object");
    require(controlProperty.attribute("type") == "object" &&
            control.attribute("class") == "org.opensourcephysics.media.core.StepperClipControl" && control.nextSiblingElement().isNull());
    const QStringList controlFields{"rate", "delta_t", "frame", "looping"};
    for (auto field = control.firstChildElement(); !field.isNull(); field = field.nextSiblingElement())
    {
        const auto name = field.attribute("name");
        require(field.tagName() == "property" && controlFields.contains(name) && field.firstChildElement().isNull());
        require(!uniqueProperty(control, name).isNull());
        if (name == "looping")
        {
            require(field.attribute("type") == "boolean" && (field.text() == "true" || field.text() == "false"));
        }
        else if (name == "frame")
        {
            bool valid = false;
            const auto frame = field.text().toInt(&valid);
            require(field.attribute("type") == "int" && valid && frame >= 0 && frame <= 500000);
        }
        else
        {
            bool valid = false;
            const auto number = field.text().toDouble(&valid);
            require(field.attribute("type") == "double" && valid && std::isfinite(number) && number > 0.0);
        }
    }
    require(!uniqueProperty(control, "rate").isNull() && !uniqueProperty(control, "delta_t").isNull());
    patchDouble(xml, clipObject, "starttime", clip.startTimeMs);
    if (!clip.frameDurationMs)
    {
        require(false);
        return;
    }
    patchDouble(xml, control, "delta_t", *clip.frameDurationMs);
}

QDomElement arrayProperty(QDomDocument& xml, QDomElement object, const QString& name, const QString& type)
{
    auto array = uniqueProperty(object, name);
    if (array.isNull())
    {
        array = xml.createElement("property");
        array.setAttribute("name", name);
        array.setAttribute("type", "array");
        object.appendChild(xml.createTextNode("\n"));
        object.appendChild(array);
        object.appendChild(xml.createTextNode("\n"));
    }
    require(array.attribute("type") == "array" && (array.attribute("class").isEmpty() || array.attribute("class") == type));
    array.setAttribute("class", type);
    return array;
}

int frameIndex(const QString& name)
{
    require(name.startsWith('[') && name.endsWith(']'));
    bool valid = false;
    const auto frame = name.mid(1, name.size() - 2).toInt(&valid);
    require(valid && frame >= 0);
    return frame;
}

std::set<int> readKeys(const QDomElement& object, const std::vector<Mark>& original)
{
    std::set<int> result;
    const auto keys = uniqueProperty(object, "keyFrames");
    if (!keys.isNull())
    {
        require(keys.attribute("type") == "array" && keys.attribute("class") == "[I");
        const auto array = uniqueProperty(keys, "array");
        if (array.isNull())
        {
            require(keys.firstChildElement().isNull()); // Tracker's zero-length int array has no child.
        }
        else
        {
            require(array.attribute("type") == "string" && array.firstChildElement().isNull());
            const auto text = array.text().trimmed();
            require(text.startsWith('{') && text.endsWith('}'));
            const auto values = text.mid(1, text.size() - 2).trimmed();
            require(!values.isEmpty()); // A compact empty token is not a valid OSP int-array encoding.
            if (!values.isEmpty())
            {
                for (const auto& value : values.split(','))
                {
                    bool valid = false;
                    const auto frame = value.trimmed().toInt(&valid);
                    require(valid && frame >= 0);
                    result.insert(frame);
                }
            }
        }
    }
    // Tracker treats missing/empty key arrays as all existing marks being manual keys.
    if (result.empty())
    {
        for (const auto& mark : original)
        {
            result.insert(mark.frame);
        }
    }
    return result;
}

struct FrameChanges
{
    std::vector<int> updated;
    std::vector<int> removed;
};

template <typename Sample, typename Update>
FrameChanges patchFrameData(QDomDocument& xml,
                            const QDomElement& object,
                            const std::vector<Sample>& original,
                            const std::vector<Sample>& current,
                            const QString& className,
                            Update update)
{
    std::map<int, const Sample*> marks;
    int previous = -1;
    for (const auto& mark : current)
    {
        require(mark.frame > previous);
        marks.emplace(mark.frame, &mark);
        previous = mark.frame;
    }
    FrameChanges changes;
    const auto data = arrayProperty(xml, object, "framedata", "[L" + className + ';');
    std::map<int, QDomElement> entries;
    int lastEntry = -1;
    for (auto item = data.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"))
    {
        const auto frame = frameIndex(item.attribute("name"));
        require(frame > lastEntry && entries.emplace(frame, item).second);
        lastEntry = frame;
    }
    for (const auto& mark : original)
    {
        const auto found = marks.find(mark.frame);
        if (found == marks.end())
        {
            auto parent = data;
            parent.removeChild(entries.at(mark.frame));
            entries.erase(mark.frame);
            changes.removed.push_back(mark.frame);
        }
        else if (*found->second == mark)
        {
            marks.erase(found); // Keep untouched frame objects and their unknown properties.
        }
    }
    for (const auto& [frame, mark] : marks)
    {
        auto item = entries.contains(frame) ? entries.at(frame) : QDomElement{};
        if (item.isNull())
        {
            item = xml.createElement("property");
            item.setAttribute("name", QString("[%1]").arg(frame));
            item.setAttribute("type", "object");
            auto parent = data;
            const auto next = entries.upper_bound(frame);
            if (next == entries.end())
            {
                parent.appendChild(item);
            }
            else
            {
                parent.insertBefore(item, next->second);
            }
            entries.emplace(frame, item);
        }
        require(item.attribute("type") == "object");
        auto step = item.firstChildElement("object");
        if (step.isNull())
        {
            require(item.firstChildElement().isNull());
            // A null array placeholder may contain text; replace only that known scalar representation.
            for (auto node = item.firstChild(); !node.isNull();)
            {
                const auto next = node.nextSibling();
                if (node.isText() || node.isCDATASection())
                {
                    item.removeChild(node);
                }
                else
                {
                    require(false); // A comment before the new object would stop OSP's object reader.
                }
                node = next;
            }
            item.setAttribute("type", "object");
            step = xml.createElement("object");
            item.appendChild(xml.createTextNode("\n"));
            item.appendChild(step);
            item.appendChild(xml.createTextNode("\n"));
        }
        require(step.nextSiblingElement("object").isNull() && (step.attribute("class").isEmpty() || step.attribute("class") == className));
        step.setAttribute("class", className);
        update(xml, step, *mark);
        changes.updated.push_back(frame);
    }
    // OSP's array reader consumes consecutive property lines. Deleting an entry can leave a blank
    // line in preserved formatting that makes it stop early. Format only this known structural array,
    // while the whole document is serialized without adding whitespace to unknown content.
    auto array = data;
    for (auto ancestor = QDomNode(array); !ancestor.isNull(); ancestor = ancestor.parentNode())
    {
        const auto space = ancestor.toElement().attribute("xml:space");
        if (!space.isEmpty())
        {
            require(space != "preserve");
            break;
        }
    }
    for (auto node = array.firstChild(); !node.isNull();)
    {
        const auto next = node.nextSibling();
        if (node.isText())
        {
            require(std::ranges::all_of(node.nodeValue(),
                                        [](QChar value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }));
            array.removeChild(node);
        }
        else
        {
            require(node.isElement() && node.nodeName() == "property");
        }
        node = next;
    }
    for (auto item = array.firstChildElement(); !item.isNull(); item = item.nextSiblingElement())
    {
        array.insertBefore(xml.createTextNode("\n"), item);
    }
    if (array.hasChildNodes())
    {
        array.appendChild(xml.createTextNode("\n"));
    }
    return changes;
}

void patchMarks(QDomDocument& xml, const QDomElement& object, const std::vector<Mark>& original, const std::vector<Mark>& current)
{
    auto keys = readKeys(object, original);
    const auto oldKeys = keys;
    const auto changes = patchFrameData(xml,
                                        object,
                                        original,
                                        current,
                                        "org.opensourcephysics.cabrillo.tracker.PointMass$FrameData",
                                        [](auto& document, const auto& step, const Mark& mark)
                                        {
                                            require(std::isfinite(mark.x) && std::isfinite(mark.y));
                                            patchDouble(document, step, "x", mark.x);
                                            patchDouble(document, step, "y", mark.y);
                                        });
    for (const auto frame : changes.removed)
    {
        keys.erase(frame);
    }
    keys.insert(changes.updated.begin(), changes.updated.end());
    // An empty key list makes Tracker promote every remaining automatic mark to a manual key.
    // Until that distinction is in the editable model, reject edits that cannot retain it.
    require(!keys.empty() || current.empty());
    if (keys != oldKeys)
    {
        auto container = arrayProperty(xml, object, "keyFrames", "[I");
        auto values = uniqueProperty(container, "array");
        if (keys.empty())
        {
            if (!values.isNull())
            {
                container.removeChild(values);
            }
            return; // OSP cannot decode the compact string "{}" as a zero-length int array.
        }
        if (values.isNull())
        {
            values = xml.createElement("property");
            values.setAttribute("name", "array");
            values.setAttribute("type", "string");
            container.appendChild(xml.createTextNode("\n"));
            container.appendChild(values);
            container.appendChild(xml.createTextNode("\n"));
        }
        for (auto node = values.firstChild(); !node.isNull();)
        {
            const auto next = node.nextSibling();
            if (node.isText() || node.isCDATASection())
            {
                values.removeChild(node);
            }
            node = next;
        }
        QStringList text;
        for (const auto key : keys)
        {
            text.append(QString::number(key));
        }
        values.appendChild(xml.createTextNode('{' + text.join(',') + '}'));
    }
}

std::array<double, 6> trackerInverse(const CalibrationKey& key)
{
    // ImageCoordSystem normalizes sine/cosine, builds this image affine matrix, then uses
    // Java 17 AffineTransform.createInverse(). Algebraically simplified translations can
    // hide overflowing intermediate products; check the same diagonal/shear/general cases.
    const auto angle = key.angleDegrees * std::numbers::pi / 180.0;
    const auto rawCos = std::cos(angle);
    const auto rawSin = std::sin(angle);
    const auto norm = std::sqrt(rawCos * rawCos + rawSin * rawSin);
    const auto cos = rawCos / norm;
    const auto sin = rawSin / norm;
    const auto a = key.scaleX * cos;
    const auto b = -key.scaleY * sin;
    const auto c = -key.scaleX * sin;
    const auto d = -key.scaleY * cos;
    std::array<double, 6> inverse{};
    if (b == 0 && c == 0)
    {
        require(a != 0 && d != 0);
        inverse = {1 / a, 0, 0, 1 / d, -key.originX / a, -key.originY / d};
    }
    else if (a == 0 && d == 0)
    {
        require(b != 0 && c != 0);
        inverse = {0, 1 / c, 1 / b, 0, -key.originY / b, -key.originX / c};
    }
    else
    {
        // Keep multiplication and subtraction separate, as Java's strict floating point does.
        const auto diagonal = a * d;
        const auto shear = b * c;
        const auto determinant = diagonal - shear;
        require(std::isfinite(determinant) && std::abs(determinant) > std::numeric_limits<double>::denorm_min());
        const auto cx = c * key.originY;
        const auto dx = d * key.originX;
        const auto by = b * key.originX;
        const auto ay = a * key.originY;
        inverse = {d / determinant, -b / determinant, -c / determinant, a / determinant, (cx - dx) / determinant, (by - ay) / determinant};
    }
    const std::array expected{cos / key.scaleX,
                              -sin / key.scaleX,
                              -sin / key.scaleY,
                              -cos / key.scaleY,
                              -cos * key.originX / key.scaleX + sin * key.originY / key.scaleY,
                              sin * key.originX / key.scaleX + cos * key.originY / key.scaleY};
    for (std::size_t index = 0; index < inverse.size(); ++index)
    {
        const auto tolerance = std::max(1e-10, 1e-12 * std::max(std::abs(inverse[index]), std::abs(expected[index])));
        require(std::isfinite(inverse[index]) && std::isfinite(expected[index]) && std::abs(inverse[index] - expected[index]) <= tolerance);
    }
    return inverse;
}

void requireBoundedMassFrames(const QDomDocument& xml)
{
    std::size_t frameSlots = 0;
    const auto properties = xml.elementsByTagName("property");
    for (int index = 0; index < properties.size(); ++index)
    {
        const auto data = properties.at(index).toElement();
        if (data.attribute("type") != "array" || data.attribute("class") != "[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;")
            continue;
        int lastFrame = -1;
        for (auto item = data.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"))
        {
            const auto frame = frameIndex(item.attribute("name"));
            require(frame <= 500000);
            lastFrame = std::max(lastFrame, frame);
        }
        // Include inherited arrays on retained objects such as CenterOfMass, even when those
        // objects are not natively editable. Reimport and null placeholders cannot reset the budget.
        frameSlots += static_cast<std::size_t>(lastFrame + 1);
        require(frameSlots <= 1000000);
    }
}

void requireStableTableColumns(const QDomDocument& xml, const LegacyProject& project, bool movie)
{
    std::unordered_set<std::string> massNames;
    for (const auto& mass : project.pointMasses)
        massNames.insert(mass.name);
    const auto objects = xml.elementsByTagName("object");
    for (int index = 0; index < objects.size(); ++index)
    {
        const auto object = objects.at(index).toElement();
        if (object.attribute("class") == "org.opensourcephysics.cabrillo.tracker.PointMass" &&
            !uniqueProperty(object, "number_formats").isNull())
        {
            // This newly supported metadata requires the audited format grammar even for a
            // mass-only edit, which otherwise permits opaque source content to pass through.
            if (movie)
                validateTrackerMovieEditSchema(xml);
            else
                validateTrackerEditSchema(xml);
            break;
        }
    }
    const auto defaultPattern = [](const std::string& variable, const std::string& pattern)
    {
        return ((variable == "frame" || variable == "step") && pattern == "0") || (variable == "t" && pattern == "0.000") ||
               pattern.empty();
    };
    for (int index = 0; index < objects.size(); ++index)
    {
        const auto object = objects.at(index).toElement();
        if (object.attribute("class") != "org.opensourcephysics.cabrillo.tracker.PointMass")
            continue;
        const auto owner = uniqueProperty(object, "name").text().toStdString();
        for (auto cell = uniqueProperty(object, "number_formats").firstChildElement(); !cell.isNull();)
        {
            const auto variable = cell.text().toStdString();
            cell = cell.nextSiblingElement();
            require(!cell.isNull());
            if (!defaultPattern(variable, cell.text().toStdString()))
                throw ProjectError("Tracker cannot retain custom number formats for '" + owner + "' (MoTrack issue #64).");
            cell = cell.nextSiblingElement();
        }
    }
    for (int index = 0; index < objects.size(); ++index)
    {
        const auto object = objects.at(index).toElement();
        if (object.attribute("class") != "org.opensourcephysics.cabrillo.tracker.TableTView")
            continue;
        const auto columns = uniqueProperty(object, "track_columns");
        for (auto row = columns.firstChildElement("property"); !row.isNull(); row = row.nextSiblingElement("property"))
        {
            const auto owner = uniqueProperty(row, "[0]").text().toStdString();
            if (!massNames.contains(owner) || uniqueProperty(row, "[1]").text() != "t")
                continue;
            const auto second = uniqueProperty(row, "[2]").text();
            const auto third = uniqueProperty(row, "[3]");
            const bool subset = second == "x" && third.isNull();
            const bool defaults =
                uniqueProperty(row, "[4]").isNull() && ((second == "x" && third.text() == "y") || (second == "y" && third.text() == "x"));
            // Pinned 6.3.5 can serialize t,x as null columns, or omit a default-column view's
            // custom formats/order. Preserve these source rows natively until a safe reference
            // workflow has evidence; do not let a changed standard save imply fidelity.
            const bool reordered =
                second == "y" && third.text() == "x" && uniqueProperty(row, "[4]").text() == "frame" && uniqueProperty(row, "[5]").isNull();
            if (subset || defaults || reordered)
                throw ProjectError("Tracker cannot safely resave the table columns for '" + owner + "' (MoTrack issue #64).");
        }
        for (auto group = uniqueProperty(object, "column_formats").firstChildElement(); !group.isNull(); group = group.nextSiblingElement())
        {
            for (auto row = group.firstChildElement(); !row.isNull(); row = row.nextSiblingElement())
            {
                const auto owner = uniqueProperty(row, "[0]").text().toStdString();
                if (!massNames.contains(owner))
                    continue;
                const auto variable = uniqueProperty(row, "[1]").text().toStdString();
                const auto pattern = uniqueProperty(row, "[2]").text().toStdString();
                if (!defaultPattern(variable, pattern))
                    throw ProjectError("Tracker cannot retain custom table formats for '" + owner + "' (MoTrack issue #64).");
            }
        }
    }
}

void requireTrackerCoordinates(const LegacyProject& project, const Calibration& calibration)
{
    require(!calibration.keyframes.empty() && calibration.keyframes.front().frame == 0);
    std::vector<std::array<double, 6>> inverses;
    int previous = -1;
    for (const auto& key : calibration.keyframes)
    {
        require(key.frame > previous && key.frame <= 500000 && std::isfinite(key.originX) && std::isfinite(key.originY) &&
                std::isfinite(key.angleDegrees) && std::abs(key.angleDegrees) <= 180 && std::isfinite(key.scaleX) &&
                std::isfinite(key.scaleY) && key.scaleX != 0 && key.scaleY != 0);
        const auto determinant = key.scaleX * key.scaleY;
        require(std::isfinite(determinant) && determinant != 0);
        previous = key.frame;
    }
    auto componentCalibration = calibration;
    for (auto& key : componentCalibration.keyframes)
    {
        // Fixed origin/angle use the last loaded key, while fixed scale uses the first.
        // Compare Java's inverse with that effective calibration, not each raw source record.
        const auto effective = calibrationAt(calibration, key.frame);
        if (!effective)
        {
            require(false);
            return;
        }
        inverses.push_back(trackerInverse(*effective));
        key.originX = 0;
        key.originY = 0;
    }
    const auto check = [&](const Mark& point, bool component = false, const Mark* reconstructed = nullptr)
    {
        const auto next = std::ranges::upper_bound(calibration.keyframes, point.frame, {}, &CalibrationKey::frame);
        require(next != calibration.keyframes.begin());
        const auto& inverse = inverses[static_cast<std::size_t>(next - calibration.keyframes.begin() - 1)];
        const auto native = worldPosition(component ? componentCalibration : calibration, point);
        if (!native)
        {
            require(false);
            return;
        }
        // AffineTransform evaluates products, their sum, then translation. Test coordinates,
        // not just coefficients: cancellation can be large relative to a near-zero result.
        const auto& image = reconstructed ? *reconstructed : point;
        const auto xx = inverse[0] * image.x;
        const auto xy = inverse[2] * image.y;
        const auto yx = inverse[1] * image.x;
        const auto yy = inverse[3] * image.y;
        const std::array actual{(xx + xy) + (component ? 0 : inverse[4]), (yx + yy) + (component ? 0 : inverse[5])};
        if (component)
        {
            // Vector.refreshData transforms the components separately and squares them for magnitude.
            require(std::isfinite(actual[0] * actual[0] + actual[1] * actual[1]));
        }
        const std::array expected{native->x, native->y};
        for (std::size_t axis = 0; axis < actual.size(); ++axis)
        {
            const auto tolerance = std::max(1e-10, 1e-12 * std::max(std::abs(actual[axis]), std::abs(expected[axis])));
            require(std::isfinite(actual[axis]) && std::abs(actual[axis] - expected[axis]) <= tolerance);
        }
    };
    for (const auto& key : calibration.keyframes)
    {
        check({.frame = key.frame, .x = key.originX, .y = key.originY});
        check({.frame = key.frame, .x = 13.5, .y = 17.25}); // Independent corpus coordinate probe.
    }
    for (const auto& mass : project.pointMasses)
    {
        for (const auto& mark : mass.marks)
        {
            check(mark);
        }
    }
    for (const auto& vector : project.vectors)
    {
        for (const auto& sample : vector.samples)
        {
            check({.frame = sample.frame, .x = sample.tailX, .y = sample.tailY});
            check({.frame = sample.frame, .x = sample.tailX + sample.componentX, .y = sample.tailY + sample.componentY});
            const Mark reconstructed{.frame = sample.frame,
                                     .x = (sample.tailX + sample.componentX) - sample.tailX,
                                     .y = (sample.tailY + sample.componentY) - sample.tailY};
            check({.frame = sample.frame, .x = sample.componentX, .y = sample.componentY}, true, &reconstructed);
        }
    }
}

void patchCalibration(QDomDocument& xml, const Calibration& original, const Calibration& current, const LegacyProject& project)
{
    const bool fixed = original.fixedOrigin && original.fixedAngle && original.fixedScale && current.fixedOrigin && current.fixedAngle &&
                       current.fixedScale && original.keyframes.size() == 1 && current.keyframes.size() == 1 &&
                       original.keyframes.front().frame == 0 && current.keyframes.front().frame == 0;
    const bool keyed = !(original.fixedOrigin && original.fixedAngle && original.fixedScale) &&
                       original.fixedOrigin == current.fixedOrigin && original.fixedAngle == current.fixedAngle &&
                       original.fixedScale == current.fixedScale && !original.keyframes.empty() && original.keyframes.front().frame == 0 &&
                       project.vectors.empty();
    require(fixed || keyed); // Changing fixed flags and varying vector coordinates need separate reference cases.
    requireTrackerCoordinates(project, current);
    // Existing commands add or edit calibration keys. Key removal needs its own reversible command/oracle.
    for (const auto& key : original.keyframes)
    {
        require(std::ranges::binary_search(current.keyframes, key.frame, {}, &CalibrationKey::frame));
    }
    const auto property = uniqueProperty(xml.documentElement(), "coords");
    const auto object = property.firstChildElement("object");
    require(property.attribute("type") == "object" && object.attribute("class") == "org.opensourcephysics.media.core.ImageCoordSystem" &&
            object.nextSiblingElement().isNull());
    const auto locked = uniqueProperty(object, "locked");
    require(locked.isNull() ||
            (locked.attribute("type") == "boolean" && locked.firstChildElement().isNull() && locked.text().trimmed() == "false"));
    const auto data = uniqueProperty(object, "framedata");
    int lastFrame = current.keyframes.back().frame;
    for (auto item = data.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"))
    {
        const auto frame = frameIndex(item.attribute("name"));
        require(frame <= 500000); // Includes null entries in OSP's dense array allocation.
        lastFrame = std::max(lastFrame, frame);
    }
    if (keyed)
    {
        const auto count = project.clip.stepCount.value_or(1);
        require(project.clip.firstFrame >= 0 && project.clip.stepSize > 0 && count > 0);
        const auto lastClipFrame =
            static_cast<std::int64_t>(project.clip.firstFrame) + static_cast<std::int64_t>(count - 1) * project.clip.stepSize;
        require(lastClipFrame <= 500000);
        lastFrame = std::max(lastFrame, static_cast<int>(lastClipFrame));
        for (const auto& mass : project.pointMasses)
        {
            for (const auto& mark : mass.marks)
            {
                require(mark.frame <= 500000);
                lastFrame = std::max(lastFrame, mark.frame);
            }
        }
        // OSP updates the remaining dense transform range for each loaded key. A bound on
        // the final index alone still permits quadratic work on many closely spaced keys.
        std::size_t frameUpdates = 0;
        for (const auto& key : current.keyframes)
        {
            // Fixed origin/angle setters update the entire dense range for each loaded key.
            const auto firstAffected = current.fixedOrigin || current.fixedAngle ? 0 : key.frame;
            frameUpdates += static_cast<std::size_t>(lastFrame - firstAffected) + 1;
            require(frameUpdates <= 1000000);
        }
    }
    patchFrameData(xml,
                   object,
                   original.keyframes,
                   current.keyframes,
                   "org.opensourcephysics.media.core.ImageCoordSystem$FrameData",
                   [](auto& document, const auto& frame, const CalibrationKey& key)
                   {
                       patchDouble(document, frame, "xorigin", key.originX);
                       patchDouble(document, frame, "yorigin", key.originY);
                       patchDouble(document, frame, "angle", key.angleDegrees);
                       patchDouble(document, frame, "xscale", key.scaleX);
                       patchDouble(document, frame, "yscale", key.scaleY);
                   });
}

void sortTracks(LegacyProject& project)
{
    std::ranges::sort(project.pointMasses, {}, &PointMass::id);
    std::ranges::sort(project.vectors, {}, &VectorTrack::id);
}

bool retainsVectorComponent(double tail, double component)
{
    // Tracker stores endpoints internally, then reconstructs components by subtraction on save.
    // Use the independent corpus tolerance in image-pixel units, including cancellation error.
    const auto tip = tail + component;
    const auto reconstructed = tip - tail;
    const auto tolerance = std::max(1e-10, 1e-12 * std::max(std::abs(component), std::abs(reconstructed)));
    return std::isfinite(tip) && std::isfinite(reconstructed) && std::abs(reconstructed - component) <= tolerance;
}

void requireStableVectors(const LegacyProject& project, const std::unordered_map<TrackId, QDomElement>& objects)
{
    std::vector<const VectorSample*> samples;
    std::size_t frameSlots = 0;
    for (const auto& vector : project.vectors)
    {
        // OSP allocates a dense Java array through the highest XML index, even for null entries.
        // Bound individual indices and the sum across tracks before any destination is opened.
        int lastFrame = -1;
        const auto data = uniqueProperty(objects.at(vector.id), "framedata");
        for (auto item = data.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"))
        {
            const auto frame = frameIndex(item.attribute("name"));
            require(frame <= 500000);
            lastFrame = std::max(lastFrame, frame);
        }
        for (const auto& sample : vector.samples)
        {
            require(sample.frame >= 0 && sample.frame <= 500000);
            lastFrame = std::max(lastFrame, sample.frame);
            require(samples.size() < 4096); // Bound the conservative all-visible-tip comparison below.
            samples.push_back(&sample);
        }
        frameSlots += static_cast<std::size_t>(lastFrame + 1);
        require(frameSlots <= 500001);
        if (!vector.samples.empty())
        {
            const auto footprint = uniqueProperty(objects.at(vector.id), "footprint").text();
            require(footprint == "Footprint.Arrow" || footprint == "Footprint.BoldArrow");
        }
    }
    if (samples.empty())
    {
        return;
    }
    // Until dependencies and display scaling are modeled, only audit ordinary independent arrows
    // with a fixed origin and no other tracks that could supply visible vector snap targets.
    require(project.pointMasses.empty() &&
            std::ranges::all_of(project.unsupportedTrackTypes,
                                [](const auto& type) { return type == "org.opensourcephysics.cabrillo.tracker.CoordAxes"; }));
    if (!project.calibration || !project.calibration->fixedOrigin || project.calibration->keyframes.empty())
    {
        require(false);
        return;
    }
    require(project.calibration->fixedAngle && project.calibration->fixedScale && project.calibration->keyframes.size() == 1 &&
            project.calibration->keyframes.front().frame == 0);
    const auto& origin = project.calibration->keyframes.front();
    require(std::isfinite(origin.originX) && std::isfinite(origin.originY));
    for (const auto* sample : samples)
    {
        require(std::isfinite(sample->tailX) && std::isfinite(sample->tailY) && std::isfinite(sample->componentX) &&
                std::isfinite(sample->componentY) && retainsVectorComponent(sample->tailX, sample->componentX) &&
                retainsVectorComponent(sample->tailY, sample->componentY));
        // Tracker 6.3.5 snaps brand-new steps on first draw. Trails can expose steps from other frames.
        require(std::hypot(sample->tailX - origin.originX, sample->tailY - origin.originY) > 8);
        for (const auto* target : samples)
        {
            if (target != sample)
            {
                require(std::hypot(sample->tailX - (target->tailX + target->componentX),
                                   sample->tailY - (target->tailY + target->componentY)) > 8);
            }
        }
    }
    // Also validate edits made under an existing calibration: image-pixel tolerance alone can
    // hide reconstruction errors that a small scale amplifies in calibrated vector components.
    requireTrackerCoordinates(project, *project.calibration);
}

QDomElement appendNewTrack(QDomDocument& xml, QDomElement tracks, TrackKind kind, const std::string& name)
{
    // Native appearance is not yet persisted. Use explicit audited interchange defaults.
    const bool vector = kind == TrackKind::Vector;
    auto item = xml.createElement("property");
    item.setAttribute("name", "item");
    item.setAttribute("type", "object");
    auto object = xml.createElement("object");
    object.setAttribute("class",
                        vector ? "org.opensourcephysics.cabrillo.tracker.Vector" : "org.opensourcephysics.cabrillo.tracker.PointMass");
    patchScalar(xml, object, "name", "string", QString::fromStdString(name));
    if (!vector)
        patchDouble(xml, object, "mass", 1.0);
    patchScalar(xml, object, "visible", "boolean", "true");
    patchScalar(xml, object, "trail", "boolean", "true");
    patchScalar(xml, object, "locked", "boolean", "false");
    patchScalar(xml, object, "footprint", "string", vector ? "Footprint.BoldArrow" : "Footprint.Diamond");
    auto colorProperty = xml.createElement("property");
    colorProperty.setAttribute("name", "color");
    colorProperty.setAttribute("type", "object");
    auto color = xml.createElement("object");
    color.setAttribute("class", "java.awt.Color");
    for (const auto& [channel, value] :
         std::array<std::pair<const char*, int>, 4>{{{"red", 255}, {"green", 0}, {"blue", vector ? 255 : 0}, {"alpha", 255}}})
        patchScalar(xml, color, channel, "int", QString::number(value));
    colorProperty.appendChild(xml.createTextNode("\n"));
    colorProperty.appendChild(color);
    colorProperty.appendChild(xml.createTextNode("\n"));
    object.appendChild(colorProperty);
    object.appendChild(xml.createTextNode("\n"));
    item.appendChild(xml.createTextNode("\n"));
    item.appendChild(object);
    item.appendChild(xml.createTextNode("\n"));
    tracks.appendChild(item);
    return object;
}

void remapIds(LegacyProject& project, const std::unordered_map<TrackId, TrackId>& mapping, TrackId nextId)
{
    for (auto& track : project.pointMasses)
    {
        track.id = mapping.at(track.id);
    }
    for (auto& track : project.vectors)
    {
        track.id = mapping.at(track.id);
    }
    project.nextTrackId = nextId;
    sortTracks(project);
}

bool patchLinkedImagePaths(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline)
{
    validateTrackerEditSchema(xml);
    require(project.vectors.empty() && baseline.vectors.empty() &&
            std::ranges::all_of(project.unsupportedTrackTypes,
                                [](const auto& type) { return type == "org.opensourcephysics.cabrillo.tracker.CoordAxes"; }));
    const auto count = project.clip.mediaReferences.size();
    require(count > 0 && count <= 10000 && count == baseline.clip.mediaReferences.size());
    auto expectedClip = project.clip;
    expectedClip.mediaReferences = baseline.clip.mediaReferences;
    expectedClip.firstFrame = baseline.clip.firstFrame;
    expectedClip.stepSize = baseline.clip.stepSize;
    expectedClip.stepCount = baseline.clip.stepCount;
    require(expectedClip == baseline.clip); // Numeric timing changes need separate media acceptance.
    const auto& clip = project.clip;
    const auto steps = clip.stepCount.value_or(0);
    require(clip.firstFrame >= 0 && clip.stepSize > 0 && steps > 0 &&
            static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(steps - 1) * clip.stepSize <
                static_cast<std::int64_t>(count));
    for (const auto& mass : project.pointMasses)
        for (const auto& mark : mass.marks)
            require(mark.frame >= 0 && std::cmp_less(mark.frame, count));
    const auto clipObject = uniqueProperty(xml.documentElement(), "videoclip").firstChildElement("object");
    const auto video = uniqueProperty(clipObject, "video").firstChildElement("object");
    require(video.attribute("class") == "org.opensourcephysics.media.core.ImageVideo");
    const auto frameCount = uniqueProperty(clipObject, "video_framecount");
    if (!frameCount.isNull())
    {
        bool valid = false;
        const auto saved = frameCount.text().toUInt(&valid);
        require(valid && saved == count);
    }
    bool valid = false;
    const double duration = uniqueProperty(video, "delta_t").text().toDouble(&valid);
    require(valid && std::isfinite(duration) && duration > 0.0 && clip.frameDurationMs == duration);
    require(uniqueProperty(video, "path").text().toStdString() == baseline.clip.mediaReferences.front());
    if (project.clip.mediaReferences == baseline.clip.mediaReferences)
        return false;
    const auto paths = uniqueProperty(video, "paths");
    for (std::size_t index = 0; index < count; ++index)
        patchScalar(xml, paths, "[" + QString::number(index) + "]", "string", QString::fromStdString(clip.mediaReferences[index]));
    patchScalar(xml, video, "path", "string", QString::fromStdString(clip.mediaReferences.front()));
    baseline.clip.mediaReferences = project.clip.mediaReferences;
    return true;
}

bool patchLinkedMoviePath(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, std::span<const double> frameTimes)
{
    validateTrackerMovieEditSchema(xml);
    require(project.clip.mediaReferences.size() == 1 && baseline.clip.mediaReferences.size() == 1 && project.vectors.empty() &&
            std::ranges::all_of(project.unsupportedTrackTypes,
                                [](const auto& type) { return type == "org.opensourcephysics.cabrillo.tracker.CoordAxes"; }));
    // Movie edits retain original timing, geometry and identity. Clip ranges keep source frame numbers.
    auto unchanged = project;
    unchanged.clip.mediaReferences = baseline.clip.mediaReferences;
    unchanged.clip.firstFrame = baseline.clip.firstFrame;
    unchanged.clip.stepSize = baseline.clip.stepSize;
    unchanged.clip.stepCount = baseline.clip.stepCount;
    require(unchanged.pointMasses.size() == baseline.pointMasses.size());
    for (std::size_t index = 0; index < unchanged.pointMasses.size(); ++index)
    {
        unchanged.pointMasses[index].mass = baseline.pointMasses[index].mass;
        unchanged.pointMasses[index].marks = baseline.pointMasses[index].marks;
    }
    require(unchanged == baseline);
    const auto clip = uniqueProperty(xml.documentElement(), "videoclip").firstChildElement("object");
    const auto video = uniqueProperty(clip, "video").firstChildElement("object");
    const auto control = uniqueProperty(xml.documentElement(), "clipcontrol").firstChildElement("object");
    require(video.attribute("class") == "org.opensourcephysics.media.xuggle.XuggleVideo" &&
            control.attribute("class") == "org.opensourcephysics.media.core.VideoClipControl" &&
            uniqueProperty(video, "path").text().toStdString() == baseline.clip.mediaReferences.front());
    bool valid = false;
    const auto duration = uniqueProperty(control, "delta_t").text().toDouble(&valid);
    require(valid && std::isfinite(duration) && duration > 0.0 && !frameTimes.empty() && frameTimes.size() <= 2000000);
    for (std::size_t frame = 0; frame < frameTimes.size(); ++frame)
    {
        const auto expected = static_cast<double>(frame) * duration / 1000.0;
        require(std::isfinite(expected) && std::isfinite(frameTimes[frame]) && std::abs(frameTimes[frame] - expected) <= 1e-6);
    }
    const auto count = uniqueProperty(clip, "video_framecount");
    if (!count.isNull())
    {
        const auto saved = count.text().toUInt(&valid);
        require(valid && saved == frameTimes.size());
    }
    const auto steps = project.clip.stepCount.value_or(0);
    const auto last = static_cast<std::int64_t>(project.clip.firstFrame) + (static_cast<std::int64_t>(steps) - 1) * project.clip.stepSize;
    require(project.clip.firstFrame >= 0 && project.clip.stepSize > 0 && steps > 0 && last >= 0 && std::cmp_less(last, frameTimes.size()));
    for (const auto& mass : project.pointMasses)
        for (const auto& mark : mass.marks)
            require(mark.frame >= 0 && std::cmp_less(mark.frame, frameTimes.size()));
    if (project.clip.mediaReferences == baseline.clip.mediaReferences)
        return false;
    patchScalar(xml, video, "path", "string", QString::fromStdString(project.clip.mediaReferences.front()));
    baseline.clip.mediaReferences = project.clip.mediaReferences;
    return true;
}

std::string renderTrackerProjectImpl(const LegacyProject& project,
                                     const TrackerSource& source,
                                     LinkedMedia media,
                                     std::span<const double> frameTimes = {})
{
    require(media != LinkedMedia::None || project.clip.mediaReferences.empty());
    validateTrackerSource(source, project);
    auto baseline = parseTrackerProject(source.xml);
    if (!source.bindings || !baseline.source.bindings)
    {
        throw ProjectError("Tracker export requires original object associations.");
    }
    std::unordered_map<std::size_t, TrackId> sourceIds;
    for (const auto& binding : *source.bindings)
    {
        sourceIds.emplace(binding.sourceIndex, binding.trackId);
    }
    std::unordered_map<TrackId, TrackId> mapping;
    for (const auto& binding : *baseline.source.bindings)
    {
        mapping.emplace(binding.trackId, sourceIds.at(binding.sourceIndex));
    }
    remapIds(baseline.project, mapping, project.nextTrackId);
    auto expected = project;
    sortTracks(expected);
    const bool membershipChanged =
        !std::ranges::equal(expected.pointMasses, baseline.project.pointMasses, {}, &PointMass::id, &PointMass::id) ||
        !std::ranges::equal(expected.vectors, baseline.project.vectors, {}, &VectorTrack::id, &VectorTrack::id);
    std::unordered_set<TrackId> liveIds;
    for (const auto& track : expected.pointMasses)
        liveIds.insert(track.id);
    for (const auto& track : expected.vectors)
        liveIds.insert(track.id);
    // Reparse assigns compact IDs in surviving XML order, not the original/native ID order.
    mapping.clear();
    TrackId nextParsedId = 1;

    QDomDocument xml;
    require(static_cast<bool>(xml.setContent(QByteArray::fromStdString(source.xml), QDomDocument::ParseOption::PreserveSpacingOnlyNodes)));
    auto tracks = uniqueProperty(xml.documentElement(), "tracks");
    std::unordered_map<TrackId, QDomElement> objects;
    std::size_t position = 0;
    for (auto item = tracks.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"), ++position)
    {
        if (const auto found = sourceIds.find(position); found != sourceIds.end())
        {
            objects.emplace(found->second, item.firstChildElement("object"));
            if (liveIds.contains(found->second))
                mapping.emplace(nextParsedId++, found->second);
        }
    }
    bool changed = media == LinkedMedia::Images  ? patchLinkedImagePaths(xml, expected, baseline.project)
                   : media == LinkedMedia::Movie ? patchLinkedMoviePath(xml, expected, baseline.project, frameTimes)
                                                 : false;
    bool auditFinalSchema = media != LinkedMedia::None;
    bool massCoordinatesChecked = false;
    if (membershipChanged)
    {
        validateTrackerTrackMembership(xml, expected, source);
        auditFinalSchema = true;
        require(expected.calibration || uniqueProperty(xml.documentElement(), "coords").isNull());
        if (expected.calibration)
            requireTrackerCoordinates(expected, *expected.calibration);
        massCoordinatesChecked = true;
        require(tracks.attribute("type") == "collection" && tracks.attribute("class") == "java.util.ArrayList");
        // OSP reads consecutive property lines. Normalize only collection whitespace, never old objects.
        for (auto node = tracks.firstChild(); !node.isNull();)
        {
            const auto next = node.nextSibling();
            if (node.isText())
            {
                require(node.nodeValue().trimmed().isEmpty());
                tracks.removeChild(node);
            }
            else
                require(node.isElement() && node.nodeName() == "property");
            node = next;
        }
        std::size_t createdFrameSlots = 0;
        for (const auto& current : expected.pointMasses)
        {
            if (objects.contains(current.id))
                continue;
            // New native tracks have no source metadata to copy. Use the audited independent-mass defaults.
            require(std::ranges::all_of(current.marks, [](const auto& mark) { return mark.frame >= 0 && mark.frame <= 500000; }));
            if (!current.marks.empty())
            {
                const auto last = std::ranges::max(current.marks, {}, &Mark::frame).frame;
                createdFrameSlots += static_cast<std::size_t>(last) + 1;
                require(createdFrameSlots <= 1000000);
            }
            const auto object = appendNewTrack(xml, tracks, TrackKind::PointMass, current.name);
            objects.emplace(current.id, object);
            mapping.emplace(nextParsedId++, current.id);
            baseline.project.pointMasses.push_back({.name = current.name, .marks = {}, .id = current.id});
        }
        for (const auto& current : expected.vectors)
        {
            if (objects.contains(current.id))
                continue;
            require(expected.pointMasses.empty() &&
                    std::ranges::all_of(expected.unsupportedTrackTypes,
                                        [](const auto& type) { return type == "org.opensourcephysics.cabrillo.tracker.CoordAxes"; }));
            const auto object = appendNewTrack(xml, tracks, TrackKind::Vector, current.name);
            objects.emplace(current.id, object);
            mapping.emplace(nextParsedId++, current.id);
            baseline.project.vectors.push_back({.name = current.name, .samples = {}, .id = current.id});
        }
        std::erase_if(baseline.project.pointMasses, [&](const auto& track) { return !liveIds.contains(track.id); });
        std::erase_if(baseline.project.vectors, [&](const auto& track) { return !liveIds.contains(track.id); });
        sortTracks(baseline.project);
        changed = true;
    }
    const bool renamed = patchTrackerNames(xml, expected, baseline.project, source);
    changed = renamed || changed;
    auditFinalSchema = auditFinalSchema || renamed;
    // Name patching uses original source ordinals, so erase nodes only after its reference updates.
    if (membershipChanged)
    {
        patchTrackerDeletedReferences(xml, expected, source);
        for (const auto& [id, object] : objects)
        {
            if (!liveIds.contains(id))
                tracks.removeChild(object.parentNode());
        }
        for (auto item = tracks.firstChildElement(); !item.isNull(); item = item.nextSiblingElement())
            tracks.insertBefore(xml.createTextNode("\n"), item);
        tracks.appendChild(xml.createTextNode("\n"));
    }
    if (expected.calibration != baseline.project.calibration)
    {
        if (!expected.calibration || !baseline.project.calibration)
        {
            require(false); // Creating/removing coordinate systems is not covered by this writer slice.
        }
        else
        {
            patchCalibration(xml, *baseline.project.calibration, *expected.calibration, expected);
            massCoordinatesChecked = true;
            baseline.project.calibration = expected.calibration;
            changed = true;
        }
    }
    if (expected.lengthUnit != baseline.project.lengthUnit)
    {
        // A unit is a label; any requested rescaling belongs to the reversible calibration edit.
        patchUnit(xml, "length_unit", expected.lengthUnit);
        baseline.project.lengthUnit = expected.lengthUnit;
        changed = true;
    }
    if (expected.massUnit != baseline.project.massUnit)
    {
        patchUnit(xml, "mass_unit", expected.massUnit);
        baseline.project.massUnit = expected.massUnit;
        changed = true;
    }
    if (expected.timeUnit != baseline.project.timeUnit)
    {
        patchUnit(xml, "time_unit", expected.timeUnit);
        baseline.project.timeUnit = expected.timeUnit;
        changed = true;
    }
    if (expected.clip.firstFrame != baseline.project.clip.firstFrame || expected.clip.stepSize != baseline.project.clip.stepSize ||
        expected.clip.stepCount != baseline.project.clip.stepCount)
    {
        patchClipRange(xml, expected.clip, media);
        baseline.project.clip.firstFrame = expected.clip.firstFrame;
        baseline.project.clip.stepSize = expected.clip.stepSize;
        baseline.project.clip.stepCount = expected.clip.stepCount;
        changed = true;
    }
    if (expected.clip.frameDurationMs != baseline.project.clip.frameDurationMs ||
        expected.clip.startTimeMs != baseline.project.clip.startTimeMs)
    {
        patchClipTiming(xml, expected);
        baseline.project.clip.frameDurationMs = expected.clip.frameDurationMs;
        baseline.project.clip.startTimeMs = expected.clip.startTimeMs;
        changed = true;
    }
    for (std::size_t index = 0; index < expected.pointMasses.size(); ++index)
    {
        const auto& current = expected.pointMasses[index];
        auto& original = baseline.project.pointMasses[index];
        require(current.id == original.id && std::isfinite(current.mass) && current.mass >= MIN_POINT_MASS);
        if (current.mass != original.mass)
        {
            patchDouble(xml, objects.at(current.id), "mass", current.mass);
            original.mass = current.mass;
            changed = true;
        }
        if (current.marks != original.marks)
        {
            if (!massCoordinatesChecked)
            {
                require(expected.calibration || uniqueProperty(xml.documentElement(), "coords").isNull());
                if (expected.calibration)
                    requireTrackerCoordinates(expected, *expected.calibration);
                massCoordinatesChecked = true;
            }
            patchMarks(xml, objects.at(current.id), original.marks, current.marks);
            original.marks = current.marks;
            changed = true;
        }
    }
    for (std::size_t index = 0; index < expected.vectors.size(); ++index)
    {
        const auto& current = expected.vectors[index];
        auto& original = baseline.project.vectors[index];
        require(current.id == original.id);
        if (current.samples != original.samples)
        {
            patchFrameData(xml,
                           objects.at(current.id),
                           original.samples,
                           current.samples,
                           "org.opensourcephysics.cabrillo.tracker.Vector$FrameData",
                           [](auto& document, const auto& step, const VectorSample& sample)
                           {
                               require(std::isfinite(sample.tailX) && std::isfinite(sample.tailY) && std::isfinite(sample.componentX) &&
                                       std::isfinite(sample.componentY) && retainsVectorComponent(sample.tailX, sample.componentX) &&
                                       retainsVectorComponent(sample.tailY, sample.componentY));
                               patchDouble(document, step, "xtail", sample.tailX);
                               patchDouble(document, step, "ytail", sample.tailY);
                               patchDouble(document, step, "xcomponent", sample.componentX);
                               patchDouble(document, step, "ycomponent", sample.componentY);
                           });
            original.samples = current.samples;
            changed = true;
        }
    }
    // Never produce a superficially valid file that silently omits an unsupported edit.
    require(baseline.project == expected);
    if (!changed)
    {
        return source.xml;
    }
    requireStableTableColumns(xml, expected, media == LinkedMedia::Movie);
    // Pinned Tracker 6.3.5 never initializes saved VectorSum members and loses their names on resave.
    // Do not let clearing all independent samples bypass the nonempty-vector safeguards below.
    require(std::ranges::find(expected.unsupportedTrackTypes, "org.opensourcephysics.cabrillo.tracker.VectorSum") ==
            expected.unsupportedTrackTypes.end());
    requireStableVectors(expected, objects);
    requireBoundedMassFrames(xml);
    if (auditFinalSchema)
    {
        // Include appended metadata and patched samples in the reimport/edit budget.
        if (media == LinkedMedia::Movie)
            validateTrackerMovieEditSchema(xml);
        else
            validateTrackerEditSchema(xml);
    }
    // QDom emits UTF-8 bytes; do not retain an input declaration for a different encoding.
    for (auto node = xml.firstChild(); !node.isNull(); node = node.nextSibling())
    {
        if (node.isProcessingInstruction() && node.nodeName() == "xml")
        {
            node.setNodeValue(R"(version="1.0" encoding="UTF-8")");
        }
    }
    // A nonnegative indent inserts newlines even inside untouched scalar/mixed/xml:space content.
    auto output = xml.toByteArray(-1).toStdString();
    require(output.size() <= MAX_TRACKER_XML_BYTES);
    auto reparsed = parseLegacyProject(output);
    remapIds(reparsed, mapping, project.nextTrackId);
    require(reparsed == expected);
    return output;
}

void writeXml(const QString& path, const std::string& xml)
{
    require(path.endsWith(".trk", Qt::CaseInsensitive));
    QSaveFile output(path);
    const auto size = static_cast<qint64>(xml.size());
    if (!output.open(QIODevice::WriteOnly) || output.write(xml.data(), size) != size || !output.commit())
    {
        throw ProjectError("Could not write Tracker project: " + output.errorString().toStdString());
    }
}

void requireLocalReference(const std::string& reference)
{
    const auto local = QString::fromStdString(reference);
    require(!local.isEmpty() && local.size() <= 4096 && local.toStdString() == reference && !local.contains("!/") &&
            !local.contains("://") && !local.startsWith(':') && !local.contains("]]>"));
    for (const auto point : local.toUcs4())
        require(QChar::category(point) != QChar::Other_Control && QChar::category(point) != QChar::Other_Format);
}

} // namespace

std::string renderTrackerProject(const LegacyProject& project, const TrackerSource& source)
{
    return renderTrackerProjectImpl(project, source, LinkedMedia::None);
}

void writeTrackerProject(const QString& path, const LegacyProject& project, const TrackerSource& source)
{
    writeXml(path, renderTrackerProject(project, source));
}

namespace
{
std::string prepareTrackerDocument(const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath)
{
    if (!document.trackerSource)
        throw ProjectError("Tracker export requires its original source.");
    const auto& source = *document.trackerSource;
    require(!document.useDisplayOrientation && !document.archivePayload && !document.archiveMember && document.archiveReference.empty());
    require(document.movie ? document.alignMovieToClip && !document.movieFrameTimes.empty() : document.movieFrameTimes.empty());
    auto project = document.project;
    const auto media = document.movie ? LinkedMedia::Movie : project.clip.mediaReferences.empty() ? LinkedMedia::None : LinkedMedia::Images;
    if (media != LinkedMedia::None)
    {
        require(project.clip.mediaReferences.size() <= 10000 && !mediaProjectPath.isEmpty() && QFileInfo(mediaProjectPath).isAbsolute());
        // A file symlink can be opened under two different project directories. Require a
        // regular destination; directory aliases are resolved consistently by the reader.
        require(!QFileInfo(path).isSymbolicLink());
        const auto targetParent = localProjectDirectory(path);
        require(!targetParent.isEmpty());
        const QDir targetDirectory(targetParent);
        for (auto& reference : project.clip.mediaReferences)
        {
            requireLocalReference(reference);
            const auto resolved = workspaceMediaPath(document, mediaProjectPath, reference);
            require(!resolved.isEmpty() && !sameLocalFile(resolved, targetDirectory.filePath(QFileInfo(path).fileName())));
            QFile file(resolved);
            if (!QFileInfo(resolved).isFile() || !file.open(QIODevice::ReadOnly))
                throw ProjectError("Linked media is missing or unreadable: " + resolved.toStdString());
            reference = QDir::fromNativeSeparators(targetDirectory.relativeFilePath(resolved)).toStdString();
            requireLocalReference(reference); // Rebasing can introduce a forbidden marker from a parent directory.
            require(resolveLocalMediaPath(path, reference) == resolved);
        }
        // Native frame bounds are runtime metadata, not an edit to the preserved standard clip.
        if (project.clip.sourceFrameCount)
        {
            const auto count = *project.clip.sourceFrameCount;
            const auto clipEnd = static_cast<std::int64_t>(project.clip.firstFrame) +
                                 (static_cast<std::int64_t>(project.clip.stepCount.value_or(0)) - 1) * project.clip.stepSize + 1;
            require(count > 0 &&
                    (media == LinkedMedia::Images ? std::cmp_equal(count, project.clip.mediaReferences.size())
                                                  : std::cmp_equal(count, document.movieFrameTimes.size()) || count == clipEnd));
            project.clip.sourceFrameCount.reset();
        }
    }
    return renderTrackerProjectImpl(project, source, media, document.movieFrameTimes);
}
} // namespace

void writeTrackerDocument(const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath)
{
    writeXml(path, prepareTrackerDocument(path, document, mediaProjectPath));
}

void writeTrackerArchiveDocument(const QString& path, const WorkspaceDocument& document, const std::atomic_bool* cancelled)
{
    require(!cancelled || !cancelled->load());
    require(!document.movie);
    if (!document.archivePayload || !document.archiveMember || !document.trackerSource)
        throw ProjectError("Tracker archive export requires its original archive and selected project.");
    require(document.archiveReference.empty());
    const auto& archive = *document.archivePayload;
    const auto member = QString::fromStdString(*document.archiveMember);
    require(member.toStdString() == *document.archiveMember);
    require(archive.memberMatchesSource(member, cancelled));
    const auto originalPath = archive.projectPath(member);
    QFile original(originalPath);
    require(original.open(QIODevice::ReadOnly) && original.size() <= static_cast<qint64>(MAX_TRACKER_XML_BYTES) &&
            original.readAll().toStdString() == document.trackerSource->xml);
    auto baseline = parseLegacyProject(document.trackerSource->xml);
    archive.resolveMedia(baseline, member);
    require(baseline.clip.mediaReferences == document.project.clip.mediaReferences);
    std::unordered_set<std::string> checkedMedia;
    for (const auto& reference : document.project.clip.mediaReferences)
        if (checkedMedia.insert(reference).second && !archive.memberMatchesSource(QString::fromStdString(reference), cancelled))
            throw ProjectError("Tracker archive media changed after import; unchanged-media export cannot preserve these edits.");
    auto snapshot = document;
    snapshot.archivePayload.reset();
    snapshot.archiveMember.reset();
    // Imported archive references are validated archive-root-relative members, independent of
    // the original archive's location or the companion document's later location.
    const auto xml = prepareTrackerDocument(originalPath, snapshot, QDir(archive.root()).filePath("media-base.motrack"));
    auto exported = parseLegacyProject(xml);
    archive.resolveMedia(exported, member);
    require(exported.clip.mediaReferences == baseline.clip.mediaReferences);
    writeTrackerArchive(path, archive, member, xml, cancelled);
}

} // namespace motrack
