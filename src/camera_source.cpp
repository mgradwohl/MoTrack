#include "camera_source.hpp"

#include <QCamera>
#include <QCameraDevice>
#include <QCoreApplication>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QPermissions>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <algorithm>
#include <utility>

namespace motrack
{
namespace
{

QString pixelFormatName(QVideoFrameFormat::PixelFormat format)
{
    if (format == QVideoFrameFormat::Format_Jpeg)
        return QStringLiteral("MJPEG");
    return QVideoFrameFormat::pixelFormatToString(format).remove(QStringLiteral("Format_"));
}

CameraDevice describe(const QCameraDevice& device)
{
    CameraDevice result{.id = device.id(), .description = device.description(), .isDefault = device.isDefault(), .formats = {}};
    for (const auto& format : device.videoFormats())
        result.formats.push_back({.resolution = format.resolution(),
                                  .minimumFrameRate = format.minFrameRate(),
                                  .maximumFrameRate = format.maxFrameRate(),
                                  .pixelFormat = pixelFormatName(format.pixelFormat())});
    return result;
}

} // namespace

// Destruction order matters: the camera leaves the session first, then the session releases the sink.
struct QtCameraSource::Impl
{
    QMediaDevices devices;
    QVideoSink sink;
    QMediaCaptureSession session;
    std::unique_ptr<QCamera> camera;
    QMetaObject::Connection frames;
};

QtCameraSource::QtCameraSource() : m_Impl(std::make_unique<Impl>())
{
    connect(&m_Impl->devices, &QMediaDevices::videoInputsChanged, this, &CameraSource::devicesChanged);
    m_Impl->session.setVideoSink(&m_Impl->sink);
}

QtCameraSource::~QtCameraSource()
{
    stop();
}

QList<CameraDevice> QtCameraSource::devices() const
{
    QList<CameraDevice> result;
    for (const auto& device : QMediaDevices::videoInputs())
        result.push_back(describe(device));
    return result;
}

CameraAccess QtCameraSource::access() const
{
#if QT_CONFIG(permissions)
    switch (QCoreApplication::instance()->checkPermission(QCameraPermission{}))
    {
    case Qt::PermissionStatus::Undetermined:
        return CameraAccess::Undetermined;
    case Qt::PermissionStatus::Denied:
        return CameraAccess::Denied;
    case Qt::PermissionStatus::Granted:
        break;
    }
#endif
    return CameraAccess::Granted;
}

void QtCameraSource::requestAccess()
{
#if QT_CONFIG(permissions)
    QCoreApplication::instance()->requestPermission(QCameraPermission{}, this, [this](const QPermission&) { Q_EMIT accessChanged(); });
#else
    Q_EMIT accessChanged();
#endif
}

void QtCameraSource::start(const QByteArray& device, int format, FrameSink sink)
{
    stop();
    const auto inputs = QMediaDevices::videoInputs();
    const auto found = std::ranges::find(inputs, device, &QCameraDevice::id);
    if (found == inputs.end())
    {
        // An explicit context: lupdate does not resolve this class from its header.
        Q_EMIT failed(QCoreApplication::translate("motrack::QtCameraSource", "The camera is no longer connected."));
        return;
    }
    m_Impl->camera = std::make_unique<QCamera>(*found);
    const auto formats = found->videoFormats();
    if (format >= 0 && format < formats.size())
        m_Impl->camera->setCameraFormat(formats.at(format));
    connect(m_Impl->camera.get(),
            &QCamera::activeChanged,
            this,
            [this](bool active)
            {
                if (active)
                    Q_EMIT started();
            });
    connect(
        m_Impl->camera.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString& message) { Q_EMIT failed(message); });
    // Frames arrive on the platform's capture thread; the sink queues them without touching widgets.
    m_Impl->frames = connect(
        &m_Impl->sink,
        &QVideoSink::videoFrameChanged,
        this,
        [sink = std::move(sink)](const QVideoFrame& frame) { sink(frame); },
        Qt::DirectConnection);
    m_Impl->session.setCamera(m_Impl->camera.get());
    m_Impl->camera->start();
}

void QtCameraSource::stop()
{
    QObject::disconnect(m_Impl->frames);
    if (m_Impl->camera)
    {
        m_Impl->camera->stop();
        m_Impl->session.setCamera(nullptr);
        m_Impl->camera.reset();
    }
}

} // namespace motrack
