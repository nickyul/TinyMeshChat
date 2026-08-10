#pragma once

#include "tmc/core/app_config.h"
#include "tmc/core/result.h"
#include "tmc/core/rtp_audio_frame.h"

#include <QObject>
#include <QStringList>

#include <memory>

namespace tmc {

struct AudioDeviceLists {
    QStringList capture;
    QStringList playback;
};

class RtpAudioEngine final : public QObject {
    Q_OBJECT

public:
    explicit RtpAudioEngine(AudioPreferences preferences = {}, QObject* parent = nullptr);
    ~RtpAudioEngine() override;

    Result<void> start();
    void stop();
    Result<void> applyPreferences(const AudioPreferences& preferences);
    AudioDeviceLists refreshDevices();

    void setMuted(bool muted);
    void setDeafened(bool deafened);
    void setMicrophoneTest(bool enabled);
    void setPttPressed(bool pressed);
    void setPeerVolume(const QString& peerId, int percent);
    void setPeerNetworkLoss(const QString& peerId, double packetLossPercent);

    bool isRunning() const;
    bool isDeafened() const;
    bool microphoneTest() const;
    double microphoneLevel() const;
    AudioPreferences preferences() const;
    std::shared_ptr<IncomingRtpAudioSink> incomingSink() const;
    void setOutgoingSink(std::weak_ptr<OutgoingOpusAudioSink> sink);

    void removePeer(const QString& peerId);

signals:
    void talkingStateChanged(bool isTalking);
    void peerTalkingStateChanged(QString peerId, bool isTalking);
    void networkStatsChanged(QString peerId, double packetLossPercent, int jitterMs, int bufferMs);
    void errorOccurred(QString message);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace tmc
