#include "main_window.hpp"

#include "local_media_path.hpp"
#include "measurements.hpp"
#include "movie_worker.hpp"
#include "position_plot.hpp"
#include "track_identity.hpp"
#include "video_canvas.hpp"
#include "video_reader.hpp"
#include "workspace_document.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QCollator>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QThreadPool>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace motrack
{

namespace
{
QString physicalSourcePath(const QString& path)
{
    const QFileInfo info(path);
    const auto physical = info.canonicalFilePath();
    return physical.isEmpty() ? info.absoluteFilePath() : physical;
}
} // namespace

struct MainWindow::PreparedProject
{
    WorkspaceDocument document;
    std::shared_ptr<const TrackerArchive> archive;
    QString member;
    QString mediaProjectPath;
    bool chooseArchiveTab = false;
};

MainWindow::MainWindow()
{
    m_SavePool = new QThreadPool(this);
    m_SavePool->setObjectName("workspaceSavePool");
    m_SavePool->setMaxThreadCount(1);
    setWindowTitle("MoTrack");
    resize(1000, 650);
    auto* open = new QAction(tr("&Open project…"), this);
    open->setObjectName("openProject");
    open->setShortcut(QKeySequence::Open);
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(open);
    auto* openMovieAction = fileMenu->addAction(tr("Open &video…"));
    auto* openImagesAction = fileMenu->addAction(tr("Open &images…"));
    openImagesAction->setObjectName("openImages");
    m_Save = fileMenu->addAction(tr("&Save project"));
    m_Save->setObjectName("saveProject");
    m_Save->setShortcut(QKeySequence::Save);
    m_Save->setEnabled(false);
    m_SaveAs = fileMenu->addAction(tr("Save project &as…"));
    m_SaveAs->setObjectName("saveWorkspace");
    m_SaveAs->setShortcut(QKeySequence::SaveAs);
    m_SaveAs->setEnabled(false);
    m_Export = new QAction(tr("&Export measurements…"), this);
    m_Export->setObjectName("exportMeasurements");
    m_Export->setEnabled(false);
    fileMenu->addAction(m_Export);
    m_ExportPlot = fileMenu->addAction(tr("Export &plot data…"));
    m_ExportPlot->setObjectName("exportPlotData");
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    m_Undo = editMenu->addAction(tr("&Undo"));
    m_Undo->setObjectName("undoEdit");
    m_Undo->setShortcut(QKeySequence::Undo);
    m_Redo = editMenu->addAction(tr("&Redo"));
    m_Redo->setObjectName("redoEdit");
    m_Redo->setShortcut(QKeySequence::Redo);
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    m_ShowAxes = viewMenu->addAction(tr("Show coordinate &axes"));
    m_ShowAxes->setObjectName("showCoordinateAxes");
    m_ShowAxes->setCheckable(true);
    auto* zoomIn = viewMenu->addAction(tr("Zoom &in"));
    zoomIn->setObjectName("zoomVideoIn");
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    auto* zoomOut = viewMenu->addAction(tr("Zoom &out"));
    zoomOut->setObjectName("zoomVideoOut");
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    auto* resetZoom = viewMenu->addAction(tr("&Fit video to view"));
    resetZoom->setObjectName("resetVideoZoom");
    auto* tracksMenu = menuBar()->addMenu(tr("&Tracks"));
    m_NewPointMass = tracksMenu->addAction(tr("New &point mass"));
    m_NewPointMass->setObjectName("newPointMass");
    m_NewPointMass->setEnabled(false);
    m_NewVector = tracksMenu->addAction(tr("New &vector"));
    m_NewVector->setObjectName("newVector");
    m_NewVector->setEnabled(false);
    m_RenamePointMass = tracksMenu->addAction(tr("&Rename track…"));
    m_RenamePointMass->setObjectName("renamePointMass");
    m_SetPointMass = tracksMenu->addAction(tr("Set &mass…"));
    m_SetPointMass->setObjectName("setPointMassValue");
    m_RemovePointMass = tracksMenu->addAction(tr("&Delete track"));
    m_RemovePointMass->setObjectName("removePointMass");
    m_RemoveMark = tracksMenu->addAction(tr("Delete mark on this &frame"));
    m_RemoveMark->setObjectName("removeMark");
    m_AutoTracker = tracksMenu->addAction(tr("&AutoTracker"));
    m_AutoTracker->setObjectName("autoTracker");
    m_AutoTracker->setCheckable(true);
    m_AutoTracker->setEnabled(false);
    auto* repeat = tracksMenu->addAction(tr("Repeat previous mark"));
    repeat->setObjectName("repeatMark");
    repeat->setShortcuts({QKeySequence("Shift+Return"), QKeySequence("Shift+Enter")});
    auto* toolsMenu = menuBar()->addMenu(tr("T&ools"));
    m_MeasureDistance = toolsMenu->addAction(tr("Measure &distance"));
    m_MeasureDistance->setObjectName("measureDistance");
    m_MeasureDistance->setCheckable(true);
    m_MeasureDistance->setEnabled(false);
    m_MeasureAngle = toolsMenu->addAction(tr("Measure &angle"));
    m_MeasureAngle->setObjectName("measureAngle");
    m_MeasureAngle->setCheckable(true);
    m_MeasureAngle->setEnabled(false);
    auto* tools = addToolBar(tr("Project"));
    tools->addAction(open);
    m_ClipSettings = tools->addAction(tr("Clip Settings"));
    m_ClipSettings->setObjectName("clipSettings");
    m_ClipSettings->setEnabled(false);
    m_Calibrate = tools->addAction(tr("Calibration Stick"));
    m_Calibrate->setObjectName("calibrationStick");
    m_Calibrate->setCheckable(true);
    m_Calibrate->setEnabled(false);
    tools->addAction(m_NewPointMass);
    tools->addAction(m_NewVector);
    tools->addAction(m_AutoTracker);
    m_EditAxes = tools->addAction(tr("Set Axes"));
    m_EditAxes->setObjectName("setCoordinateAxes");
    m_EditAxes->setCheckable(true);
    m_EditAxes->setEnabled(false);
    m_TrackPicker = new QComboBox;
    m_TrackPicker->setObjectName("trackPicker");
    m_TrackPicker->setAccessibleName(tr("Selected track"));
    tools->addWidget(m_TrackPicker);

    auto* workspace = new QSplitter;
    auto* videoPane = new QWidget;
    auto* layout = new QVBoxLayout(videoPane);
    m_Video = new VideoCanvas;
    connect(zoomIn, &QAction::triggered, m_Video, &VideoCanvas::zoomIn);
    connect(zoomOut, &QAction::triggered, m_Video, &VideoCanvas::zoomOut);
    connect(resetZoom, &QAction::triggered, m_Video, &VideoCanvas::resetZoom);
    m_Video->setObjectName("videoCanvas");
    m_Video->setMinimumSize(240, 160);
    m_Video->setToolTip(tr("Shift-click to mark a point mass. For a vector, Shift-click its tail, then its tip."));
    auto* cancelVector = new QAction(this);
    cancelVector->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(cancelVector);
    connect(cancelVector, &QAction::triggered, this, &MainWindow::cancelVectorMark);
    m_Video->pick = [this](QPointF position, bool shift)
    {
        if (m_MeasureDistance->isChecked() || m_MeasureAngle->isChecked())
        {
            pickMeasurementPoint(position);
            return true;
        }
        if (m_EditAxes->isChecked())
        {
            pickAxesPoint(position);
            return true;
        }
        if (m_Calibrate->isChecked())
        {
            pickCalibrationPoint(position);
            return true;
        }
        if (shift)
        {
            markAt(position);
            return true;
        }
        return false;
    };
    m_Video->display({}, {}, tr("Open a video or a Tracker project."));
    layout->addWidget(m_Video, 1);

    auto* controls = new QHBoxLayout;
    m_Previous = new QToolButton;
    m_Previous->setObjectName("previousFrame");
    m_Previous->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_Previous->setAccessibleName(tr("Previous frame"));
    m_Previous->setToolTip(m_Previous->accessibleName());
    m_Next = new QToolButton;
    m_Next->setObjectName("nextFrame");
    m_Next->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    m_Next->setAccessibleName(tr("Next frame"));
    m_Next->setToolTip(m_Next->accessibleName());
    m_Play = new QToolButton;
    m_Play->setObjectName("playClip");
    m_PlayTimer.setSingleShot(true);
    m_AutoTrackTimer.setSingleShot(true);
    setPlaying(false);
    m_StepSlider = new QSlider(Qt::Horizontal);
    m_StepSlider->setObjectName("stepSlider");
    m_StepSlider->setTracking(false);
    m_StepSlider->setRange(0, 0);
    m_StepSlider->setAccessibleName(tr("Clip position"));
    m_FrameLabel = new QLabel;
    m_FrameLabel->setObjectName("frameLabel");
    controls->addWidget(m_Previous);
    controls->addWidget(m_Play);
    controls->addWidget(m_Next);
    controls->addWidget(m_StepSlider, 1);
    controls->addWidget(m_FrameLabel);
    layout->addLayout(controls);
    workspace->addWidget(videoPane);

    m_Marks = new QTableWidget(0, 6);
    m_Marks->setObjectName("marksTable");
    m_Marks->setHorizontalHeaderLabels({tr("Track"), tr("Frame"), tr("Image x (px)"), tr("Image y (px)"), tr("World x"), tr("World y")});
    m_Marks->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* analysis = new QSplitter(Qt::Vertical);
    auto* plotPane = new QWidget;
    auto* plotLayout = new QVBoxLayout(plotPane);
    m_PlotQuantity = new QComboBox;
    m_PlotQuantity->setObjectName("plotQuantity");
    m_PlotQuantity->setAccessibleName(tr("Plot quantity"));
    m_PlotQuantity->addItem(tr("x position"), static_cast<int>(PlotQuantity::PositionX));
    m_PlotQuantity->addItem(tr("y position"), static_cast<int>(PlotQuantity::PositionY));
    m_PlotQuantity->addItem(tr("x velocity"), static_cast<int>(PlotQuantity::VelocityX));
    m_PlotQuantity->addItem(tr("y velocity"), static_cast<int>(PlotQuantity::VelocityY));
    m_PlotQuantity->addItem(tr("speed"), static_cast<int>(PlotQuantity::Speed));
    m_PlotQuantity->addItem(tr("x acceleration"), static_cast<int>(PlotQuantity::AccelerationX));
    m_PlotQuantity->addItem(tr("y acceleration"), static_cast<int>(PlotQuantity::AccelerationY));
    m_PlotQuantity->addItem(tr("x momentum"), static_cast<int>(PlotQuantity::MomentumX));
    m_PlotQuantity->addItem(tr("y momentum"), static_cast<int>(PlotQuantity::MomentumY));
    m_PlotQuantity->addItem(tr("kinetic energy"), static_cast<int>(PlotQuantity::KineticEnergy));
    m_FitLine = new QCheckBox(tr("Linear fit"));
    m_FitLine->setObjectName("linearFit");
    m_FitSummary = new QLabel;
    m_FitSummary->setObjectName("linearFitSummary");
    m_FitSummary->setWordWrap(true);
    m_FitSummary->hide();
    m_Plot = new PositionPlot;
    m_Plot->seekFrame = [this](int frame)
    {
        m_StepSlider->setValue((frame - m_Session.document.project.clip.firstFrame) / m_Session.document.project.clip.stepSize);
    };
    auto* plotControls = new QHBoxLayout;
    plotControls->addWidget(m_PlotQuantity, 1);
    plotControls->addWidget(m_FitLine);
    plotLayout->addLayout(plotControls);
    plotLayout->addWidget(m_FitSummary);
    plotLayout->addWidget(m_Plot, 1);
    analysis->addWidget(plotPane);
    analysis->addWidget(m_Marks);
    workspace->addWidget(analysis);
    workspace->setStretchFactor(0, 2);
    workspace->setStretchFactor(1, 1);
    setCentralWidget(workspace);
    statusBar()->showMessage(tr("Ready"));

    displayStep(0);
    refreshEditViews(0);
    connect(m_StepSlider, &QSlider::valueChanged, this, &MainWindow::displayStep);
    connect(m_Previous, &QToolButton::clicked, m_StepSlider, [this] { m_StepSlider->triggerAction(QSlider::SliderSingleStepSub); });
    connect(m_Next, &QToolButton::clicked, m_StepSlider, [this] { m_StepSlider->triggerAction(QSlider::SliderSingleStepAdd); });
    connect(&m_PlayTimer, &QTimer::timeout, m_Next, &QToolButton::click);
    connect(&m_AutoTrackTimer, &QTimer::timeout, m_Next, &QToolButton::click);
    connect(m_Play,
            &QToolButton::clicked,
            this,
            [this]
            {
                stopAutoTracking();
                cancelMeasurement();
                m_EditAxes->setChecked(false);
                setPlaying(!m_Playing);
                if (m_Playing && m_StepSlider->value() == m_StepSlider->maximum())
                {
                    m_StepSlider->setValue(0);
                }
                else
                {
                    schedulePlayback();
                }
            });
    connect(open,
            &QAction::triggered,
            this,
            [this]
            {
                const auto path =
                    QFileDialog::getOpenFileName(this, tr("Open project"), {}, tr("MoTrack and Tracker projects (*.motrack *.trk *.trz)"));
                if (!path.isEmpty())
                {
                    openProject(path);
                }
            });
    connect(openMovieAction,
            &QAction::triggered,
            this,
            [this]
            {
                const auto path = QFileDialog::getOpenFileName(this, tr("Open video"));
                if (!path.isEmpty())
                {
                    openMovie(path);
                }
            });
    connect(openImagesAction,
            &QAction::triggered,
            this,
            [this]
            {
                QStringList patterns;
                const auto formats = QImageReader::supportedImageFormats();
                for (const auto& format : formats)
                {
                    patterns.append("*." + QString::fromLatin1(format));
                }
                auto paths = QFileDialog::getOpenFileNames(this, tr("Open image sequence"), {}, tr("Images (%1)").arg(patterns.join(' ')));
                if (paths.isEmpty())
                {
                    return;
                }
                bool accepted = false;
                const auto rate = QInputDialog::getDouble(
                    this, tr("Image sequence timing"), tr("Frames per second (0 if unknown)"), 0.0, 0.0, 1000.0, 3, &accepted);
                if (accepted)
                {
                    QCollator collator;
                    collator.setNumericMode(true);
                    std::ranges::sort(paths,
                                      [&collator](const auto& left, const auto& right)
                                      {
                                          const auto order = collator.compare(left, right);
                                          return order < 0 || (order == 0 && left < right);
                                      });
                    openImageSequence(paths, rate);
                }
            });
    connect(m_SaveAs, &QAction::triggered, this, &MainWindow::chooseWorkspacePath);
    connect(m_Save, &QAction::triggered, this, &MainWindow::saveCurrentWorkspace);
    connect(m_Export,
            &QAction::triggered,
            this,
            [this]
            {
                const auto path = QFileDialog::getSaveFileName(this, tr("Export measurements"), {}, tr("CSV files (*.csv)"));
                if (!path.isEmpty())
                {
                    saveMeasurements(path.endsWith(".csv", Qt::CaseInsensitive) ? path : path + ".csv");
                }
            });
    connect(m_ExportPlot,
            &QAction::triggered,
            this,
            [this]
            {
                const auto csv = m_Plot->csv();
                const auto path = QFileDialog::getSaveFileName(this, tr("Export plot data"), {}, tr("CSV files (*.csv)"));
                if (!path.isEmpty())
                {
                    saveCsv(path.endsWith(".csv", Qt::CaseInsensitive) ? path : path + ".csv", csv);
                }
            });
    connect(m_NewPointMass,
            &QAction::triggered,
            this,
            [this]
            {
                stopAutoTracking();
                const auto index = m_Session.document.project.pointMasses.size();
                auto number = index + 1;
                auto name = tr("Point Mass %1").arg(number).toStdString();
                while (std::ranges::any_of(m_Session.document.project.pointMasses, [&name](const auto& mass) { return mass.name == name; }))
                {
                    name = tr("Point Mass %1").arg(++number).toStdString();
                }
                if (!m_Session.history.createPointMass(m_Session.document.project, std::move(name)))
                {
                    return;
                }
                refreshEditViews(static_cast<int>(index));
                statusBar()->showMessage(tr("Shift-click the image to mark the selected point mass."));
            });
    connect(m_NewVector,
            &QAction::triggered,
            this,
            [this]
            {
                stopAutoTracking();
                setPlaying(false);
                cancelMeasurement();
                m_Calibrate->setChecked(false);
                m_EditAxes->setChecked(false);
                auto number = m_Session.document.project.vectors.size() + 1;
                auto name = tr("Vector %1").arg(number).toStdString();
                while (std::ranges::any_of(m_Session.document.project.vectors, [&name](const auto& track) { return track.name == name; }))
                {
                    name = tr("Vector %1").arg(++number).toStdString();
                }
                if (!m_Session.history.createVector(m_Session.document.project, name))
                {
                    return;
                }
                refreshEditViews(
                    static_cast<int>(m_Session.document.project.pointMasses.size() + m_Session.document.project.vectors.size() - 1));
                statusBar()->showMessage(tr("Shift-click the vector tail, then its tip. Escape cancels."));
            });
    connect(m_RenamePointMass, &QAction::triggered, this, &MainWindow::renameSelectedPointMass);
    connect(m_SetPointMass, &QAction::triggered, this, &MainWindow::setSelectedPointMassValue);
    connect(m_RemovePointMass, &QAction::triggered, this, &MainWindow::removeSelectedPointMass);
    connect(m_RemoveMark, &QAction::triggered, this, &MainWindow::removeCurrentMark);
    connect(m_Undo, &QAction::triggered, this, [this] { undoOrRedo(false); });
    connect(m_Redo, &QAction::triggered, this, [this] { undoOrRedo(true); });
    connect(m_ClipSettings, &QAction::triggered, this, &MainWindow::editClipSettings);
    connect(repeat, &QAction::triggered, this, &MainWindow::repeatPreviousMark);
    connect(m_AutoTracker,
            &QAction::toggled,
            this,
            [this](bool active)
            {
                if (active)
                {
                    startAutoTracking();
                }
                else
                {
                    stopAutoTracking();
                }
            });
    connect(m_Calibrate,
            &QAction::toggled,
            this,
            [this](bool active)
            {
                cancelVectorMark();
                clearCalibrationPoint();
                if (active)
                {
                    stopAutoTracking();
                    setPlaying(false);
                    cancelMeasurement();
                    m_EditAxes->setChecked(false);
                    statusBar()->showMessage(tr("Click the first calibration endpoint."));
                }
                else
                {
                    statusBar()->clearMessage();
                }
            });
    connect(m_ShowAxes, &QAction::toggled, this, &MainWindow::refreshAxesOverlay);
    connect(m_EditAxes,
            &QAction::toggled,
            this,
            [this](bool active)
            {
                m_AxesOrigin.reset();
                m_Video->setCalibrationPoint(std::nullopt);
                if (active)
                {
                    stopAutoTracking();
                    setPlaying(false);
                    cancelMeasurement();
                    m_Calibrate->setChecked(false);
                    m_ShowAxes->setChecked(true);
                    statusBar()->showMessage(tr("Click the coordinate origin."));
                }
                else
                {
                    statusBar()->clearMessage();
                }
            });
    connect(m_MeasureDistance,
            &QAction::toggled,
            this,
            [this](bool active)
            {
                if (active)
                {
                    beginMeasurement(false);
                }
                else
                {
                    cancelMeasurement();
                    statusBar()->clearMessage();
                }
            });
    connect(m_MeasureAngle,
            &QAction::toggled,
            this,
            [this](bool active)
            {
                if (active)
                {
                    beginMeasurement(true);
                }
                else
                {
                    cancelMeasurement();
                    statusBar()->clearMessage();
                }
            });
    connect(m_TrackPicker,
            &QComboBox::currentIndexChanged,
            this,
            [this]
            {
                cancelVectorMark();
                stopAutoTracking();
                updatePointMassActions();
                refreshPlot();
            });
    connect(m_PlotQuantity, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPlot);
    connect(m_FitLine,
            &QCheckBox::toggled,
            this,
            [this](bool visible)
            {
                m_Plot->setFitVisible(visible);
                m_FitSummary->setVisible(visible);
                if (visible)
                {
                    m_FitSummary->setText(m_Plot->fitSummary());
                }
            });
}

MainWindow::~MainWindow()
{
    cancelProjectOpen();
    cancelMovieOpen();
    ++m_RequestGeneration;
    m_MovieThread.quit();
    m_MovieThread.wait();
}

bool MainWindow::loadProject(const QString& path)
{
    cancelProjectOpen();
    try
    {
        return applyProject(path, prepareProject(path, nullptr), nullptr);
    }
    catch (const ProjectError&)
    {
        statusBar()->showMessage(tr("Could not open this project."));
        return false;
    }
}

MainWindow::PreparedProject MainWindow::prepareProject(const QString& path, const std::atomic_bool* cancelled)
{
    PreparedProject prepared;
    prepared.mediaProjectPath = physicalSourcePath(path);
    if (path.endsWith(".motrack", Qt::CaseInsensitive))
    {
        prepared.document = readWorkspaceDocument(path, cancelled);
        prepared.archive = prepared.document.archivePayload;
        if (prepared.document.archiveMember)
        {
            prepared.member = QString::fromStdString(*prepared.document.archiveMember);
        }
        if (!prepared.archive && !prepared.document.archiveReference.empty())
        {
            const auto source = resolveLocalMediaPath(path, prepared.document.archiveReference);
            prepared.archive = std::make_shared<TrackerArchive>(TrackerArchive::open(source, cancelled));
        }
    }
    else if (path.endsWith(".trz", Qt::CaseInsensitive))
    {
        prepared.archive = std::make_shared<TrackerArchive>(TrackerArchive::open(path, cancelled));
        const auto& archive = *prepared.archive;
        const auto& projects = archive.projects();
        prepared.chooseArchiveTab = projects.size() > 1;
        if (!prepared.chooseArchiveTab)
        {
            prepared.member = projects.front();
            prepared.document = importTrackerDocument(archive.projectPath(prepared.member));
        }
    }
    else
    {
        prepared.document = importTrackerDocument(path);
    }
    if (!prepared.archive)
        retainWorkspaceMediaLocations(prepared.document, prepared.mediaProjectPath);
    return prepared;
}

void MainWindow::cancelProjectOpen()
{
    if (m_ProjectOpenCancelled)
    {
        *m_ProjectOpenCancelled = true;
        m_ProjectOpenCancelled.reset();
        m_ProjectOpenWatcher.reset();
    }
}

void MainWindow::openProject(const QString& path)
{
    cancelProjectOpen();
    cancelMovieOpen();
    statusBar()->showMessage(tr("Reading project…"));
    m_ProjectOpenCancelled = std::make_shared<std::atomic_bool>(false);
    using Result = std::shared_ptr<PreparedProject>;
    m_ProjectOpenWatcher = std::make_unique<QFutureWatcher<Result>>();
    auto* watcher = m_ProjectOpenWatcher.get();
    const auto cancelled = m_ProjectOpenCancelled;
    connect(watcher,
            &QFutureWatcher<Result>::finished,
            this,
            [this, path, watcher, cancelled]
            {
                completeAfterSave(
                    [this, path, cancelled, result = watcher->result()]
                    {
                        if (*cancelled)
                        {
                            return;
                        }
                        try
                        {
                            if (!result)
                            {
                                throw ProjectError("Could not prepare project.");
                            }
                            applyProject(path, std::move(*result), cancelled.get());
                        }
                        catch (const ProjectError&)
                        {
                            if (!*cancelled)
                            {
                                statusBar()->showMessage(tr("Could not open this project."));
                            }
                        }
                    });
            });
    watcher->setFuture(QtConcurrent::run(
        [path, cancelled]() -> Result
        {
            try
            {
                return std::make_shared<PreparedProject>(prepareProject(path, cancelled.get()));
            }
            catch (const ProjectError&)
            {
                return {};
            }
        }));
}

void MainWindow::completeAfterSave(std::function<void()> completion)
{
    if (m_Saving)
    {
        m_AfterSave.push_back(std::move(completion));
    }
    else
    {
        completion();
    }
}

bool MainWindow::applyProject(const QString& path, PreparedProject prepared, const std::atomic_bool* cancelled)
{
    auto& document = prepared.document;
    if (prepared.chooseArchiveTab && prepared.archive)
    {
        const auto& archive = *prepared.archive;
        prepared.member = QInputDialog::getItem(this, tr("Open Tracker archive"), tr("Choose a Tracker tab"), archive.projects(), 0, false);
        if (prepared.member.isEmpty())
        {
            if (!cancelled || !*cancelled)
            {
                statusBar()->clearMessage();
            }
            return false;
        }
        document = importTrackerDocument(archive.projectPath(prepared.member));
    }
    if (cancelled)
    {
        if (*cancelled)
        {
            return false;
        }
        if (!confirmDiscard())
        {
            statusBar()->clearMessage();
            return false;
        }
        if (*cancelled)
        {
            return false;
        }
    }

    cancelMovieOpen();
    releaseArchive();
    m_MediaProjectPath = prepared.mediaProjectPath;
    document.archivePayload = std::move(prepared.archive);
    document.archiveMember = prepared.member.isEmpty() ? std::nullopt : std::optional(prepared.member.toStdString());
    if (document.archivePayload)
    {
        document.archiveReference.clear();
        // Native media paths were already resolved relative to the archive root when imported.
        document.archivePayload->resolveMedia(document.project,
                                              path.endsWith(".motrack", Qt::CaseInsensitive) ? QString{} : prepared.member);
        m_MediaProjectPath = QDir(document.archivePayload->root()).absoluteFilePath("archive.trk");
    }
    document.alignMovieToClip = document.alignMovieToClip || !path.endsWith(".motrack", Qt::CaseInsensitive);
    m_Session.replace(std::move(document), path);
    m_Sequence = m_Session.document.movie
                     ? std::nullopt
                     : ImageSequence::open(m_MediaProjectPath, m_Session.document.project.clip, m_Session.document.mediaLocations);
    m_MoviePath.clear();
    m_MoviePresentationTimes.clear();
    const auto& clip = m_Session.document.project.clip;
    const auto steps = clip.stepCount.value_or(0);
    if (!m_Sequence && clip.mediaReferences.size() == 1 && steps > 0)
    {
        const auto movie = workspaceMediaPath(m_Session.document, m_MediaProjectPath, clip.mediaReferences.front());
        if (QFileInfo(movie).isFile())
        {
            m_MoviePath = movie;
            const auto last = static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(steps - 1) * clip.stepSize;
            if (!clip.sourceFrameCount && last < std::numeric_limits<int>::max())
            {
                m_Session.document.project.clip.sourceFrameCount = static_cast<int>(last + 1);
            }
        }
    }
    m_Session.document.movie = !m_MoviePath.isEmpty();
    activateProject(path);
    statusBar()->showMessage(tr("Opened %1 marks. Unsupported tracks: %2.")
                                 .arg(m_Marks->rowCount())
                                 .arg(m_Session.document.project.unsupportedTrackTypes.size()));
    if (!m_MoviePath.isEmpty())
    {
        scheduleMovieIndex(m_MoviePath, false);
    }
    return true;
}

void MainWindow::releaseArchive()
{
    if (m_Session.document.archivePayload)
    {
        ++m_RequestGeneration;
        if (m_MovieWorker)
        {
            // Release the decoder's file handle before removing extracted media on Windows.
            QMetaObject::invokeMethod(m_MovieWorker, "clear", Qt::BlockingQueuedConnection);
        }
        m_Session.document.archivePayload.reset();
        m_Session.document.archiveMember.reset();
    }
}

void MainWindow::cancelMovieOpen()
{
    if (m_MovieOpenCancelled)
    {
        *m_MovieOpenCancelled = true;
        m_MovieOpenCancelled.reset();
        m_MovieOpenWatcher.reset();
    }
}

bool MainWindow::openImageSequence(const QStringList& paths, double frameRate)
{
    const auto duration = frameRate > 0.0 ? 1000.0 / frameRate : 0.0;
    if (paths.isEmpty() || !std::in_range<int>(paths.size()) || !std::isfinite(frameRate) || frameRate < 0.0 || !std::isfinite(duration))
    {
        statusBar()->showMessage(tr("Could not open this image sequence."));
        return false;
    }
    LegacyProject project;
    const auto count = static_cast<int>(paths.size());
    project.clip.stepCount = count;
    project.clip.sourceFrameCount = count;
    if (duration > 0.0)
    {
        project.clip.frameDurationMs = duration;
    }
    const auto path = physicalSourcePath(paths.front());
    const QDir mediaDirectory(localProjectDirectory(path));
    for (const auto& imagePath : paths)
    {
        const auto physical = QFileInfo(imagePath).canonicalFilePath();
        project.clip.mediaReferences.push_back(
            mediaDirectory.relativeFilePath(physical.isEmpty() ? QFileInfo(imagePath).absoluteFilePath() : physical).toStdString());
    }
    auto sequence = ImageSequence::open(path, project.clip);
    const auto first = sequence ? sequence->readStep(0) : std::nullopt;
    if (!first || first->image.isNull())
    {
        statusBar()->showMessage(tr("Could not open this image sequence."));
        return false;
    }
    cancelProjectOpen();
    cancelMovieOpen();
    if (!confirmDiscard())
    {
        return false;
    }
    releaseArchive();
    m_MediaProjectPath = path;
    m_Session.replace({.project = std::move(project), .movieFrameTimes = {}}, path, DocumentSourceKind::Media);
    retainWorkspaceMediaLocations(m_Session.document, path);
    m_Sequence = std::move(sequence);
    m_MoviePath.clear();
    m_MoviePresentationTimes.clear();
    activateProject(path);
    statusBar()->showMessage(tr("Opened %1 images.").arg(count));
    return true;
}

void MainWindow::openMovie(const QString& path)
{
    cancelProjectOpen();
    statusBar()->showMessage(tr("Reading video frames…"));
    scheduleMovieIndex(physicalSourcePath(path), true);
}

void MainWindow::scheduleMovieIndex(const QString& path, bool standalone)
{
    cancelMovieOpen();
    m_MovieOpenCancelled = std::make_shared<std::atomic_bool>(false);
    m_MovieOpenWatcher = std::make_unique<QFutureWatcher<MovieIndex>>();
    auto* watcher = m_MovieOpenWatcher.get();
    connect(watcher,
            &QFutureWatcher<MovieIndex>::finished,
            this,
            [this, path, standalone, watcher, cancelled = m_MovieOpenCancelled]
            {
                completeAfterSave(
                    [this, path, standalone, cancelled, index = watcher->result()]() mutable
                    {
                        if (*cancelled)
                        {
                            return;
                        }
                        if (!standalone)
                        {
                            m_MoviePresentationTimes = std::move(index.presentationTimes);
                            if (!index.frameTimes.empty())
                            {
                                m_Session.document.movieFrameTimes = std::move(index.frameTimes);
                            }
                            refreshPlot();
                            return;
                        }
                        if (index.frameCount == 0)
                        {
                            statusBar()->showMessage(tr("Could not open this video."));
                            return;
                        }
                        if (!confirmDiscard() || *cancelled)
                        {
                            statusBar()->clearMessage();
                            return;
                        }
                        releaseArchive();
                        m_MediaProjectPath = path;
                        m_Session.replace({}, path, DocumentSourceKind::Media);
                        m_Session.document.project.clip.stepCount = index.frameCount;
                        m_Session.document.project.clip.sourceFrameCount = index.frameCount;
                        m_Session.document.project.clip.mediaReferences.push_back(
                            QDir(localProjectDirectory(path)).relativeFilePath(QFileInfo(path).canonicalFilePath()).toStdString());
                        retainWorkspaceMediaLocations(m_Session.document, path);
                        m_Sequence.reset();
                        m_MoviePath = path;
                        m_Session.document.movie = true;
                        m_Session.document.useDisplayOrientation = true;
                        m_Session.document.alignMovieToClip = false;
                        m_MoviePresentationTimes = std::move(index.presentationTimes);
                        m_Session.document.movieFrameTimes = std::move(index.frameTimes);
                        activateProject(path);
                        statusBar()->showMessage(tr("Opened video with %1 frames.").arg(index.frameCount));
                    });
            });
    const auto orientation = standalone || m_Session.document.useDisplayOrientation ? VideoOrientation::Display : VideoOrientation::Encoded;
    watcher->setFuture(QtConcurrent::run(
        [path, orientation, cancelled = m_MovieOpenCancelled, archive = standalone ? nullptr : m_Session.document.archivePayload]
        {
            // Keep extracted media available until the indexer closes its input.
            static_cast<void>(archive);
            return buildMovieIndex(path, orientation, *cancelled);
        }));
}

void MainWindow::activateProject(const QString& path)
{
    stopAutoTracking();
    cancelMeasurement();
    setPlaying(false);
    m_Video->resetZoom();
    ++m_RequestGeneration;
    if (m_MovieWorker)
    {
        QMetaObject::invokeMethod(m_MovieWorker, "clear", Qt::QueuedConnection);
    }
    m_DisplayedFrame.reset();
    m_Calibrate->setChecked(false);
    m_Calibrate->setEnabled(false);
    m_EditAxes->setChecked(false);
    m_EditAxes->setEnabled(false);
    m_MeasureDistance->setEnabled(false);
    m_MeasureAngle->setEnabled(false);
    refreshAxesOverlay();
    m_NewPointMass->setEnabled(true);
    m_NewVector->setEnabled(true);
    m_SaveAs->setEnabled(true);
    m_Save->setEnabled(true);
    refreshEditViews(0);
    syncClipControls();
    setWindowTitle(tr("%1[*] — MoTrack").arg(QFileInfo(path).fileName()));
}

void MainWindow::refreshImageMediaLocations()
{
    if (!m_Session.document.movie)
    {
        // Keep a known image reader if a temporarily missing, unusually named image cannot
        // be identified. A later successful save also retries when no reader was available.
        auto sequence = ImageSequence::open(m_MediaProjectPath, m_Session.document.project.clip, m_Session.document.mediaLocations);
        if (sequence)
            m_Sequence = std::move(sequence);
        if (m_Sequence)
        {
            const auto step = m_StepSlider->value();
            const auto count = m_Sequence->stepCount();
            {
                const QSignalBlocker blocker(m_StepSlider);
                m_StepSlider->setRange(0, std::max(0, count - 1));
                m_StepSlider->setValue(step);
            }
            m_ClipSettings->setEnabled(count > 0);
            displayStep(m_StepSlider->value());
        }
    }
}

void MainWindow::syncClipControls()
{
    setPlaying(false);
    if (m_Sequence)
    {
        m_Sequence = ImageSequence::open(m_MediaProjectPath, m_Session.document.project.clip, m_Session.document.mediaLocations);
    }
    const int stepCount =
        m_Sequence ? m_Sequence->stepCount() : (m_MoviePath.isEmpty() ? 0 : m_Session.document.project.clip.stepCount.value_or(0));
    m_ClipSettings->setEnabled(stepCount > 0);
    {
        const QSignalBlocker blocker(m_StepSlider);
        m_StepSlider->setRange(0, std::max(0, stepCount - 1));
        m_StepSlider->setValue(0);
    }
    if (stepCount == 0)
    {
        m_Video->display({}, {}, tr("No supported local video or image sequence is attached to this project."));
    }
    displayStep(0);
}

void MainWindow::undoOrRedo(bool redo)
{
    cancelVectorMark();
    stopAutoTracking();
    const auto before = clipRange(m_Session.document.project.clip);
    const bool changed = redo ? m_Session.history.redo(m_Session.document.project) : m_Session.history.undo(m_Session.document.project);
    if (changed)
    {
        if (before != clipRange(m_Session.document.project.clip))
        {
            syncClipControls();
        }
        refreshEditViews(m_TrackPicker->currentIndex());
    }
}

void MainWindow::editClipSettings()
{
    stopAutoTracking();
    setPlaying(false);
    const auto request = m_RequestGeneration.load();
    const auto& clip = m_Session.document.project.clip;
    const auto last = static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(m_StepSlider->maximum()) * clip.stepSize;
    const auto safeLast = static_cast<int>(std::min<std::int64_t>(last, std::numeric_limits<int>::max()));
    const auto sourceLast = m_Sequence ? static_cast<int>(clip.mediaReferences.size()) - 1 : clip.sourceFrameCount.value_or(0) - 1;
    const auto maxFrame = std::max(safeLast, sourceLast);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Clip Settings"));
    auto* layout = new QFormLayout(&dialog);
    auto* first = new QSpinBox;
    auto* end = new QSpinBox;
    auto* stride = new QSpinBox;
    first->setObjectName("clipStart");
    end->setObjectName("clipEnd");
    stride->setObjectName("clipStride");
    first->setRange(0, safeLast);
    first->setValue(clip.firstFrame);
    end->setRange(clip.firstFrame, maxFrame);
    end->setValue(safeLast);
    stride->setRange(1, std::max({1, maxFrame, clip.stepSize}));
    stride->setValue(clip.stepSize);
    layout->addRow(tr("&Start frame"), first);
    layout->addRow(tr("&End frame"), end);
    layout->addRow(tr("Step si&ze"), stride);
    connect(first, &QSpinBox::valueChanged, end, &QSpinBox::setMinimum);
    connect(end, &QSpinBox::valueChanged, first, &QSpinBox::setMaximum);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted && request == m_RequestGeneration &&
        m_Session.history.setClipRange(m_Session.document.project, first->value(), end->value(), stride->value()))
    {
        syncClipControls();
        refreshEditViews(m_TrackPicker->currentIndex());
    }
}

