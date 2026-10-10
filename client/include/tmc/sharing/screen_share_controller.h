#pragma once

#include "tmc/core/result.h"
#include "tmc/network/relay_server.h"
#include "tmc/sharing/screen_quality.h"

#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QVideoSink>

#include <functional>
#include <memory>

namespace tmc {

class SharedAudioOutput;

class ScreenShareController final : public QObject {
    Q_OBJECT

public:
    using Sender = std::function<bool(const QString&, const QJsonObject&)>;
    ScreenShareController(QString localIdentity, QStringList stun,
                          std::shared_ptr<SharedAudioOutput> output, Sender sender,
                          QObject* parent = nullptr);
    ~ScreenShareController() override;
    QVariantList sources();
    QVariantList streams() const;
    Result<void> start(int sourceIndex, bool systemAudio, ScreenQuality quality = {});
    void stop();
    void clear();
    void setPeerConnected(const QString& peerId, bool connected);
    void receive(const QString& peerId, const QJsonObject& message);
    void setIceServers(QStringList stun, QList<RelayServer> turn, qint64 expiresAtMs = 0);
    void setStunServers(QStringList stun);
    void subscribe(const QString& streamId, bool selected);
    void selectAudio(const QString& streamId);
    void setQuality(const QString& streamId, ScreenQuality quality);
    void retry(const QString& streamId);
    void setVideoSink(const QString& streamId, QVideoSink* sink, bool attach = true);
    bool sharing() const;
    bool viewing() const;
    QString owner() const;
    QString title() const;
    QString localStream() const;
    bool systemAudioSupported() const;
    void setAudioMuted(bool muted);
    void setAudioVolume(int percent);
    QJsonObject diagnostics() const;
signals:
    void changed();
    void errorOccurred(QString message);

private:
    struct State;
    std::unique_ptr<State> state_;
    void announce(const QString& peerId);
    void ensureMedia(const QString& peerId, bool reset = false);
    void removeMedia(const QString& peerId);
    void updateSubscription(const QString& peerId);
    void sendSubscription(const QString& streamId);
    void updateAudioCapture();
    void maintain();
};

} // namespace tmc
