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
    dispatcher.registerHandler(PacketType::PeerSnapshot, membership);
    dispatcher.registerHandler(PacketType::PeerAnnounce, membership);
    dispatcher.registerHandler(PacketType::PeerLeave, membership);

    const auto messaging = [this](const PacketContext& context, const Packet& packet) {
        handleMessaging(context, packet);
    };
    dispatcher.registerHandler(PacketType::ChatMessage, messaging);
    dispatcher.registerHandler(PacketType::ChatAck, messaging);

    const auto voice = [this](const PacketContext& context, const Packet& packet) {
        handleVoice(context, packet);
    };
    dispatcher.registerHandler(PacketType::VoiceState, voice);
    dispatcher.registerHandler(PacketType::VoiceQuality, voice);

    const auto routing = [this](const PacketContext& context, const Packet& packet) {
        handleRouting(context, packet);
    };
    dispatcher.registerHandler(PacketType::RouteRequest, routing);
    dispatcher.registerHandler(PacketType::RouteReply, routing);

    const auto meshSignaling = [this](const PacketContext& context, const Packet& packet) {
        handleMeshSignaling(context, packet);
    };
    dispatcher.registerHandler(PacketType::LinkOffer, meshSignaling);
    dispatcher.registerHandler(PacketType::LinkAnswer, meshSignaling);

    const auto sessionSignaling = [this](const PacketContext& context, const Packet& packet) {
        handleSessionSignaling(context, packet);
    };
    dispatcher.registerHandler(PacketType::SessionOffer, sessionSignaling);
    dispatcher.registerHandler(PacketType::SessionAnswer, sessionSignaling);

    const auto heartbeat = [this](const PacketContext& context, const Packet& packet) {
        handleHeartbeat(context, packet);
    };
    dispatcher.registerHandler(PacketType::Ping, heartbeat);
    dispatcher.registerHandler(PacketType::Pong, heartbeat);
}

void SessionPacketHandlers::handleMembership(const PacketContext& context, const Packet& packet) {
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
        remote.deviceId = payload.deviceId;
        remote.createdAt = payload.identityCreatedAt;
        session_.connections_->setRemote(context.connectionId, remote);
        session_.mesh_.rememberPeer(remote);
        emit session_.peerChanged(remote.peerId, remote.displayName, true);
        session_.connections_->markHelloReceived(context.connectionId, remote);
        return;
    }

    if (packet.type == PacketType::PeerAnnounce) {
        if (!session_.router_.rememberPacket(packet.packetId)) {
            return;
        }
        auto payload = std::get<PeerAnnouncePayload>(packet.payload);
        if (payload.peer.peerId != packet.senderId) {
            return;
        }
        session_.router_.observeRoute(packet.senderId, context.connectionId, payload.hops + 1);
        session_.mesh_.routeAvailable(packet.senderId);
        session_.mesh_.rememberPeer(payload.peer);
        if (packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            ++payload.hops;
            forwarded.payload = payload;
            session_.broadcastService(forwarded, context.connectionId);
        }
        session_.ensureDynamicMesh();
        session_.updateMesh();
        return;
    }

    if (packet.type == PacketType::PeerLeave) {
        if (session_.router_.rememberPacket(packet.packetId) && packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            session_.broadcastService(forwarded, context.connectionId);
        }
        const auto leaving = session_.mesh_.peer(packet.senderId);
        const auto direct = session_.connections_->infoForPeer(packet.senderId);
        if (direct) {
            session_.connections_->discard(direct->connectionId);
        }
        if (session_.mesh_.forgetPeer(packet.senderId)) {
            session_.router_.forgetPeer(packet.senderId);
            session_.voice_->removePeer(packet.senderId);
            emit session_.peerChanged(packet.senderId, leaving.displayName, false);
            session_.updateMesh();
        }
        return;
    }

    const auto& payload = std::get<PeerSnapshotPayload>(packet.payload);
    const bool changed =
        session_.mesh_.ingestPeerList(payload.peers, session_.app_.identity().peerId);
    session_.router_.observeDirect(packet.senderId, context.connectionId);
    for (const auto& peer : session_.mesh_.peers()) {
        if (peer.peerId == session_.app_.identity().peerId) {
            continue;
        }
        const auto direct = session_.connections_->infoForPeer(peer.peerId);
        if (peer.peerId != packet.senderId && (!direct || !direct->open)) {
            session_.router_.observeRoute(peer.peerId, context.connectionId, 2);
        }
        emit session_.peerChanged(peer.peerId, peer.displayName, direct && direct->open);
    }
    if (changed) {
        session_.broadcastPeerList(context.connectionId);
    }
    session_.ensureDynamicMesh();
    session_.updateMesh();
}

