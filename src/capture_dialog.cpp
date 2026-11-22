#include "capture_dialog.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace motrack
{

/// The latest preview image, letterboxed into the widget.
class CapturePreview final : public QWidget
{
  public:
    explicit CapturePreview(QWidget* parent) : QWidget(parent)
    {
        setObjectName("capturePreview");
        setMinimumSize(320, 180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    void setImage(QImage image)
    {
        m_Image = std::move(image);
        update();
    }
    [[nodiscard]] const QImage& image() const
    {
        return m_Image;
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().color(QPalette::Shadow));
        if (m_Image.isNull())
            return;
        QRect target(QPoint{}, m_Image.size().scaled(size(), Qt::KeepAspectRatio));
        target.moveCenter(rect().center());
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, m_Image);
    }

  private:
    QImage m_Image;
};

namespace
{

int countForPlural(qint64 count)
{
    return static_cast<int>(std::clamp<qint64>(count, 0, std::numeric_limits<int>::max()));
}

QString frameRate(double rate)
{
    return QLocale().toString(rate, 'g', 4);
}

QString formatLabel(const CameraFormat& format)
{
    auto label = QStringLiteral("%1 × %2").arg(format.resolution.width()).arg(format.resolution.height());
    // Some cameras report no frame rate for a mode; show none rather than "0 fps".
    if (format.maximumFrameRate > 0)
        label = CaptureDialog::tr("%1, %2").arg(
            label,
            std::abs(format.maximumFrameRate - format.minimumFrameRate) < 0.01
                ? CaptureDialog::tr("%1 fps").arg(frameRate(format.maximumFrameRate))
                : CaptureDialog::tr("%1–%2 fps").arg(frameRate(format.minimumFrameRate), frameRate(format.maximumFrameRate)));
    return format.pixelFormat.isEmpty() ? label : CaptureDialog::tr("%1 (%2)").arg(label, format.pixelFormat);
}

bool busy(CaptureDialog::State state)
{
    using enum CaptureDialog::State;
    return state == Recording || state == Finishing || state == Recorded || state == Saving;
}

} // namespace

CaptureDialog::CaptureDialog(std::unique_ptr<CameraSource> source, QWidget* parent)
    : QDialog(parent), m_Pipeline(std::make_unique<CapturePipeline>()), m_Source(std::move(source))
{
    setObjectName("captureDialog");
    setWindowTitle(tr("Capture from camera"));
    resize(720, 600);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    m_Camera = new QComboBox(this);
    m_Camera->setObjectName("captureCamera");
    m_Format = new QComboBox(this);
    m_Format->setObjectName("captureFormat");
    form->addRow(tr("&Camera:"), m_Camera);
    form->addRow(tr("&Format:"), m_Format);
    layout->addLayout(form);
    m_Preview = new CapturePreview(this);
    layout->addWidget(m_Preview, 1);
    m_Elapsed = new QLabel(this);
    m_Elapsed->setObjectName("captureElapsed");
    layout->addWidget(m_Elapsed);
    m_Status = new QLabel(this);
    m_Status->setObjectName("captureStatus");
    m_Status->setWordWrap(true);
    layout->addWidget(m_Status);
    auto* buttons = new QHBoxLayout;
    m_Record = new QPushButton(tr("&Record"), this);
    m_Record->setObjectName("captureRecord");
    m_Save = new QPushButton(tr("&Save and open…"), this);
    m_Save->setObjectName("captureSave");
    m_Save->setToolTip(tr("Save the clip to a permanent file and open it as a new experiment."));
    m_Discard = new QPushButton(tr("&Discard recording"), this);
    m_Discard->setObjectName("captureDiscard");
    m_Close = new QPushButton(tr("Cancel"), this);
    m_Close->setObjectName("captureCancel");
    buttons->addWidget(m_Record);
    buttons->addWidget(m_Save);
    buttons->addWidget(m_Discard);
    buttons->addStretch(1);
    buttons->addWidget(m_Close);
    layout->addLayout(buttons);

    connect(m_Record, &QPushButton::clicked, this, &CaptureDialog::toggleRecording);
    connect(m_Save, &QPushButton::clicked, this, &CaptureDialog::chooseDestination);
    connect(m_Discard, &QPushButton::clicked, this, &CaptureDialog::discardRecording);
    connect(m_Close, &QPushButton::clicked, this, &CaptureDialog::reject);
    connect(m_Camera,
            &QComboBox::currentIndexChanged,
            this,
            [this]
            {
                refreshFormats();
                checkAccess();
            });
    connect(m_Format, &QComboBox::currentIndexChanged, this, &CaptureDialog::checkAccess);

    connect(m_Pipeline.get(),
            &CapturePipeline::previewReady,
            this,
            [this](const QImage& image)
            {
                m_Preview->setImage(image);
                m_Pipeline->acknowledgePreview();
            });
    connect(m_Pipeline.get(), &CapturePipeline::progress, this, &CaptureDialog::showProgress);
    connect(m_Pipeline.get(),
            &CapturePipeline::recordingFinished,
            this,
            [this](const CaptureResult& result)
            {
                if (m_Closed)
                {
                    m_Temporary.reset(); // Finished just as the window closed; the clip is discarded.
                    return;
                }
                m_Result = result;
                auto text = summary(result);
                if (!m_CameraActive)
                    text += QLatin1Char('\n') + tr("The camera stopped during the recording; the clip ends at its last frame.");
                setState(State::Recorded, text);
            });
    connect(m_Pipeline.get(),
            &CapturePipeline::recordingFailed,
            this,
            [this](const QString& message)
            {
                m_Temporary.reset();
                m_Result.reset();
                setState(m_CameraActive ? State::Previewing : State::Unavailable, tr("The recording failed: %1").arg(message));
            });
    connect(m_Pipeline.get(), &CapturePipeline::recordingCancelled, this, [this] { m_Temporary.reset(); });
    connect(m_Pipeline.get(),
            &CapturePipeline::frameRejected,
            this,
            [this](const QString& message)
            { m_Status->setText(tr("MoTrack cannot read this camera format (%1). Choose another format.").arg(message)); });

    connect(m_Source.get(), &CameraSource::devicesChanged, this, &CaptureDialog::refreshDevices);
    connect(m_Source.get(), &CameraSource::accessChanged, this, &CaptureDialog::checkAccess);
    connect(m_Source.get(),
            &CameraSource::started,
            this,
            [this]
            {
                m_CameraActive = true;
                if (m_State == State::Starting)
                    setState(State::Previewing);
            });
    connect(m_Source.get(),
            &CameraSource::failed,
            this,
            [this](const QString& message)
            {
                m_CameraActive = false;
                const auto text =
                    tr("The camera could not be used: %1").arg(message) + QLatin1Char('\n') +
                    tr("Another application may be using it, or camera access may be turned off in the system privacy settings.");
                if (m_State == State::Recording)
                {
                    // Keep the frames recorded so far; the clip can still be saved or discarded.
                    m_Pipeline->stopRecording();
                    setState(State::Finishing, text);
                    return;
                }
                if (busy(m_State))
                {
                    m_Status->setText(text); // A finished or finishing recording is unaffected.
                    return;
                }
                setState(State::Unavailable, text);
            });
    setState(State::Unavailable);
    refreshDevices();
}

CaptureDialog::~CaptureDialog()
{
    // A save in progress stops at its next chunk and removes its partial copy; then the camera stops,
    // the pipeline closes any movie, and the temporary folder is removed (member order).
    if (m_SaveCancelled)
        *m_SaveCancelled = true;
    m_Saving.waitForFinished();
    m_Source->stop();
}

CaptureDialog::State CaptureDialog::state() const
{
    return m_State;
}

QString CaptureDialog::temporaryRecording() const
{
    return m_Temporary ? m_Temporary->filePath(QStringLiteral("capture.mov")) : QString{};
}

const std::optional<CaptureResult>& CaptureDialog::recording() const
{
    return m_Result;
}

QImage CaptureDialog::previewImage() const
{
    return m_Preview->image();
}

void CaptureDialog::refreshDevices()
{
    const auto previous = m_Camera->currentData().toByteArray();
    m_Devices = m_Source->devices();
    {
        const QSignalBlocker block(m_Camera);
        m_Camera->clear();
        int selected = -1;
        for (int index = 0; index < m_Devices.size(); ++index)
        {
            const auto& device = m_Devices.at(index);
            m_Camera->addItem(device.description, device.id);
            if (device.id == previous || (selected < 0 && previous.isEmpty() && device.isDefault))
                selected = index;
        }
        m_Camera->setCurrentIndex(selected >= 0 ? selected : (m_Devices.isEmpty() ? -1 : 0));
    }
    const bool unchanged = !previous.isEmpty() && m_Camera->currentData().toByteArray() == previous;
    if (unchanged && (m_CameraActive || busy(m_State)))
        return;
    refreshFormats();
    checkAccess();
}

void CaptureDialog::refreshFormats()
{
    const QSignalBlocker block(m_Format);
    m_Format->clear();
    m_Format->addItem(tr("Camera default"), -1);
    const auto camera = m_Camera->currentIndex();
    if (camera < 0)
        return;
    const auto& formats = m_Devices.at(camera).formats;
    // A plain loop: Clang with libstdc++ 14 rejects views::iota | ranges::to (its forward_like).
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(formats.size()));
    for (int index = 0; index < formats.size(); ++index)
        order.push_back(index);
    // Largest frames first, then the fastest frame rate.
    std::ranges::stable_sort(order,
                             [&formats](int left, int right)
                             {
                                 const auto& a = formats.at(left);
                                 const auto& b = formats.at(right);
                                 const auto areaA = std::int64_t{a.resolution.width()} * a.resolution.height();
                                 const auto areaB = std::int64_t{b.resolution.width()} * b.resolution.height();
                                 return areaA != areaB ? areaA > areaB : a.maximumFrameRate > b.maximumFrameRate;
                             });
    const CameraFormat* previous = nullptr;
    for (const auto index : order)
    {
        const auto& format = formats.at(index);
        if (previous && *previous == format)
            continue;
        m_Format->addItem(formatLabel(format), index);
        previous = &format;
    }
}

