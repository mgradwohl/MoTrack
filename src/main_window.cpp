#include "main_window.hpp"

#include "measurements.hpp"
#include "position_plot.hpp"
#include "video_reader.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>

namespace motrack
{

class VideoCanvas final : public QWidget
{
  public:
    std::function<bool(QPointF, bool)> pick;

    void setCalibrationPoint(std::optional<QPointF> position)
    {
        m_CalibrationPoint = position;
        update();
    }

    void display(QImage image, QList<QPointF> marks, const QString& message)
    {
        m_Image = std::move(image);
        m_Marks = std::move(marks);
        m_Message = message;
        setAccessibleName(message);
        update();
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (!m_Image.isNull() && pick && event->button() == Qt::LeftButton)
        {
            const auto area = imageArea();
            const auto position = event->position();
            if (area.contains(position) && !area.isEmpty())
            {
                const QPointF pixel((position.x() - area.left()) * m_Image.width() / area.width(),
                                    (position.y() - area.top()) * m_Image.height() / area.height());
                if (pick(pixel, event->modifiers().testFlag(Qt::ShiftModifier)))
                {
                    event->accept();
                    return;
                }
            }
        }
        QWidget::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent* /*event*/) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().base());
        if (m_Image.isNull())
        {
            painter.setPen(palette().text().color());
            painter.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, m_Message);
            return;
        }

        const auto area = imageArea();
        painter.drawImage(area, m_Image);
        painter.setClipRect(area);
        painter.setPen(QPen(Qt::yellow, 2));
        for (const auto& mark : m_Marks)
        {
            painter.drawEllipse(canvasPoint(mark, area), 5, 5);
        }
        if (m_CalibrationPoint)
        {
            painter.setPen(QPen(Qt::cyan, 2));
            painter.drawEllipse(canvasPoint(*m_CalibrationPoint, area), 6, 6);
        }
    }

  private:
    [[nodiscard]] QRectF imageArea() const
    {
        const auto scaled = m_Image.size().scaled(size(), Qt::KeepAspectRatio);
        return {QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled};
    }

    [[nodiscard]] QPointF canvasPoint(const QPointF& pixel, const QRectF& area) const
    {
        return {area.left() + pixel.x() * area.width() / m_Image.width(), area.top() + pixel.y() * area.height() / m_Image.height()};
    }

    QImage m_Image;
    QList<QPointF> m_Marks;
    QString m_Message;
    std::optional<QPointF> m_CalibrationPoint;
};

