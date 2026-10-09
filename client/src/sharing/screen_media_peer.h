#pragma once

#include "tmc/network/relay_server.h"
#include "tmc/sharing/screen_quality.h"

#include <QJsonObject>
#include <QObject>

#include <functional>
#include <memory>

namespace webrtc {

class VideoFrame;

}

namespace tmc {

class ScreenMediaEngine;
class ScreenVideoBridge;
class SharedAudioOutput;

class ScreenMediaPeer final : public QObject {
    Q_OBJECT

public:
    ScreenMediaPeer(ScreenMediaEngine& engine, QString local, QString remote, QStringList stun,
                    QList<RelayServer> turn, std::shared_ptr<SharedAudioOutput> output,
                    QObject* parent = nullptr);
    ~ScreenMediaPeer() override;
    void begin(QString mediaId, quint64 generation);
    void receive(const QJsonObject& message);
    void setSending(QString stream, bool video, bool audio, ScreenQuality quality);
    void setReceiving(QString stream, bool video, bool audio, ScreenVideoBridge* bridge);
    void pushFrame(const webrtc::VideoFrame& frame);
    void updateIce(QStringList stun, QList<RelayServer> turn);
    void restartIce();
    void preferSoftware();
    void requestStats();
    void close();
signals:
    void outgoing(QJsonObject message);
    void stateChanged(QString state);
    void errorOccurred(QString message);
    void statsChanged(QJsonObject stats);
    void firstFrame();

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace tmc
