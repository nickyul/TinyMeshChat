#pragma once

#include "tmc/app/connection_manager.h"
#include "tmc/app/mesh_coordinator.h"
#include "tmc/app/mesh_session_state.h"
#include "tmc/app/messaging_service.h"
#include "tmc/app/signaling_router.h"
#include "tmc/core/app_config.h"
#include "tmc/core/result.h"
#include "tmc/messaging/chat_message.h"
#include "tmc/network/connection_policy.h"
#include "tmc/protocol/packet.h"
#include "tmc/signaling/invitation.h"
#include "tmc/signaling_client/server_invitation.h"
#include "tmc/signaling_protocol/envelope.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <memory>

namespace tmc {

class ApplicationController;
class VoiceSession;
class SignalingClient;

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

    // GUI opts in explicitly; console sessions retain their existing behavior.
    void configureSignalingServer(const QString& url);
    Result<void> createServerInvitation();
    Result<void> joinServerInvitation(const ServerInvitation& invitation);
    QString signalingState() const;
    bool signalingConnected() const;
    bool signalingBusy() const;
    bool serverMesh() const;
    Result<void> importServerAccess(const QString& text);
    Result<void> createAccessInvitation();
    QVariantList acquaintances() const;
    Result<void> inviteAcquaintance(const QString& peerId);
    void respondToOnlineInvitation(const QString& invitationId, bool accept);

    Result<void> sendMessage(const QString& text);

    Result<void> startCall();
    void leaveCall();
    void setMuted(bool muted);
    void setDeafened(bool deafened);
    void setMicrophoneTest(bool enabled);
    void setPttPressed(bool pressed);
    void setPeerVolume(const QString& peerId, int percent);
    double microphoneLevel() const;
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
    void signalingReady(QString kind, QString text, QByteArray document);
    void statusChanged(QString status);
    void meshChanged(int connected, int expected);
    void peerChanged(QString peerId, QString displayName, bool connected);
    void peerRttChanged(QString peerId, int milliseconds);
    void messageReceived(tmc::ChatMessage message, bool local);
    void deliveryChanged(QString messageId, int acknowledged, int expected);
    void callStateChanged(bool active, bool muted);
    void audioStateChanged();
    void localTalkingChanged(bool talking);
    void peerTalkingChanged(QString peerId, bool talking);
    void peerVoiceChanged(QString peerId, bool joined, bool muted);
    void peerAudioStatsChanged(QString peerId, double packetLossPercent, int jitterMs,
                               int bufferMs);
    void connectionAttemptChanged(QString connectionId, tmc::ConnectionAttemptState state);
    void invitationStateChanged(bool pending, QString state);
    void errorOccurred(QString message);
    void signalingServerChanged();
    void serverInvitationReady(QString link);
    void accessInvitationReady(QString link);
    void acquaintancesChanged();
    void onlineInvitationReceived(QString invitationId, QString displayName, QString server);
    void onlineInvitationClosed(QString invitationId);

private:
    enum class PacketChannel {
        Control,
        Chat,
    };

    void connectConnectionSignals();
    void connectMeshSignals();
    void connectVoiceSignals();
    void configureKeepalive();
    void connectApplicationSignals();

    void handleLinkOpened(const QString& connectionId, const PeerIdentity& remote);
    void handleLinkRemoved(const QString& connectionId, const PeerIdentity& remote, bool wasOpen);
    void handleKeepaliveTimeout();
    void emitSignaling(const QString& connectionId, const QString& sdp);

    void handleIncoming(const QString& connectionId, const QString& text, PacketChannel channel);
    void handlePacket(const QString& connectionId, const Packet& packet);
    void sendPeerProof(const QString& connectionId);
    void handlePeerProof(const QString& connectionId, const Packet& packet);
    void handlePeerHello(const QString& connectionId, const Packet& packet);
    void handlePeerSnapshot(const QString& connectionId, const Packet& packet);
    void handlePeerAnnounce(const QString& connectionId, const Packet& packet);
    void handlePeerLeave(const QString& connectionId, const Packet& packet);
    void handleChatMessage(const QString& connectionId, const Packet& packet);
    void handleChatAck(const Packet& packet);
    void handleVoiceState(const Packet& packet);
    void handleVoiceQuality(const Packet& packet);
    void handleRouteRequest(const QString& connectionId, const Packet& packet);
    void handleRouteReply(const QString& connectionId, const Packet& packet);
    void handleLinkOffer(const QString& connectionId, const Packet& packet);
    void handleLinkAnswer(const QString& connectionId, const Packet& packet);
    void handleSessionOffer(const QString& connectionId, const Packet& packet);
    void handleSessionAnswer(const QString& connectionId, const Packet& packet);
    void handlePing(const QString& connectionId, const Packet& packet);
    void handlePong(const QString& connectionId, const Packet& packet);
    bool routeLinkSignaling(const QString& connectionId, const Packet& packet);