void SessionPacketHandlers::handleRouting(const PacketContext& context, const Packet& packet) {
    const auto payload = std::get<RoutePayload>(packet.payload);
    if (!session_.router_.rememberPacket(packet.packetId)) {
        return;
    }
    session_.router_.observeRoute(packet.senderId, context.connectionId, payload.hops + 1);

    if (packet.type == PacketType::RouteRequest) {
        session_.router_.rememberReverseRoute(payload.requestId, context.connectionId);
        if (packet.targetId == session_.app_.identity().peerId) {
            auto reply = session_.basePacket(PacketType::RouteReply,
                                             RoutePayload{payload.requestId, 0});
            reply.targetId = packet.senderId;
            reply.ttl = session_.policy_.maxPeers;
            session_.router_.rememberPacket(reply.packetId);
            session_.sendPacket(context.connectionId, reply);
        } else if (packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            forwarded.payload = RoutePayload{payload.requestId, payload.hops + 1};
            session_.broadcastService(forwarded, context.connectionId);
        }
        return;
    }

    if (packet.targetId == session_.app_.identity().peerId) {
        session_.flushRouted(packet.senderId);
        return;
    }
    const auto reverse = session_.router_.reverseHop(payload.requestId);
    if (reverse && *reverse != context.connectionId && packet.ttl > 1) {
        auto forwarded = packet;
        --forwarded.ttl;
        forwarded.payload = RoutePayload{payload.requestId, payload.hops + 1};
        session_.sendPacket(*reverse, forwarded);
    }
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
    if (packet.type == PacketType::VoiceQuality) {
        if (!packet.targetId.isEmpty() && packet.targetId != session_.app_.identity().peerId) {
            return;
        }
        const auto& payload = std::get<VoiceQualityPayload>(packet.payload);
        session_.voice_->updateNetworkFeedback(packet.senderId, payload.packetLossPercent);
        return;
    }
    const auto& payload = std::get<VoiceStatePayload>(packet.payload);
    session_.voice_->updatePeer(packet.senderId, payload.joined, payload.muted);
}

void SessionPacketHandlers::handleMeshSignaling(const PacketContext& context,
                                                const Packet& packet) {
    const auto& payload = std::get<LinkSignalingPayload>(packet.payload);
    if (!session_.router_.rememberPacket(packet.packetId)) {
        return;
    }
    session_.router_.observeRoute(packet.senderId, context.connectionId, 2);
    if (packet.targetId != session_.app_.identity().peerId) {
        if (packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            if (!session_.sendRouted(forwarded, context.connectionId)) {
                session_.broadcastService(forwarded, context.connectionId);
            }
        }
        return;
    }

    if (packet.type == PacketType::LinkOffer) {
        const auto remote = session_.mesh_.peer(packet.senderId);
        if (remote.peerId.isEmpty()) {
            return;
        }
        if (!session_.mesh_.acceptLinkGeneration(remote.peerId, payload.generation)) {
            return;
        }
        const auto existing = session_.connections_->infoForPeer(remote.peerId);
        if (session_.connections_->contains(payload.connectionId) ||
            (existing && (existing->open || !existing->everOpened))) {
            return;
        }
        if (existing) {
            session_.connections_->discard(existing->connectionId);
        }
        session_.mesh_.rememberPeer(remote);
        const auto created = session_.connections_->create(
            payload.connectionId, remote, false, true, payload.generation);
        if (!created) {
            return;
        }
        emit session_.peerChanged(remote.peerId, remote.displayName, false);
        emit session_.statusChanged("Получен автоматический offer от " +
                                    remote.displayName + "…");
        const auto accepted =
            session_.connections_->acceptOffer(payload.connectionId, payload.sdp);
        if (!accepted) {
            emit session_.statusChanged("Не удалось принять автоматический offer: " +
                                        accepted.error());
        }
        return;
    }

    const auto connection = session_.connections_->info(payload.connectionId);
    if (!connection || !connection->meshManaged || connection->answerApplied ||
        connection->generation != payload.generation) {
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
        return;
    }
    session_.receivePong(context.connectionId, std::get<HeartbeatPayload>(packet.payload));
}

void SessionPacketHandlers::handleSessionSignaling(const PacketContext& context,
                                                   const Packet& packet) {
    const auto& payload = std::get<SessionSignalingPayload>(packet.payload);
    const auto connection = session_.connections_->info(context.connectionId);
    if (!connection || !connection->open || payload.connectionId != context.connectionId ||
        (!packet.targetId.isEmpty() &&
         packet.targetId != session_.app_.identity().peerId)) {
        return;
    }
    if (packet.type == PacketType::SessionOffer) {
        session_.audioNegotiations_.insert(context.connectionId, payload.negotiation);
        const auto accepted =
            session_.connections_->acceptAudioOffer(context.connectionId, payload.sdp);
        if (!accepted) {
            emit session_.errorOccurred("Не удалось применить audio offer: " + accepted.error());
        }
        return;
    }
    if (session_.audioNegotiations_.value(context.connectionId) != payload.negotiation) {
        return;
    }
    const auto accepted =
        session_.connections_->acceptAudioAnswer(context.connectionId, payload.sdp);
    if (!accepted) {
        emit session_.errorOccurred("Не удалось применить audio answer: " + accepted.error());
    }
}

} // namespace tmc