bool MainWindow::confirmDiscard()
{
    if (m_Saving)
    {
        return false;
    }
    if (!m_Session.history.isModified())
    {
        return true;
    }
    const auto choice = QMessageBox::question(this,
                                              tr("Save tracking edits?"),
                                              tr("Save your project to keep your tracking edits."),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);
    return choice == QMessageBox::Discard || (choice == QMessageBox::Save && saveCurrentWorkspace());
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscard())
    {
        cancelProjectOpen();
        cancelMovieOpen();
        event->accept();
    }
    else
    {
        event->ignore();
    }
}

void MainWindow::refreshEditViews(int track)
{
    m_NewPointMass->setEnabled(!m_Session.sourcePath().isEmpty() && hasTrackIdCapacity(m_Session.document.project));
    m_NewVector->setEnabled(!m_Session.sourcePath().isEmpty() && hasTrackIdCapacity(m_Session.document.project));
    cancelVectorMark();
    const QSignalBlocker blocker(m_TrackPicker);
    m_TrackPicker->clear();
    for (const auto& mass : m_Session.document.project.pointMasses)
    {
        m_TrackPicker->addItem(QString::fromStdString(mass.name));
    }
    for (const auto& vector : m_Session.document.project.vectors)
    {
        m_TrackPicker->addItem(tr("%1 (vector)").arg(QString::fromStdString(vector.name)));
    }
    m_TrackPicker->setCurrentIndex(std::min(std::max(0, track), m_TrackPicker->count() - 1));
    m_Undo->setEnabled(m_Session.history.canUndo());
    m_Redo->setEnabled(m_Session.history.canRedo());
    setWindowModified(m_Session.history.isModified());
    displayMarks();
    refreshPlot();
    m_Export->setEnabled(m_Marks->rowCount() > 0);
    if (m_DisplayedFrame && !m_DisplayedFrame->image.isNull())
    {
        displayFrame(*m_DisplayedFrame, {});
    }
    updateAutoTrackerAction();
    updatePointMassActions();
}

