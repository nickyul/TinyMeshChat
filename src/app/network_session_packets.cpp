#include "tmc/app/network_session.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/voice_session.h"
#include "tmc/core/logger.h"
#include "tmc/protocol/packet_codec.h"

namespace tmc {

void NetworkSession::handleIncoming(const QString& connectionId, const QString& text,
                                    PacketChannel channel) {
    const auto connection = connections_->info(connectionId);
    if (!connection) return;
    auto decoded = PacketCodec::decode(text.toUtf8(), mesh_.meshId());
    if (!decoded) {
        Logger::instance().log(QtWarningMsg, "protocol", decoded.error());
        emit errorOccurred("Получен некорректный сетевой пакет: " + decoded.error());
        return;
    }

    const bool chatPacket = decoded.value().type == PacketType::ChatMessage ||
                            decoded.value().type == PacketType::ChatAck;
    const bool chatChannel = channel == PacketChannel::Chat;
    if (chatPacket != chatChannel) {
        emit errorOccurred(chatChannel ? "Служебный пакет получен в chat DataChannel."
                                       : "Пакет чата получен в control DataChannel.");
        return;
    }

    const auto type = decoded.value().type;
    const bool routed = type == PacketType::PeerAnnounce || type == PacketType::PeerLeave ||
                        type == PacketType::RouteRequest || type == PacketType::RouteReply ||
                        type == PacketType::LinkOffer || type == PacketType::LinkAnswer;
    if (!routed && connection && !connection->remote.peerId.isEmpty() &&
        decoded.value().senderId != connection->remote.peerId) {
        emit errorOccurred("Packet sender does not match the direct WebRTC peer.");
        return;
    }
    if (connection && !connection->open && decoded.value().type != PacketType::PeerHello &&
        decoded.value().type != PacketType::PeerProof) {
        emit errorOccurred("До подтверждения ключа разрешён только обмен peer.hello / peer.proof.");
        return;
    }

    handlePacket(connectionId, decoded.value());
}

void NetworkSession::handlePacket(const QString& connectionId, const Packet& packet) {
    switch (packet.type) {
    case PacketType::PeerHello:
        handlePeerHello(connectionId, packet);
        return;
    case PacketType::PeerProof:
        handlePeerProof(connectionId, packet);
        return;
    case PacketType::PeerSnapshot:
        handlePeerSnapshot(connectionId, packet);
        return;
    case PacketType::PeerAnnounce:
        handlePeerAnnounce(connectionId, packet);
        return;
    case PacketType::PeerLeave:
        handlePeerLeave(connectionId, packet);
        return;
    case PacketType::ChatMessage:
        handleChatMessage(connectionId, packet);
        return;
    case PacketType::ChatAck:
        handleChatAck(packet);
        return;
    case PacketType::VoiceState:
        handleVoiceState(packet);
        return;
    case PacketType::VoiceQuality:
        handleVoiceQuality(packet);
        return;
    case PacketType::RouteRequest:
        handleRouteRequest(connectionId, packet);
        return;
    case PacketType::RouteReply:
        handleRouteReply(connectionId, packet);
        return;
    case PacketType::LinkOffer:
        handleLinkOffer(connectionId, packet);
        return;
    case PacketType::LinkAnswer:
        handleLinkAnswer(connectionId, packet);
        return;
    case PacketType::SessionOffer:
        handleSessionOffer(connectionId, packet);
        return;
    case PacketType::SessionAnswer:
        handleSessionAnswer(connectionId, packet);
        return;
    case PacketType::SignalingState:
    case PacketType::SignalingJoinRequest:
    case PacketType::SignalingJoinInvitation:
        handleRecoveryPacket(connectionId, packet);
        return;
    case PacketType::Ping:
        handlePing(connectionId, packet);
        return;
    case PacketType::Pong:
        handlePong(connectionId, packet);
        return;
    }
}

void NetworkSession::handlePeerHello(const QString& connectionId, const Packet& packet) {
    const auto connection = connections_->info(connectionId);
    const auto& hello = std::get<HelloPayload>(packet.payload);
    if (!connection || packet.senderId == app_.identity().peerId ||
        security::identityId(hello.publicKey) != packet.senderId ||
        (!connection->remote.peerId.isEmpty() && connection->remote.peerId != packet.senderId)) {
        connections_->discard(connectionId);
        emit errorOccurred("Ключ участника не соответствует заявленной идентичности.");
        return;
    }
    const auto previous = remoteHellos_.constFind(connectionId);
    if (previous != remoteHellos_.cend() && (previous->publicKey != hello.publicKey || previous->nonce != hello.nonce)) {
        connections_->discard(connectionId);
        return;
    }
    remoteHellos_.insert(connectionId, hello);
    // Only transportOpened sends our first hello, once both DataChannels are ready.
    // An early remote hello is cached; sendHello will then send the proof too.
    sendPeerProof(connectionId);
}

void NetworkSession::sendPeerProof(const QString& connectionId) {
    const auto remote = remoteHellos_.constFind(connectionId);
    const auto fingerprints = connections_->fingerprints(connectionId);
    if (!app_.signingKey() || remote == remoteHellos_.cend() || !localHelloNonces_.contains(connectionId) ||
        fingerprints.first.isEmpty() || fingerprints.second.isEmpty()) return;
    const auto signature = app_.signingKey()->sign(security::transcript("tmc.p2p-auth.v1", {
        mesh_.meshId(), connectionId, app_.identity().peerId, security::identityId(remote->publicKey),
        localHelloNonces_.value(connectionId), remote->nonce, fingerprints.first, fingerprints.second,
        app_.identity().displayName}));
    sendPacket(connectionId, basePacket(PacketType::PeerProof, PeerProofPayload{signature}));
}

void NetworkSession::handlePeerProof(const QString& connectionId, const Packet& packet) {
    const auto hello = remoteHellos_.constFind(connectionId);
    const auto fingerprints = connections_->fingerprints(connectionId);
    if (hello == remoteHellos_.cend() || !localHelloNonces_.contains(connectionId) ||
        fingerprints.first.isEmpty() || fingerprints.second.isEmpty()) return;
    const auto remoteId = security::identityId(hello->publicKey);
    const auto message = security::transcript("tmc.p2p-auth.v1", {mesh_.meshId(), connectionId,
        remoteId, app_.identity().peerId, hello->nonce, localHelloNonces_.value(connectionId),
        fingerprints.second, fingerprints.first, hello->displayName});
    if (packet.senderId != remoteId || !security::verify(hello->publicKey, message,
            std::get<PeerProofPayload>(packet.payload).signature)) {
        connections_->discard(connectionId);
        emit errorOccurred("Не удалось подтвердить ключ P2P-участника.");
        return;
    }
    const PeerIdentity remote{remoteId, hello->displayName};
    const bool recovery = hello->signalingVersion == 1;
    mesh_.rememberPeer(remote);
    if (recovery) recoveryCapableConnections_.insert(connectionId);
    const auto connection = connections_->info(connectionId);
    if (signaling_ && connection && connection->open) {
        const auto saved = app_.rememberAcquaintance(remote);
        if (!saved) emit errorOccurred(saved.error());
    }
    connections_->markHelloReceived(connectionId, remote);
}

void NetworkSession::handlePeerSnapshot(const QString& connectionId, const Packet& packet) {
    const auto& payload = std::get<PeerSnapshotPayload>(packet.payload);
    const auto localPeerId = app_.identity().peerId;
    const bool changed = mesh_.ingestPeerList(payload.peers, localPeerId);
    router_.observeDirect(packet.senderId, connectionId);
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == localPeerId) {
            continue;
        }
        const auto direct = connections_->infoForPeer(peer.peerId);
        if (peer.peerId != packet.senderId && (!direct || !direct->open)) {
            router_.observeRoute(peer.peerId, connectionId, 2);
        }
        emit peerChanged(peer.peerId, peer.displayName, direct && direct->open);
    }
    if (changed) {
        broadcastPeerList(connectionId);
    }
    ensureDynamicMesh();
    updateMesh();
}