void CaptureDialog::checkAccess()
{
    if (busy(m_State))
        return;
    if (m_Devices.isEmpty())
    {
        m_Source->stop();
        m_CameraActive = false;
        m_Preview->setImage({});
        setState(State::Unavailable, tr("No camera was found. Connect a camera; it appears here when the system reports it."));
        return;
    }
    const auto denied = tr("MoTrack is not allowed to use the camera. Allow camera access for MoTrack in the system privacy settings, "
                           "then open this window again.");
    switch (m_Source->access())
    {
    case CameraAccess::Granted:
        startCamera();
        break;
    case CameraAccess::Undetermined:
        if (m_State == State::WaitingForAccess)
        {
            setState(State::Unavailable, denied); // The request was answered without a decision.
            break;
        }
        setState(State::WaitingForAccess, tr("Waiting for permission to use the camera…"));
        m_Source->requestAccess();
        break;
    case CameraAccess::Denied:
        setState(State::Unavailable, denied);
        break;
    }
}

void CaptureDialog::startCamera()
{
    if (busy(m_State) || m_Camera->currentIndex() < 0)
        return;
    m_CameraActive = false;
    m_Preview->setImage({});
    setState(State::Starting, tr("Starting the camera…"));
    m_Source->start(m_Camera->currentData().toByteArray(), m_Format->currentData().toInt(), m_Pipeline->frameSink());
}

