#pragma once

#include "tmc/audio/shared_audio_output.h"
#include "tmc/core/result.h"
#include "tmc/network/relay_server.h"

#include <QStringList>

#include <api/peer_connection_interface.h>
#include <memory>
#include <rtc_base/thread.h>

#include "system_audio_device.h"

namespace webrtc {

class LogSink;

}

namespace tmc {

class ScreenMediaEngine final {
public:
    explicit ScreenMediaEngine(std::shared_ptr<SharedAudioOutput> output);
    ~ScreenMediaEngine();
    Result<void> initialize();
    webrtc::PeerConnectionInterface::RTCConfiguration
    configuration(const QStringList& stun, const QList<RelayServer>& turn) const;

    webrtc::Thread* signaling() const {
        return signaling_.get();
    }

    webrtc::Thread* worker() const {
        return worker_.get();
    }

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory() const {
        return factory_;
    }

    webrtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource() const {
        return audioSource_;
    }

    void pushAudio(const float* stereo, size_t frames);
    void clearAudio();

private:
    std::shared_ptr<SharedAudioOutput> output_;
    std::unique_ptr<webrtc::Thread> network_, worker_, signaling_;
    std::unique_ptr<webrtc::LogSink> videoLogs_;
    webrtc::scoped_refptr<SystemAudioDevice> audioDevice_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource_;
};

} // namespace tmc