void MainWindow::refreshPlot()
{
    const bool vector = selectedVector().has_value();
    m_PlotQuantity->setEnabled(!vector);
    m_FitLine->setEnabled(!vector);
    const auto quantity = static_cast<PlotQuantity>(m_PlotQuantity->currentData().toInt());
    const auto track = m_TrackPicker->currentIndex();
    const auto first = static_cast<std::size_t>(m_Session.document.project.clip.firstFrame);
    const auto offset = m_Session.document.alignMovieToClip && first < m_Session.document.movieFrameTimes.size()
                            ? m_Session.document.project.clip.startTimeMs / 1000.0 - m_Session.document.movieFrameTimes.at(first)
                            : 0.0;
    auto series =
        track >= 0 ? measurementPlot(
                         m_Session.document.project, static_cast<std::size_t>(track), quantity, m_Session.document.movieFrameTimes, offset)
                   : PlotSeries{};
    const auto timeUnit = QString::fromStdString(series.timeUnit);
    const auto horizontal = series.timed ? tr("Time (%1)").arg(timeUnit) : tr("Frame");
    auto unit = series.calibrated ? QString::fromStdString(m_Session.document.project.lengthUnit) : tr("px");
    if (!unit.isEmpty())
    {
        switch (quantity)
        {
        case PlotQuantity::VelocityX:
        case PlotQuantity::VelocityY:
        case PlotQuantity::Speed:
            unit = tr("%1/%2").arg(unit, timeUnit);
            break;
        case PlotQuantity::AccelerationX:
        case PlotQuantity::AccelerationY:
            unit = tr("%1/%2²").arg(unit, timeUnit);
            break;
        case PlotQuantity::MomentumX:
        case PlotQuantity::MomentumY:
            unit = m_Session.document.project.massUnit.empty()
                       ? QString{}
                       : tr("%1·%2/%3").arg(QString::fromStdString(m_Session.document.project.massUnit), unit, timeUnit);
            break;
        case PlotQuantity::KineticEnergy:
            unit = m_Session.document.project.massUnit.empty()
                       ? QString{}
                       : tr("%1·%2²/%3²").arg(QString::fromStdString(m_Session.document.project.massUnit), unit, timeUnit);
            break;
        default:
            break;
        }
    }
    const auto label = m_PlotQuantity->currentText();
    const auto vertical = unit.isEmpty() ? label : tr("%1 (%2)").arg(label, unit);
    m_ExportPlot->setEnabled(!series.samples.empty());
    m_Plot->setSeries(std::move(series), horizontal, vertical);
    if (m_FitLine->isChecked())
    {
        m_FitSummary->setText(m_Plot->fitSummary());
    }
}

