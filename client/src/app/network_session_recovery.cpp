#include "tmc/app/network_session.h"

#include "tmc/app/application_controller.h"
#include "tmc/core/uuid.h"
#include "tmc/signaling_client/signaling_client.h"

#include <QJsonArray>

#include <utility>

namespace tmc {

void NetworkSession::resetRoomRecovery() {
    recoveryRequestId_.clear();
    recoveryInvites_.clear();
    recoveryTokens_.clear();
    recoveryJoin_.reset();
    recoveryAfter_ = recoveryClock_.elapsed() + 3000;
}

bool NetworkSession::sendRecoveryPacket(const QString& peerId, PacketType type, PacketPayload payload) {
    const auto link = connections_->infoForPeer(peerId);
    if (!link || !link->open || !recoveryCapableConnections_.contains(link->connectionId)) return false;
    auto packet = basePacket(type, std::move(payload));
    packet.targetId = peerId;
    return sendPacket(link->connectionId, packet);
}

void NetworkSession::broadcastSignalingState() {
    if (!signaling_ || !mesh_.joined()) return;
    SignalingStatePayload state;
    if (serverMesh_) {
        state.server = signaling_->url().toString(QUrl::FullyEncoded);
        state.sessionId = signaling_->sessionId();
        state.roomId = serverRoomId_;
    }
    for (const auto& id : connections_->openConnectionIds()) {
        const auto link = connections_->info(id);
        if (link) sendRecoveryPacket(link->remote.peerId, PacketType::SignalingState, state);
    }
    recoveryBroadcastAt_ = recoveryClock_.elapsed();
}

void NetworkSession::recoverServerRoom() {
    continueServerBootstrap();
    const auto now = recoveryClock_.elapsed();
    if (now - recoveryBroadcastAt_ >= 2000) broadcastSignalingState();
    for (auto it = recoveryPeers_.begin(); it != recoveryPeers_.end();) {
        const auto link = connections_->infoForPeer(it.key());
        if (now - it->seenAt > 15000 || !link || !link->open) {
            recoveryTokens_.remove(it.key());
            it = recoveryPeers_.erase(it);
        } else {
            ++it;
        }
    }
    if (!serverMesh_ || !mesh_.joined() || !signalingConnected() ||
        !serverOperation_.isEmpty() || now < recoveryAfter_) return;

    // Any surviving room wins over creating a room. If clients briefly create
    // separate rooms, the smallest room UUID makes them converge deterministically.
    QString room = serverRoomId_;
    QString issuer;
    QString leader = app_.identity().peerId;
    for (auto it = recoveryPeers_.cbegin(); it != recoveryPeers_.cend(); ++it) {
        const auto& state = it->state;
        if (state.sessionId.isEmpty() || QUrl(state.server) != signaling_->url()) continue;
        if (it.key() < leader) leader = it.key();
        if (!state.roomId.isEmpty() &&
            (room.isEmpty() || state.roomId < room ||
             (state.roomId == room && (issuer.isEmpty() || it.key() < issuer)))) {
            room = state.roomId;
            issuer = it.key();
        }
    }
    if (!serverRoomId_.isEmpty() && serverRoomId_ == room) {
        recoveryJoin_.reset();
        requestServerInvitationWhenReady();
        return;
    }
    if (recoveryJoin_) {
        if (now - recoveryJoin_->createdAt < 10000 && recoveryJoin_->peerId == issuer &&
            recoveryJoin_->payload.roomId == room) return;
        recoveryJoin_.reset();
    }
    if (!serverRoomId_.isEmpty()) {
        // Leave only signaling membership; keep the logical mesh and every P2P link.
        serverOperation_ = QStringLiteral("recover-leave");
        const auto result = signaling_->request("room.leave", {{"roomId", serverRoomId_}});
        if (const auto* requestId = std::get_if<QString>(&result)) {
            recoveryRequestId_ = *requestId;
        } else {
            recoveryRequestId_.clear();
        }
        serverRoomId_.clear();
        serverPeerId_.clear();
        serverPeers_.clear();
        recoveryInvites_.clear();
        recoveryTokens_.clear();
        broadcastSignalingState();
    } else if (!room.isEmpty() && !issuer.isEmpty()) {
        RecoveryInvitation request{issuer,
            {createUuid(), signaling_->sessionId(), room, {}}, now};
        recoveryJoin_ = request;
        if (!sendRecoveryPacket(issuer, PacketType::SignalingJoinRequest, request.payload)) {
            recoveryJoin_.reset();
            recoveryAfter_ = now + 3000;
        }
    } else if (leader == app_.identity().peerId) {
        serverOperation_ = QStringLiteral("recover-create");
        const auto result = signaling_->request("room.create");
        if (const auto* requestId = std::get_if<QString>(&result)) {
            recoveryRequestId_ = *requestId;
        } else {
            recoveryRequestId_.clear();
        }
    }
    if (serverOperation_.startsWith("recover-") && recoveryRequestId_.isEmpty()) {
        // request() can fail synchronously and emit connectionLost().
        const bool uncertainMembership = serverOperation_ == "recover-leave";
        serverOperation_.clear();
        recoveryAfter_ = now + 5000;
        if (uncertainMembership && signalingConnected()) signaling_->connectTo(signaling_->url());
    }
    emit signalingServerChanged();
}

void NetworkSession::handleRecoveryPacket(const QString& connectionId, const Packet& packet) {
    const auto link = connections_->info(connectionId);
    if (!signaling_ || !link || !link->open || !mesh_.joined() || packet.ttl != 0 ||
        packet.targetId != app_.identity().peerId || packet.senderId != link->remote.peerId ||
        !recoveryCapableConnections_.contains(connectionId)) return;
    const auto now = recoveryClock_.elapsed();
    if (packet.type == PacketType::SignalingState) {
        const auto& state = std::get<SignalingStatePayload>(packet.payload);
        const auto cached = recoveryTokens_.constFind(packet.senderId);
        if (cached != recoveryTokens_.cend() &&
            (cached->payload.sessionId != state.sessionId || !state.roomId.isEmpty())) {
            recoveryTokens_.remove(packet.senderId);
        }
        recoveryPeers_.insert(packet.senderId, {state, now});
        return;
    }
    if (!serverMesh_ || !signalingConnected()) return;
    const auto state = recoveryPeers_.constFind(packet.senderId);
    if (state == recoveryPeers_.cend() || now - state->seenAt > 15000 ||
        QUrl(state->state.server) != signaling_->url()) return;
    const auto& payload = std::get<SignalingJoinPayload>(packet.payload);
    if (packet.type == PacketType::SignalingJoinInvitation) {
        if (!recoveryJoin_ || !serverOperation_.isEmpty() || !serverRoomId_.isEmpty() ||
            recoveryJoin_->peerId != packet.senderId ||
            recoveryJoin_->payload.requestId != payload.requestId ||
            recoveryJoin_->payload.roomId != payload.roomId ||
            payload.sessionId != signaling_->sessionId() ||
            state->state.roomId != payload.roomId) return;
        serverOperation_ = QStringLiteral("recover-join");
        const auto result = signaling_->request("room.join",
            {{"roomId", payload.roomId}, {"token", payload.token}});
        if (const auto* requestId = std::get_if<QString>(&result)) {
            recoveryRequestId_ = *requestId;
        } else {
            recoveryRequestId_.clear();
            serverOperation_.clear();
            recoveryJoin_.reset();
            recoveryAfter_ = now + 5000;
        }
        emit signalingServerChanged();
        return;
    }
    if (serverRoomId_.isEmpty() || payload.roomId != serverRoomId_ ||
        payload.sessionId != state->state.sessionId || !state->state.roomId.isEmpty()) return;

    auto cached = recoveryTokens_.find(packet.senderId);
    if (cached != recoveryTokens_.end() && cached->payload.sessionId == payload.sessionId &&
        cached->payload.roomId == payload.roomId && now - cached->createdAt < 540000) {
        auto reply = cached->payload;
        reply.requestId = payload.requestId;
        sendRecoveryPacket(packet.senderId, PacketType::SignalingJoinInvitation, reply);
        return;
    }
    // One token request per live peer/session, bounded by the mesh's six members.
    for (auto it = recoveryInvites_.begin(); it != recoveryInvites_.end(); ++it) {
        if (it->peerId == packet.senderId) {
            if (it->payload.sessionId == payload.sessionId && it->payload.roomId == payload.roomId)
                it->payload.requestId = payload.requestId;
            return;
        }
    }
    if (recoveryInvites_.size() >= policy_.maxPeers - 1) return;
    const auto result = signaling_->request("invite.create", {{"roomId", serverRoomId_}});
    if (const auto* requestId = std::get_if<QString>(&result)) {
        recoveryInvites_.insert(*requestId, {packet.senderId, payload, now});
    }
}

bool NetworkSession::handleRecoveryResponse(const signaling_protocol::Envelope& response) {
    const auto id = *response.requestId;
    const auto job = recoveryInvites_.find(id);
    if (job != recoveryInvites_.end()) {
        auto invitation = job.value();
        recoveryInvites_.erase(job);
        const auto peer = recoveryPeers_.constFind(invitation.peerId);
        if (serverRoomId_ != invitation.payload.roomId ||
            response.body.value("roomId").toString() != serverRoomId_ ||
            peer == recoveryPeers_.cend() || !peer->state.roomId.isEmpty() ||
            peer->state.sessionId != invitation.payload.sessionId) return true;
        invitation.payload.token = response.body.value("token").toString();
        recoveryTokens_.insert(invitation.peerId, invitation);
        sendRecoveryPacket(invitation.peerId, PacketType::SignalingJoinInvitation, invitation.payload);
        return true;
    }
    if (id != recoveryRequestId_) return false;
    recoveryRequestId_.clear();
    const auto operation = serverOperation_;
    serverOperation_.clear();
    if (operation == "recover-create" || operation == "recover-join") {
        const auto room = response.body.value("roomId").toString();
        if (operation == "recover-join" && (!recoveryJoin_ || room != recoveryJoin_->payload.roomId)) {
            signaling_->connectTo(signaling_->url());
            return true;
        }
        serverRoomId_ = room;
        serverPeerId_ = response.body.value("peerId").toString();
        serverPeers_.clear();
        for (const auto& peer : response.body.value("peers").toArray()) serverPeers_.insert(peer.toString());
        recoveryJoin_.reset();
        // Membership recovery never starts SDP negotiation for existing mesh peers.
        emit statusChanged("Комната сигналинга готова. Серверные приглашения снова доступны.");
    }
    recoveryAfter_ = recoveryClock_.elapsed() + 1000;
    broadcastSignalingState();
    emit signalingServerChanged();
    return true;
}

bool NetworkSession::handleRecoveryFailure(const QString& requestId, const QString& code) {
    if (recoveryInvites_.remove(requestId)) {
        if (code == "not_in_room") signaling_->connectTo(signaling_->url());
        return true;
    }
    if (requestId != recoveryRequestId_) return false;
    const bool leaving = serverOperation_ == "recover-leave";
    recoveryRequestId_.clear();
    serverOperation_.clear();
    recoveryJoin_.reset();
    recoveryAfter_ = recoveryClock_.elapsed() + 5000;
    if (serverInvitationRequested_ && (code == "resource_limit" || code == "room_full")) {
        serverInvitationRequested_ = false;
        onlineInvitationTargets_.remove(pendingContactTarget_);
        pendingContactTarget_.clear();
        emit acquaintancesChanged();
        emit errorOccurred("Не удалось подготовить приглашение: достигнут лимит сервера или комнаты.");
    }
    if (leaving || code == "busy") signaling_->connectTo(signaling_->url());
    emit signalingServerChanged();
    return true;
}

void NetworkSession::requestServerInvitationWhenReady() {
    if (!serverInvitationRequested_ || !signalingConnected() || serverRoomId_.isEmpty() ||
        !serverOperation_.isEmpty()) return;
    if (mesh_.peerCount() >= policy_.maxPeers || invitationPending()) {
        serverInvitationRequested_ = false;
        onlineInvitationTargets_.remove(pendingContactTarget_);
        pendingContactTarget_.clear();
        emit acquaintancesChanged();
        emit signalingServerChanged();
        return;
    }
    if (!pendingContactTarget_.isEmpty()) {
        submitOnlineInvitation();
        return;
    }
    serverOperation_ = QStringLiteral("invite");
    const auto result = signaling_->request("invite.create", {{"roomId", serverRoomId_}});
    if (const auto* requestId = std::get_if<QString>(&result)) {
        serverInvitationRequestId_ = *requestId;
    } else {
        serverInvitationRequestId_.clear();
        serverOperation_.clear();
        serverInvitationRequested_ = false;
        emit errorOccurred("Не удалось запросить приглашение.");
    }
    emit signalingServerChanged();
}

} // namespace tmc
