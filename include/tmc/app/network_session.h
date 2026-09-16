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
#include "tmc/rendezvous/rendezvous_service.h"
#include "tmc/signaling/invitation.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace tmc {

class ApplicationController;
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

    QList<ContactPresence> contacts() const;
    Result<void> requestContactConnection(const QString& peerId);
    Result<void> acceptContactConnection(const QString& requestId);
    void declineContactConnection(const QString& requestId);

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
    void contactChanged(tmc::ContactPresence contact);
    void contactConnectionRequest(QString peerId, QString displayName, QString requestId,
                                  QString meshId);
    void contactConnectionRequestExpired(QString requestId);
    void errorOccurred(QString message);

private:
    enum class PacketChannel {
        Control,
        Chat,
    };

    void connectConnectionSignals();
    void connectMeshSignals();
    void connectVoiceSignals();
    void connectRendezvousSignals();
    void configureKeepalive();
    void connectApplicationSignals();

    void handleLinkOpened(const QString& connectionId, const PeerIdentity& remote);
    void handleLinkRemoved(const QString& connectionId, const PeerIdentity& remote, bool wasOpen);
    void handleKeepaliveTimeout();
    void emitSignaling(const QString& connectionId, const QString& sdp);

    void handleIncoming(const QString& connectionId, const QString& text, PacketChannel channel);
    void handlePacket(const QString& connectionId, const Packet& packet);
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
    void handleRendezvousMetadata(const QString& connectionId, const Packet& packet);
    bool routeLinkSignaling(const QString& connectionId, const Packet& packet);

    bool sendPacket(const QString& connectionId, const Packet& packet);
    Packet basePacket(PacketType type, PacketPayload payload) const;

    void sendHello(const QString& connectionId);
    void sendPing(const QString& connectionId);
    void receivePong(const QString& connectionId, const HeartbeatPayload& payload);
    void sendPeerList(const QString& connectionId);
    void announceLocalPeer(const QString& excludedConnection = {});
    void sendVoiceState(const QString& connectionId);
    void sendRendezvousMetadata(const QString& connectionId, bool acknowledgement);
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
    void startRendezvousOffer(const QString& requestId);
    void handleRendezvousResponse(const QString& peerId, const QString& requestId,
                                  const QString& response, const QString& meshId);
    void handleRendezvousSignaling(const QString& peerId, const QByteArray& document);

    ApplicationController& app_;
    ConnectionPolicy policy_;

    std::unique_ptr<ConnectionManager> connections_;
    MeshCoordinator mesh_;
    SignalingRouter router_;
    MessagingService messaging_;
    std::unique_ptr<VoiceSession> voice_;
    std::unique_ptr<RendezvousService> rendezvous_;

    QTimer* keepalive_{};
    QString manualInvitationConnectionId_;
    QHash<QString, QList<Packet>> pendingRouted_;
    QHash<QString, QString> routeRequests_;
    QHash<QString, quint64> audioNegotiations_;
    struct RendezvousNegotiation {
        PeerIdentity peer;
        QString requestId;
        QString meshId;
        bool initiatedLocally{false};
        bool accepted{false};
    };
    QHash<QString, RendezvousNegotiation> rendezvousNegotiations_;
    QHash<QString, QString> rendezvousConnections_;
    QHash<QString, QByteArray> deferredRendezvousOffers_;
    bool pttPressed_{false};

    struct PendingPing {
        QString nonce;
        qint64 sentAtNs{0};
    };

    QHash<QString, PendingPing> pendingPings_;
};

} // namespace tmc