void MainWindow::markAt(const QPointF& position)
{
    const auto track = m_TrackPicker->currentIndex();
    if (track < 0 || !m_DisplayedFrame || m_DisplayedFrame->image.isNull())
    {
        return;
    }
    if (selectedVector())
    {
        markVectorAt(position);
        return;
    }
    const Mark mark{.frame = m_DisplayedFrame->number, .x = position.x(), .y = position.y()};
    stopAutoTracking();
    setPlaying(false);
    if (m_Session.history.markPointMass(m_Session.document.project, static_cast<std::size_t>(track), mark))
    {
        refreshEditViews(track);
        m_StepSlider->triggerAction(QSlider::SliderSingleStepAdd);
    }
}

std::optional<std::size_t> MainWindow::selectedVector() const
{
    const auto index = m_TrackPicker->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) < m_Session.document.project.pointMasses.size())
    {
        return std::nullopt;
    }
    const auto vector = static_cast<std::size_t>(index) - m_Session.document.project.pointMasses.size();
    return vector < m_Session.document.project.vectors.size() ? std::optional(vector) : std::nullopt;
}

void MainWindow::cancelVectorMark()
{
    if (m_VectorTail)
    {
        m_VectorTail.reset();
        m_Video->setCalibrationPoint(std::nullopt);
        statusBar()->clearMessage();
    }
}

