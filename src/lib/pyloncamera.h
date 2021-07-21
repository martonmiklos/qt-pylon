#ifndef PYLON_CAMERA_H
#define PYLON_CAMERA_H

#include <QAbstractVideoSurface>
#include <QObject>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVector>

#include <pylon/ImageEventHandler.h>
#include <pylon/ConfigurationEventHandler.h>
#include <pylon/stdinclude.h>

namespace Pylon {
    class CInstantCamera;
    class CPylonImage;
    class CGrabResultPtr;
    class IPylonDevice;
}

class PylonCamera : public QObject,
        public Pylon::CImageEventHandler,
        public Pylon::CConfigurationEventHandler
{
    Q_OBJECT
    Q_PROPERTY(bool isOpen READ isOpen NOTIFY isOpenChanged)
    Q_PROPERTY(QString name READ name NOTIFY nameChanged)
    Q_PROPERTY(QAbstractVideoSurface *videoSurface READ videoSurface \
               WRITE setVideoSurface NOTIFY videoSurfaceChanged)

public:
    explicit PylonCamera(QObject *parent = nullptr);
    ~PylonCamera();

    virtual bool open(Pylon::IPylonDevice *pDevice = nullptr, bool saveConfig = true);
    void setVideoSurface(QAbstractVideoSurface *surface);
    QAbstractVideoSurface *videoSurface() const;
    QString name() const;
    bool isOpen() const;
    void close();
    void setConfig(const QString& configStr);

    Pylon::String_t originalConfig() const;

    QString deviceType() const;
    QString errorString() const;

    QString ipAddress() const;
    void setIpAddress(const QString &ipAddress);

    void setOutputLine(bool outputLine);

    void setHardwareTriggerEnabled(bool hwTriggerEnabled);

    QString serialNumber() const;
    void setSerialNumber(const QString &serialNumber);

    void loadUserDataSet(const QString &setName);

signals:
    void isOpenChanged();
    void nameChanged();
    void videoSurfaceChanged();
    void captured(const QVector<QImage> &imgs);
    void grabbingStarted();
    void grabbingStopped();

    // Internal usage only
    // frame will be in Qimge::Format_RGB32
    void frameGrabbedInternal(const QImage &frame);
    void cameraRemovedInternal();

public slots:
    bool start(bool saveConfig = true);
    void stop();
    bool isGrabbing() const;
    virtual bool capture(int n = 1, const QString &config = QString(), bool keepGrabbing = false);

private slots:
    void renderFrame(const QImage &frame);
    void handleCameraRemoved();

private:
    // from Pylon::CImageEventHandler
    // FIXME Move to p_impl??
    virtual void OnImageGrabbed(Pylon::CInstantCamera& camera,
                                const Pylon::CGrabResultPtr& ptrGrab);

    // from Pylon::CConfigurationEventHandler
    // FIXME Move to p_impl??
    virtual void OnCameraDeviceRemoved(Pylon::CInstantCamera&);

    bool startGrabbing();
    void stopGrabbing();
    void setName(const char *name);

    QVector<Pylon::CPylonImage> grabImage(int n = 1, bool keepGrabbing = false);
    QImage toQImage(Pylon::CPylonImage &pylonImage);
    void restoreOriginalConfig();

    QAbstractVideoSurface *m_surface = nullptr;
    Pylon::CInstantCamera *m_camera = nullptr;
    bool m_startRequested = false;
    QString m_name, m_deviceType, m_errorString, m_ipAddress, m_serialNumber;
    Pylon::String_t m_config;
    Pylon::String_t m_originalConfig;
};

#endif // PYLON_CAMERA_H
