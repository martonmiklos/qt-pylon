#include "pyloncamera.h"

#include <QDateTime>
#include <QDebug>
#include <QImage>
#include <QVideoFrame>
#include <QVideoSurfaceFormat>
#include <QtConcurrent/QtConcurrentRun>

#include <pylon/PylonIncludes.h>
#include <pylon/EnumParameter.h>
#include <pylon/BooleanParameter.h>

using Pylon::CEnumParameter;
using Pylon::CBooleanParameter;
using Pylon::CDeviceInfo;
using Pylon::CFeaturePersistence;
using Pylon::CGrabResultPtr;
using Pylon::CImageEventHandler;
using Pylon::CImageFormatConverter;
using Pylon::CInstantCamera;
using Pylon::CPylonImage;
using Pylon::CTlFactory;
using Pylon::Cleanup_None;
using Pylon::GrabLoop_ProvidedByInstantCamera;
using Pylon::GrabStrategy_OneByOne;
using Pylon::PixelType_RGB8packed;
using Pylon::PylonInitialize;
using Pylon::PylonTerminate;
using Pylon::RegistrationMode_ReplaceAll;
using Pylon::String_t;
using Pylon::TimeoutHandling_Return;

static long _frame_counter = 0;

PylonCamera::PylonCamera(QObject *parent) :
    QObject(parent)
{
    qRegisterMetaType<QVector<QImage> >("QVector<QImage>");
    PylonInitialize();
}

PylonCamera::~PylonCamera()
{
    stop();
    close();

    disconnect(this, &PylonCamera::cameraRemovedInternal, this, &PylonCamera::handleCameraRemoved);
    if (m_camera) {
        m_camera->Close();
        m_camera->DestroyDevice();
        delete m_camera;
        m_camera = nullptr;
    }

    PylonTerminate();
}

QString PylonCamera::name() const
{
    return m_name;
}

void PylonCamera::setName(const char *name)
{
    m_name = QString(name);
    emit nameChanged();
}

void PylonCamera::setConfig(const QString& configStr)
{
    m_config = configStr.toStdString().c_str();
    qDebug() << "Using default custom config: " << m_config.c_str();
}

QAbstractVideoSurface *PylonCamera::videoSurface() const
{
    return m_surface;
}

void PylonCamera::setVideoSurface(QAbstractVideoSurface *surface)
{
    m_surface = surface;
    emit videoSurfaceChanged();

    if (m_startRequested) {
        start();
    } else {
        stop();
    }
}

bool PylonCamera::isOpen() const
{
    return m_camera != nullptr && m_camera->IsOpen();
}

void PylonCamera::close()
{
    if (isOpen()) {
        if (m_camera->IsGrabbing())
            m_camera->StopGrabbing();
        m_camera->Close();
        m_camera->DeregisterImageEventHandler(this);
        emit isOpenChanged();
    }
}

bool PylonCamera::open(Pylon::IPylonDevice* pDevice, bool saveConfig)
{
    if (isOpen())
        return true;

    try {
        Pylon::CDeviceInfo di;
        if (!m_ipAddress.isEmpty())
            di.SetIpAddress(m_ipAddress.toLocal8Bit().constData());
        if (!m_serialNumber.isEmpty())
            di.SetSerialNumber(m_serialNumber.toLocal8Bit().constData());
        CTlFactory& TlFactory = CTlFactory::GetInstance();
        Pylon::DeviceInfoList_t lstDevices;
        TlFactory.EnumerateDevices(lstDevices);
        if (lstDevices.empty())
            return false;

        bool found = false;
        for (auto & cdi : lstDevices) {
            if ((!m_ipAddress.isEmpty() && cdi.GetIpAddress() == di.GetIpAddress())
                    || (!m_serialNumber.isEmpty() && cdi.GetSerialNumber() == di.GetSerialNumber())) {
                di = cdi;
                found = true;
                break;
            }
        }

        if (!found)
            return false;

        if (pDevice == nullptr)
            pDevice = CTlFactory::GetInstance().CreateDevice(di);
        m_camera = new CInstantCamera(pDevice);
        setName(m_camera->GetDeviceInfo().GetUserDefinedName().c_str());
        qDebug() << "Opening device" << m_name << "..";
        m_deviceType = m_camera->GetDeviceInfo().GetModelName().c_str();

        m_camera->Open();

        if (saveConfig) {
            CFeaturePersistence::SaveToString(m_originalConfig, &m_camera->GetNodeMap());
            if (m_config.empty()) {
                CFeaturePersistence::SaveToString(m_config, &m_camera->GetNodeMap());
                qDebug() << "Saved original config: ( size:" << m_config.size() << " )";
            }
        }

        connect(this, &PylonCamera::cameraRemovedInternal, this, &PylonCamera::handleCameraRemoved);
        m_camera->RegisterImageEventHandler(this, RegistrationMode_ReplaceAll, Cleanup_None);

        emit isOpenChanged();
        return true;
    }
    catch (GenICam::GenericException &e) {
        if (m_camera->IsOpen())
            m_camera->Close();
        m_camera = nullptr;
        qWarning() << "Camera Error: " << e.GetDescription();
        m_errorString = e.GetDescription();
    }
    return false;
}