MainWindow::MainWindow()
{
    setWindowTitle("MoTrack");
    resize(1000, 650);
    auto* open = new QAction(tr("&Open Tracker project…"), this);
    open->setObjectName("openProject");
    open->setShortcut(QKeySequence::Open);
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(open);
    m_Export = new QAction(tr("&Export measurements…"), this);
    m_Export->setObjectName("exportMeasurements");
    m_Export->setEnabled(false);
    fileMenu->addAction(m_Export);
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    m_Undo = editMenu->addAction(tr("&Undo"));
    m_Undo->setObjectName("undoEdit");
    m_Undo->setShortcut(QKeySequence::Undo);
    m_Redo = editMenu->addAction(tr("&Redo"));
    m_Redo->setObjectName("redoEdit");
    m_Redo->setShortcut(QKeySequence::Redo);
    auto* tracksMenu = menuBar()->addMenu(tr("&Tracks"));
    m_NewPointMass = tracksMenu->addAction(tr("New &point mass"));
    m_NewPointMass->setObjectName("newPointMass");
    m_NewPointMass->setEnabled(false);
    auto* repeat = tracksMenu->addAction(tr("Repeat previous mark"));
    repeat->setObjectName("repeatMark");
    repeat->setShortcuts({QKeySequence("Shift+Return"), QKeySequence("Shift+Enter")});
    auto* tools = addToolBar(tr("Project"));
    tools->addAction(open);
    m_Calibrate = tools->addAction(tr("Calibration Stick"));
    m_Calibrate->setObjectName("calibrationStick");
    m_Calibrate->setCheckable(true);
    m_Calibrate->setEnabled(false);
    tools->addAction(m_NewPointMass);
    m_TrackPicker = new QComboBox;
    m_TrackPicker->setObjectName("trackPicker");
    m_TrackPicker->setAccessibleName(tr("Selected point mass"));
    tools->addWidget(m_TrackPicker);

    auto* workspace = new QSplitter;
    auto* videoPane = new QWidget;
    auto* layout = new QVBoxLayout(videoPane);
    m_Video = new VideoCanvas;
    m_Video->setObjectName("videoCanvas");
    m_Video->setMinimumSize(240, 160);
    m_Video->setToolTip(tr("Shift-click the image to mark the selected point mass."));
    m_Video->pick = [this](QPointF position, bool shift)
    {
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
    m_Video->display({}, {}, tr("Open a Tracker project to inspect its saved marks."));
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
    m_StepSlider = new QSlider(Qt::Horizontal);
    m_StepSlider->setObjectName("stepSlider");
    m_StepSlider->setTracking(false);
    m_StepSlider->setAccessibleName(tr("Clip position"));
    m_FrameLabel = new QLabel;
    m_FrameLabel->setObjectName("frameLabel");
    controls->addWidget(m_Previous);
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
    m_Plot = new PositionPlot;
    m_Plot->seekFrame = [this](int frame)
    {
        m_StepSlider->setValue((frame - m_Project.clip.firstFrame) / m_Project.clip.stepSize);
    };
    plotLayout->addWidget(m_PlotQuantity);
    plotLayout->addWidget(m_Plot, 1);
    analysis->addWidget(plotPane);
    analysis->addWidget(m_Marks);
    workspace->addWidget(analysis);
    workspace->setStretchFactor(0, 2);
    workspace->setStretchFactor(1, 1);
    setCentralWidget(workspace);
    statusBar()->showMessage(tr("Ready"));
    m_MovieWorkers.setMaxThreadCount(1);

    displayStep(0);
    refreshEditViews(0);
    connect(m_StepSlider, &QSlider::valueChanged, this, &MainWindow::displayStep);
    connect(m_Previous, &QToolButton::clicked, m_StepSlider, [this] { m_StepSlider->triggerAction(QSlider::SliderSingleStepSub); });
    connect(m_Next, &QToolButton::clicked, m_StepSlider, [this] { m_StepSlider->triggerAction(QSlider::SliderSingleStepAdd); });
    connect(open,
            &QAction::triggered,
            this,
            [this]
            {
                const auto path = QFileDialog::getOpenFileName(this, tr("Open Tracker project"), {}, tr("Tracker projects (*.trk)"));
                if (!path.isEmpty() && confirmDiscard())
                {
                    loadProject(path);
                }
            });
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
    connect(m_NewPointMass,
            &QAction::triggered,
            this,
            [this]
            {
                const auto index = m_Project.pointMasses.size();
                m_Edits.createPointMass(m_Project, tr("Point Mass %1").arg(index + 1).toStdString());
                refreshEditViews(static_cast<int>(index));
                statusBar()->showMessage(tr("Shift-click the image to mark the selected point mass."));
            });
    connect(m_Undo,
            &QAction::triggered,
            this,
            [this]
            {
                if (m_Edits.undo(m_Project))
                {
                    refreshEditViews(m_TrackPicker->currentIndex());
                }
            });
    connect(m_Redo,
            &QAction::triggered,
            this,
            [this]
            {
                if (m_Edits.redo(m_Project))
                {
                    refreshEditViews(m_TrackPicker->currentIndex());
                }
            });
    connect(repeat, &QAction::triggered, this, &MainWindow::repeatPreviousMark);
    connect(m_Calibrate,
            &QAction::toggled,
            this,
            [this](bool active)
            {
                clearCalibrationPoint();
                if (active)
                {
                    statusBar()->showMessage(tr("Click the first calibration endpoint."));
                }
                else
                {
                    statusBar()->clearMessage();
                }
            });
    connect(m_TrackPicker, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPlot);
    connect(m_PlotQuantity, &QComboBox::currentIndexChanged, this, &MainWindow::refreshPlot);
}

MainWindow::~MainWindow()
{
    ++*m_RequestGeneration;
}

bool MainWindow::loadProject(const QString& path)
{
    try
    {
        m_Project = readLegacyProject(std::filesystem::path(path.toStdU16String()));
    }
    catch (const ProjectError&)
    {
        statusBar()->showMessage(tr("Could not open this Tracker project."));
        return false;
    }

    ++*m_RequestGeneration;
    m_FrameWatcher.reset();
    m_DisplayedFrame.reset();
    m_Calibrate->setChecked(false);
    m_Calibrate->setEnabled(false);
    m_Edits.clear();
    m_NewPointMass->setEnabled(true);
    refreshEditViews(0);
    m_Sequence = ImageSequence::open(path, m_Project.clip);
    m_MoviePath.clear();
    const auto& clip = m_Project.clip;
    if (!m_Sequence && clip.mediaReferences.size() == 1 && clip.stepCount.value_or(0) > 0)
    {
        const auto movie = resolveLocalMediaPath(path, clip.mediaReferences.front());
        if (QFileInfo(movie).isFile())
        {
            m_MoviePath = movie;
        }
    }
    const int stepCount = m_Sequence ? m_Sequence->stepCount() : (m_MoviePath.isEmpty() ? 0 : clip.stepCount.value_or(0));
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
    statusBar()->showMessage(
        tr("Imported %1 marks. Unsupported tracks: %2.").arg(m_Marks->rowCount()).arg(m_Project.unsupportedTrackTypes.size()));
    setWindowTitle(tr("%1[*] — MoTrack").arg(QFileInfo(path).fileName()));
    return true;
}

bool MainWindow::confirmDiscard()
{
    return !m_Edits.canUndo() || QMessageBox::question(this,
                                                       tr("Discard tracking edits?"),
                                                       tr("Your tracking edits are only stored in this session."),
                                                       QMessageBox::Discard | QMessageBox::Cancel,
                                                       QMessageBox::Cancel) == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (confirmDiscard())
    {
        event->accept();
    }
    else
    {
        event->ignore();
    }
}

void MainWindow::refreshEditViews(int track)
{
    const QSignalBlocker blocker(m_TrackPicker);
    m_TrackPicker->clear();
    for (const auto& mass : m_Project.pointMasses)
    {
        m_TrackPicker->addItem(QString::fromStdString(mass.name));
    }
    m_TrackPicker->setCurrentIndex(std::min(std::max(0, track), m_TrackPicker->count() - 1));
    m_Undo->setEnabled(m_Edits.canUndo());
    m_Redo->setEnabled(m_Edits.canRedo());
    setWindowModified(m_Edits.canUndo());
    displayMarks();
    refreshPlot();
    m_Export->setEnabled(m_Marks->rowCount() > 0);
    if (m_DisplayedFrame && !m_DisplayedFrame->image.isNull())
    {
        displayFrame(*m_DisplayedFrame, {});
    }
}

void MainWindow::refreshPlot()
{
    const auto quantity = static_cast<PlotQuantity>(m_PlotQuantity->currentData().toInt());
    const auto track = m_TrackPicker->currentIndex();
    auto series = track >= 0 ? measurementPlot(m_Project, static_cast<std::size_t>(track), quantity) : PlotSeries{};
    const auto horizontal = series.timed ? tr("Time (s)") : tr("Frame");
    auto unit = series.calibrated ? QString::fromStdString(m_Project.lengthUnit) : tr("px");
    if ((quantity == PlotQuantity::VelocityX || quantity == PlotQuantity::VelocityY) && !unit.isEmpty())
    {
        unit = tr("%1/s").arg(unit);
    }
    const auto label = m_PlotQuantity->currentText();
    const auto vertical = unit.isEmpty() ? label : tr("%1 (%2)").arg(label, unit);
    m_Plot->setSeries(std::move(series), horizontal, vertical);
}

void MainWindow::markAt(const QPointF& position)
{
    const auto track = m_TrackPicker->currentIndex();
    if (track < 0 || !m_DisplayedFrame || m_DisplayedFrame->image.isNull())
    {
        return;
    }
    const Mark mark{.frame = m_DisplayedFrame->number, .x = position.x(), .y = position.y()};
    if (m_Edits.markPointMass(m_Project, static_cast<std::size_t>(track), mark))
    {
        refreshEditViews(track);
        m_StepSlider->triggerAction(QSlider::SliderSingleStepAdd);
    }
}

void MainWindow::repeatPreviousMark()
{
    const auto track = m_TrackPicker->currentIndex();
    if (track < 0 || !m_DisplayedFrame)
    {
        return;
    }
    const auto& marks = m_Project.pointMasses.at(static_cast<std::size_t>(track)).marks;
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
    auto* unit = new QLineEdit(m_Project.lengthUnit.empty() ? QStringLiteral("m") : QString::fromStdString(m_Project.lengthUnit));
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

    if (m_Edits.calibrateDistance(m_Project,
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

bool MainWindow::saveMeasurements(const QString& path)
{
    QSaveFile file(path);
    const auto csv = measurementsCsv(m_Project);
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
    for (const auto& track : m_Project.pointMasses)
    {
        for (const auto& mark : track.marks)
        {
            const auto row = m_Marks->rowCount();
            m_Marks->insertRow(row);
            m_Marks->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(track.name)));
            m_Marks->setItem(row, 1, new QTableWidgetItem(QString::number(mark.frame)));
            m_Marks->setItem(row, 2, new QTableWidgetItem(QString::number(mark.x, 'g', 12)));
            m_Marks->setItem(row, 3, new QTableWidgetItem(QString::number(mark.y, 'g', 12)));
            const auto world = m_Project.calibration ? worldPosition(*m_Project.calibration, mark) : std::nullopt;
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
    m_StepSlider->setEnabled(m_StepSlider->maximum() > 0);
    m_Previous->setEnabled(step > 0);
    m_Next->setEnabled(step < m_StepSlider->maximum());
    if (const auto frame = m_Sequence ? m_Sequence->readStep(step) : std::nullopt)
    {
        displayFrame(*frame, tr("This image frame could not be opened."));
    }
    else if (!m_MoviePath.isEmpty())
    {
        const auto number = m_Project.clip.firstFrame + static_cast<std::int64_t>(step) * m_Project.clip.stepSize;
        if (std::in_range<int>(number))
        {
            requestMovieFrame(static_cast<int>(number));
        }
        else
        {
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
        m_Calibrate->setChecked(false);
    }
    m_DisplayedFrame = frame;
    m_Calibrate->setEnabled(!frame.image.isNull());
    m_Plot->selectFrame(frame.number);
    const auto label = tr("Frame %1").arg(frame.number);
    m_FrameLabel->setText(label);
    QList<QPointF> points;
    for (const auto& track : m_Project.pointMasses)
    {
        const auto mark = std::ranges::lower_bound(track.marks, frame.number, {}, &Mark::frame);
        if (mark != track.marks.end() && mark->frame == frame.number)
        {
            points.emplaceBack(mark->x, mark->y);
        }
    }
    m_Video->display(frame.image, std::move(points), frame.image.isNull() ? failureMessage : label);
}

void MainWindow::requestMovieFrame(int frame)
{
    const auto generation = ++*m_RequestGeneration;
    m_DisplayedFrame.reset();
    m_Calibrate->setChecked(false);
    m_Calibrate->setEnabled(false);
    m_Plot->selectFrame(frame);
    m_FrameLabel->setText(tr("Frame %1").arg(frame));
    m_Video->display({}, {}, tr("Decoding frame…"));
    m_FrameWatcher = std::make_unique<QFutureWatcher<QImage>>();
    auto* watcher = m_FrameWatcher.get();
    connect(watcher,
            &QFutureWatcher<QImage>::finished,
            this,
            [this, watcher, generation, frame]
            {
                if (generation == *m_RequestGeneration)
                {
                    displayFrame({.number = frame, .image = watcher->result()}, tr("This video frame could not be decoded."));
                }
            });
    watcher->setFuture(QtConcurrent::run(&m_MovieWorkers,
                                         [path = m_MoviePath, request = m_RequestGeneration, generation, frame]() -> QImage
                                         {
                                             if (generation != *request)
                                             {
                                                 return {};
                                             }
                                             try
                                             {
                                                 VideoReader reader(path);
                                                 while (generation == *request)
                                                 {
                                                     const auto next = reader.readNext();
                                                     if (!next)
                                                     {
                                                         break;
                                                     }
                                                     if (std::cmp_equal(next->ordinal, frame))
                                                     {
                                                         return next->image;
                                                     }
                                                 }
                                             }
                                             catch (const VideoError&)
                                             {
                                                 return {};
                                             }
                                             return {};
                                         }));
}

} // namespace motrack
