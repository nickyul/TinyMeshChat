#pragma once

#include "tmc/sharing/screen_quality.h"

#include <api/scoped_refptr.h>
#include <media/base/adapted_video_track_source.h>
#include <mutex>

namespace tmc {

class PeerScreenSource : public webrtc::AdaptedVideoTrackSource {
public:
    explicit PeerScreenSource(ScreenQuality quality = {});

    SourceState state() const override {
        return kLive;
    }

    bool remote() const override {
        return false;
    }

    bool is_screencast() const override {
        return true;
    }

    std::optional<bool> needs_denoising() const override {
        return false;
    }

    void setQuality(ScreenQuality quality);
    void push(const webrtc::VideoFrame& frame);

private:
    std::mutex mutex_;
    ScreenQuality quality_;
    int64_t nextFrameUs_{-1};
};

} // namespace tmc
