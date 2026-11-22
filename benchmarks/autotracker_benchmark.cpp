#include "app_locale.hpp"
#include "legacy_view.hpp"
#include "model/tracker_document.hpp"
#include "template_tracker.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QString>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Docs: tracker-document.md, open question 5. During an AutoTracker run each accepted match is one mark edit and one
// document revision, and the index-based views read the legacy view, which is rebuilt once per revision. This
// measures, per AutoTracker step, the typed edit (copy-on-write of one frame chunk), the full legacy view rebuild,
// today's in-place TrackEdits mark and the template match itself, for the longest point mass of the given experiment
// and for the same track extended to longer runs.
namespace
{

using Clock = std::chrono::steady_clock;

double microseconds(Clock::duration duration, std::size_t count)
{
    return std::chrono::duration<double, std::micro>(duration).count() / static_cast<double>(std::max<std::size_t>(count, 1));
}

std::string readXml(const QString& argument, std::shared_ptr<motrack::TrackerArchive>& archive)
{
    auto path = argument;
    if (const auto bang = argument.indexOf(".trz!"); bang >= 0)
    {
        archive = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(argument.left(bang + 4)));
        path = archive->projectPath(argument.mid(bang + 5));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        throw std::runtime_error("Could not open " + argument.toStdString());
    }
    return file.readAll().toStdString();
}

/// The document with its longest point mass extended to `length` marks (the original marks first, then each later
/// frame one pixel further along), and that track's identity.
std::pair<motrack::model::TrackerDocument, motrack::model::ObjectId> extended(const motrack::model::TrackerDocument& document,
                                                                              std::size_t length)
{
    auto parts = document.parts();
    motrack::model::ObjectSlot* longest = nullptr;
    for (auto& object : parts.objects)
    {
        if (const auto* mass = std::get_if<motrack::model::PointMassState>(object.value.get());
            mass && object.state == motrack::model::ObjectState::Live &&
            (!longest || mass->marks.size() > std::get<motrack::model::PointMassState>(*longest->value).marks.size()))
        {
            longest = &object;
        }
    }
    if (!longest)
    {
        throw std::runtime_error("The experiment has no point mass.");
    }
    auto state = std::get<motrack::model::PointMassState>(*longest->value);
    auto next = state.marks.range() ? state.marks.range()->second + 1 : 0;
    while (state.marks.size() < length)
    {
        state.marks.set(next, {.value = {.x = 10.0 + next % 600, .y = 20.0 + next % 400}});
        ++next;
    }
    longest->value = std::make_shared<const motrack::model::TypedValue>(std::move(state));
    const auto id = longest->id;
    return {motrack::model::TrackerDocument::assemble(std::move(parts)).value(), id};
}

void run(const motrack::model::TrackerDocument& document,
         motrack::model::ObjectId id,
         const QImage& frame,
         const motrack::TemplateTracker& tracker)
{
    const auto& base = std::get<motrack::model::PointMassState>(*document.object(id)->value);
    const auto marks = base.marks.entries();
    const auto steps = marks.size();

    // The typed edit a SetField commit makes: a new value sharing every chunk but the edited frame's.
    std::size_t copied = 0;
    auto start = Clock::now();
    for (const auto& [number, element] : marks)
    {
        auto edited = std::make_shared<motrack::model::TypedValue>(base);
        auto& state = std::get<motrack::model::PointMassState>(*edited);
        state.marks.set(number, {.value = {.x = element.value.x + 0.5, .y = element.value.y}, .node = element.node});
        copied += state.marks.chunkCount() - state.marks.sharedChunkCount(base.marks);
    }
    const auto typed = microseconds(Clock::now() - start, steps);

    // The legacy view the next paint reads: rebuilt in full once per revision. Its cost depends on the track's length,
    // not on the edit, so a sample of rebuilds gives the per-step cost.
    const auto rebuilds = std::min<std::size_t>(steps, 200);
    start = Clock::now();
    std::size_t checksum = 0;
    for (std::size_t step = 0; step < rebuilds; ++step)
    {
        checksum += motrack::legacyProject(document).pointMasses.size();
    }
    const auto view = microseconds(Clock::now() - start, rebuilds);

    // Today: TrackEdits marks the reduced model in place, with one undo entry per mark.
    auto project = motrack::legacyProject(document);
    const auto index =
        static_cast<std::size_t>(std::ranges::find(project.pointMasses, id.value, &motrack::PointMass::id) - project.pointMasses.begin());
    motrack::TrackEdits edits;
    start = Clock::now();
    for (const auto& [number, element] : marks)
    {
        checksum += edits.markPointMass(project, index, {.frame = number, .x = element.value.x + 0.5, .y = element.value.y}) ? 1 : 0;
    }
    const auto legacy = microseconds(Clock::now() - start, steps);

    // The match AutoTracker runs on each decoded frame (decoding excluded).
    start = Clock::now();
    for (std::size_t step = 0; step < steps; ++step)
    {
        checksum += tracker.find(frame, QPoint(320, 240)) ? 1 : 0;
    }
    const auto match = microseconds(Clock::now() - start, steps);

    std::cout << steps << " marks: typed edit " << typed << " us (" << static_cast<double>(copied) / static_cast<double>(steps)
              << " chunks copied), legacy view rebuild " << view << " us, TrackEdits mark " << legacy << " us, template match " << match
              << " us; rebuild/match " << view / match << ", whole run's rebuilds " << view * static_cast<double>(steps) / 1000.0
              << " ms (checksum " << checksum << ")\n"
              << std::flush;
}

} // namespace

int main(int argc, char** argv)
try
{
    QCoreApplication application(argc, argv);
    motrack::configureProcessLocale();
    if (argc < 2)
    {
        std::fputs("Usage: motrack_autotracker_benchmark <project.trk|archive.trz!member.trk> [run lengths...]\n", stderr);
        return 1;
    }
    const auto arguments = QCoreApplication::arguments();
    std::shared_ptr<motrack::TrackerArchive> archive;
    const auto document = motrack::importTypedTracker(readXml(arguments.at(1), archive));
    std::vector<std::size_t> lengths{0};
    for (const auto& argument : arguments.mid(2))
    {
        const auto text = argument.toStdString();
        std::size_t length = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), length).ec != std::errc{} || length > 500000)
        {
            throw std::runtime_error("Run lengths are mark counts up to 500000.");
        }
        lengths.push_back(length);
    }
    // A 640 x 480 frame with a bright disk, as BallToss's video shows a ball.
    QImage frame(640, 480, QImage::Format_RGB32);
    frame.fill(Qt::black);
    {
        QPainter painter(&frame);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPoint(322, 241), 6, 6);
    }
    const auto tracker = motrack::TemplateTracker::fromImage(frame, QPoint(322, 241));
    if (!tracker)
    {
        throw std::runtime_error("Could not take the template.");
    }
    for (const auto length : lengths)
    {
        const auto [sized, id] = extended(document, length);
        run(sized, id, frame, *tracker);
    }
    return 0;
}
catch (const std::exception& error)
{
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return 1;
}