    bool sendPacket(const QString& connectionId, const Packet& packet);
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
    void updateAudioTransportGates();
    void clearSessionData();
    void initializeSignalingClient();
    void submitServerJoin();
    void handleServerResponse(const QString& type, const signaling_protocol::Envelope& response);
    void handleServerEvent(const signaling_protocol::Envelope& event);
    void handleServerPayload(const QString& peerId, const QString& payload);
    void emitServerSignaling(const QString& connectionId, const QString& sdp);
    void startServerOffer(const QString& peerId);
    void continueServerBootstrap();
    void leaveServerRoom();
    void resetRoomRecovery();
    void recoverServerRoom();
    void broadcastSignalingState();
    bool sendRecoveryPacket(const QString& peerId, PacketType type, PacketPayload payload);
    void handleRecoveryPacket(const QString& connectionId, const Packet& packet);
    bool handleRecoveryResponse(const signaling_protocol::Envelope& response);
    bool handleRecoveryFailure(const QString& requestId, const QString& code);
    void requestServerInvitationWhenReady();
    void initializePresence();
    void publishPresence();
    void resetPresence();
    void submitOnlineInvitation();
    bool handlePresenceResponse(const QString& type, const signaling_protocol::Envelope& response);
    bool handlePresenceEvent(const signaling_protocol::Envelope& event);
    bool handlePresenceFailure(const QString& requestId, const QString& type, const QString& code);

    ApplicationController& app_;
    ConnectionPolicy policy_;

    std::unique_ptr<ConnectionManager> connections_;
    MeshCoordinator mesh_;
    SignalingRouter router_;
    MessagingService messaging_;
    std::unique_ptr<VoiceSession> voice_;

    QTimer* keepalive_{};
    QString manualInvitationConnectionId_;
    QHash<QString, QList<Packet>> pendingRouted_;
    QHash<QString, QString> routeRequests_;
    QHash<QString, quint64> audioNegotiations_;
    bool pttPressed_{false};

    std::unique_ptr<SignalingClient> signaling_;
    QString serverRoomId_;
    QString serverPeerId_;
    QString serverOperation_;
    QSet<QString> serverPeers_;
    QSet<QString> serverBootstrapAttempts_;
    QHash<QString, QString> serverLinks_; // WebRTC connectionId -> room membership peerId
    QHash<QString, QString> serverSignalRequests_; // requestId -> WebRTC connectionId
    std::optional<ServerInvitation> pendingServerJoin_;
    bool serverMesh_{false};
    QTimer serverJoinDeadline_;
    bool serverInvitationRequested_{false};
    QString serverInvitationRequestId_;
    QTimer roomRecoveryTimer_;
    QElapsedTimer recoveryClock_;
    qint64 recoveryAfter_{0};
    qint64 recoveryBroadcastAt_{0};
    QString recoveryRequestId_; // Our in-flight room.create/leave/join, never replayed.
    QSet<QString> recoveryCapableConnections_;
    struct RecoveryPeer {
        SignalingStatePayload state;
        qint64 seenAt{0};
    };
    struct RecoveryInvitation {
        QString peerId;
        SignalingJoinPayload payload;
        qint64 createdAt{0};
    };
    QHash<QString, RecoveryPeer> recoveryPeers_; // Persistent identity -> current WS state.
    QHash<QString, RecoveryInvitation> recoveryInvites_; // WS requestId -> P2P recipient.
    QHash<QString, RecoveryInvitation> recoveryTokens_; // Recipient -> unconsumed token.
    std::optional<RecoveryInvitation> recoveryJoin_;
    QTimer presencePublishTimer_;
    QJsonObject lastPresence_;
    QHash<QString, bool> contactPresence_;
    bool presenceRegistered_{false};
    bool presenceConflictReported_{false};
    QString pendingContactTarget_;
    QSet<QString> onlineInvitationTargets_;
    QHash<QString, QString> onlineInvitationRequests_; // Request -> target identity.
    QHash<QString, QString> onlineResponseRequests_; // Request -> incoming invitation ID.
    struct OnlineInvitation {
        QString id, senderId, meshId;
        bool accepting{false};
    };
    std::optional<OnlineInvitation> incomingOnlineInvitation_;

    struct PendingPing {
        QString nonce;
        qint64 sentAtNs{0};
    };

    QHash<QString, PendingPing> pendingPings_;
    QHash<QString, QString> localHelloNonces_;
    QHash<QString, HelloPayload> remoteHellos_;
};

} // namespace tmc