void MainWindow::markVectorAt(const QPointF& position)
{
    const auto vector = selectedVector();
    if (!vector || !m_DisplayedFrame)
    {
        return;
    }
    const auto frame = m_DisplayedFrame->number;
    stopAutoTracking();
    setPlaying(false);
    if (!m_VectorTail)
    {
        m_VectorTail = position;
        m_Video->setCalibrationPoint(position);
        statusBar()->showMessage(tr("Shift-click the vector tip. Escape cancels."));
        return;
    }
    const VectorSample sample{.frame = frame,
                              .tailX = m_VectorTail->x(),
                              .tailY = m_VectorTail->y(),
                              .componentX = position.x() - m_VectorTail->x(),
                              .componentY = position.y() - m_VectorTail->y()};
    if (m_Session.history.markVector(m_Session.document.project, *vector, sample))
    {
        refreshEditViews(m_TrackPicker->currentIndex());
        m_StepSlider->triggerAction(QSlider::SliderSingleStepAdd);
    }
}

void MainWindow::renameSelectedPointMass()
{
    const auto selected = m_TrackPicker->currentIndex();
    if (selected < 0)
    {
        return;
    }
    stopAutoTracking();
    setPlaying(false);
    const auto index = static_cast<std::size_t>(selected);
    const auto vector = selectedVector();
    const auto before = QString::fromStdString(vector ? m_Session.document.project.vectors.at(*vector).name
                                                      : m_Session.document.project.pointMasses.at(index).name);
    const auto request = m_RequestGeneration.load();
    bool accepted = false;
    const auto after = QInputDialog::getText(this, tr("Rename track"), tr("Track name"), QLineEdit::Normal, before, &accepted).trimmed();
    if (!accepted || request != m_RequestGeneration || before == after)
    {
        return;
    }
    if (vector ? m_Session.history.renameVector(m_Session.document.project, *vector, after.toStdString())
               : m_Session.history.renamePointMass(m_Session.document.project, index, after.toStdString()))
    {
        refreshEditViews(selected);
    }
    else
    {
        statusBar()->showMessage(tr("Choose a unique, nonempty track name."));
    }
}

