#pragma once

#include "tmc/audio/audio_network_stats.h"

#include <QByteArray>
#include <QString>

#include <array>
#include <map>
#include <memory>
#include <optional>

#include <opus/opus.h>

namespace tmc {

class RemoteAudioStream final {
public:
    static constexpr int SampleRate = 48000;
    static constexpr int FrameSamples = 960;
    using PcmFrame = std::array<opus_int16, FrameSamples>;

    explicit RemoteAudioStream(QString peerId, int volumePercent = 100);
    ~RemoteAudioStream();

    RemoteAudioStream(const RemoteAudioStream&) = delete;
    RemoteAudioStream& operator=(const RemoteAudioStream&) = delete;

    bool valid() const;
    void enqueue(quint32 rtpTimestamp, QByteArray opusPayload, qint64 receivedAtNs);
    bool render(PcmFrame& output);
    void setVolume(int percent);
    float volume() const;
    std::optional<AudioNetworkStats> takeStats(qint64 nowNs);

private:
    struct Packet {
        QByteArray payload;
        qint64 receivedAtNs{0};
    };

    qint64 unwrapTimestamp(quint32 timestamp);
    int decode(const QByteArray* payload, bool fec, PcmFrame& output);
    void adaptBuffer(bool concealed);
    void discardObsoletePackets();

    QString peerId_;
    OpusDecoder* decoder_{};
    std::map<qint64, Packet> packets_;
    quint32 latestRawTimestamp_{0};
    qint64 latestExtendedTimestamp_{0};
    qint64 expectedFrame_{0};
    qint64 lastTransitTicks_{0};
    qint64 lastStatsAtNs_{0};
    double jitterTicks_{0.0};
    double smoothedLossPercent_{0.0};
    float volume_{1.0F};
    int targetFrames_{2};
    int stableFrames_{0};
    int consecutiveConcealments_{0};
    quint64 receivedPackets_{0};
    quint64 concealedFrames_{0};
    quint64 fecRecoveredFrames_{0};
    quint64 latePackets_{0};
    quint64 windowExpectedFrames_{0};
    quint64 windowLostFrames_{0};
    bool hasTimestamp_{false};
    bool hasTransit_{false};
    bool started_{false};
    bool hasLossSample_{false};
};

} // namespace tmc
