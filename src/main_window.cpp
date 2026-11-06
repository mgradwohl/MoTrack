#include "main_window.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace motrack
{

class VideoCanvas final : public QWidget
{
  public:
    void display(QImage image, QList<QPointF> marks, const QString& message)
    {
        m_Image = std::move(image);
        m_Marks = std::move(marks);
        m_Message = message;
        setAccessibleName(message);
        update();
    }

  protected:
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

        const auto scaled = m_Image.size().scaled(size(), Qt::KeepAspectRatio);
        const QRectF area(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
        painter.drawImage(area, m_Image);
        painter.setClipRect(area);
        painter.setPen(QPen(Qt::yellow, 2));
        for (const auto& mark : m_Marks)
        {
            const QPointF point(area.left() + mark.x() * area.width() / m_Image.width(),
                                area.top() + mark.y() * area.height() / m_Image.height());
            painter.drawEllipse(point, 5, 5);
        }
    }

  private:
    QImage m_Image;
    QList<QPointF> m_Marks;
    QString m_Message;
};

MainWindow::MainWindow()
{
    setWindowTitle("MoTrack");
    resize(1000, 650);
    auto* open = new QAction(tr("&Open Tracker project…"), this);
    open->setShortcut(QKeySequence::Open);
    menuBar()->addMenu(tr("&File"))->addAction(open);
    addToolBar(tr("Project"))->addAction(open);

    auto* workspace = new QSplitter;
    auto* videoPane = new QWidget;
    auto* layout = new QVBoxLayout(videoPane);
    m_Video = new VideoCanvas;
    m_Video->setObjectName("videoCanvas");
    m_Video->setMinimumSize(240, 160);
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
    m_StepSlider->setAccessibleName(tr("Clip position"));
    m_FrameLabel = new QLabel;
    m_FrameLabel->setObjectName("frameLabel");
    controls->addWidget(m_Previous);
    controls->addWidget(m_Next);
    controls->addWidget(m_StepSlider, 1);
    controls->addWidget(m_FrameLabel);
    layout->addLayout(controls);
    workspace->addWidget(videoPane);

    m_Marks = new QTableWidget(0, 4);
    m_Marks->setObjectName("marksTable");
    m_Marks->setHorizontalHeaderLabels({tr("Track"), tr("Frame"), tr("Image x (px)"), tr("Image y (px)")});
    m_Marks->setEditTriggers(QAbstractItemView::NoEditTriggers);
    workspace->addWidget(m_Marks);
    workspace->setStretchFactor(0, 2);
    workspace->setStretchFactor(1, 1);
    setCentralWidget(workspace);
    statusBar()->showMessage(tr("Ready"));

    displayStep(0);
    connect(m_StepSlider, &QSlider::valueChanged, this, &MainWindow::displayStep);
    connect(m_Previous, &QToolButton::clicked, m_StepSlider, [this] { m_StepSlider->triggerAction(QSlider::SliderSingleStepSub); });
    connect(m_Next, &QToolButton::clicked, m_StepSlider, [this] { m_StepSlider->triggerAction(QSlider::SliderSingleStepAdd); });
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

    displayMarks();
    m_Sequence = ImageSequence::open(path, m_Project.clip);
    {
        const QSignalBlocker blocker(m_StepSlider);
        m_StepSlider->setRange(0, m_Sequence ? m_Sequence->stepCount() - 1 : 0);
        m_StepSlider->setValue(0);
    }
    if (!m_Sequence)
    {
        m_Video->display({}, {}, tr("No supported image sequence is attached to this project."));
    }
    displayStep(0);
    statusBar()->showMessage(
        tr("Imported %1 marks. Unsupported tracks: %2.").arg(m_Marks->rowCount()).arg(m_Project.unsupportedTrackTypes.size()));
    setWindowTitle(tr("%1 — MoTrack").arg(QFileInfo(path).fileName()));
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
        }
    }
}

void MainWindow::displayStep(int step)
{
    const auto frame = m_Sequence ? m_Sequence->readStep(step) : std::nullopt;
    m_StepSlider->setEnabled(m_StepSlider->maximum() > 0);
    m_Previous->setEnabled(step > 0);
    m_Next->setEnabled(step < m_StepSlider->maximum());
    if (!frame)
    {
        m_FrameLabel->clear();
        return;
    }

    const auto label = tr("Frame %1").arg(frame->number);
    m_FrameLabel->setText(label);
    QList<QPointF> points;
    for (const auto& track : m_Project.pointMasses)
    {
        const auto mark = std::ranges::lower_bound(track.marks, frame->number, {}, &Mark::frame);
        if (mark != track.marks.end() && mark->frame == frame->number)
        {
            points.emplaceBack(mark->x, mark->y);
        }
    }
    m_Video->display(frame->image, std::move(points), frame->image.isNull() ? tr("This image frame could not be opened.") : label);
}

} // namespace motrack
