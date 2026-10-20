#pragma once
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <memory>
extern "C" {
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
}

class NvidiaVideo : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool neuralAvailable READ neuralAvailable NOTIFY changed)
    Q_PROPERTY(QString neuralAvailabilityReason READ neuralAvailabilityReason NOTIFY changed)
    Q_PROPERTY(bool fsr READ fsr WRITE setFsr NOTIFY changed)
    Q_PROPERTY(double finalSharpness READ finalSharpness WRITE setFinalSharpness NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool neural READ neural WRITE setNeural NOTIFY changed)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QVariantMap gpuTimings READ gpuTimings NOTIFY changed)
    Q_PROPERTY(double milliseconds READ milliseconds NOTIFY changed)
    Q_PROPERTY(double neuralBlend READ neuralBlend WRITE setNeuralBlend NOTIFY changed)
    Q_PROPERTY(bool frameGeneration READ frameGeneration WRITE setFrameGeneration NOTIFY changed)
    Q_PROPERTY(QString frameGenerationStatus READ frameGenerationStatus NOTIFY changed)
    Q_PROPERTY(int renderPreset READ renderPreset WRITE setRenderPreset NOTIFY changed)
    Q_PROPERTY(double dlssSharpness READ dlssSharpness WRITE setDlssSharpness NOTIFY changed)
    Q_PROPERTY(bool neuralBefore READ neuralBefore NOTIFY changed)
    Q_PROPERTY(QVariantMap neuralConfiguration READ neuralConfiguration NOTIFY changed)
public:
    explicit NvidiaVideo(QObject *parent=nullptr);
    ~NvidiaVideo();
    bool fsr() const;void setFsr(bool value);
    double finalSharpness() const;void setFinalSharpness(double value);
    bool enabled() const;
    bool hybrid() const;
    void setHybrid(bool value);
    bool neural() const;
    bool neuralAvailable() const;
    QString neuralAvailabilityReason() const;
    int quality() const;
    QString status() const;
    QVariantMap gpuTimings() const;
    double milliseconds() const;
    double neuralBlend() const;
    void setNeuralBlend(double value);
    void setEnabled(bool value);
    void setNeural(bool value);
    void setQuality(int value);
    bool frameGeneration() const;
    void setFrameGeneration(bool value);
    QString frameGenerationStatus() const;
    int renderPreset() const;
    void setRenderPreset(int value);
    double dlssSharpness() const;
    void setDlssSharpness(double value);
    bool neuralBefore() const;
    QVariantMap neuralConfiguration() const;
    Q_INVOKABLE bool saveNeuralConfiguration(double scale,int passes,int style,double intensity,bool before);
    void noteInputFrame();
    int nextPhaseDelayMs() const;
    bool render(pl_vulkan vk, pl_renderer renderer, const pl_frame *source,
                const pl_frame_mix *mix, const pl_frame *target, const pl_render_params *params, const pl_hook *spatialHook=nullptr,float sharpness=0.f);
    void shutdown();
    bool retainsDeviceUntilExit() const;
    static QByteArray sharpeningShader(double strength);
    static bool selfTest(const QString &reportPath);
signals:
    void changed();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
