#pragma once

#include "capture_pipeline.hpp"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

class QVideoFrame;

namespace motrack
{

/// A capture mode a camera reports: frame size, frame-rate range and the pixel layout it delivers.
struct CameraFormat
{
    QSize resolution;
    double minimumFrameRate = 0;
    double maximumFrameRate = 0;
    QString pixelFormat;

    friend bool operator==(const CameraFormat&, const CameraFormat&) = default;
};

struct CameraDevice
{
    QByteArray id;
    QString description;
    bool isDefault = false;
    QList<CameraFormat> formats;
};

enum class CameraAccess : std::uint8_t
{
    Granted,
    Undetermined,
    Denied,
};

/// The camera side of capture, behind an interface so tests can supply frames without hardware.
/// Lives on the GUI thread. Frames reach the sink passed to start() on whatever thread the platform
/// delivers them (Qt 6.8's backends use the GUI thread); the sink must be thread-safe. stop() and
/// destruction stop delivery to new frames, but a frame already being delivered may still reach the sink.
/// failed() also reports a camera that stops delivering on its own (unplugged, for example).
class CameraSource : public QObject
{
    Q_OBJECT

  public:
    using FrameSink = std::function<void(const QVideoFrame&)>;

    using QObject::QObject;
    [[nodiscard]] virtual QList<CameraDevice> devices() const = 0;
    [[nodiscard]] virtual CameraAccess access() const = 0;
    /// Asks the platform for camera access (macOS prompts once); accessChanged follows.
    virtual void requestAccess() = 0;
    /// Starts the camera with one of its formats (an index into CameraDevice::formats, or -1 for the
    /// camera's own choice); started or failed follows.
    virtual void start(const QByteArray& device, int format, FrameSink sink) = 0;
    virtual void stop() = 0;
    /// Which frame field holds each frame's own presentation time on this platform.
    [[nodiscard]] virtual FrameTimeField frameTimeField() const
    {
        return FrameTimeField::StartTime;
    }

  Q_SIGNALS:
    void devicesChanged();
    void accessChanged();
    void started();
    void failed(const QString& message);
};

/// Qt Multimedia's cameras: Media Foundation on Windows, AVFoundation on macOS and V4L2 on Linux,
/// through Qt's FFmpeg media backend. Opens a camera only in start().
class QtCameraSource final : public CameraSource
{
    Q_OBJECT

  public:
    QtCameraSource();
    ~QtCameraSource() override;
    [[nodiscard]] QList<CameraDevice> devices() const override;
    [[nodiscard]] CameraAccess access() const override;
    void requestAccess() override;
    void start(const QByteArray& device, int format, FrameSink sink) override;
    void stop() override;
    /// EndTime with Qt 6.8 to 6.11's macOS camera, whose startTime is the previous frame's time.
    [[nodiscard]] FrameTimeField frameTimeField() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace motrack
