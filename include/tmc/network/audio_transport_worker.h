#pragma once

#include "tmc/core/rtp_audio_frame.h"

#include <QString>

#include <atomic>
#include <memory>

namespace rtc {

class Track;

} // namespace rtc

namespace tmc {

class AudioTransportEndpoint final {
public:
    AudioTransportEndpoint() = default;

    void setTrack(std::shared_ptr<rtc::Track> track);
    void clearTrack();
    bool send(const OutgoingOpusAudioFrame& frame);
    bool isOpen() const;

    void recordReceived();
    quint64 framesAttempted() const;
    quint64 framesSent() const;
    quint64 framesReceived() const;

private:
    std::atomic<std::shared_ptr<rtc::Track>> track_;
    std::atomic<quint64> framesAttempted_{0};
    std::atomic<quint64> framesSent_{0};
    std::atomic<quint64> framesReceived_{0};
};

class AudioTransportWorker final : public OutgoingOpusAudioSink {
public:
    AudioTransportWorker();
    ~AudioTransportWorker() override;

    AudioTransportWorker(const AudioTransportWorker&) = delete;
    AudioTransportWorker& operator=(const AudioTransportWorker&) = delete;

    void setIncomingSink(std::weak_ptr<IncomingRtpAudioSink> sink);
    void registerConnection(const QString& connectionId,
                            std::shared_ptr<AudioTransportEndpoint> endpoint);
    void updateConnection(const QString& connectionId, const QString& peerId, bool open);
    void unregisterConnection(const QString& connectionId);

    void setReceiveEnabled(bool enabled);
    void setTransmitEnabled(bool enabled);
    void clearOutgoing();

    void enqueueIncoming(const QString& connectionId, quint32 ssrc, quint16 sequenceNumber,
                         quint32 rtpTimestamp, QByteArray opusPayload, qint64 receivedAtNs);
    void enqueue(OutgoingOpusAudioFrame frame) override;

    quint64 droppedIncomingFrames() const;
    quint64 droppedOutgoingFrames() const;
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace tmc
