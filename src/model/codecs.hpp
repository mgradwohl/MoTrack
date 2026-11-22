#pragma once

#include <string_view>

/// The label and name codecs that standard export applies to edited Tracker text. They are part of the typed
/// model's domain checks (docs/tracker-document.md, "Validation and atomicity"). The implementation classifies
/// characters with Qt Core's QChar tables, linked privately, so the rules match the ones MoTrack has always applied;
/// the interface is standard-only.
namespace motrack::model
{

/// Whether standard export can store `unit` as a Tracker unit label: non-empty, without surrounding white space,
/// at most 256 UTF-16 units of valid UTF-8, and free of control, format and digit characters.
[[nodiscard]] bool isTrackerUnitLabel(std::string_view unit);

/// Whether standard export can store `name` as a Tracker track name that saved views and references can name again:
/// valid UTF-8 of 1 to 4096 UTF-16 units, without surrounding white space, not "null", free of the packed view
/// separators ',' and ';', of "]]>" (which would end its CDATA section) and of control and format characters.
[[nodiscard]] bool isTrackerTrackName(std::string_view name);

} // namespace motrack::model
