#pragma once

#include "tmc/network/connection_state.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <memory>

namespace rtc {

class PeerConnection;
class DataChannel;

} // namespace rtc

namespace tmc {

class PeerConnection final : public QObject {
    Q_OBJECT

public:
    explicit PeerConnection(const QStringList& stunServers, QObject* parent = nullptr);
    ~PeerConnection() override;

    void createOffer();
    void acceptOffer(const QString&);
    void acceptAnswer(const QString&);

    bool sendText(const QString&);
    bool sendVoiceFrame(quint32 sequence, const QByteArray& opusPayload);

signals:
    void localDescriptionReady(QString type, QString sdp);
    void stateChanged(tmc::ConnectionState);
    void channelOpened();
    void channelClosed();
    void textReceived(QString);
    void voiceFrameReceived(quint32 sequence, QByteArray opusPayload);
    void errorOccurred(QString);

private:
    struct State;

    void configureChannel(const std::shared_ptr<rtc::DataChannel>&);
    void configureVoiceChannel(const std::shared_ptr<rtc::DataChannel>&);

    std::shared_ptr<State> state_;
};

} // namespace tmc
