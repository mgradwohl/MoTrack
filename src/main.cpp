#include "app_locale.hpp"
#include "desktop_launch.hpp"
#include "ui_translations.hpp"
#include "workspace_window.hpp"

#include <QApplication>
#include <QDir>
#include <QLocale>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace
{

void writeLine(std::FILE* stream, const QString& text)
{
    std::fputs((text + '\n').toUtf8().constData(), stream);
}

} // namespace

int main(int argc, char** argv)
try
{
    QApplication app(argc, argv);
    // QApplication sets the C locale from the environment on Unix: put LC_NUMERIC back to "C" before anything
    // parses or prints a number.
    motrack::configureProcessLocale();
    // macOS can deliver Finder and Dock open requests as soon as events are processed; keep them until the window exists.
    motrack::FileOpenRequests openRequests;
    app.installEventFilter(&openRequests);
    const motrack::UiTranslations translations(QLocale{});
    // Relative arguments refer to the directory the launcher started in.
    auto launch = motrack::parseLaunchArguments(app.arguments(), QDir::currentPath());
    for (const auto& option : launch.unknownOptions)
        writeLine(stderr, "MoTrack: ignoring unknown option " + option);
    motrack::WorkspaceWindow window;
    window.show();
    if (launch.smokeTest)
    {
        // Report each opened source in tab order so callers can check paths across the process boundary.
        QObject::connect(&window,
                         &motrack::WorkspaceWindow::filesOpened,
                         &app,
                         [&window](const QStringList& problems, const QStringList& notices)
                         {
                             auto& session = window.context()->session();
                             for (const auto document : session.tabs())
                                 writeLine(stdout, "opened\t" + session.sourcePath(session.sourceId(document)));
                             for (const auto& problem : problems)
                                 writeLine(stderr, "MoTrack: " + problem);
                             // Archive limits such as skipped unsafe members are warnings; the files opened.
                             for (const auto& notice : notices)
                                 writeLine(stderr, "MoTrack notice: " + notice);
                             std::fflush(stdout);
                             QCoreApplication::exit(problems.isEmpty() && !session.tabs().empty() ? EXIT_SUCCESS : EXIT_FAILURE);
                         });
        QTimer::singleShot(15000, &app, [] { QCoreApplication::exit(EXIT_FAILURE); });
    }
    // Without arguments a smoke test waits for an operating-system open request, such as `open -a` on macOS.
    if (!launch.requested.files.isEmpty() || !launch.requested.remote.isEmpty())
        window.openFiles(std::move(launch.requested));
    openRequests.attach(&window);
    return app.exec();
}
catch (const std::exception& error)
{
    std::fputs("MoTrack: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
}
