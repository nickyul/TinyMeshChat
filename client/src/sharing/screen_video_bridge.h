#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QVideoFrame>
#include <QVideoSink>

#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <memory>
#include <mutex>

namespace tmc {

class ScreenVideoBridge final : public QObject,
                                public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit ScreenVideoBridge(QObject* parent = nullptr);
    void addSink(QVideoSink* sink);
    void removeSink(QVideoSink* sink);
    void deliver(QVideoFrame frame);
    void clear();
    void OnFrame(const webrtc::VideoFrame& frame) override;

private:
    struct Delivery;
    std::shared_ptr<Delivery> delivery_;
    QList<QPointer<QVideoSink>> sinks_;
    QVideoFrame latest_;
};

} // namespace tmc
