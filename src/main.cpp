#include "main_window.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QLocale>
#include <QTranslator>

#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char** argv)
try
{
    QApplication app(argc, argv);
    QTranslator translator;
    if (translator.load(QLocale(), "motrack", "_", ":/i18n"))
    {
        app.installTranslator(&translator);
    }
    const auto args = app.arguments();
    motrack::MainWindow window;
    window.show();
    const bool smokeTest = args.size() == 3 && args[1] == "--smoke-test";
    if (smokeTest)
    {
        return window.loadProject(args.last()) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (args.size() == 2)
    {
        const auto extension = QFileInfo(args.last()).suffix();
        if (extension.compare("trk", Qt::CaseInsensitive) == 0 || extension.compare("motrack", Qt::CaseInsensitive) == 0)
        {
            window.loadProject(args.last());
        }
        else
        {
            window.openMovie(args.last());
        }
    }
    return app.exec();
}
catch (const std::exception& error)
{
    std::fputs("MoTrack: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
}