void PylonCamera::stop()
{
    if (!isOpen())
        return;

    stopGrabbing();
    m_startRequested = false;
}

bool PylonCamera::start(bool saveConfig)
{
    m_startRequested = true;
    open(nullptr, saveConfig);

    if (!isOpen()) {
        qWarning() << "Failed to open camera!";
        return false;
    }

    if (m_camera->IsGrabbing()) {
        qWarning() << "Camera already started!";
        return true;
    }

    if (!m_surface) {
        qWarning() << "Surface not set. Start pending.";
        return true;
    }

    try {
        restoreOriginalConfig();

        auto v = grabImage();
        CPylonImage img = v.first();
        if (!img.IsValid()) {
            qWarning() << "Failed to get camera format metadata!";
            return false;
        }

        QSize size(img.GetWidth(), img.GetHeight());
        QVideoFrame::PixelFormat f = QVideoFrame::pixelFormatFromImageFormat(QImage::Format_RGB32);
        QVideoSurfaceFormat format(size, f);
        if (m_surface->start(format)) {
            QImage qimage = toQImage(img).convertToFormat(QImage::Format_RGB32);
            m_surface->present(qimage);
        } else {
            qWarning() << "Failed to start videoSurface" << m_surface->error();
        }
    }
    catch (GenICam::GenericException &e) {
        //m_camera = nullptr;
        qWarning() << "Camera Error: " << e.GetDescription();
        m_errorString = e.GetDescription();
        return false;
    }

    return startGrabbing();
}

bool PylonCamera::capture(int nFrames, const QString &config, bool keepGrabbing)
{
    if (!isOpen()) {
        qWarning() << "Failed to capture: Camera not open!";
        return false;
    }

    //stopGrabbing();

    if (!config.isEmpty()) {
        try {
            qDebug() << "Configuring camera [ config.size=" << config.size() << "]";
            String_t strconfig(config.toStdString().c_str());
            CFeaturePersistence::LoadFromString(strconfig, &m_camera->GetNodeMap(), true);
        }
        catch (GenICam::GenericException &e) {
            qWarning() << "Failed to config camera: " << e.GetDescription();
            restoreOriginalConfig();
            return false;
        }
    }

    if (nFrames == 0) {
        auto v = grabImage(nFrames, keepGrabbing).first();
        auto img = PylonCamera::toQImage(v);
        emit frameGrabbedInternal(img);
        QVector<QImage> vect;
        vect << img;
        if (m_surface && !m_surface->isActive()) {
            QVideoFrame::PixelFormat f = QVideoFrame::pixelFormatFromImageFormat(QImage::Format_RGB32);
            QVideoSurfaceFormat format(img.size(), f);
            m_surface->start(format);
        }
        renderFrame(img.convertToFormat(QImage::Format_RGB32));
        emit captured(vect);
    } else {
        QtConcurrent::run([this, nFrames]() {
            auto framesLeft = nFrames;
            while (framesLeft) {
                auto v = grabImage(nFrames);
                if (v.size()) {
                    QVector<QImage> images(v.size());
                    framesLeft -= v.size();

                    for(int i = 0; i < v.size(); ++i) {
                        images[i] = PylonCamera::toQImage(v[i]);
                    }
                    emit frameGrabbedInternal(images.last());
                    emit captured(images);
                }
            }
        });
    }

    return true;
}

bool PylonCamera::startGrabbing()
{
    if (!isOpen())
        throw std::runtime_error("Camera failed to initialize");

    connect(this, &PylonCamera::frameGrabbedInternal, this, &PylonCamera::renderFrame);
    try {
        m_camera->RegisterConfiguration(this, RegistrationMode_ReplaceAll, Cleanup_None);
        m_camera->StartGrabbing(GrabStrategy_OneByOne, GrabLoop_ProvidedByInstantCamera);
        emit grabbingStarted();
    }   catch (GenICam::GenericException &e) {
        qWarning() << "Camera Error: " << e.GetDescription();
        m_errorString = e.GetDescription();
        return false;
    }
    return true;
}

void PylonCamera::stopGrabbing()
{
    if (!isOpen())
        return;

    disconnect(this, &PylonCamera::frameGrabbedInternal, this, &PylonCamera::renderFrame);
    m_camera->DeregisterConfiguration(this);

    if (m_camera->IsGrabbing())
        m_camera->StopGrabbing();
    emit grabbingStopped();
}

