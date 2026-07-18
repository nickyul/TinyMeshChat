#include "tmc/app/voice_session.h"

#include "tmc/audio/audio_engine.h"

#include <utility>

namespace tmc {

VoiceSession::VoiceSession(AudioPreferences preferences, QObject* parent)
    : QObject(parent), audio_(std::make_unique<AudioEngine>(std::move(preferences))) {
    connect(audio_.get(), &AudioEngine::encodedFrameReady, this, &VoiceSession::encodedFrameReady);
    connect(audio_.get(), &AudioEngine::errorOccurred, this, &VoiceSession::errorOccurred);
    connect(audio_.get(), &AudioEngine::microphoneLevelChanged, this,
            &VoiceSession::microphoneLevelChanged);
    connect(audio_.get(), &AudioEngine::microphoneTestPlaybackFinished, this, [this] {
        if (!active_ && !microphoneTest_) {
            audio_->stop();
        }
    });
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
    audio_->setMicrophoneTest(enabled);
}

void VoiceSession::setPeerVolume(const QString& peerId, int percent) {
    audio_->setPeerVolume(peerId, percent);
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

void VoiceSession::receiveFrame(const QString& peerId, quint32 sequence, const QByteArray& payload,
                                qint64 transportReceivedAtNs) {
    if (active_ && !peerId.isEmpty()) {
        audio_->receiveFrame(peerId, sequence, payload, transportReceivedAtNs);
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