void MainWindow::removeSelectedPointMass()
{
    const auto selected = m_TrackPicker->currentIndex();
    const auto vector = selectedVector();
    if (selected >= 0 && (vector ? m_Session.history.removeVector(m_Session.document.project, *vector)
                                 : m_Session.history.removePointMass(m_Session.document.project, static_cast<std::size_t>(selected))))
    {
        stopAutoTracking();
        setPlaying(false);
        refreshEditViews(selected);
    }
}

void MainWindow::setSelectedPointMassValue()
{
    const auto selected = m_TrackPicker->currentIndex();
    if (selected < 0 || selectedVector())
    {
        return;
    }
    stopAutoTracking();
    setPlaying(false);
    const auto index = static_cast<std::size_t>(selected);
    const auto request = m_RequestGeneration.load();
    const QLocale locale;
    const auto before = locale.toString(m_Session.document.project.pointMasses.at(index).mass, 'g', 15);
    const auto massUnit = QString::fromStdString(m_Session.document.project.massUnit);
    const auto label = massUnit.isEmpty() ? tr("Mass") : tr("Mass (%1)").arg(massUnit);
    bool accepted = false;
    const auto text = QInputDialog::getText(
        this, tr("Set point-mass value"), "<qt>" + label.toHtmlEscaped() + "</qt>", QLineEdit::Normal, before, &accepted);
    if (!accepted || request != m_RequestGeneration || before == text)
    {
        return;
    }
    bool valid = false;
    const auto mass = locale.toDouble(text, &valid);
    if (valid && m_Session.history.setPointMassValue(m_Session.document.project, index, mass))
    {
        refreshEditViews(selected);
    }
    else
    {
        statusBar()->showMessage(tr("Enter a finite, positive mass."));
    }
}

void MainWindow::removeCurrentMark()
{
    const auto selected = m_TrackPicker->currentIndex();
    const auto vector = selectedVector();
    if (selected >= 0 && m_DisplayedFrame &&
        (vector ? m_Session.history.removeVectorSample(m_Session.document.project, *vector, m_DisplayedFrame->number)
                : m_Session.history.removeMark(m_Session.document.project, static_cast<std::size_t>(selected), m_DisplayedFrame->number)))
    {
        stopAutoTracking();
        setPlaying(false);
        refreshEditViews(selected);
    }
}

void MainWindow::updatePointMassActions()
{
    const auto selected = m_TrackPicker->currentIndex();
    const bool hasTrack = selected >= 0;
    m_RenamePointMass->setEnabled(hasTrack);
    const auto vector = selectedVector();
    m_SetPointMass->setEnabled(hasTrack && !vector);
    m_RemovePointMass->setEnabled(hasTrack);
    const bool hasMark =
        hasTrack && m_DisplayedFrame &&
        (vector ? std::ranges::binary_search(
                      m_Session.document.project.vectors.at(*vector).samples, m_DisplayedFrame->number, {}, &VectorSample::frame)
                : std::ranges::binary_search(m_Session.document.project.pointMasses.at(static_cast<std::size_t>(selected)).marks,
                                             m_DisplayedFrame->number,
                                             {},
                                             &Mark::frame));
    m_RemoveMark->setEnabled(hasMark);
}

void MainWindow::repeatPreviousMark()
{
    const auto track = m_TrackPicker->currentIndex();
    if (track < 0 || !m_DisplayedFrame)
    {
        return;
    }
    if (const auto vector = selectedVector())
    {
        const auto& samples = m_Session.document.project.vectors.at(*vector).samples;
        const auto previous = std::ranges::lower_bound(samples, m_DisplayedFrame->number, {}, &VectorSample::frame);
        if (previous != samples.begin() && !m_DisplayedFrame->image.isNull())
        {
            auto sample = *std::prev(previous);
            sample.frame = m_DisplayedFrame->number;
            stopAutoTracking();
            setPlaying(false);
            if (m_Session.history.markVector(m_Session.document.project, *vector, sample))
            {
                refreshEditViews(track);
                m_StepSlider->triggerAction(QSlider::SliderSingleStepAdd);
            }
        }
        return;
    }
    const auto& marks = m_Session.document.project.pointMasses.at(static_cast<std::size_t>(track)).marks;
    const auto previous = std::ranges::lower_bound(marks, m_DisplayedFrame->number, {}, &Mark::frame);
    if (previous != marks.begin())
    {
        const auto& mark = *std::prev(previous);
        markAt({mark.x, mark.y});
    }
}

void MainWindow::clearCalibrationPoint()
{
    m_CalibrationFirst.reset();
    m_Video->setCalibrationPoint(std::nullopt);
}

void MainWindow::refreshAxesOverlay()
{
    if (!m_ShowAxes->isChecked() || !m_DisplayedFrame || m_DisplayedFrame->image.isNull())
    {
        m_Video->setAxes(std::nullopt);
        return;
    }
    const auto& frame = *m_DisplayedFrame;
    if (m_Session.document.project.calibration)
    {
        m_Video->setAxes(calibrationAt(*m_Session.document.project.calibration, frame.number));
        return;
    }
    m_Video->setAxes(CalibrationKey{.frame = frame.number,
                                    .originX = frame.image.width() / 2.0,
                                    .originY = frame.image.height() / 2.0,
                                    .angleDegrees = 0.0,
                                    .scaleX = 1.0,
                                    .scaleY = 1.0});
}

void MainWindow::pickAxesPoint(const QPointF& position)
{
    if (!m_DisplayedFrame)
    {
        return;
    }
    if (!m_AxesOrigin)
    {
        m_AxesOrigin = position;
        m_Video->setCalibrationPoint(position);
        statusBar()->showMessage(tr("Click a point along the positive x axis."));
        return;
    }
    const auto origin = *m_AxesOrigin;
    if (!m_Session.history.setCoordinateAxes(m_Session.document.project,
                                             {.frame = m_DisplayedFrame->number,
                                              .originX = origin.x(),
                                              .originY = origin.y(),
                                              .positiveX = position.x(),
                                              .positiveY = position.y()}))
    {
        statusBar()->showMessage(tr("Choose a different point for the positive x axis."));
        return;
    }
    m_EditAxes->setChecked(false);
    refreshEditViews(m_TrackPicker->currentIndex());
    statusBar()->showMessage(tr("Coordinate axes updated."));
}

void MainWindow::beginMeasurement(bool angle)
{
    cancelMeasurement();
    auto* action = angle ? m_MeasureAngle : m_MeasureDistance;
    const QSignalBlocker blocker(action);
    action->setChecked(true);
    stopAutoTracking();
    setPlaying(false);
    m_Calibrate->setChecked(false);
    m_EditAxes->setChecked(false);
    statusBar()->showMessage(angle ? tr("Click the vertex of the angle.") : tr("Click the first distance point."));
}

void MainWindow::cancelMeasurement()
{
    cancelVectorMark();
    if (m_MeasureDistance->isChecked() || m_MeasureAngle->isChecked() || !m_MeasurementPoints.isEmpty())
    {
        const QSignalBlocker distance(m_MeasureDistance);
        const QSignalBlocker angle(m_MeasureAngle);
        m_MeasureDistance->setChecked(false);
        m_MeasureAngle->setChecked(false);
        m_MeasurementPoints.clear();
        m_Video->setMeasurementPolyline({});
        statusBar()->clearMessage();
    }
}

void MainWindow::pickMeasurementPoint(const QPointF& point)
{
    if (!m_DisplayedFrame)
    {
        return;
    }
    const auto frame = m_DisplayedFrame->number;
    const bool angle = m_MeasureAngle->isChecked();
    if (!m_MeasurementPoints.isEmpty() && point == m_MeasurementPoints.front())
    {
        statusBar()->showMessage(tr("Choose a different measurement point."));
        return;
    }
    m_MeasurementPoints.append(point);
    auto polyline = m_MeasurementPoints;
    if (angle && polyline.size() > 1)
    {
        polyline.swapItemsAt(0, 1);
    }
    m_Video->setMeasurementPolyline(std::move(polyline));
    const qsizetype required = angle ? 3 : 2;
    if (m_MeasurementPoints.size() < required)
    {
        statusBar()->showMessage(angle ? tr("Click a point on the next side of the angle.") : tr("Click the second distance point."));
        return;
    }

    std::array<WorldPoint, 3> pixels{};
    std::array<WorldPoint, 3> world{};
    bool calibrated = m_Session.document.project.calibration.has_value();
    for (qsizetype index = 0; index < required; ++index)
    {
        const auto coordinate = m_MeasurementPoints.at(index);
        const auto slot = static_cast<std::size_t>(index);
        pixels.at(slot) = {.x = coordinate.x(), .y = coordinate.y()};
        if (m_Session.document.project.calibration)
        {
            const auto converted =
                worldPosition(*m_Session.document.project.calibration, {.frame = frame, .x = coordinate.x(), .y = coordinate.y()});
            if (converted)
            {
                world.at(slot) = *converted;
            }
            else
            {
                calibrated = false;
            }
        }
    }
    const auto& coordinates = calibrated ? world : pixels;
    const auto result = angle ? interiorAngleDegrees(coordinates.at(0), coordinates.at(1), coordinates.at(2))
                              : pointDistance(coordinates.at(0), coordinates.at(1));
    auto* action = angle ? m_MeasureAngle : m_MeasureDistance;
    const QSignalBlocker blocker(action);
    action->setChecked(false);
    if (!result)
    {
        statusBar()->showMessage(tr("Could not measure these points."));
        return;
    }
    const auto value = QLocale().toString(*result, 'g', 6);
    const auto unit = calibrated ? QString::fromStdString(m_Session.document.project.lengthUnit) : tr("px");
    statusBar()->showMessage(angle ? tr("Angle %1°").arg(value) : tr("Distance %1 %2").arg(value, unit).trimmed());
}

