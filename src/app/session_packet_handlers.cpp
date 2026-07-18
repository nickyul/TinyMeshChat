#include "tmc/app/session_packet_handlers.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/network_session.h"
#include "tmc/app/voice_session.h"
#include "tmc/protocol/packet_dispatcher.h"

namespace tmc {

SessionPacketHandlers::SessionPacketHandlers(NetworkSession& session) : session_(session) {
}

void SessionPacketHandlers::registerWith(PacketDispatcher& dispatcher) {
    const auto membership = [this](const PacketContext& context, const Packet& packet) {
        handleMembership(context, packet);
    };
    dispatcher.registerHandler(PacketType::PeerHello, membership);
    dispatcher.registerHandler(PacketType::PeerHelloAck, membership);
    dispatcher.registerHandler(PacketType::PeerList, membership);

    const auto messaging = [this](const PacketContext& context, const Packet& packet) {
        handleMessaging(context, packet);
    };
    dispatcher.registerHandler(PacketType::ChatMessage, messaging);
    dispatcher.registerHandler(PacketType::ChatAck, messaging);

    dispatcher.registerHandler(PacketType::VoiceState,
                               [this](const PacketContext& context, const Packet& packet) {
                                   handleVoice(context, packet);
                               });

    const auto meshSignaling = [this](const PacketContext& context, const Packet& packet) {
        handleMeshSignaling(context, packet);
    };
    dispatcher.registerHandler(PacketType::MeshOffer, meshSignaling);
    dispatcher.registerHandler(PacketType::MeshAnswer, meshSignaling);

    const auto heartbeat = [this](const PacketContext& context, const Packet& packet) {
        handleHeartbeat(context, packet);
    };
    dispatcher.registerHandler(PacketType::Ping, heartbeat);
    dispatcher.registerHandler(PacketType::Pong, heartbeat);
}

void SessionPacketHandlers::handleMembership(const PacketContext& context, const Packet& packet) {
    if (packet.type == PacketType::PeerHelloAck) {
        return;
    }
    if (packet.type == PacketType::PeerHello) {
        const auto connection = session_.connections_->info(context.connectionId);
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
        session_.connections_->setRemote(context.connectionId, remote);
        const bool joined = session_.mesh_.rememberPeer(remote);
        emit session_.peerChanged(remote.peerId, remote.displayName, true);
        session_.sendPacket(context.connectionId,
                            session_.basePacket(PacketType::PeerHelloAck, EmptyPayload{}));
        session_.sendPeerList(context.connectionId);
        if (joined) {
            session_.broadcastPeerList(context.connectionId);
        }
        session_.ensureDynamicMesh();
        return;
    }

    const auto& payload = std::get<PeerListPayload>(packet.payload);
    const bool changed =
        session_.mesh_.ingestPeerList(payload.peers, session_.app_.identity().peerId);
    for (const auto& peer : session_.mesh_.peers()) {
        if (peer.peerId == session_.app_.identity().peerId) {
            continue;
        }
        const auto direct = session_.connections_->infoForPeer(peer.peerId);
        emit session_.peerChanged(peer.peerId, peer.displayName, direct && direct->open);
    }
    if (changed) {
        session_.broadcastPeerList(context.connectionId);
    }
    session_.ensureDynamicMesh();
    session_.updateMesh();
}

void SessionPacketHandlers::handleMessaging(const PacketContext& context, const Packet& packet) {
    if (packet.type == PacketType::ChatMessage) {
        const auto& payload = std::get<ChatMessagePayload>(packet.payload);
        auto received = session_.messaging_.receiveMessage(
            packet, session_.mesh_.meshId(),
            session_.basePacket(PacketType::ChatAck, ChatAckPayload{payload.messageId}));
        if (!received) {
            emit session_.errorOccurred(received.error());
            return;
        }
        if (received.value().message) {
            emit session_.messageReceived(*received.value().message, false);
        }
        session_.sendPacket(context.connectionId, received.value().acknowledgement);
        return;
    }

    const auto& payload = std::get<ChatAckPayload>(packet.payload);
    if (session_.messaging_.receiveAcknowledgement(packet)) {
        const auto counts = session_.messaging_.deliveryCounts(payload.messageId);
        emit session_.deliveryChanged(payload.messageId, counts.first, counts.second);
    }
}

void SessionPacketHandlers::handleVoice(const PacketContext&, const Packet& packet) {
    const auto& payload = std::get<VoiceStatePayload>(packet.payload);
    session_.voice_->updatePeer(packet.senderId, payload.joined, payload.muted);
}

void SessionPacketHandlers::handleMeshSignaling(const PacketContext& context,
                                                const Packet& packet) {
    auto payload = std::get<MeshSignalingPayload>(packet.payload);
    if (!session_.mesh_.rememberRoute(payload.routeId)) {
        return;
    }
    if (payload.targetPeerId != session_.app_.identity().peerId) {
        if (payload.hopCount < session_.policy_.maxPeers) {
            ++payload.hopCount;
            session_.broadcastService(packet.type, payload, context.connectionId);
        }
        return;
    }

    if (packet.type == PacketType::MeshOffer) {
        const auto existing = session_.connections_->infoForPeer(payload.fromPeer.peerId);
        if (session_.connections_->contains(payload.connectionId) ||
            (existing && (existing->open || !existing->everOpened))) {
            return;
        }
        if (existing) {
            session_.connections_->discard(existing->connectionId);
        }
        session_.mesh_.rememberPeer(payload.fromPeer);
        const auto created = session_.connections_->create(payload.connectionId, payload.fromPeer,
                                                           false, true);
        if (!created) {
            return;
        }
        emit session_.peerChanged(payload.fromPeer.peerId, payload.fromPeer.displayName, false);
        emit session_.statusChanged("Получен автоматический offer от " +
                                    payload.fromPeer.displayName + "…");
        const auto accepted =
            session_.connections_->acceptOffer(payload.connectionId, payload.sdp);
        if (!accepted) {
            emit session_.statusChanged("Не удалось принять автоматический offer: " +
                                        accepted.error());
        }
        return;
    }

    const auto connection = session_.connections_->info(payload.connectionId);
    if (!connection || !connection->meshManaged || connection->answerApplied) {
        return;
    }
    emit session_.statusChanged("Получен mesh answer от " + connection->remote.displayName + "…");
    const auto accepted = session_.connections_->acceptAnswer(payload.connectionId, payload.sdp);
    if (!accepted) {
        emit session_.statusChanged("Не удалось принять mesh answer: " + accepted.error());
        if (connection->localOffer) {
            session_.mesh_.scheduleRetry(connection->remote);
        }
    }
}

void SessionPacketHandlers::handleHeartbeat(const PacketContext& context, const Packet& packet) {
    if (packet.type == PacketType::Ping) {
        session_.sendPacket(context.connectionId,
                            session_.basePacket(PacketType::Pong,
                                                std::get<HeartbeatPayload>(packet.payload)));
    }
}

} // namespace tmc
