#pragma once

#include "tmc/network/connection_state.h"

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <memory>

namespace rtc {

class PeerConnection;
class DataChannel;
class Track;

} // namespace rtc

namespace tmc {

class AudioTransportEndpoint;
class AudioTransportWorker;

struct PeerConnectionSnapshot {
    bool controlOpen{false};
    bool chatOpen{false};
    bool audioTrackOpen{false};
    quint64 audioFramesAttempted{0};
    quint64 audioFramesSent{0};
    quint64 audioFramesReceived{0};
    quint64 controlBufferedBytes{0};
    quint64 chatBufferedBytes{0};
    quint64 queuedControlBytes{0};
    quint64 queuedChatBytes{0};
};

class PeerConnection final : public QObject {
    Q_OBJECT

public:
    PeerConnection(const QStringList& stunServers, QString connectionId,
                   std::weak_ptr<AudioTransportWorker> audioTransport, QObject* parent = nullptr);
    ~PeerConnection() override;

    void createOffer();
    void acceptOffer(const QString&);
    void acceptAnswer(const QString&);
    void createAudioOffer();
    void acceptAudioOffer(const QString&);
    void acceptAudioAnswer(const QString&);

    bool sendControl(const QString&);
    bool sendChat(const QString&);
    std::shared_ptr<AudioTransportEndpoint> audioEndpoint() const;
    PeerConnectionSnapshot snapshot() const;

signals:
    void localDescriptionReady(QString type, QString sdp);
    void audioDescriptionReady(QString type, QString sdp);
    void stateChanged(tmc::ConnectionState);
    void iceStateChanged(QString state);
    void gatheringStateChanged(QString state);
    void candidateDiscovered(QString type, QString transport);
    void selectedCandidatePairChanged(QString localType, QString remoteType);
    void controlChannelOpened();
    void controlChannelClosed();
    void chatChannelOpened();
    void chatChannelClosed();
    void controlTextReceived(QString);
    void chatTextReceived(QString);
    void errorOccurred(QString);

private:
    enum class TextChannel {
        Control,
        Chat,
    };

    struct State;

    void configureControlChannel(const std::shared_ptr<rtc::DataChannel>&);
    void configureChatChannel(const std::shared_ptr<rtc::DataChannel>&);
    void configureTextChannel(const std::shared_ptr<rtc::DataChannel>&, TextChannel);
    void configureAudioTrack(const std::shared_ptr<rtc::Track>&);
    bool sendText(const std::shared_ptr<rtc::DataChannel>& channel, const QString& text,
                  TextChannel);
    void flushTextQueue(TextChannel);

    std::shared_ptr<State> state_;
};

} // namespace tmc