void MainWindow::pickCalibrationPoint(const QPointF& position)
{
    if (!m_DisplayedFrame)
    {
        return;
    }
    if (!m_CalibrationFirst)
    {
        m_CalibrationFirst = position;
        m_Video->setCalibrationPoint(position);
        statusBar()->showMessage(tr("Click the second calibration endpoint."));
        return;
    }
    const auto delta = position - *m_CalibrationFirst;
    if (delta.isNull())
    {
        statusBar()->showMessage(tr("Choose a different calibration endpoint."));
        return;
    }
    const auto frame = m_DisplayedFrame->number;
    const auto imageSize = m_DisplayedFrame->image.size();

    QDialog dialog(this);
    dialog.setObjectName("calibrationDialog");
    dialog.setWindowTitle(tr("Calibration Stick"));
    auto* layout = new QFormLayout(&dialog);
    auto* length = new QDoubleSpinBox;
    length->setObjectName("knownDistance");
    length->setDecimals(6);
    length->setRange(0.000001, 1'000'000'000.0);
    length->setValue(1.0);
    auto* unit =
        new QLineEdit(m_Session.document.project.lengthUnit.empty() ? QStringLiteral("m")
                                                                    : QString::fromStdString(m_Session.document.project.lengthUnit));
    unit->setObjectName("lengthUnit");
    layout->addRow(tr("Known distance"), length);
    layout->addRow(tr("Length unit"), unit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    m_Calibrate->setChecked(false);
    if (!accepted)
    {
        return;
    }

    if (m_Session.history.calibrateDistance(m_Session.document.project,
                                            {.frame = frame,
                                             .imageDeltaX = delta.x(),
                                             .imageDeltaY = delta.y(),
                                             .knownLength = length->value(),
                                             .defaultOriginX = imageSize.width() / 2.0,
                                             .defaultOriginY = imageSize.height() / 2.0,
                                             .unit = unit->text().trimmed().toStdString()}))
    {
        refreshEditViews(m_TrackPicker->currentIndex());
        statusBar()->showMessage(tr("Calibration updated."));
    }
    else
    {
        statusBar()->showMessage(tr("Could not apply this calibration."));
    }
}

bool MainWindow::chooseWorkspacePath()
{
    QFileDialog dialog(this, tr("Save MoTrack project"), {}, tr("MoTrack projects (*.motrack)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("motrack");
    return dialog.exec() == QDialog::Accepted && saveWorkspace(dialog.selectedFiles().value(0));
}

bool MainWindow::saveCurrentWorkspace()
{
    return m_Session.nativeSavePath().isEmpty() ? chooseWorkspacePath() : saveWorkspace(m_Session.nativeSavePath());
}

bool MainWindow::saveWorkspace(const QString& path)
{
    if (m_Saving)
    {
        return false;
    }
    if (m_Session.sourcePath().isEmpty() || !path.endsWith(".motrack", Qt::CaseInsensitive))
    {
        statusBar()->showMessage(tr("Choose a project filename ending in .motrack."));
        return false;
    }
    auto document = m_Session.document;
    // Keep the synchronous result used by Save-before-close, but let the GUI paint and process worker
    // completions. User input is deferred while the owned snapshot is validated and serialized off-thread.
    const auto checkpoint = m_Session.saveCheckpoint();
    bool alive = true;
    QEventLoop loop;
    connect(this, &QObject::destroyed, &loop, [&alive] { alive = false; });
    m_Saving = true;
    statusBar()->showMessage(tr("Saving project…"));
    struct SaveResult
    {
        bool saved = false;
        QHash<QString, QString> mediaLocations;
    };
    QFutureWatcher<SaveResult> watcher;
    connect(&watcher, &QFutureWatcher<SaveResult>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(m_SavePool,
                                        [path, sourcePath = m_MediaProjectPath, document = std::move(document)]() mutable -> SaveResult
                                        {
                                            try
                                            {
                                                refreshWorkspaceMediaLocations(document, sourcePath);
                                                rebaseWorkspaceMedia(document, sourcePath, path);
                                                writeWorkspaceDocument(path, document);
                                                return {.saved = true, .mediaLocations = std::move(document.mediaLocations)};
                                            }
                                            catch (const std::exception&)
                                            {
                                                return {};
                                            }
                                        }));
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!alive)
    {
        return false;
    }
    m_Saving = false;
    for (auto& completion : std::exchange(m_AfterSave, {}))
    {
        // Finish save bookkeeping and its caller (including close) before applying prepared opens.
        // Another save may start first, so check the guard again when the queued callback runs.
        QTimer::singleShot(0, this, [this, completion = std::move(completion)]() mutable { completeAfterSave(std::move(completion)); });
    }
    const auto result = watcher.result();
    const auto outcome = m_Session.completeSave(checkpoint, path, result.saved);
    if (outcome == SaveOutcome::Replaced)
    {
        return false;
    }
    if (outcome == SaveOutcome::Failed)
    {
        statusBar()->showMessage(tr("Could not save this project."));
        return false;
    }
    // The snapshot may have resolved media restored through a temporary alias since opening.
    // Keep those physical locations for subsequent saves and clip edits after that alias is removed.
    m_Session.document.mediaLocations = result.mediaLocations;
    refreshImageMediaLocations();
    setWindowTitle(tr("%1[*] — MoTrack").arg(QFileInfo(path).fileName()));
    if (outcome == SaveOutcome::Stale)
    {
        setWindowModified(true);
        statusBar()->showMessage(tr("Project changed while saving. Save again to keep the latest edits."));
        return false;
    }
    setWindowModified(false);
    statusBar()->showMessage(tr("Project saved."));
    return true;
}

bool MainWindow::saveMeasurements(const QString& path)
{
    return saveCsv(path, measurementsCsv(m_Session.document.project));
}

bool MainWindow::saveCsv(const QString& path, const std::string& csv)
{
    QSaveFile file(path);
    const auto size = static_cast<qint64>(csv.size());
    if (!file.open(QIODevice::WriteOnly) || file.write(csv.data(), size) != size || !file.commit())
    {
        statusBar()->showMessage(tr("Could not save the measurements."));
        return false;
    }
    statusBar()->showMessage(tr("Measurements exported."));
    return true;
}

void MainWindow::displayMarks()
{
    m_Marks->setRowCount(0);
    for (const auto& track : m_Session.document.project.pointMasses)
    {
        for (const auto& mark : track.marks)
        {
            const auto row = m_Marks->rowCount();
            m_Marks->insertRow(row);
            m_Marks->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(track.name)));
            m_Marks->setItem(row, 1, new QTableWidgetItem(QString::number(mark.frame)));
            m_Marks->setItem(row, 2, new QTableWidgetItem(QString::number(mark.x, 'g', 12)));
            m_Marks->setItem(row, 3, new QTableWidgetItem(QString::number(mark.y, 'g', 12)));
            const auto world =
                m_Session.document.project.calibration ? worldPosition(*m_Session.document.project.calibration, mark) : std::nullopt;
            if (world)
            {
                m_Marks->setItem(row, 4, new QTableWidgetItem(QString::number(world->x, 'g', 12)));
                m_Marks->setItem(row, 5, new QTableWidgetItem(QString::number(world->y, 'g', 12)));
            }
        }
    }
}

void MainWindow::displayStep(int step)
{
    cancelVectorMark();
    if (m_AutoTrackMatch)
    {
        stopAutoTracking();
    }
    m_PlayTimer.stop();
    m_AutoTrackTimer.stop();
    m_Play->setEnabled(m_StepSlider->maximum() > 0);
    m_StepSlider->setEnabled(m_StepSlider->maximum() > 0);
    m_Previous->setEnabled(step > 0);
    m_Next->setEnabled(step < m_StepSlider->maximum());
    if (const auto frame = m_Sequence ? m_Sequence->readStep(step) : std::nullopt)
    {
        displayFrame(*frame, tr("This image frame could not be opened."));
    }
    else if (!m_MoviePath.isEmpty())
    {
        const auto number =
            m_Session.document.project.clip.firstFrame + static_cast<std::int64_t>(step) * m_Session.document.project.clip.stepSize;
        if (std::in_range<int>(number))
        {
            requestMovieFrame(static_cast<int>(number));
        }
        else
        {
            setPlaying(false);
            stopAutoTracking(tr("AutoTracker stopped because this frame could not be opened."));
            m_FrameLabel->clear();
            m_Video->display({}, {}, tr("This video frame could not be decoded."));
        }
    }
    else
    {
        m_FrameLabel->clear();
        m_Plot->selectFrame(std::nullopt);
    }
}

void MainWindow::displayFrame(const ImageFrame& frame, const QString& failureMessage)
{
    if (!m_DisplayedFrame || m_DisplayedFrame->number != frame.number)
    {
        cancelMeasurement();
        m_Calibrate->setChecked(false);
        m_EditAxes->setChecked(false);
    }
    m_DisplayedFrame = frame;
    m_Calibrate->setEnabled(!frame.image.isNull());
    m_EditAxes->setEnabled(!frame.image.isNull());
    m_MeasureDistance->setEnabled(!frame.image.isNull());
    m_MeasureAngle->setEnabled(!frame.image.isNull());
    refreshAxesOverlay();
    m_Plot->selectFrame(frame.number);
    const auto label = tr("Frame %1").arg(frame.number);
    m_FrameLabel->setText(label);
    QList<QPointF> points;
    for (const auto& track : m_Session.document.project.pointMasses)
    {
        const auto mark = std::ranges::lower_bound(track.marks, frame.number, {}, &Mark::frame);
        if (mark != track.marks.end() && mark->frame == frame.number)
        {
            points.emplaceBack(mark->x, mark->y);
        }
    }
    QList<QLineF> vectors;
    for (const auto& track : m_Session.document.project.vectors)
    {
        const auto sample = std::ranges::lower_bound(track.samples, frame.number, {}, &VectorSample::frame);
        if (sample != track.samples.end() && sample->frame == frame.number)
        {
            vectors.emplaceBack(sample->tailX, sample->tailY, sample->tailX + sample->componentX, sample->tailY + sample->componentY);
        }
    }
    m_Video->display(frame.image, std::move(points), frame.image.isNull() ? failureMessage : label, std::move(vectors));
    if (m_Playing && (frame.image.isNull() || m_StepSlider->value() == m_StepSlider->maximum()))
    {
        setPlaying(false);
    }
    schedulePlayback();
    updateAutoTrackerAction();
    updatePointMassActions();
    autoTrackFrame(frame);
}

void MainWindow::setPlaying(bool playing)
{
    m_Playing = playing;
    m_PlayTimer.stop();
    m_Play->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    m_Play->setAccessibleName(playing ? tr("Pause") : tr("Play"));
    m_Play->setToolTip(m_Play->accessibleName());
}

void MainWindow::updateAutoTrackerAction()
{
    m_AutoTracker->setEnabled(m_AutoTrack.has_value() ||
                              (m_TrackPicker->currentIndex() >= 0 && !selectedVector() && m_DisplayedFrame &&
                               !m_DisplayedFrame->image.isNull() && m_StepSlider->value() < m_StepSlider->maximum()));
}

void MainWindow::startAutoTracking()
{
    const auto track = m_TrackPicker->currentIndex();
    if (track < 0 || selectedVector() || !m_DisplayedFrame || m_StepSlider->value() >= m_StepSlider->maximum())
    {
        stopAutoTracking();
        return;
    }
    const auto& frame = *m_DisplayedFrame;
    const auto& marks = m_Session.document.project.pointMasses.at(static_cast<std::size_t>(track)).marks;
    const auto mark = std::ranges::lower_bound(marks, frame.number, {}, &Mark::frame);
    if (mark == marks.end() || mark->frame != frame.number || mark->x < 0.0 || mark->x >= frame.image.width() || mark->y < 0.0 ||
        mark->y >= frame.image.height())
    {
        stopAutoTracking(tr("Mark a point mass on this frame to start AutoTracker."));
        return;
    }
    const auto center = QPointF(mark->x, mark->y).toPoint();
    const auto pattern = TemplateTracker::fromImage(frame.image, center);
    if (!pattern)
    {
        stopAutoTracking(tr("Choose a textured point away from the edge of the image."));
        return;
    }
    m_AutoTrack =
        AutoTrackSession{.pattern = *pattern, .track = static_cast<std::size_t>(track), .lastFrame = frame.number, .predicted = center};
    cancelMeasurement();
    setPlaying(false);
    m_Calibrate->setChecked(false);
    m_EditAxes->setChecked(false);
    statusBar()->showMessage(tr("AutoTracker is following the selected point mass."));
    m_AutoTrackTimer.start(0);
}

void MainWindow::stopAutoTracking(const QString& message)
{
    m_AutoTrackTimer.stop();
    if (auto* pending = std::exchange(m_AutoTrackMatch, nullptr))
    {
        pending->deleteLater();
    }
    m_AutoTrack.reset();
    const QSignalBlocker blocker(m_AutoTracker);
    m_AutoTracker->setChecked(false);
    updateAutoTrackerAction();
    if (!message.isEmpty())
    {
        statusBar()->showMessage(message);
    }
}

void MainWindow::autoTrackFrame(const ImageFrame& frame)
{
    if (!m_AutoTrack || m_AutoTrack->lastFrame == frame.number)
    {
        return;
    }
    auto& session = *m_AutoTrack;
    if (frame.image.isNull() || frame.number - session.lastFrame != m_Session.document.project.clip.stepSize)
    {
        stopAutoTracking(tr("AutoTracker stopped because the next clip frame was unavailable."));
        return;
    }
    const auto& marks = m_Session.document.project.pointMasses.at(session.track).marks;
    if (std::ranges::binary_search(marks, frame.number, {}, &Mark::frame))
    {
        stopAutoTracking(tr("AutoTracker stopped at an existing mark."));
        return;
    }
    auto* match = new QFutureWatcher<std::optional<QPoint>>(this);
    m_AutoTrackMatch = match;
    connect(match,
            &QFutureWatcher<std::optional<QPoint>>::finished,
            this,
            [this, match, number = frame.number]
            {
                if (m_AutoTrackMatch == match)
                {
                    m_AutoTrackMatch = nullptr;
                    match->deleteLater();
                    applyAutoTrackMatch(number, match->result());
                }
            });
    match->setFuture(QtConcurrent::run([pattern = session.pattern, image = frame.image, predicted = session.predicted]
                                       { return pattern.find(image, predicted); }));
}

void MainWindow::applyAutoTrackMatch(int frame, const std::optional<QPoint>& point)
{
    if (!m_AutoTrack)
    {
        return;
    }
    if (!point)
    {
        stopAutoTracking(tr("AutoTracker could not find a reliable match. Mark this frame to continue."));
        return;
    }
    auto& session = *m_AutoTrack;
    session.lastFrame = frame;
    session.predicted = *point;
    m_Session.history.markPointMass(m_Session.document.project,
                                    session.track,
                                    {.frame = frame, .x = static_cast<double>(point->x()), .y = static_cast<double>(point->y())});
    refreshEditViews(static_cast<int>(session.track));
    if (m_StepSlider->value() == m_StepSlider->maximum())
    {
        stopAutoTracking(tr("AutoTracker reached the end of the clip."));
    }
    else
    {
        m_AutoTrackTimer.start(0);
    }
}

void MainWindow::schedulePlayback()
{
    if (!m_Playing || !m_DisplayedFrame || m_DisplayedFrame->image.isNull() || m_StepSlider->value() >= m_StepSlider->maximum())
    {
        return;
    }
    const auto frame = static_cast<std::size_t>(m_DisplayedFrame->number);
    const auto next = frame + static_cast<std::size_t>(m_Session.document.project.clip.stepSize);
    const auto delayMs = next < m_Session.document.movieFrameTimes.size()
                             ? 1000.0 * (m_Session.document.movieFrameTimes.at(next) - m_Session.document.movieFrameTimes.at(frame))
                             : m_Session.document.project.clip.frameDurationMs.value_or(40.0) * m_Session.document.project.clip.stepSize;
    m_PlayTimer.start(static_cast<int>(std::lround(std::clamp(delayMs, 1.0, 60'000.0))));
}

void MainWindow::requestMovieFrame(int frame)
{
    const auto generation = ++m_RequestGeneration;
    cancelMeasurement();
    m_DisplayedFrame.reset();
    updateAutoTrackerAction();
    updatePointMassActions();
    m_Calibrate->setChecked(false);
    m_Calibrate->setEnabled(false);
    m_EditAxes->setChecked(false);
    m_EditAxes->setEnabled(false);
    m_MeasureDistance->setEnabled(false);
    m_MeasureAngle->setEnabled(false);
    m_Plot->selectFrame(frame);
    m_FrameLabel->setText(tr("Frame %1").arg(frame));
    if (!m_Playing)
    {
        m_Video->display({}, {}, tr("Decoding frame…"));
    }
    if (!m_MovieWorker)
    {
        m_MovieWorker = new MovieWorker(m_RequestGeneration);
        m_MovieWorker->moveToThread(&m_MovieThread);
        connect(&m_MovieThread, &QThread::finished, m_MovieWorker, &QObject::deleteLater);
        connect(m_MovieWorker,
                &MovieWorker::frameDecoded,
                this,
                [this](int number, quint64 request, const QImage& image)
                {
                    if (request == m_RequestGeneration)
                    {
                        displayFrame({.number = number, .image = image}, tr("This video frame could not be decoded."));
                    }
                });
        m_MovieThread.start();
    }
    const bool hasTimestamp = frame >= 0 && std::cmp_less(frame, m_MoviePresentationTimes.size());
    const qint64 timestamp = hasTimestamp ? m_MoviePresentationTimes.at(static_cast<std::size_t>(frame)).count() : 0;
    QMetaObject::invokeMethod(m_MovieWorker,
                              "readFrame",
                              Qt::QueuedConnection,
                              m_MoviePath,
                              frame,
                              generation,
                              m_Session.document.useDisplayOrientation,
                              hasTimestamp,
                              timestamp);
}

} // namespace motrack
