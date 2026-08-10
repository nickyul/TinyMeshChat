#pragma once

#include "tmc/core/result.h"
#include "tmc/identity/peer_identity.h"
#include "tmc/network/connection_attempt_state.h"
#include "tmc/network/connection_policy.h"
#include "tmc/network/connection_state.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

#include <memory>
#include <optional>

namespace tmc {

class AudioTransportWorker;

enum class ConnectionKind { ManualOffer, ManualAnswer, MeshOffer, MeshAnswer };

constexpr bool isOffer(ConnectionKind kind) {
    return kind == ConnectionKind::ManualOffer || kind == ConnectionKind::MeshOffer;
}

constexpr bool isMeshManaged(ConnectionKind kind) {
    return kind == ConnectionKind::MeshOffer || kind == ConnectionKind::MeshAnswer;
}

struct ConnectionInfo {
    // Identity and lifetime.
    QString connectionId;
    PeerIdentity remote;
    qint64 createdAtMs{0};
    bool open{false};

    // Signaling role and progress.
    ConnectionKind kind{ConnectionKind::ManualOffer};
    bool answerApplied{false};
    quint64 generation{0};

    // Transport and handshake readiness.
    bool controlChannelOpen{false};
    bool chatChannelOpen{false};
    bool audioTrackOpen{false};
    bool helloReceived{false};
    ConnectionState transportState{ConnectionState::Disconnected};
    ConnectionAttemptState attemptState{ConnectionAttemptState::Gathering};

    // ICE and connection diagnostics.
    QString iceState{"new"};
    QString selectedCandidatePair;
    QString lastError;
    int hostCandidates{0};
    int serverReflexiveCandidates{0};
    int relayCandidates{0};
    int roundTripTimeMs{-1};

    // Traffic diagnostics.
    quint64 audioFramesAttempted{0};
    quint64 audioFramesSent{0};
    quint64 audioFramesReceived{0};
    quint64 controlBufferedBytes{0};
    quint64 chatBufferedBytes{0};
    quint64 queuedControlBytes{0};
    quint64 queuedChatBytes{0};
};

class ConnectionManager final : public QObject {
    Q_OBJECT

public:
    ConnectionManager(QStringList stunServers, ConnectionPolicy policy, QObject* parent = nullptr);
    ~ConnectionManager() override;

    Result<void> create(const QString& connectionId, const PeerIdentity& remote,
                        ConnectionKind kind, quint64 generation = 0);
    Result<void> startOffer(const QString& connectionId);
    Result<void> acceptOffer(const QString& connectionId, const QString& sdp);
    Result<void> acceptAnswer(const QString& connectionId, const QString& sdp);
    Result<void> startAudioOffer(const QString& connectionId);
    Result<void> acceptAudioOffer(const QString& connectionId, const QString& sdp);
    Result<void> acceptAudioAnswer(const QString& connectionId, const QString& sdp);
    void markHelloReceived(const QString& connectionId, const PeerIdentity& remote);

    void discard(const QString& connectionId);
    void markTimedOut(const QString& connectionId, const QString& message);

    bool contains(const QString& connectionId) const;
    std::optional<ConnectionInfo> info(const QString& connectionId) const;
    std::optional<ConnectionInfo> infoForPeer(const QString& peerId) const;
    QList<ConnectionInfo> connections() const;
    QList<ConnectionInfo> recentAttempts() const;
    QStringList openConnectionIds() const;
    QStringList inactiveConnectionIds(qint64 nowMs, qint64 timeoutMs) const;
    int connectedPeerCount() const;

    void setStunServers(QStringList stunServers);
    int recordRoundTripTime(const QString& connectionId, int sampleMs);
    std::shared_ptr<AudioTransportWorker> audioTransport() const;

    bool sendControl(const QString& connectionId, const QString& text);
    bool sendChat(const QString& connectionId, const QString& text);

signals:
    void localDescriptionReady(QString connectionId, QString sdp);
    void audioDescriptionReady(QString connectionId, QString type, QString sdp);
    void transportOpened(QString connectionId, tmc::PeerIdentity remote);
    void linkOpened(QString connectionId, tmc::PeerIdentity remote);
    void linkRemoved(QString connectionId, tmc::PeerIdentity remote, bool wasOpen);
    void controlTextReceived(QString connectionId, QString text);
    void chatTextReceived(QString connectionId, QString text);
    void attemptChanged(QString connectionId, tmc::ConnectionAttemptState state);
    void attemptFailed(tmc::PeerIdentity remote, tmc::ConnectionKind kind, QString message);
    void statusChanged(QString status);

private:
    struct Link;

    std::shared_ptr<Link> current(const QString& connectionId) const;
    bool isCurrent(const std::shared_ptr<Link>& link) const;

    void configure(const std::shared_ptr<Link>& link);
    void connectIceSignals(const std::shared_ptr<Link>& link);
    void connectSignalingSignals(const std::shared_ptr<Link>& link);
    void connectTransportSignals(const std::shared_ptr<Link>& link);
    void connectChannelSignals(const std::shared_ptr<Link>& link);
    void connectDataSignals(const std::shared_ptr<Link>& link);
    void updateTransportReadiness(const std::shared_ptr<Link>& link);
    void updateHandshakeReadiness(const std::shared_ptr<Link>& link);
    void setAttemptState(const std::shared_ptr<Link>& link, ConnectionAttemptState state);
    void startDeadline(const std::shared_ptr<Link>& link, int seconds, QString message);
    void cancelDeadline(const std::shared_ptr<Link>& link);
    void suspect(const std::shared_ptr<Link>& link, QString message);
    void fail(const std::shared_ptr<Link>& link, ConnectionAttemptState state,
              const QString& message);
    void remove(const std::shared_ptr<Link>& link);

    ConnectionInfo snapshot(const std::shared_ptr<Link>& link) const;

    QStringList stunServers_;
    ConnectionPolicy policy_;
    QHash<QString, std::shared_ptr<Link>> links_;
    QList<ConnectionInfo> recentAttempts_;
    std::shared_ptr<AudioTransportWorker> audioTransport_;
};

} // namespace tmc

Q_DECLARE_METATYPE(tmc::ConnectionKind)
