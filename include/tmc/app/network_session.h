#pragma once

#include "tmc/app/connection_manager.h"
#include "tmc/app/mesh_coordinator.h"
#include "tmc/app/mesh_session_state.h"
#include "tmc/app/messaging_service.h"
#include "tmc/app/signaling_router.h"
#include "tmc/app/topology_controller.h"
#include "tmc/core/app_config.h"
#include "tmc/core/result.h"
#include "tmc/messaging/chat_message.h"
#include "tmc/network/connection_policy.h"
#include "tmc/protocol/packet.h"
#include "tmc/signaling/invitation.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace tmc {

class ApplicationController;
class PacketDispatcher;
class SessionPacketHandlers;
class VoiceSession;

class NetworkSession final : public QObject {
    Q_OBJECT

public:
    NetworkSession(ApplicationController& app, ConnectionPolicy policy, QObject* parent = nullptr);
    ~NetworkSession() override;

    Result<void> createMesh();
    void leaveMesh();

    Result<void> createInvitation();
    void cancelInvitation();
    Result<void> recreateInvitation();
    Result<void> importSignalingText(const QString& text);
    Result<void> importSignalingDocument(const QByteArray& document);

    Result<void> sendMessage(const QString& text);

    Result<void> startCall();
    void leaveCall();
    void setMuted(bool muted);
    void setDeafened(bool deafened);
    void setMicrophoneTest(bool enabled);
    void setPeerVolume(const QString& peerId, int percent);
    Result<void> applyAudioPreferences(const AudioPreferences& preferences);
    QPair<QStringList, QStringList> refreshAudioDevices();

    bool callActive() const;
    bool muted() const;
    bool deafened() const;
    bool microphoneTest() const;
    bool invitationPending() const;
    QString invitationState() const;

    Result<QPair<int, int>> deliveryCounts(const QString& messageId) const;

    MeshSessionState meshState() const;
    int connectedPeerCount() const;
    int knownPeerCount() const;
    QString diagnostics() const;
    QString peerDisplayName(const QString& peerId) const;

signals:
    void meshStateChanged(tmc::MeshSessionState state);
    void signalingReady(QString kind, QString text, QByteArray document, QString suggestedName);
    void statusChanged(QString status);
    void meshChanged(int connected, int expected);
    void peerChanged(QString peerId, QString displayName, bool connected);
    void peerRttChanged(QString peerId, int milliseconds);
    void messageReceived(tmc::ChatMessage message, bool local);
    void deliveryChanged(QString messageId, int acknowledged, int expected);
    void callStateChanged(bool active, bool muted);
    void audioStateChanged();
    void microphoneLevelChanged(double level);
    void peerVoiceChanged(QString peerId, bool joined, bool muted);
    void peerAudioStatsChanged(QString peerId, double packetLossPercent, int jitterMs,
                               int bufferMs);
    void connectionAttemptChanged(QString connectionId, tmc::ConnectionAttemptState state);
    void invitationStateChanged(bool pending, QString state);
    void errorOccurred(QString message);

private:
    friend class SessionPacketHandlers;

    Result<Invitation> decodeSignaling(const QByteArray& document) const;
    void emitSignaling(const QString& connectionId, const QString& type, const QString& sdp);

    void handleIncoming(const QString& connectionId, const QString& text, bool chatChannel);

    void sendPacket(const QString& connectionId, const Packet& packet);
    Packet basePacket(PacketType type, PacketPayload payload) const;

    void sendHello(const QString& connectionId);
    void sendPing(const QString& connectionId);
    void receivePong(const QString& connectionId, const HeartbeatPayload& payload);
    void sendPeerList(const QString& connectionId);
    void announceLocalPeer(const QString& excludedConnection = {});
    void sendVoiceState(const QString& connectionId);
    void broadcastVoiceState();
    void broadcastPeerList(const QString& excludedConnection = {});

    void ensureDynamicMesh();
    void startMeshOffer(const PeerIdentity& peer);
    void broadcastService(const Packet& packet, const QString& excludedConnection = {});
    bool sendRouted(Packet packet, const QString& excludedConnection = {});
    void requestRoute(const QString& peerId);
    void flushRouted(const QString& peerId);

    void updateMesh();
    void clearSessionData();

    ApplicationController& app_;
    ConnectionPolicy policy_;
    std::unique_ptr<ConnectionManager> connections_;
    std::unique_ptr<PacketDispatcher> packetDispatcher_;
    std::unique_ptr<SessionPacketHandlers> packetHandlers_;
    MeshCoordinator mesh_;
    SignalingRouter router_;
    TopologyController topology_;
    MessagingService messaging_;
    std::unique_ptr<VoiceSession> voice_;
    QTimer* keepalive_{};
    QString manualInvitationConnectionId_;
    QHash<QString, QList<Packet>> pendingRouted_;
    QHash<QString, QString> routeRequests_;
    QHash<QString, quint64> audioNegotiations_;
    struct PendingPing {
        QString nonce;
        qint64 sentAtNs{0};
    };
    QHash<QString, PendingPing> pendingPings_;
};

} // namespace tmc

Q_DECLARE_METATYPE(tmc::ChatMessage)
