#pragma once

#include "tmc/core/app_config.h"
#include "tmc/core/result.h"
#include "tmc/core/voice_frame_timing.h"

#include <QByteArray>
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
    void setPeerVolume(const QString& peerId, int percent);
    void setPeerNetworkLoss(const QString& peerId, double packetLossPercent);

    bool isRunning() const;
    bool isDeafened() const;
    bool microphoneTest() const;
    AudioPreferences preferences() const;

    void receiveFrame(const QString& peerId, quint32 rtpTimestamp, const QByteArray& opusPayload,
                      qint64 transportReceivedAtNs);
    void removePeer(const QString& peerId);

signals:
    void encodedFrameReady(quint32 sequence, QByteArray opusPayload, tmc::VoiceFrameTiming timing);
    void microphoneLevelChanged(double level);
    void microphoneTestPlaybackFinished();
    void networkStatsChanged(QString peerId, double packetLossPercent, int jitterMs, int bufferMs);
    void errorOccurred(QString message);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace tmc
