#pragma once

#include "tmc/core/result.h"
#include "tmc/sharing/screen_quality.h"

#include <QObject>
#include <QVariantList>
#include <QVideoFrame>

#include <functional>
#include <memory>

namespace webrtc {

class VideoFrame;

}

namespace tmc {

class ScreenCaptureSource final : public QObject {
    Q_OBJECT

public:
    using FrameSink = std::function<void(const webrtc::VideoFrame&)>;
    explicit ScreenCaptureSource(QObject* parent = nullptr);
    ~ScreenCaptureSource() override;
    QVariantList sources();
    Result<void> start(int index, ScreenQuality quality, FrameSink sink);
    void setQuality(ScreenQuality quality);
    void stop();
    QString title() const;
signals:
    void firstFrame();
    void errorOccurred(QString error);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace tmc
