#include "tmc/app/session_packet_handlers.h"

#include "tmc/app/connection_manager.h"
#include "tmc/app/mesh_coordinator.h"
#include "tmc/app/messaging_service.h"
#include "tmc/app/signaling_router.h"
#include "tmc/app/voice_session.h"
#include "tmc/core/logger.h"

#include <utility>

namespace tmc {

SessionPacketHandlers::SessionPacketHandlers(ConnectionManager& connections, MeshCoordinator& mesh,
                                             SignalingRouter& router,
                                             MessagingService& messaging, VoiceSession& voice,
                                             ConnectionPolicy policy, Callbacks callbacks)
    : connections_(connections), mesh_(mesh), router_(router), messaging_(messaging),
      voice_(voice), policy_(policy), callbacks_(std::move(callbacks)) {
}

bool SessionPacketHandlers::handle(const QString& connectionId, const Packet& packet) {
    switch (packet.type) {
    case PacketType::PeerHello:
    case PacketType::PeerSnapshot:
    case PacketType::PeerAnnounce:
    case PacketType::PeerLeave:
        handleMembership(connectionId, packet);
        return true;
    case PacketType::ChatMessage:
    case PacketType::ChatAck:
        handleMessaging(connectionId, packet);
        return true;
    case PacketType::VoiceState:
    case PacketType::VoiceQuality:
        handleVoice(packet);
        return true;
    case PacketType::RouteRequest:
    case PacketType::RouteReply:
        handleRouting(connectionId, packet);
        return true;
    case PacketType::LinkOffer:
    case PacketType::LinkAnswer:
        handleMeshSignaling(connectionId, packet);
        return true;
    case PacketType::SessionOffer:
    case PacketType::SessionAnswer:
        handleSessionSignaling(connectionId, packet);
        return true;
    case PacketType::Ping:
    case PacketType::Pong:
        handleHeartbeat(connectionId, packet);
        return true;
    }
    return false;
}

void SessionPacketHandlers::handleMembership(const QString& connectionId, const Packet& packet) {
    if (packet.type == PacketType::PeerHello) {
        const auto connection = connections_.info(connectionId);
        if (!connection) {
            return;
        }
        const auto& payload = std::get<HelloPayload>(packet.payload);
        auto remote = connection->remote;
        if (remote.peerId.isEmpty()) {
            remote.peerId = packet.senderId;
        }
        if (!payload.displayName.isEmpty()) {
            remote.displayName = payload.displayName;
        }
        connections_.setRemote(connectionId, remote);
        mesh_.rememberPeer(remote);
        callbacks_.peerChanged(remote.peerId, remote.displayName, true);
        connections_.markHelloReceived(connectionId, remote);
        return;
    }

    if (packet.type == PacketType::PeerAnnounce) {
        if (!router_.rememberPacket(packet.packetId)) {
            return;
        }
        auto payload = std::get<PeerAnnouncePayload>(packet.payload);
        if (payload.peer.peerId != packet.senderId) {
            return;
        }
        router_.observeRoute(packet.senderId, connectionId, payload.hops + 1);
        mesh_.routeAvailable(packet.senderId);
        mesh_.rememberPeer(payload.peer);
        if (packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            ++payload.hops;
            forwarded.payload = payload;
            callbacks_.broadcastService(forwarded, connectionId);
        }
        callbacks_.ensureDynamicMesh();
        callbacks_.updateMesh();
        return;
    }

    if (packet.type == PacketType::PeerLeave) {
        if (router_.rememberPacket(packet.packetId) && packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            callbacks_.broadcastService(forwarded, connectionId);
        }
        const auto leaving = mesh_.peer(packet.senderId);
        const auto direct = connections_.infoForPeer(packet.senderId);
        if (direct) {
            connections_.discard(direct->connectionId);
        }
        if (mesh_.forgetPeer(packet.senderId)) {
            router_.forgetPeer(packet.senderId);
            voice_.removePeer(packet.senderId);
            callbacks_.peerChanged(packet.senderId, leaving.displayName, false);
            callbacks_.updateMesh();
        }
        return;
    }

    const auto& payload = std::get<PeerSnapshotPayload>(packet.payload);
    const bool changed =
        mesh_.ingestPeerList(payload.peers, callbacks_.localIdentity().peerId);
    router_.observeDirect(packet.senderId, connectionId);
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == callbacks_.localIdentity().peerId) {
            continue;
        }
        const auto direct = connections_.infoForPeer(peer.peerId);
        if (peer.peerId != packet.senderId && (!direct || !direct->open)) {
            router_.observeRoute(peer.peerId, connectionId, 2);
        }
        callbacks_.peerChanged(peer.peerId, peer.displayName, direct && direct->open);
    }
    if (changed) {
        callbacks_.broadcastPeerList(connectionId);
    }
    callbacks_.ensureDynamicMesh();
    callbacks_.updateMesh();
}

