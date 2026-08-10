#pragma once

#include "tmc/core/app_config.h"
#include "tmc/core/result.h"

#include <QHash>
#include <QObject>
#include <QStringList>

#include <memory>

namespace tmc {

class RtpAudioEngine;
class IncomingRtpAudioSink;
class OutgoingOpusAudioSink;

class VoiceSession final : public QObject {
    Q_OBJECT

public:
    explicit VoiceSession(AudioPreferences preferences = {}, QObject* parent = nullptr);
    ~VoiceSession() override;

    Result<void> start();
    void leave();
    void setMuted(bool muted);
    void setDeafened(bool deafened);
    void setMicrophoneTest(bool enabled);
    void setPttPressed(bool pressed);
    void setPeerVolume(const QString& peerId, int percent);
    void updateNetworkFeedback(const QString& peerId, double packetLossPercent);
    Result<void> applyPreferences(const AudioPreferences& preferences);
    QPair<QStringList, QStringList> refreshDevices();
    void clear();

    void updatePeer(const QString& peerId, bool joined, bool muted);
    void removePeer(const QString& peerId);

    bool active() const;
    bool muted() const;
    bool deafened() const;
    bool microphoneTest() const;
    double microphoneLevel() const;
    AudioPreferences preferences() const;
    std::shared_ptr<IncomingRtpAudioSink> incomingAudioSink() const;
    void setOutgoingAudioSink(std::weak_ptr<OutgoingOpusAudioSink> sink);

signals:
    void stateChanged(bool active, bool muted);
    void peerChanged(QString peerId, bool joined, bool muted);
    void errorOccurred(QString message);
    void talkingStateChanged(bool isTalking);
    void peerTalkingStateChanged(QString peerId, bool isTalking);
    void networkStatsChanged(QString peerId, double packetLossPercent, int jitterMs, int bufferMs);

private:
    std::unique_ptr<RtpAudioEngine> audio_;
    QHash<QString, QPair<bool, bool>> peers_;
    bool active_{false};
    bool muted_{false};
    bool deafened_{false};
    bool microphoneTest_{false};
};

} // namespace tmc
