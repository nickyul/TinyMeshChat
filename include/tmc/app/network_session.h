#pragma once

#include "tmc/app/connection_manager.h"
#include "tmc/app/mesh_coordinator.h"
#include "tmc/app/mesh_session_state.h"
#include "tmc/app/messaging_service.h"
#include "tmc/core/result.h"
#include "tmc/messaging/chat_message.h"
#include "tmc/network/connection_policy.h"
#include "tmc/signaling/invitation.h"

#include <QHash>
#include <QObject>
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
    Result<void> importSignalingText(const QString& text);
    Result<void> importSignalingDocument(const QByteArray& document);

    Result<void> sendMessage(const QString& text);

    Result<void> startCall();
    void leaveCall();
    void setMuted(bool muted);

    bool callActive() const;
    bool muted() const;

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
    void messageReceived(tmc::ChatMessage message, bool local);
    void deliveryChanged(QString messageId, int acknowledged, int expected);
    void callStateChanged(bool active, bool muted);
    void peerVoiceChanged(QString peerId, bool joined, bool muted);
    void connectionAttemptChanged(QString connectionId, tmc::ConnectionAttemptState state);
    void errorOccurred(QString message);

private:
    Result<Invitation> decodeSignaling(const QByteArray& document) const;
    void emitSignaling(const QString& connectionId, const QString& type, const QString& sdp);

    void handleIncoming(const QString& connectionId, const QString& text);
    void handlePacket(const QString& connectionId, const class Packet& packet);
    void handleMeshOffer(const QString& sourceConnectionId, const class Packet& packet);
    void handleMeshAnswer(const QString& sourceConnectionId, const class Packet& packet);

    void sendPacket(const QString& connectionId, const class Packet& packet);
    class Packet basePacket(const QString& type, const QJsonObject& payload) const;

    void sendHello(const QString& connectionId);
    void sendPeerList(const QString& connectionId);
    void sendVoiceState(const QString& connectionId);
    void broadcastVoiceState();
    void broadcastPeerList(const QString& excludedConnection = {});

    void handlePeerList(const QString& sourceConnectionId, const class Packet& packet);
    void ensureDynamicMesh();
    void startMeshOffer(const PeerIdentity& peer);
    void scheduleMeshRetry(const PeerIdentity& peer);

    void broadcastService(const QString& type, QJsonObject payload,
                          const QString& excludedConnection = {});

    void updateMesh();
    void clearSessionData();

    ApplicationController& app_;
    ConnectionPolicy policy_;
    std::unique_ptr<ConnectionManager> connections_;
    MeshCoordinator mesh_;
    MessagingService messaging_;
    std::unique_ptr<VoiceSession> voice_;
    QTimer* keepalive_{};
};

} // namespace tmc

Q_DECLARE_METATYPE(tmc::ChatMessage)
