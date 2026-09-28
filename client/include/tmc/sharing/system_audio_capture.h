#pragma once
#include "tmc/core/result.h"
#include <QByteArray>
#include <QObject>
#include <functional>
#include <memory>

namespace tmc {
class SystemAudioCapture final : public QObject {
    Q_OBJECT
public:
    explicit SystemAudioCapture(QObject* parent = nullptr);
    ~SystemAudioCapture() override;
    Result<void> start();
    void stop();
    static bool supported();
signals:
    void samples(QByteArray stereoFloat48k);
    void errorOccurred(QString message);
private:
    using Sink = std::function<void(const float*, int)>;
    struct Delivery;
    struct Native;
    Sink makeSink();
    std::function<void(QString)> makeErrorSink();
    void invalidateSink();
    std::shared_ptr<Delivery> delivery_;
    std::unique_ptr<Native> native_;
};
} // namespace tmc
