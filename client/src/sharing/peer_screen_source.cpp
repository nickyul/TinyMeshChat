#include "peer_screen_source.h"

#include <api/video/i420_buffer.h>

namespace tmc {

PeerScreenSource::PeerScreenSource(ScreenQuality quality)
    : AdaptedVideoTrackSource(2), quality_(quality) {
}

void PeerScreenSource::setQuality(ScreenQuality quality) {
    std::lock_guard lock(mutex_);
    if (quality_.fps != quality.fps) {
        nextFrameUs_ = -1;
    }
    quality_ = quality;
}

void PeerScreenSource::push(const webrtc::VideoFrame& frame) {
    ScreenQuality quality;
    {
        std::lock_guard lock(mutex_);
        quality = quality_;
        // Keep a cadence rather than resetting the deadline to each arrival. A little
        // scheduling jitter around 60 Hz must not turn an otherwise healthy stream into 30 Hz.
        const auto timestamp = frame.timestamp_us();
        const int64_t interval = 1000000 / quality.fps;
        if (nextFrameUs_ >= 0 && timestamp + 1000 < nextFrameUs_) {
            return;
        }
        if (nextFrameUs_ < 0 || timestamp - nextFrameUs_ > interval) {
            nextFrameUs_ = timestamp;
        }
        nextFrameUs_ += interval;
    }
    const auto maximum = quality.fittedSize({frame.width(), frame.height()});
    int width, height, cropWidth, cropHeight, cropX, cropY;
    if (!AdaptFrame(maximum.width(), maximum.height(), frame.timestamp_us(), &width, &height,
                    &cropWidth, &cropHeight, &cropX, &cropY)) {
        return;
    }
    // One adapter per sender. No sink wants are aggregated across viewers.
    const auto raw = frame.video_frame_buffer()->ToI420();
    if (!raw) {
        return;
    }
    const auto adapted = webrtc::I420Buffer::Create(width, height);
    adapted->ScaleFrom(*raw);
    OnFrame(webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(adapted)
                .set_timestamp_us(frame.timestamp_us())
                .set_rotation(webrtc::kVideoRotation_0)
                .build());
}

} // namespace tmc
