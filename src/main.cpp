#include "legacy_project.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QToolBar>
#include <QTranslator>

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace
{

class MainWindow final : public QMainWindow
{
    Q_DECLARE_TR_FUNCTIONS(MainWindow)

  public:
    MainWindow()
    {
        setWindowTitle("MoTrack");
        resize(1000, 650);
        auto* open = new QAction(tr("&Open Tracker project…"), this);
        open->setShortcut(QKeySequence::Open);
        menuBar()->addMenu(tr("&File"))->addAction(open);
        addToolBar(tr("Project"))->addAction(open);

        auto* workspace = new QSplitter;
        auto* video = new QLabel(tr("Open a Tracker project to inspect its saved marks."));
        video->setAlignment(Qt::AlignCenter);
        video->setWordWrap(true);
        workspace->addWidget(video);
        m_Marks = new QTableWidget(0, 4);
        m_Marks->setHorizontalHeaderLabels({tr("Track"), tr("Frame"), tr("Image x (px)"), tr("Image y (px)")});
        m_Marks->setEditTriggers(QAbstractItemView::NoEditTriggers);
        workspace->addWidget(m_Marks);
        workspace->setStretchFactor(0, 2);
        workspace->setStretchFactor(1, 1);
        setCentralWidget(workspace);
        statusBar()->showMessage(tr("Ready"));

        connect(open,
                &QAction::triggered,
                this,
                [this]
                {
                    const auto path = QFileDialog::getOpenFileName(this, tr("Open Tracker project"), {}, tr("Tracker projects (*.trk)"));
                    if (!path.isEmpty())
                    {
                        loadProject(path);
                    }
                });
    }

    bool loadProject(const QString& path)
    {
        try
        {
            const auto project = motrack::readLegacyProject(std::filesystem::path(path.toStdU16String()));
            m_Marks->setRowCount(0);
            for (const auto& track : project.pointMasses)
            {
                for (const auto& mark : track.marks)
                {
                    const auto row = m_Marks->rowCount();
                    m_Marks->insertRow(row);
                    m_Marks->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(track.name)));
                    m_Marks->setItem(row, 1, new QTableWidgetItem(QString::number(mark.frame)));
                    m_Marks->setItem(row, 2, new QTableWidgetItem(QString::number(mark.x, 'g', 12)));
                    m_Marks->setItem(row, 3, new QTableWidgetItem(QString::number(mark.y, 'g', 12)));
                }
            }
            statusBar()->showMessage(
                tr("Imported %1 marks. Unsupported tracks: %2.").arg(m_Marks->rowCount()).arg(project.unsupportedTrackTypes.size()));
            return true;
        }
        catch (const motrack::ProjectError&)
        {
            statusBar()->showMessage(tr("Could not open this Tracker project."));
            return false;
        }
    }

  private:
    QTableWidget* m_Marks;
};

} // namespace

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
    MainWindow window;
    window.show();
    if (args.size() == 3 && args[1] == "--smoke-test")
    {
        return window.loadProject(args[2]) ? 0 : 1;
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
