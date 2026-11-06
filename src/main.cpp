#include "main_window.hpp"

#include <QApplication>
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
    if (args.size() == 2 || smokeTest)
    {
        const bool opened = window.loadProject(args.last());
        if (smokeTest)
        {
            return opened ? EXIT_SUCCESS : EXIT_FAILURE;
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
