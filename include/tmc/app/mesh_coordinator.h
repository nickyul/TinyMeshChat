#pragma once

#include "tmc/app/mesh_session_state.h"
#include "tmc/app/peer_registry.h"
#include "tmc/network/connection_policy.h"

#include <QJsonArray>
#include <QObject>
#include <QSet>

namespace tmc {

class MeshCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit MeshCoordinator(ConnectionPolicy policy, QObject* parent = nullptr);

    void create(const PeerIdentity& localIdentity, const QString& meshId);
    void beginJoin(const PeerIdentity& localIdentity, const PeerIdentity& inviter,
                   const QString& meshId);
    void leave();

    void markEstablished();

    bool rememberPeer(const PeerIdentity& peer);
    bool ingestPeerList(const QJsonArray& peers, const QString& localPeerId);
    QJsonArray peerList() const;

    bool rememberRoute(const QString& routeId);
    bool shouldInitiateLink(const QString& localPeerId, const QString& remotePeerId) const;
    bool canAttemptLink(const QString& peerId) const;

    void connectionOpened(const PeerIdentity& peer);
    void scheduleRetry(const PeerIdentity& peer);

    QString meshId() const;
    MeshSessionState state() const;
    bool established() const;
    int peerCount() const;
    PeerIdentity peer(const QString& peerId) const;
    QList<PeerIdentity> peers() const;

signals:
    void stateChanged(tmc::MeshSessionState state);
    void retryRequested(tmc::PeerIdentity peer);
    void statusChanged(QString status);

private:
    void resetRuntime();
    void updateState();
    void setState(MeshSessionState state);

    ConnectionPolicy policy_;
    PeerRegistry peers_;
    QString meshId_;
    MeshSessionState state_{MeshSessionState::Disconnected};
    bool established_{false};
    QSet<QString> seenRoutes_;
    QHash<QString, int> retryCounts_;
    QHash<QString, quint64> retryGenerations_;
    QSet<QString> retryScheduled_;
    QSet<QString> degradedPeers_;
    quint64 sessionGeneration_{0};
};

} // namespace tmc
