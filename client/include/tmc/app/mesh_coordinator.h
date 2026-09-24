#pragma once

#include "tmc/app/mesh_session_state.h"
#include "tmc/identity/peer_identity.h"
#include "tmc/network/connection_policy.h"

#include <QHash>
#include <QObject>

namespace tmc {

class MeshCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit MeshCoordinator(ConnectionPolicy policy, QObject* parent = nullptr);

    void create(const PeerIdentity& localIdentity, const QString& meshId);
    void beginJoin(const PeerIdentity& localIdentity, const QString& meshId);
    void leave();

    bool rememberPeer(const PeerIdentity& peer);
    bool forgetPeer(const QString& peerId);
    bool ingestPeerList(const QList<PeerIdentity>& peers, const QString& localPeerId);

    bool canAttemptLink(const QString& peerId) const;

    void connectionOpened(const PeerIdentity& peer);
    void scheduleRetry(const PeerIdentity& peer);
    void resetRetryBackoff();
    void routeAvailable(const QString& peerId);
    quint64 nextLinkGeneration(const QString& peerId);
    bool acceptLinkGeneration(const QString& peerId, quint64 generation);

    QString meshId() const;
    MeshSessionState state() const;
    bool joined() const;
    int peerCount() const;
    PeerIdentity peer(const QString& peerId) const;
    QList<PeerIdentity> peers() const;

signals:
    void stateChanged(tmc::MeshSessionState state);
    void retryRequested(tmc::PeerIdentity peer);
    void statusChanged(QString status);

private:
    struct PeerRetryState {
        int attempts{0};
        quint64 token{0};
        bool scheduled{false};
        bool degraded{false};
    };

    void resetRuntime();
    void updateState();
    void setState(MeshSessionState state);

    ConnectionPolicy policy_;

    // Current mesh membership and public state.
    QHash<QString, PeerIdentity> peers_;
    QString meshId_;
    MeshSessionState state_{MeshSessionState::Disconnected};
    bool joined_{false};

    // Per-peer retry backoff for direct links.
    QHash<QString, PeerRetryState> retryStates_;
    quint64 nextRetryToken_{0};

    // Latest signaling generation for each direct link.
    QHash<QString, quint64> linkGenerations_;
};

} // namespace tmc