bool PylonCamera::isGrabbing() const
{
    if (!m_camera)
        return false;
    return m_camera->IsGrabbing();
}

void PylonCamera::OnImageGrabbed(CInstantCamera& camera, const CGrabResultPtr& ptrGrab)
{
    Q_UNUSED(camera)
    CImageFormatConverter fc;
    fc.OutputPixelFormat = PixelType_RGB8packed;

    CPylonImage pylonImage;

    fc.Convert(pylonImage, ptrGrab);
    if (!pylonImage.IsValid()) {
        qWarning() << "failed to convert frame" << _frame_counter;
        return;
    }

    QImage qimage = toQImage(pylonImage).convertToFormat(QImage::Format_RGB32);
    emit frameGrabbedInternal(qimage);
}

void PylonCamera::OnCameraDeviceRemoved( CInstantCamera& /*camera*/)
{
    qDebug() << "camera removed!";
    emit cameraRemovedInternal();
}

void PylonCamera::handleCameraRemoved()
{
    if (!isOpen())
        return;

    stopGrabbing();
    m_startRequested = false;

    disconnect(this, &PylonCamera::cameraRemovedInternal, this, &PylonCamera::handleCameraRemoved);
    m_camera->DeregisterImageEventHandler(this);

    m_camera->Close();
    m_camera->DestroyDevice();
    delete m_camera;
    m_camera = nullptr;

    emit isOpenChanged();
}

QImage PylonCamera::toQImage(CPylonImage &pylonImage)
{
    int width = pylonImage.GetWidth();
    int height = pylonImage.GetHeight();
    void *buffer = pylonImage.GetBuffer();
    int step = pylonImage.GetAllocatedBufferSize() / height;
    QImage img(static_cast<uchar*>(buffer), width, height, step, QImage::Format_RGB888);
    return img;
}

void PylonCamera::renderFrame(const QImage &img)
{
    if (!m_surface)
        return;

    QVideoFrame frame(img);
    bool r = m_surface->present(frame);
    if (!r)
        qDebug() << m_surface->error();
}

QVector<CPylonImage> PylonCamera::grabImage(int nFrames, bool keepGrabbing)
{
    if (!isOpen())
        throw std::runtime_error("Camera failed to initialize");

    QVector<CPylonImage> images;
    CImageFormatConverter fc;
    fc.OutputPixelFormat = PixelType_RGB8packed;

    CGrabResultPtr ptrGrab;

    if (!m_camera->IsGrabbing())
        m_camera->StartGrabbing(nFrames);

    qWarning() << "started grabbing";
    while(m_camera->IsGrabbing()){
        CPylonImage image;
        m_camera->RetrieveResult(10000000, ptrGrab, TimeoutHandling_Return);
        qWarning() << "result retrived";
        if (ptrGrab->GrabSucceeded()) {
            qWarning() << "grab succeeded";
            fc.Convert(image, ptrGrab);
            if (!keepGrabbing)
                m_camera->StopGrabbing();
            images += image;
        } else {
            qWarning() << "grab unsuccessful";
        }
    }

    return images;
}

void PylonCamera::restoreOriginalConfig()
{
    if (m_originalConfig.size()) {
        qDebug() << "Restoring original camera config [ config.size="
                 << m_originalConfig.size() << "]";
        CFeaturePersistence::LoadFromString(m_originalConfig, &m_camera->GetNodeMap());
    }
}

QString PylonCamera::serialNumber() const
{
    return m_serialNumber;
}

void PylonCamera::setSerialNumber(const QString &serialNumber)
{
    m_serialNumber = serialNumber;
}

QString PylonCamera::ipAddress() const
{
    return m_ipAddress;
}

void PylonCamera::setIpAddress(const QString &ipAddress)
{
    m_ipAddress = ipAddress;
}

void PylonCamera::setOutputLine(bool outputLine)
{
    try {
        auto & nodemap = m_camera->GetNodeMap();
        CEnumParameter(nodemap, "LineSelector").SetValue("Out1");
        CBooleanParameter(nodemap, "UserOutputValue").SetValue(outputLine);
    } catch (GenICam::GenericException &e) {
        qWarning() << e.what();
    }
}

void PylonCamera::setHardwareTriggerEnabled(bool hwTriggerEnabled)
{
    auto& nodemap = m_camera->GetNodeMap();
    // Select the Frame Start trigger
    CEnumParameter(nodemap, "TriggerSelector").SetValue("FrameStart");
    // Enable triggered image acquisition for the Frame Start trigger
    CEnumParameter(nodemap, "TriggerMode").SetValue(hwTriggerEnabled ? "On" : "Off");
}

QString PylonCamera::errorString() const
{
    return m_errorString;
}

QString PylonCamera::deviceType() const
{
    return m_deviceType;
}

Pylon::String_t PylonCamera::originalConfig() const
{
    return m_originalConfig;
}