void SessionPacketHandlers::handleRouting(const QString& connectionId, const Packet& packet) {
    const auto payload = std::get<RoutePayload>(packet.payload);
    if (!router_.rememberPacket(packet.packetId)) {
        return;
    }
    router_.observeRoute(packet.senderId, connectionId, payload.hops + 1);

    if (packet.type == PacketType::RouteRequest) {
        router_.rememberReverseRoute(payload.requestId, connectionId);
        if (packet.targetId == callbacks_.localIdentity().peerId) {
            auto reply =
                callbacks_.makePacket(PacketType::RouteReply, RoutePayload{payload.requestId, 0});
            reply.targetId = packet.senderId;
            reply.ttl = policy_.maxPeers;
            router_.rememberPacket(reply.packetId);
            callbacks_.sendPacket(connectionId, reply);
        } else if (packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            forwarded.payload = RoutePayload{payload.requestId, payload.hops + 1};
            callbacks_.broadcastService(forwarded, connectionId);
        }
        return;
    }

    if (packet.targetId == callbacks_.localIdentity().peerId) {
        callbacks_.flushRouted(packet.senderId);
        return;
    }
    const auto reverse = router_.reverseHop(payload.requestId);
    if (reverse && *reverse != connectionId && packet.ttl > 1) {
        auto forwarded = packet;
        --forwarded.ttl;
        forwarded.payload = RoutePayload{payload.requestId, payload.hops + 1};
        callbacks_.sendPacket(*reverse, forwarded);
    }
}

void SessionPacketHandlers::handleMessaging(const QString& connectionId, const Packet& packet) {
    if (packet.type == PacketType::ChatMessage) {
        const auto& payload = std::get<ChatMessagePayload>(packet.payload);
        auto received = messaging_.receiveMessage(packet);
        if (!received) {
            callbacks_.errorOccurred(received.error());
            return;
        }
        if (received.value()) {
            callbacks_.messageReceived(*received.value(), false);
        }
        callbacks_.sendPacket(
            connectionId,
            callbacks_.makePacket(PacketType::ChatAck, ChatAckPayload{payload.messageId}));
        return;
    }

    const auto& payload = std::get<ChatAckPayload>(packet.payload);
    if (messaging_.receiveAcknowledgement(payload.messageId, packet.senderId)) {
        const auto counts = messaging_.deliveryCounts(payload.messageId);
        callbacks_.deliveryChanged(payload.messageId, counts.first, counts.second);
    }
}

void SessionPacketHandlers::handleVoice(const Packet& packet) {
    if (packet.type == PacketType::VoiceQuality) {
        if (!packet.targetId.isEmpty() && packet.targetId != callbacks_.localIdentity().peerId) {
            return;
        }
        const auto& payload = std::get<VoiceQualityPayload>(packet.payload);
        voice_.updateNetworkFeedback(packet.senderId, payload.packetLossPercent);
        return;
    }
    const auto& payload = std::get<VoiceStatePayload>(packet.payload);
    voice_.updatePeer(packet.senderId, payload.joined, payload.muted);
}

