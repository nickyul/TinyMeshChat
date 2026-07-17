#pragma once

#include "tmc/core/result.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>

#include <memory>

namespace tmc {

class AudioEngine;

class VoiceSession final : public QObject {
    Q_OBJECT

public:
    explicit VoiceSession(QObject* parent = nullptr);
    ~VoiceSession() override;

    Result<void> start();
    void leave();
    void setMuted(bool muted);
    void clear();

    void receiveFrame(const QString& peerId, quint32 sequence, const QByteArray& payload);
    void updatePeer(const QString& peerId, bool joined, bool muted);
    void removePeer(const QString& peerId);

    QJsonObject statePayload() const;

    bool active() const;
    bool muted() const;

signals:
    void encodedFrameReady(quint32 sequence, QByteArray payload);
    void stateChanged(bool active, bool muted);
    void peerChanged(QString peerId, bool joined, bool muted);
    void errorOccurred(QString message);

private:
    std::unique_ptr<AudioEngine> audio_;
    QHash<QString, QPair<bool, bool>> peers_;
    bool active_{false};
    bool muted_{false};
};

} // namespace tmc
