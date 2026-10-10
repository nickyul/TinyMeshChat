#include "screen_video_bridge.h"

#include <QVideoFrameFormat>

#include <api/video/i420_buffer.h>
#include <cstring>

namespace tmc {

struct ScreenVideoBridge::Delivery {
    std::mutex mutex;
    QVideoFrame pending;
    bool queued{false};
};

ScreenVideoBridge::ScreenVideoBridge(QObject* parent)
    : QObject(parent), delivery_(std::make_shared<Delivery>()) {
}

void ScreenVideoBridge::addSink(QVideoSink* sink) {
    if (!sink) {
        return;
    }
    if (!sinks_.contains(sink)) {
        sinks_.append(sink);
    }
    sink->setVideoFrame(latest_);
}

void ScreenVideoBridge::removeSink(QVideoSink* sink) {
    sinks_.removeAll(sink);
}

void ScreenVideoBridge::clear() {
    deliver({});
}

void ScreenVideoBridge::deliver(QVideoFrame frame) {
    const auto delivery = delivery_;
    {
        std::lock_guard lock(delivery->mutex);
        delivery->pending = std::move(frame);
        if (delivery->queued) {
            return;
        }
        delivery->queued = true;
    }
    QMetaObject::invokeMethod(
        this,
        [this, delivery] {
            {
                std::lock_guard lock(delivery->mutex);
                latest_ = std::move(delivery->pending);
                delivery->queued = false;
            }
            for (auto it = sinks_.begin(); it != sinks_.end();) {
                if (!*it) {
                    it = sinks_.erase(it);
                } else {
                    (*it)->setVideoFrame(latest_);
                    ++it;
                }
            }
        },
        Qt::QueuedConnection);
}

void ScreenVideoBridge::OnFrame(const webrtc::VideoFrame& frame) {
    auto raw = frame.video_frame_buffer()->ToI420();
    if (!raw) {
        return;
    }
    if (frame.rotation() != webrtc::kVideoRotation_0) {
        raw = webrtc::I420Buffer::Rotate(*raw, frame.rotation());
    }
    QVideoFrame result(
        QVideoFrameFormat({raw->width(), raw->height()}, QVideoFrameFormat::Format_YUV420P));
    if (!result.map(QVideoFrame::WriteOnly)) {
        return;
    }
    const uint8_t* planes[]{raw->DataY(), raw->DataU(), raw->DataV()};
    const int strides[]{raw->StrideY(), raw->StrideU(), raw->StrideV()};
    for (int plane = 0; plane < 3; ++plane) {
        const int rows = plane == 0 ? raw->height() : (raw->height() + 1) / 2;
        const int columns = plane == 0 ? raw->width() : (raw->width() + 1) / 2;
        for (int row = 0; row < rows; ++row) {
            std::memcpy(result.bits(plane) + row * result.bytesPerLine(plane),
                        planes[plane] + row * strides[plane], columns);
        }
    }
    result.unmap();
    deliver(std::move(result));
}

} // namespace tmc