void CaptureDialog::toggleRecording()
{
    if (m_State == State::Recording)
    {
        m_Pipeline->stopRecording();
        setState(State::Finishing, tr("Finishing the recording…"));
        return;
    }
    if (m_State != State::Previewing)
        return;
    if (m_Pipeline->isRecording())
    {
        m_Status->setText(tr("The previous recording is still closing. Try again in a moment."));
        return;
    }
    m_Temporary = std::make_unique<QTemporaryDir>(QDir(QDir::tempPath()).filePath(QStringLiteral("motrack-capture-XXXXXX")));
    if (!m_Temporary->isValid())
    {
        m_Temporary.reset();
        m_Status->setText(tr("Could not create a temporary folder for the recording."));
        return;
    }
    m_Result.reset();
    (void)m_Pipeline->startRecording(temporaryRecording());
    setState(State::Recording);
    showProgress(0, 0, 0);
}

void CaptureDialog::discardRecording()
{
    if (m_State != State::Recorded)
        return;
    m_Result.reset();
    m_Temporary.reset();
    if (m_CameraActive)
        setState(State::Previewing);
    else
    {
        setState(State::Unavailable);
        checkAccess();
    }
}

void CaptureDialog::chooseDestination()
{
    if (m_State != State::Recorded)
        return;
    const QDir folder(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    const auto name = tr("Camera capture %1.mov").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")));
    auto* chooser = new QFileDialog(this, tr("Save recording"), folder.filePath(name), tr("QuickTime movies (*.mov)"));
    chooser->setObjectName("captureSaveDialog");
    chooser->setAcceptMode(QFileDialog::AcceptSave);
    chooser->setDefaultSuffix(QStringLiteral("mov"));
    chooser->setAttribute(Qt::WA_DeleteOnClose);
    connect(chooser, &QFileDialog::fileSelected, this, &CaptureDialog::saveRecording);
    chooser->open();
}

void CaptureDialog::saveRecording(const QString& path)
{
    if (m_State != State::Recorded || !m_Result || path.isEmpty())
        return;
    const auto source = m_Result->path;
    m_SaveCancelled = std::make_shared<std::atomic_bool>(false);
    setState(State::Saving, tr("Saving the recording…"));
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher,
            &QFutureWatcher<QString>::finished,
            this,
            [this, watcher, path]
            {
                const auto error = watcher->result();
                watcher->deleteLater();
                finishSaving(path, error);
            });
    m_Saving = QtConcurrent::run([source, path, cancelled = m_SaveCancelled] { return moveCaptureFile(source, path, *cancelled); });
    watcher->setFuture(m_Saving);
}