void NetworkSession::handlePeerAnnounce(const QString& connectionId, const Packet& packet) {
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
        broadcastService(forwarded, connectionId);
    }
    ensureDynamicMesh();
    updateMesh();
}

void NetworkSession::handlePeerLeave(const QString& connectionId, const Packet& packet) {
    if (router_.rememberPacket(packet.packetId) && packet.ttl > 1) {
        auto forwarded = packet;
        --forwarded.ttl;
        broadcastService(forwarded, connectionId);
    }
    const auto leaving = mesh_.peer(packet.senderId);
    const auto direct = connections_->infoForPeer(packet.senderId);
    if (direct) {
        connections_->discard(direct->connectionId);
    }
    if (mesh_.forgetPeer(packet.senderId)) {
        router_.forgetPeer(packet.senderId);
        voice_->removePeer(packet.senderId);
        emit peerChanged(packet.senderId, leaving.displayName, false);
        updateMesh();
    }
}

void NetworkSession::handleChatMessage(const QString& connectionId, const Packet& packet) {
    const auto& payload = std::get<ChatMessagePayload>(packet.payload);
    auto received = messaging_.receiveMessage(packet);
    if (!received) {
        emit errorOccurred(received.error());
        return;
    }
    if (received.value()) {
        emit messageReceived(*received.value(), false);
    }
    sendPacket(connectionId,
               basePacket(PacketType::ChatAck, ChatAckPayload{payload.messageId}));
}

