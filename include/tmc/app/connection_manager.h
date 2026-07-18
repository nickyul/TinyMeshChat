#pragma once

#include "tmc/core/result.h"
#include "tmc/core/voice_frame_timing.h"
#include "tmc/identity/peer_identity.h"
#include "tmc/network/connection_attempt_state.h"
#include "tmc/network/connection_policy.h"
#include "tmc/network/connection_state.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>

#include <memory>
#include <optional>

namespace tmc {

struct ConnectionInfo {
    QString connectionId;
    PeerIdentity remote;
    bool open{false};
    bool everOpened{false};
    bool localOffer{false};
    bool meshManaged{false};
    bool answerApplied{false};
    ConnectionState transportState{ConnectionState::Disconnected};
    ConnectionAttemptState attemptState{ConnectionAttemptState::Gathering};
    qint64 lastActivityMs{0};
    qint64 createdAtMs{0};
    QString iceState{"new"};
    QString selectedCandidatePair;
    QString lastError;
    int hostCandidates{0};
    int serverReflexiveCandidates{0};
    int relayCandidates{0};
    quint64 droppedVoiceFrames{0};
};

class ConnectionManager final : public QObject {
    Q_OBJECT

public:
    ConnectionManager(QStringList stunServers, ConnectionPolicy policy, QObject* parent = nullptr);
    ~ConnectionManager() override;

    Result<void> create(const QString& connectionId, const PeerIdentity& remote, bool localOffer,
                        bool meshManaged);
    Result<void> startOffer(const QString& connectionId);
    Result<void> acceptOffer(const QString& connectionId, const QString& sdp);
    Result<void> acceptAnswer(const QString& connectionId, const QString& sdp);
    void setRemote(const QString& connectionId, const PeerIdentity& remote);

    void discard(const QString& connectionId);
    void discardStale(const QString& peerId = {});

    bool contains(const QString& connectionId) const;
    std::optional<ConnectionInfo> info(const QString& connectionId) const;
    std::optional<ConnectionInfo> infoForPeer(const QString& peerId) const;
    QList<ConnectionInfo> connections() const;
    QList<ConnectionInfo> recentAttempts() const;
    QStringList openConnectionIds() const;
    QStringList inactiveConnectionIds(qint64 nowMs, qint64 timeoutMs) const;
    int connectedPeerCount() const;

    void setStunServers(QStringList stunServers);

    bool sendText(const QString& connectionId, const QString& text);
    void sendVoiceFrameToOpen(quint32 sequence, const QByteArray& payload,
                              const VoiceFrameTiming& timing);

signals:
    void localDescriptionReady(QString connectionId, QString type, QString sdp);
    void linkOpened(QString connectionId, tmc::PeerIdentity remote);
    void linkRemoved(QString connectionId, tmc::PeerIdentity remote, bool wasOpen);
    void textReceived(QString connectionId, QString text);
    void voiceFrameReceived(QString connectionId, quint32 sequence, QByteArray payload,
                            qint64 receivedAtNs);
    void attemptChanged(QString connectionId, tmc::ConnectionAttemptState state);
    void attemptFailed(tmc::PeerIdentity remote, bool meshManaged, bool localOffer,
                       QString message);
    void statusChanged(QString status);

private:
    struct Link;

    std::shared_ptr<Link> current(const QString& connectionId) const;
    bool isCurrent(const std::shared_ptr<Link>& link) const;

    void configure(const std::shared_ptr<Link>& link);
    void setAttemptState(const std::shared_ptr<Link>& link, ConnectionAttemptState state);
    void startDeadline(const std::shared_ptr<Link>& link, int seconds, QString message);
    void cancelDeadline(const std::shared_ptr<Link>& link);
    void fail(const std::shared_ptr<Link>& link, ConnectionAttemptState state,
              const QString& message);
    void remove(const std::shared_ptr<Link>& link);

    ConnectionInfo snapshot(const std::shared_ptr<Link>& link) const;

    QStringList stunServers_;
    ConnectionPolicy policy_;
    QHash<QString, std::shared_ptr<Link>> links_;
    QList<ConnectionInfo> recentAttempts_;
};

} // namespace tmc
