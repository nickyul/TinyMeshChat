#pragma once

#include "core/result.h"
#include "messaging/chat_message.h"
#include "messaging/delivery_tracker.h"
#include "signaling/invitation.h"
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <memory>

namespace tmc {
class ApplicationController;
class AudioEngine;
class PeerConnection;

class NetworkSession final : public QObject {
    Q_OBJECT

  public:
    explicit NetworkSession(ApplicationController& app, QObject* parent = nullptr);
    ~NetworkSession() override;

    Result<void> createRoom(const QString& name);
    Result<void> createInvitation();
    Result<void> importSignalingText(const QString& text);
    Result<void> importSignalingDocument(const QByteArray& document);
    Result<void> sendMessage(const QString& text);
    Result<void> startCall();
    void leaveCall();
    void setMuted(bool muted);
    bool callActive() const { return callActive_; }
    bool muted() const { return muted_; }
    Result<QPair<int, int>> deliveryCounts(const QString& messageId) const;

    QString roomId() const { return roomId_; }
    QString roomName() const { return roomName_; }
    int connectedPeerCount() const;
    int knownPeerCount() const;
    QString diagnostics() const;
    QString peerDisplayName(const QString& peerId) const;

  signals:
    void roomChanged(QString roomId, QString roomName);
    void signalingReady(QString kind, QString text, QByteArray document, QString suggestedName);
    void statusChanged(QString status);
    void meshChanged(int connected, int expected);
    void peerChanged(QString peerId, QString displayName, bool connected);
    void messageReceived(tmc::ChatMessage message, bool local);
    void deliveryChanged(QString messageId, int acknowledged, int expected);
    void callStateChanged(bool active, bool muted);
    void peerVoiceChanged(QString peerId, bool joined, bool muted);
    void errorOccurred(QString message);

  private:
    struct Link;

    Result<Invitation> decodeSignaling(const QByteArray& document) const;
    bool rememberPeer(const PeerIdentity& peer);
    bool rememberMessage(const QString& messageId);
    QList<PeerIdentity> knownPeers() const;
    std::shared_ptr<Link> makeLink(const QString& connectionId);
    void discardLink(const std::shared_ptr<Link>& link);
    void discardStaleLinks(const QString& peerId = {});
    void configureLink(const std::shared_ptr<Link>& link);
    void emitSignaling(const std::shared_ptr<Link>& link, const QString& type, const QString& sdp);
    void handleIncoming(const std::shared_ptr<Link>& link, const QString& text);
    void handlePacket(const std::shared_ptr<Link>& link, const class Packet& packet);
    void handleMeshOffer(const std::shared_ptr<Link>& source, const class Packet& packet);
    void handleMeshAnswer(const std::shared_ptr<Link>& source, const class Packet& packet);
    void sendPacket(const std::shared_ptr<Link>& link, const class Packet& packet);
    class Packet basePacket(const QString& type, const QJsonObject& payload) const;
    void sendHello(const std::shared_ptr<Link>& link);
    void sendPeerList(const std::shared_ptr<Link>& link);
    void sendVoiceState(const std::shared_ptr<Link>& link);
    void broadcastVoiceState();
    void broadcastPeerList(const QString& excludedConnection = {});
    void handlePeerList(const std::shared_ptr<Link>& source, const class Packet& packet);
    void ensureDynamicMesh();
    void broadcastService(const QString& type, QJsonObject payload,
                          const QString& excludedConnection = {});
    bool rememberRoute(const QString& routeId);
    std::shared_ptr<Link> linkForPeer(const QString& peerId) const;
    void updateMesh();

    ApplicationController& app_;
    std::unique_ptr<AudioEngine> audio_;
    QHash<QString, std::shared_ptr<Link>> links_;
    QHash<QString, PeerIdentity> peers_;
    DeliveryTracker delivery_;
    QString roomId_;
    QString roomName_;
    qint64 logicalClock_{0};
    QTimer* keepalive_{};
    QSet<QString> seenRoutes_;
    QSet<QString> seenMessageIds_;
    QQueue<QString> seenMessageOrder_;
    QHash<QString, QPair<bool, bool>> peerVoiceStates_;
    bool callActive_{false};
    bool muted_{false};
};
} // namespace tmc

Q_DECLARE_METATYPE(tmc::ChatMessage)