void NetworkSession::handleChatAck(const Packet& packet) {
    const auto& payload = std::get<ChatAckPayload>(packet.payload);
    if (messaging_.receiveAcknowledgement(payload.messageId, packet.senderId)) {
        const auto counts = messaging_.deliveryCounts(payload.messageId);
        emit deliveryChanged(payload.messageId, counts.first, counts.second);
    }
}

void NetworkSession::handleVoiceState(const Packet& packet) {
    const auto& payload = std::get<VoiceStatePayload>(packet.payload);
    voice_->updatePeer(packet.senderId, payload.joined, payload.muted);
}

void NetworkSession::handleVoiceQuality(const Packet& packet) {
    if (!packet.targetId.isEmpty() && packet.targetId != app_.identity().peerId) {
        return;
    }
    const auto& payload = std::get<VoiceQualityPayload>(packet.payload);
    voice_->updateNetworkFeedback(packet.senderId, payload.packetLossPercent);
}

void NetworkSession::handleRouteRequest(const QString& connectionId, const Packet& packet) {
    const auto payload = std::get<RoutePayload>(packet.payload);
    if (!router_.rememberPacket(packet.packetId)) {
        return;
    }
    router_.observeRoute(packet.senderId, connectionId, payload.hops + 1);

    router_.rememberReverseRoute(payload.requestId, connectionId);
    if (packet.targetId == app_.identity().peerId) {
        auto reply = basePacket(PacketType::RouteReply, RoutePayload{payload.requestId, 0});
        reply.targetId = packet.senderId;
        reply.ttl = policy_.maxPeers;
        router_.rememberPacket(reply.packetId);
        sendPacket(connectionId, reply);
    } else if (packet.ttl > 1) {
        auto forwarded = packet;
        --forwarded.ttl;
        forwarded.payload = RoutePayload{payload.requestId, payload.hops + 1};
        broadcastService(forwarded, connectionId);
    }
}

void NetworkSession::handleRouteReply(const QString& connectionId, const Packet& packet) {
    const auto payload = std::get<RoutePayload>(packet.payload);
    if (!router_.rememberPacket(packet.packetId)) {
        return;
    }
    router_.observeRoute(packet.senderId, connectionId, payload.hops + 1);

    if (packet.targetId == app_.identity().peerId) {
        flushRouted(packet.senderId);
        return;
    }
    const auto reverse = router_.reverseHop(payload.requestId);
    if (reverse && *reverse != connectionId && packet.ttl > 1) {
        auto forwarded = packet;
        --forwarded.ttl;
        forwarded.payload = RoutePayload{payload.requestId, payload.hops + 1};
        sendPacket(*reverse, forwarded);
    }
}

bool NetworkSession::routeLinkSignaling(const QString& connectionId, const Packet& packet) {
    const auto& payload = std::get<LinkSignalingPayload>(packet.payload);
    if (!router_.rememberPacket(packet.packetId)) {
        return false;
    }
    Logger::instance().log(
        QtInfoMsg, "mesh_signaling",
        QString("receive type=%1 link=%2 generation=%3 sender=%4 target=%5 via=%6 ttl=%7")
            .arg(toString(packet.type), payload.connectionId.left(8))
            .arg(payload.generation)
            .arg(packet.senderId.left(8), packet.targetId.left(8), connectionId.left(8))
            .arg(packet.ttl));
    router_.observeRoute(packet.senderId, connectionId, 2);
    if (packet.targetId != app_.identity().peerId) {
        if (packet.ttl > 1) {
            auto forwarded = packet;
            --forwarded.ttl;
            broadcastService(forwarded, connectionId);
        }
        return false;
    }
    return true;
}

