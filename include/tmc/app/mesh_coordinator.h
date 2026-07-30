#pragma once

#include "tmc/app/mesh_session_state.h"
#include "tmc/app/peer_registry.h"
#include "tmc/network/connection_policy.h"

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
    bool forgetPeer(const QString& peerId);
    bool ingestPeerList(const QList<PeerIdentity>& peers, const QString& localPeerId);
    QList<PeerIdentity> peerList() const;

    bool canAttemptLink(const QString& peerId) const;

    void connectionOpened(const PeerIdentity& peer);
    void scheduleRetry(const PeerIdentity& peer);
    void resetRetryBackoff();
    void routeAvailable(const QString& peerId);
    quint64 nextLinkGeneration(const QString& peerId);
    bool acceptLinkGeneration(const QString& peerId, quint64 generation);

    QString meshId() const;
    MeshSessionState state() const;
    bool established() const;
    int peerCount() const;
    qint64 revision() const;
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
    QHash<QString, int> retryCounts_;
    QHash<QString, quint64> retryGenerations_;
    QHash<QString, quint64> linkGenerations_;
    QSet<QString> retryScheduled_;
    QSet<QString> degradedPeers_;
    quint64 sessionGeneration_{0};
    qint64 revision_{0};
};

} // namespace tmc
