#include "tmc/app/voice_session.h"

#include "tmc/audio/rtp_audio_engine.h"

#include <utility>

namespace tmc {

VoiceSession::VoiceSession(AudioPreferences preferences, QObject* parent)
    : QObject(parent), audio_(std::make_unique<RtpAudioEngine>(std::move(preferences))) {
    connect(audio_.get(), &RtpAudioEngine::errorOccurred, this, &VoiceSession::errorOccurred);
    connect(audio_.get(), &RtpAudioEngine::networkStatsChanged, this,
            &VoiceSession::networkStatsChanged);
    connect(audio_.get(), &RtpAudioEngine::talkingStateChanged, this,
            &VoiceSession::talkingStateChanged);
    connect(audio_.get(), &RtpAudioEngine::peerTalkingStateChanged, this,
            &VoiceSession::peerTalkingStateChanged);
}

VoiceSession::~VoiceSession() = default;

bool VoiceSession::active() const {
    return active_;
}

bool VoiceSession::muted() const {
    return muted_;
}

bool VoiceSession::deafened() const {
    return deafened_;
}

bool VoiceSession::microphoneTest() const {
    return microphoneTest_;
}

double VoiceSession::microphoneLevel() const {
    return audio_->microphoneLevel();
}

AudioPreferences VoiceSession::preferences() const {
    return audio_->preferences();
}

std::shared_ptr<IncomingRtpAudioSink> VoiceSession::incomingAudioSink() const {
    return audio_->incomingSink();
}

void VoiceSession::setOutgoingAudioSink(std::weak_ptr<OutgoingOpusAudioSink> sink) {
    audio_->setOutgoingSink(std::move(sink));
}

Result<void> VoiceSession::start() {
    if (active_) {
        return Result<void>::success();
    }
    const auto started = audio_->start();
    if (!started) {
        return started;
    }
    active_ = true;
    muted_ = false;
    audio_->setPttPressed(false);
    audio_->setMuted(false);
    emit stateChanged(true, false);
    return Result<void>::success();
}

void VoiceSession::leave() {
    if (!active_ && !audio_->isRunning()) {
        return;
    }
    active_ = false;
    muted_ = false;
    audio_->setPttPressed(false);
    if (!microphoneTest_) {
        audio_->stop();
    }
    emit stateChanged(false, false);
}

void VoiceSession::setMuted(bool muted) {
    if (!active_ || muted_ == muted) {
        return;
    }
    muted_ = muted;
    if (muted_) {
        audio_->setPttPressed(false);
    }
    audio_->setMuted(muted);
    emit stateChanged(true, muted_);
}

void VoiceSession::setDeafened(bool deafened) {
    deafened_ = deafened;
    audio_->setDeafened(deafened);
}

void VoiceSession::setMicrophoneTest(bool enabled) {
    if (microphoneTest_ == enabled) {
        return;
    }
    if (enabled && !audio_->isRunning()) {
        const auto started = audio_->start();
        if (!started) {
            emit errorOccurred(started.error());
            return;
        }
    }
    microphoneTest_ = enabled;
    audio_->setPttPressed(false);
    audio_->setMicrophoneTest(enabled);

    if (!enabled && !active_) {
        audio_->stop();
    }
}

void VoiceSession::setPttPressed(bool pressed) {
    audio_->setPttPressed(active_ && !muted_ && !microphoneTest_ && pressed);
}

void VoiceSession::setPeerVolume(const QString& peerId, int percent) {
    audio_->setPeerVolume(peerId, percent);
}

void VoiceSession::updateNetworkFeedback(const QString& peerId, double packetLossPercent) {
    audio_->setPeerNetworkLoss(peerId, packetLossPercent);
}

Result<void> VoiceSession::applyPreferences(const AudioPreferences& preferences) {
    return audio_->applyPreferences(preferences);
}

QPair<QStringList, QStringList> VoiceSession::refreshDevices() {
    const auto devices = audio_->refreshDevices();
    return {devices.capture, devices.playback};
}

void VoiceSession::clear() {
    microphoneTest_ = false;
    audio_->setPttPressed(false);
    audio_->setMicrophoneTest(false);
    leave();
    audio_->stop();
    const auto peerIds = peers_.keys();
    peers_.clear();
    for (const auto& peerId : peerIds) {
        audio_->removePeer(peerId);
        emit peerChanged(peerId, false, false);
    }
}

void VoiceSession::updatePeer(const QString& peerId, bool joined, bool muted) {
    if (peerId.isEmpty()) {
        return;
    }
    peers_[peerId] = {joined, muted};
    if (!joined) {
        audio_->removePeer(peerId);
    }
    emit peerChanged(peerId, joined, muted);
}

void VoiceSession::removePeer(const QString& peerId) {
    if (peerId.isEmpty()) {
        return;
    }
    peers_.remove(peerId);
    audio_->removePeer(peerId);
    emit peerChanged(peerId, false, false);
}

} // namespace tmc