void NetworkSession::handleLinkOffer(const QString& connectionId, const Packet& packet) {
    if (!routeLinkSignaling(connectionId, packet)) {
        return;
    }

    const auto& payload = std::get<LinkSignalingPayload>(packet.payload);
    const auto remote = mesh_.peer(packet.senderId);
    if (remote.peerId.isEmpty()) {
        return;
    }
    if (!mesh_.acceptLinkGeneration(remote.peerId, payload.generation)) {
        return;
    }
    const auto existing = connections_->infoForPeer(remote.peerId);
    if (connections_->contains(payload.connectionId) || existing) {
        return;
    }
    mesh_.rememberPeer(remote);
    const auto created = connections_->create(payload.connectionId, remote,
                                              ConnectionKind::MeshAnswer,
                                              payload.generation);
    if (!created) {
        return;
    }
    emit peerChanged(remote.peerId, remote.displayName, false);
    emit statusChanged("Получен автоматический offer от " + remote.displayName + "…");
    const auto accepted = connections_->acceptOffer(payload.connectionId, payload.sdp);
    if (!accepted) {
        emit statusChanged("Не удалось принять автоматический offer: " + accepted.error());
    }
}

void NetworkSession::handleLinkAnswer(const QString& connectionId, const Packet& packet) {
    if (!routeLinkSignaling(connectionId, packet)) {
        return;
    }

    const auto& payload = std::get<LinkSignalingPayload>(packet.payload);
    const auto connection = connections_->info(payload.connectionId);
    if (!connection || connection->kind != ConnectionKind::MeshOffer ||
        connection->remote.peerId != packet.senderId || connection->answerApplied ||
        connection->generation != payload.generation) {
        return;
    }
    emit statusChanged("Получен mesh answer от " + connection->remote.displayName + "…");
    const auto accepted = connections_->acceptAnswer(payload.connectionId, payload.sdp);
    if (!accepted) {
        emit statusChanged("Не удалось принять mesh answer: " + accepted.error());
        mesh_.scheduleRetry(connection->remote);
    }
}

void NetworkSession::handleSessionOffer(const QString& connectionId, const Packet& packet) {
    const auto& payload = std::get<SessionSignalingPayload>(packet.payload);
    const auto connection = connections_->info(connectionId);
    if (!connection || !connection->open || payload.connectionId != connectionId ||
        (!packet.targetId.isEmpty() && packet.targetId != app_.identity().peerId)) {
        return;
    }
    audioNegotiations_.insert(connectionId, payload.negotiation);
    const auto accepted = connections_->acceptAudioOffer(connectionId, payload.sdp);
    if (!accepted) {
        emit errorOccurred("Не удалось применить audio offer: " + accepted.error());
    }
}

void NetworkSession::handleSessionAnswer(const QString& connectionId, const Packet& packet) {
    const auto& payload = std::get<SessionSignalingPayload>(packet.payload);
    const auto connection = connections_->info(connectionId);
    if (!connection || !connection->open || payload.connectionId != connectionId ||
        (!packet.targetId.isEmpty() && packet.targetId != app_.identity().peerId)) {
        return;
    }
    if (audioNegotiations_.value(connectionId) != payload.negotiation) {
        return;
    }
    const auto accepted = connections_->acceptAudioAnswer(connectionId, payload.sdp);
    if (!accepted) {
        emit errorOccurred("Не удалось применить audio answer: " + accepted.error());
    }
}

void NetworkSession::handlePing(const QString& connectionId, const Packet& packet) {
    sendPacket(connectionId,
               basePacket(PacketType::Pong,
                          std::get<HeartbeatPayload>(packet.payload)));
}

void NetworkSession::handlePong(const QString& connectionId, const Packet& packet) {
    receivePong(connectionId, std::get<HeartbeatPayload>(packet.payload));
}

} // namespace tmc
