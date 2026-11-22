#pragma once

#include "osp/render.hpp"
#include "osp/value.hpp"

#include <QByteArray>
#include <QDomDocument>
#include <QString>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

/// Test helper: writes compact XMLControl snippets (one line, single quotes, `type` left out where obvious) in
/// the layout OSP's own writer produces, so synthetic test documents read in MoTrack exactly as in Tracker.
/// QDom only decodes the snippet; the written document is what `motrack_osp` and Tracker read.
namespace motrack::test
{

inline bool indexedName(const QString& name)
{
    return name.size() > 2 && name.startsWith('[') && name.endsWith(']');
}

inline osp::ObjectValue layoutObject(const QDomElement& element);

inline osp::PropertyValue layoutProperty(const QDomElement& element)
{
    osp::PropertyValue value;
    const auto name = element.attribute("name");
    if (element.hasAttribute("name"))
    {
        value.name = name.toStdString();
    }
    const auto object = element.firstChildElement("object");
    const auto first = element.firstChildElement("property");
    auto type = element.attribute("type");
    if (type.isEmpty())
    {
        // An object child, or an "[n]" element reading "null", is an object property.
        if (!object.isNull() || (first.isNull() && indexedName(name) && element.text() == "null"))
        {
            type = "object";
        }
        else if (!first.isNull())
        {
            bool indexed = true;
            for (auto child = first; !child.isNull(); child = child.nextSiblingElement("property"))
            {
                indexed = indexed && indexedName(child.attribute("name"));
            }
            type = indexed ? "array" : "collection";
        }
        else
        {
            type = "string";
        }
    }
    value.typeName = type.toStdString();
    if (type == "array" || type == "collection")
    {
        value.className =
            element.hasAttribute("class") ? element.attribute("class").toStdString() : (type == "collection" ? "java.util.ArrayList" : "");
        for (auto child = first; !child.isNull(); child = child.nextSiblingElement("property"))
        {
            value.elements.push_back(layoutProperty(child));
        }
    }
    else if (type == "object")
    {
        if (!object.isNull())
        {
            value.object = std::make_shared<const osp::ObjectValue>(layoutObject(object));
        }
    }
    else
    {
        value.content = element.text().toStdString();
    }
    return value;
}

inline osp::ObjectValue layoutObject(const QDomElement& element)
{
    osp::ObjectValue value{.className = element.attribute("class").toStdString()};
    for (auto child = element.firstChildElement("property"); !child.isNull(); child = child.nextSiblingElement("property"))
    {
        value.properties.push_back(layoutProperty(child));
    }
    return value;
}

/// The snippet as OSP writes it (an XML declaration, four-space indentation and LF line endings).
inline std::string ospLayout(std::string_view xml)
{
    QDomDocument document;
    if (!document.setContent(QByteArray(xml.data(), static_cast<qsizetype>(xml.size()))))
    {
        throw std::invalid_argument("Test XML is not well formed.");
    }
    auto written = osp::write(layoutObject(document.documentElement()));
    if (!written)
    {
        throw std::invalid_argument("OSP cannot write the test XML: " + written.error().message);
    }
    return std::move(*written);
}

/// The property lines of a compact snippet in OSP's layout (four-space indentation, LF endings), for insertion
/// into an object of a document written by OSP or Tracker.
inline std::string ospProperties(std::string_view properties)
{
    const auto document = ospLayout("<object class='Fragment'>" + std::string(properties) + "</object>");
    const auto begin = document.find('\n', document.find("<object class=\"Fragment\">")) + 1;
    return document.substr(begin, document.rfind("</object>") - begin);
}

} // namespace motrack::test
