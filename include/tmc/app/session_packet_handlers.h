#pragma once

#include "tmc/messaging/chat_message.h"
#include "tmc/network/connection_policy.h"
#include "tmc/protocol/packet.h"

#include <functional>

namespace tmc {

class ConnectionManager;
class MeshCoordinator;
class MessagingService;
class PacketDispatcher;
struct PacketContext;
class SignalingRouter;
class VoiceSession;

class SessionPacketHandlers {
public:
    struct Callbacks {
        std::function<PeerIdentity()> localIdentity;
        std::function<Packet(PacketType, PacketPayload)> makePacket;
        std::function<bool(const QString&, const Packet&)> sendPacket;
        std::function<void(const Packet&, const QString&)> broadcastService;
        std::function<void(const QString&)> broadcastPeerList;
        std::function<void()> ensureDynamicMesh;
        std::function<void()> updateMesh;
        std::function<void(const QString&)> flushRouted;
        std::function<void(const QString&, const HeartbeatPayload&)> receivePong;
        std::function<void(const QString&, quint64)> rememberAudioNegotiation;
        std::function<bool(const QString&, quint64)> isCurrentAudioNegotiation;

        std::function<void(QString, QString, bool)> peerChanged;
        std::function<void(QString)> statusChanged;
        std::function<void(QString)> errorOccurred;
        std::function<void(ChatMessage, bool)> messageReceived;
        std::function<void(QString, int, int)> deliveryChanged;
    };

    SessionPacketHandlers(ConnectionManager& connections, MeshCoordinator& mesh,
                          SignalingRouter& router, MessagingService& messaging,
                          VoiceSession& voice, ConnectionPolicy policy, Callbacks callbacks);

    void registerWith(PacketDispatcher& dispatcher);

private:
    void handleMembership(const PacketContext& context, const Packet& packet);
    void handleMessaging(const PacketContext& context, const Packet& packet);
    void handleVoice(const PacketContext& context, const Packet& packet);
    void handleRouting(const PacketContext& context, const Packet& packet);
    void handleMeshSignaling(const PacketContext& context, const Packet& packet);
    void handleSessionSignaling(const PacketContext& context, const Packet& packet);
    void handleHeartbeat(const PacketContext& context, const Packet& packet);

    ConnectionManager& connections_;
    MeshCoordinator& mesh_;
    SignalingRouter& router_;
    MessagingService& messaging_;
    VoiceSession& voice_;
    ConnectionPolicy policy_;
    Callbacks callbacks_;
};

} // namespace tmc