void CaptureDialog::finishSaving(const QString& path, const QString& error)
{
    if (m_Closed)
    {
        // Closed while saving: whatever reached the destination is the user's; nothing opens.
        m_Result.reset();
        m_Temporary.reset();
        return;
    }
    if (!error.isEmpty() || !m_Result)
    {
        setState(State::Recorded, tr("Could not save the recording: %1").arg(error));
        return;
    }
    auto result = *m_Result;
    result.path = QFileInfo(path).absoluteFilePath();
    m_Result.reset();
    m_Temporary.reset();
    m_Source->stop();
    m_CameraActive = false;
    const QPointer self(this);
    Q_EMIT recordingSaved(result.path, result);
    if (self)
        accept();
}

void CaptureDialog::reject()
{
    m_Closed = true;
    if (m_SaveCancelled)
        *m_SaveCancelled = true;
    m_Pipeline->cancelRecording();
    m_Source->stop();
    m_CameraActive = false;
    // An open movie file is removed once the pipeline reports it closed (or on destruction).
    if (!m_Pipeline->isRecording() && m_State != State::Saving)
    {
        m_Result.reset();
        m_Temporary.reset();
    }
    QDialog::reject();
}

void CaptureDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    m_Pipeline->setPreviewBound((m_Preview->size() * m_Preview->devicePixelRatioF()).expandedTo({160, 90}));
}

void CaptureDialog::showProgress(qint64 frames, qint64 dropped, qint64 duration)
{
    const auto seconds = static_cast<double>(duration) / 1e6;
    const auto minutes = static_cast<int>(seconds / 60);
    const auto clock =
        QStringLiteral("%1:%2").arg(minutes).arg(QLocale().toString(seconds - minutes * 60.0, 'f', 1).rightJustified(4, u'0'));
    auto text = tr("Time %1 · Frames %n", nullptr, countForPlural(frames)).arg(clock);
    if (dropped > 0)
        text += QStringLiteral(" · ") + tr("Dropped %n", nullptr, countForPlural(dropped));
    m_Elapsed->setText(text);
}

QString CaptureDialog::summary(const CaptureResult& result) const
{
    const QLocale locale;
    const auto seconds = static_cast<double>(result.duration.count()) / 1e6;
    QStringList text;
    if (result.frames > 1 && seconds > 0)
        text << tr("Recorded %n frames in %1 s, an average of %2 frames per second.", nullptr, countForPlural(result.frames))
                    .arg(locale.toString(seconds, 'f', 2), locale.toString(static_cast<double>(result.frames - 1) / seconds, 'f', 2));
    else
        text << tr("Recorded a single frame.");
    if (result.dropped > 0)
        text << tr("Camera frames dropped because they could not be recorded in time: %n. The clip keeps the real times of the "
                   "frames it contains.",
                   nullptr,
                   countForPlural(result.dropped));
    if (result.clock == CaptureClock::Arrival)
        text << tr("The camera supplied no frame times, so each frame is timed by when it reached MoTrack.");
    text << tr("Save the clip to open it as a new experiment.");
    return text.join(QLatin1Char(' '));
}

void CaptureDialog::setState(State state, const QString& message)
{
    m_State = state;
    m_Status->setText(message);
    const bool idle = !busy(state);
    m_Camera->setEnabled(idle && !m_Devices.isEmpty());
    m_Format->setEnabled(idle && m_Camera->currentIndex() >= 0);
    m_Record->setEnabled(state == State::Previewing || state == State::Recording);
    m_Record->setText(state == State::Recording ? tr("&Stop") : tr("&Record"));
    const bool recorded = state == State::Recorded || state == State::Saving;
    m_Save->setVisible(recorded);
    m_Save->setEnabled(state == State::Recorded);
    m_Discard->setVisible(recorded);
    m_Discard->setEnabled(state == State::Recorded);
    m_Close->setText(recorded ? tr("Discard and close") : tr("Cancel"));
    if (idle)
        m_Elapsed->clear();
}

} // namespace motrack