void SessionPacketHandlers::handleMeshSignaling(const QString& connectionId,
                                                const Packet& packet) {
    const auto& payload = std::get<LinkSignalingPayload>(packet.payload);
    if (!router_.rememberPacket(packet.packetId)) {
        return;
    }
    Logger::instance().log(
        QtInfoMsg, "mesh_signaling",
        QString("receive type=%1 link=%2 generation=%3 sender=%4 target=%5 via=%6 ttl=%7")
            .arg(toString(packet.type), payload.connectionId.left(8))
            .arg(payload.generation)
            .arg(packet.senderId.left(8), packet.targetId.left(8),
                 connectionId.left(8))
            .arg(packet.ttl));
    router_.observeRoute(packet.senderId, connectionId, 2);
    if (packet.targetId != callbacks_.localIdentity().peerId) {
        if (packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            callbacks_.broadcastService(forwarded, connectionId);
        }
        return;
    }

    if (packet.type == PacketType::LinkOffer) {
        const auto remote = mesh_.peer(packet.senderId);
        if (remote.peerId.isEmpty()) {
            return;
        }
        if (!mesh_.acceptLinkGeneration(remote.peerId, payload.generation)) {
            return;
        }
        const auto existing = connections_.infoForPeer(remote.peerId);
        if (connections_.contains(payload.connectionId) ||
            (existing && (existing->open || !existing->everOpened))) {
            return;
        }
        if (existing) {
            connections_.discard(existing->connectionId);
        }
        mesh_.rememberPeer(remote);
        const auto created =
            connections_.create(payload.connectionId, remote, false, true, payload.generation);
        if (!created) {
            return;
        }
        callbacks_.peerChanged(remote.peerId, remote.displayName, false);
        callbacks_.statusChanged("Получен автоматический offer от " + remote.displayName + "…");
        const auto accepted = connections_.acceptOffer(payload.connectionId, payload.sdp);
        if (!accepted) {
            callbacks_.statusChanged("Не удалось принять автоматический offer: " +
                                     accepted.error());
        }
        return;
    }

    const auto connection = connections_.info(payload.connectionId);
    if (!connection || !connection->meshManaged || connection->answerApplied ||
        connection->generation != payload.generation) {
        return;
    }
    callbacks_.statusChanged("Получен mesh answer от " + connection->remote.displayName + "…");
    const auto accepted = connections_.acceptAnswer(payload.connectionId, payload.sdp);
    if (!accepted) {
        callbacks_.statusChanged("Не удалось принять mesh answer: " + accepted.error());
        if (connection->localOffer) {
            mesh_.scheduleRetry(connection->remote);
        }
    }
}

void SessionPacketHandlers::handleHeartbeat(const QString& connectionId, const Packet& packet) {
    if (packet.type == PacketType::Ping) {
        callbacks_.sendPacket(
            connectionId,
            callbacks_.makePacket(PacketType::Pong,
                                  std::get<HeartbeatPayload>(packet.payload)));
        return;
    }
    callbacks_.receivePong(connectionId, std::get<HeartbeatPayload>(packet.payload));
}

void SessionPacketHandlers::handleSessionSignaling(const QString& connectionId,
                                                   const Packet& packet) {
    const auto& payload = std::get<SessionSignalingPayload>(packet.payload);
    const auto connection = connections_.info(connectionId);
    if (!connection || !connection->open || payload.connectionId != connectionId ||
        (!packet.targetId.isEmpty() &&
         packet.targetId != callbacks_.localIdentity().peerId)) {
        return;
    }
    if (packet.type == PacketType::SessionOffer) {
        callbacks_.rememberAudioNegotiation(connectionId, payload.negotiation);
        const auto accepted = connections_.acceptAudioOffer(connectionId, payload.sdp);
        if (!accepted) {
            callbacks_.errorOccurred("Не удалось применить audio offer: " + accepted.error());
        }
        return;
    }
    if (!callbacks_.isCurrentAudioNegotiation(connectionId, payload.negotiation)) {
        return;
    }
    const auto accepted = connections_.acceptAudioAnswer(connectionId, payload.sdp);
    if (!accepted) {
        callbacks_.errorOccurred("Не удалось применить audio answer: " + accepted.error());
    }
}

} // namespace tmc
