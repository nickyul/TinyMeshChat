#include "tmc/app/network_session.h"
#include "tmc/app/application_controller.h"
#include "tmc/signaling_client/signaling_client.h"
#include <QJsonArray>
#include <QVariantMap>
#include <algorithm>

namespace tmc {

void NetworkSession::initializePresence() {
    presencePublishTimer_.setSingleShot(true);
    connect(&presencePublishTimer_, &QTimer::timeout, this, &NetworkSession::publishPresence);
    connect(&app_, &ApplicationController::acquaintancesChanged, this, [this] {
        emit acquaintancesChanged();
        presencePublishTimer_.start(0);
    });
    connect(&app_, &ApplicationController::displayNameChanged, this, [this] { presencePublishTimer_.start(0); });
    connect(&mesh_, &MeshCoordinator::stateChanged, this, [this] {
        presencePublishTimer_.start(0);
        emit acquaintancesChanged();
    });
}

QVariantList NetworkSession::acquaintances() const {
    auto peers = app_.acquaintances();
    std::sort(peers.begin(), peers.end(), [](const auto& a, const auto& b) {
        const auto compared = QString::localeAwareCompare(a.displayName, b.displayName);
        return compared == 0 ? a.peerId < b.peerId : compared < 0;
    });
    QVariantList rows;
    for (const auto& peer : peers) {
        const bool known = presenceRegistered_ && signalingConnected() && contactPresence_.contains(peer.peerId);
        const auto status = !security::validKey(peer.peerId) ? QStringLiteral("legacy") : !known ? QStringLiteral("unknown") : contactPresence_.value(peer.peerId)
            ? QStringLiteral("online") : QStringLiteral("offline");
        rows.append(QVariantMap{{"peerId", peer.peerId}, {"displayName", peer.displayName},
            {"presence", status}, {"inviting", onlineInvitationTargets_.contains(peer.peerId)},
            {"inMesh", !mesh_.peer(peer.peerId).peerId.isEmpty()}});
    }
    return rows;
}

void NetworkSession::publishPresence() {
    if (!signalingConnected()) return;
    QJsonArray known;
    for (const auto& peer : app_.acquaintances()) if (security::validKey(peer.peerId)) known.append(peer.peerId);
    const QJsonObject body{{"identityId", app_.identity().peerId}, {"displayName", app_.identity().displayName},
        {"knownPeers", known}, {"busy", !mesh_.meshId().isEmpty() || !connections_->connections().isEmpty()}};
    if (body == lastPresence_) return;
    const auto result = signaling_->request("presence.publish", body);
    if (std::holds_alternative<QString>(result)) lastPresence_ = body;
}

void NetworkSession::resetPresence() {
    presencePublishTimer_.stop();
    lastPresence_ = {};
    contactPresence_.clear();
    presenceRegistered_ = false;
    presenceConflictReported_ = false;
    pendingContactTarget_.clear();
    onlineInvitationTargets_.clear();
    onlineInvitationRequests_.clear();
    onlineResponseRequests_.clear();
    if (incomingOnlineInvitation_) {
        const auto id = incomingOnlineInvitation_->id;
        incomingOnlineInvitation_.reset();
        emit onlineInvitationClosed(id);
    }
    emit acquaintancesChanged();
}

Result<void> NetworkSession::inviteAcquaintance(const QString& peerId) {
    if (!presenceRegistered_ || !signalingConnected() || !contactPresence_.value(peerId))
        return Result<void>::failure("Знакомый сейчас недоступен на этом сервере.");
    if (!mesh_.joined() || mesh_.meshId().isEmpty())
        return Result<void>::failure("Сначала создайте mesh или завершите подключение.");
    if (signalingBusy() || invitationPending() || onlineInvitationTargets_.contains(peerId))
        return Result<void>::failure("Дождитесь завершения текущего приглашения.");
    if (mesh_.peerCount() >= policy_.maxPeers)
        return Result<void>::failure("Достигнут лимит участников mesh.");
    if (!mesh_.peer(peerId).peerId.isEmpty())
        return Result<void>::failure("Этот знакомый уже числится в текущем mesh.");
    pendingContactTarget_ = peerId;
    onlineInvitationTargets_.insert(peerId);
    if (!serverMesh_) {
        recoveryAfter_ = recoveryClock_.elapsed() + (mesh_.peerCount() == 1 ? 0 : 3000);
    }
    serverMesh_ = true;
    serverInvitationRequested_ = true;
    broadcastSignalingState();
    requestServerInvitationWhenReady();
    recoverServerRoom();
    emit acquaintancesChanged();
    emit signalingServerChanged();
    return Result<void>::success();
}

void NetworkSession::submitOnlineInvitation() {
    const auto target = pendingContactTarget_;
    pendingContactTarget_.clear();
    serverInvitationRequested_ = false;
    const auto request = signaling_->request("contact.invite", {{"toIdentityId", target},
        {"roomId", serverRoomId_}, {"meshId", mesh_.meshId()}});
    if (const auto* requestId = std::get_if<QString>(&request)) {
        onlineInvitationRequests_.insert(*requestId, target);
        emit statusChanged("Онлайн-приглашение отправляется…");
    } else {
        onlineInvitationTargets_.remove(target);
        emit errorOccurred("Не удалось отправить онлайн-приглашение.");
    }
    emit acquaintancesChanged();
    emit signalingServerChanged();
}

void NetworkSession::respondToOnlineInvitation(const QString& invitationId, bool accept) {
    if (!incomingOnlineInvitation_ || incomingOnlineInvitation_->id != invitationId ||
        incomingOnlineInvitation_->accepting || !signalingConnected()) return;
    const bool busy = !mesh_.meshId().isEmpty() || !connections_->connections().isEmpty() || signalingBusy();
    const auto decision = !accept ? QStringLiteral("decline") : busy ? QStringLiteral("busy") : QStringLiteral("accept");
    incomingOnlineInvitation_->accepting = decision == "accept";
    const auto request = signaling_->request("contact.respond", {{"invitationId", invitationId}, {"decision", decision}});
    const auto* requestId = std::get_if<QString>(&request);
    if (requestId) onlineResponseRequests_.insert(*requestId, invitationId);
    if (decision != "accept" || !requestId) {
        incomingOnlineInvitation_.reset();
        emit onlineInvitationClosed(invitationId);
    }
    if (!requestId) emit errorOccurred("Не удалось отправить ответ на приглашение.");
}

bool NetworkSession::handlePresenceResponse(const QString& type, const signaling_protocol::Envelope& response) {
    if (type == "presence.publish") {
        presenceRegistered_ = true;
        presenceConflictReported_ = false;
        emit acquaintancesChanged();
        return true;
    }
    if (type == "contact.invite") {
        onlineInvitationRequests_.remove(*response.requestId);
        emit statusChanged("Приглашение доставлено. Ожидается ответ знакомого.");
        return true;
    }
    if (type == "contact.respond") {
        onlineResponseRequests_.remove(*response.requestId);
        return true;
    }
    return false;
}

bool NetworkSession::handlePresenceEvent(const signaling_protocol::Envelope& event) {
    const auto& body = event.body;
    if (event.type == "presence.snapshot") {
        contactPresence_.clear();
        for (const auto& item : body.value("peers").toArray()) {
            const auto peer = item.toObject();
            contactPresence_.insert(peer.value("identityId").toString(), peer.value("online").toBool());
        }
        emit acquaintancesChanged();
        return true;
    }
    if (!event.type.startsWith("contact.")) return false;
    const auto id = body.value("invitationId").toString();
    if (event.type == "contact.invitation") {
        const auto sender = body.value("fromIdentityId").toString();
        const bool known = std::any_of(app_.acquaintances().cbegin(), app_.acquaintances().cend(),
            [&](const auto& peer) { return peer.peerId == sender; });
        if (!known || incomingOnlineInvitation_ || !mesh_.meshId().isEmpty() ||
            !connections_->connections().isEmpty() || signalingBusy()) {
            // Best effort: if this cannot be sent, the server invitation expires.
            (void)signaling_->request("contact.respond", {{"invitationId", id}, {"decision", "busy"}});
            return true;
        }
        incomingOnlineInvitation_ = OnlineInvitation{id, sender, body.value("meshId").toString(), false};
        emit onlineInvitationReceived(id, body.value("displayName").toString(), signaling_->url().toDisplayString());
    } else if (event.type == "contact.accepted") {
        if (!incomingOnlineInvitation_ || incomingOnlineInvitation_->id != id ||
            !incomingOnlineInvitation_->accepting || incomingOnlineInvitation_->meshId != body.value("meshId").toString()) return true;
        incomingOnlineInvitation_.reset();
        emit onlineInvitationClosed(id);
        const auto joined = joinServerInvitation({signaling_->url(), body.value("roomId").toString(),
            body.value("meshId").toString(), body.value("token").toString()});
        if (!joined) emit errorOccurred(joined.error());
    } else if (event.type == "contact.closed") {
        if (incomingOnlineInvitation_ && incomingOnlineInvitation_->id == id) {
            incomingOnlineInvitation_.reset();
            emit onlineInvitationClosed(id);
            emit statusChanged("Онлайн-приглашение больше не действует.");
        }
    } else if (event.type == "contact.result") {
        onlineInvitationTargets_.remove(body.value("toIdentityId").toString());
        const auto status = body.value("status").toString();
        emit statusChanged(status == "accepted" ? "Знакомый принял приглашение. Ожидается P2P-подключение."
            : status == "declined" ? "Знакомый отклонил приглашение."
            : status == "busy" ? "Знакомый занят в другом mesh."
            : status == "expired" ? "Время ожидания ответа на приглашение истекло."
            : "Онлайн-приглашение отменено или недоступно.");
        emit acquaintancesChanged();
    }
    return true;
}

bool NetworkSession::handlePresenceFailure(const QString& requestId, const QString& type, const QString& code) {
    if (type == "presence.publish") {
        presenceRegistered_ = false;
        contactPresence_.clear();
        lastPresence_ = {};
        if (!presenceConflictReported_) {
            emit errorOccurred(code == "identity_in_use" ? "Эта идентичность уже подключена к серверу. Используйте разные профили клиентов."
                : "Сервер не поддерживает присутствие или отклонил его регистрацию.");
            presenceConflictReported_ = true;
        }
        if (code == "identity_in_use") presencePublishTimer_.start(5000);
        emit acquaintancesChanged();
        return true;
    }
    if (type == "contact.invite") {
        onlineInvitationTargets_.remove(onlineInvitationRequests_.take(requestId));
        emit acquaintancesChanged();
        emit errorOccurred(code == "busy" ? "Знакомый занят или уже отвечает на приглашение."
            : code == "recipient_unavailable" ? "Знакомый сейчас недоступен."
            : code == "rate_limited" ? "Подождите несколько секунд перед повторным приглашением."
            : code == "room_full" ? "В комнате уже шесть участников."
            : "Не удалось доставить онлайн-приглашение.");
        return true;
    }
    if (type == "contact.respond") {
        const auto invitation = onlineResponseRequests_.take(requestId);
        if (incomingOnlineInvitation_ && incomingOnlineInvitation_->id == invitation) {
            const auto id = incomingOnlineInvitation_->id;
            incomingOnlineInvitation_.reset();
            emit onlineInvitationClosed(id);
        }
        if (!invitation.isEmpty()) emit statusChanged("Онлайн-приглашение больше не действует.");
        return true;
    }
    return false;
}

} // namespace tmc
