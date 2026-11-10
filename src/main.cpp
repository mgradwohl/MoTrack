#include "workspace_window.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char** argv)
try
{
    QApplication app(argc, argv);
    const QLocale locale;
    QTranslator qtTranslator;
    const auto loadQtCatalog = [&](const QString& directory)
    {
        return qtTranslator.load(locale, "qtbase", "_", directory) || qtTranslator.load(locale, "qt", "_", directory);
    };
    auto qtLoaded = loadQtCatalog(QLibraryInfo::path(QLibraryInfo::TranslationsPath));
#ifdef Q_OS_MACOS
    qtLoaded = qtLoaded || loadQtCatalog(QCoreApplication::applicationDirPath() + "/../Resources/translations");
#endif
    if (qtLoaded)
    {
        app.installTranslator(&qtTranslator);
    }
    QTranslator translator;
    if (translator.load(locale, "motrack", "_", ":/i18n"))
    {
        app.installTranslator(&translator);
    }
    const auto args = app.arguments();
    motrack::WorkspaceWindow window;
    window.show();
    const bool smokeTest = args.size() == 3 && args[1] == "--smoke-test";
    if (smokeTest)
    {
        QObject::connect(
            window.context().get(), &motrack::WorkspaceContext::inputOpened, &app, [] { QCoreApplication::exit(EXIT_SUCCESS); });
        QObject::connect(
            window.context().get(), &motrack::WorkspaceContext::inputFailed, &app, [] { QCoreApplication::exit(EXIT_FAILURE); });
        QTimer::singleShot(15000, &app, [] { QCoreApplication::exit(EXIT_FAILURE); });
        (void)window.openProject(args.last());
    }
    else if (args.size() == 2)
    {
        const auto extension = QFileInfo(args.last()).suffix();
        if (extension.compare("trk", Qt::CaseInsensitive) == 0 || extension.compare("trz", Qt::CaseInsensitive) == 0 ||
            extension.compare("motrack", Qt::CaseInsensitive) == 0)
        {
            (void)window.openProject(args.last());
        }
        else
        {
            (void)window.openMovie(args.last());
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
