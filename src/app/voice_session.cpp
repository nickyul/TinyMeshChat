#include "tmc/app/voice_session.h"

#include "tmc/audio/audio_engine.h"

namespace tmc {

VoiceSession::VoiceSession(QObject* parent)
    : QObject(parent), audio_(std::make_unique<AudioEngine>()) {
    connect(audio_.get(), &AudioEngine::encodedFrameReady, this, &VoiceSession::encodedFrameReady);
    connect(audio_.get(), &AudioEngine::errorOccurred, this, &VoiceSession::errorOccurred);
}

VoiceSession::~VoiceSession() = default;

bool VoiceSession::active() const {
    return active_;
}

bool VoiceSession::muted() const {
    return muted_;
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
    audio_->stop();
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

void VoiceSession::clear() {
    leave();
    const auto peerIds = peers_.keys();
    peers_.clear();
    for (const auto& peerId : peerIds) {
        audio_->removePeer(peerId);
        emit peerChanged(peerId, false, false);
    }
}

void VoiceSession::receiveFrame(const QString& peerId, quint32 sequence,
                                const QByteArray& payload) {
    if (active_ && !peerId.isEmpty()) {
        audio_->receiveFrame(peerId, sequence, payload);
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

QJsonObject VoiceSession::statePayload() const {
    return {{"joined", active_}, {"muted", muted_}};
}

} // namespace tmc
