#include "tmc/app/network_session.h"

#include "tmc/app/application_controller.h"
#include "tmc/core/limits.h"
#include "tmc/core/uuid.h"
#include "tmc/signaling_client/signaling_client.h"
#include "tmc/signaling_client/access_invitation.h"
#include "tmc/signaling_protocol/message_codec.h"

#include <QJsonArray>

#include <algorithm>

namespace tmc {
using signaling_protocol::Envelope;
using signaling_protocol::EnvelopeCodec;

QString NetworkSession::signalingState() const {
    if (signalingConnected() && serverMesh_ && mesh_.joined() &&
        (serverRoomId_.isEmpty() || serverOperation_.startsWith("recover-"))) {
        return QStringLiteral("recovering");
    }
    return signaling_ ? signaling_->state() : QStringLiteral("disabled");
}

bool NetworkSession::signalingConnected() const { return signaling_ && signaling_->ready(); }
bool NetworkSession::signalingBusy() const {
    return !serverOperation_.isEmpty() || serverInvitationRequested_ || recoveryJoin_.has_value();
}
bool NetworkSession::serverMesh() const { return serverMesh_; }

void NetworkSession::initializeSignalingClient() {
    if (signaling_) {
        return;
    }
    signaling_ = std::make_unique<SignalingClient>();
    signaling_->configureAccess(app_.signingKey(), app_.serverAccess());
    connect(signaling_.get(), &SignalingClient::accessRequired, this, &NetworkSession::errorOccurred);
    connect(signaling_.get(), &SignalingClient::accessGranted, this, [this](const QString& server, const QJsonObject& grant) {
        const auto saved = app_.saveServerAccess(server, grant);
        if (!saved) emit errorOccurred(saved.error());
        else emit statusChanged("Постоянный доступ к серверу получен и сохранён.");
    });
    recoveryClock_.start();
    initializePresence();
    roomRecoveryTimer_.setInterval(1000);
    connect(&roomRecoveryTimer_, &QTimer::timeout, this, &NetworkSession::recoverServerRoom);
    roomRecoveryTimer_.start();
    connect(signaling_.get(), &SignalingClient::stateChanged,
            this, &NetworkSession::signalingServerChanged);
    connect(signaling_.get(), &SignalingClient::readyChanged, this, [this] {
        recoveryAfter_ = recoveryClock_.elapsed() + 3000;
        submitServerJoin();
        broadcastSignalingState();
        publishPresence();
    });
    connect(signaling_.get(), &SignalingClient::responseReceived,
            this, &NetworkSession::handleServerResponse);
    connect(signaling_.get(), &SignalingClient::eventReceived,
            this, &NetworkSession::handleServerEvent);
    connect(signaling_.get(), &SignalingClient::connectionLost, this, [this] {
        const bool affected = serverMesh_;
        resetPresence();
        serverRoomId_.clear();
        serverPeerId_.clear();
        serverPeers_.clear();
        serverBootstrapAttempts_.clear();
        serverSignalRequests_.clear();
        serverOperation_.clear();
        serverInvitationRequested_ = false;
        serverInvitationRequestId_.clear();
        resetRoomRecovery();
        pendingServerJoin_.reset();
        serverJoinDeadline_.stop();
        const auto links = serverLinks_.keys();
        for (const auto& id : links) {
            const auto info = connections_->info(id);
            if (info && !info->open) {
                connections_->discard(id);
            }
        }
        if (affected && !mesh_.joined()) {
            serverMesh_ = false;
            mesh_.leave();
            clearSessionData();
            updateMesh();
        }
        broadcastSignalingState();
        emit signalingServerChanged();
        if (affected) {
            emit statusChanged(signaling_->state() == "access-required"
                ? "Требуется разрешение доступа к серверу. P2P-соединения сохраняются."
                : "Сервер сигналинга недоступен. Переподключение автоматическое; P2P-соединения сохраняются.");
        }
    });
    connect(signaling_.get(), &SignalingClient::requestFailed, this,
            [this](const QString& requestId, const QString& type, const QString& code) {
                if (type == "access.invite") {
                    emit errorOccurred("Не удалось создать приглашение доступа. Попробуйте позже.");
                    return;
                }
                if (handlePresenceFailure(requestId, type, code)) return;
                if (handleRecoveryFailure(requestId, code)) return;
                if (type == "invite.create" && requestId != serverInvitationRequestId_) return;
                if (type == "invite.create") {
                    serverInvitationRequested_ = false;
                    serverInvitationRequestId_.clear();
                }
                if (type == "signal.send") {
                    const auto id = serverSignalRequests_.take(requestId);
                    const auto info = connections_->info(id);
                    if (info && !info->open) connections_->discard(id);
                } else {
                    serverOperation_.clear();
                }
                if (type == "room.leave") signaling_->stop();
                if (type == "room.join") {
                    pendingServerJoin_.reset();
                    serverJoinDeadline_.stop();
                    if (serverRoomId_.isEmpty() &&
                        (code == "invalid_invitation" || code == "room_full")) {
                        // The join was explicitly rejected: no membership was created.
                        // Roll back the provisional mesh without reconnecting the socket
                        // and registering presence a second time.
                        serverMesh_ = false;
                    }
                    leaveMesh();
                }
                emit signalingServerChanged();
                const QString explanation = code == "invalid_invitation"
                    ? "Приглашение использовано, истекло или больше не действует. Запросите новое."
                    : code == "room_full" ? "В mesh уже шесть участников."
                    : code == "busy" ? "Сессия уже участвует в другом mesh."
                    : code == "resource_limit" ? "Достигнут лимит сервера. Попробуйте позже."
                    : "Сервер отклонил запрос: " + code;
                emit errorOccurred(explanation);
            });
    serverJoinDeadline_.setSingleShot(true);
    serverJoinDeadline_.setInterval(120000);
    connect(&serverJoinDeadline_, &QTimer::timeout, this, [this] {
        if (serverMesh_ && !mesh_.joined()) {
            leaveMesh();
            emit errorOccurred("Не удалось установить P2P-соединение. Проверьте сеть и запросите новое приглашение.");
        }
    });
}

void NetworkSession::configureSignalingServer(const QString& url) {
    initializeSignalingClient();
    if (serverMesh_ || signalingBusy()) {
        emit errorOccurred("Выйдите из mesh перед сменой серверного подключения.");
        return;
    }
    if (url.isEmpty()) {
        signaling_->stop();
    } else if (!signaling_->connectTo(QUrl(url, QUrl::StrictMode))) {
        emit errorOccurred("Некорректный адрес сервера.");
    }
}

Result<void> NetworkSession::createServerInvitation() {
    if (!signalingConnected()) {
        return Result<void>::failure("Подключитесь к серверу в настройках сигналинга.");
    }
    if (signalingBusy() || invitationPending()) {
        return Result<void>::failure("Дождитесь текущей операции или отмените ручное приглашение.");
    }
    if (mesh_.meshId().isEmpty() || !mesh_.joined()) {
        return Result<void>::failure("Сначала создайте mesh или завершите подключение.");
    }
    if (mesh_.peerCount() >= policy_.maxPeers || serverPeers_.size() >= policy_.maxPeers - 1) {
        return Result<void>::failure("Достигнут лимит участников mesh.");
    }
    if (!serverMesh_) recoveryAfter_ = recoveryClock_.elapsed() + 3000;
    serverMesh_ = true;
    serverInvitationRequested_ = true;
    broadcastSignalingState();
    requestServerInvitationWhenReady();
    recoverServerRoom();
    emit signalingServerChanged();
    return Result<void>::success();
}

Result<void> NetworkSession::joinServerInvitation(const ServerInvitation& invitation) {
    if (!mesh_.meshId().isEmpty() || signalingBusy() || !connections_->connections().isEmpty()) {
        return Result<void>::failure("Сначала выйдите из текущего mesh.");
    }
    if (encodeServerInvitation(invitation).isEmpty()) {
        return Result<void>::failure("Некорректное серверное приглашение.");
    }
    initializeSignalingClient();
    if (!signaling_->ready() || signaling_->url() != invitation.server) {
        // The caller has already obtained the user's confirmation for this server.
        signaling_->connectTo(invitation.server);
    }
    if (signaling_->state() == "unavailable") {
        return Result<void>::failure("Не удалось подключиться к серверу приглашения.");
    }
    clearSessionData();
    serverMesh_ = true;
    pendingServerJoin_ = invitation;
    serverOperation_ = QStringLiteral("join");
    mesh_.beginJoin(app_.identity(), invitation.meshId);
    serverJoinDeadline_.start();
    submitServerJoin();
    emit signalingServerChanged();
    return Result<void>::success();
}

void NetworkSession::submitServerJoin() {
    if (!signalingConnected() || !pendingServerJoin_ || serverOperation_ != "join") {
        return;
    }
    serverOperation_ = QStringLiteral("joining");
    const auto& invitation = *pendingServerJoin_;
    const auto result = signaling_->request(QStringLiteral("room.join"),
                                           {{"roomId", invitation.roomId}, {"token", invitation.token}});
    if (std::holds_alternative<SignalingClient::RequestError>(result)) {
        leaveMesh();
        emit errorOccurred("Не удалось отправить запрос вступления.");
    }
    emit signalingServerChanged();
}

void NetworkSession::handleServerResponse(const QString& type, const Envelope& response) {
    if (type == "access.invite") {
        const auto link = encodeAccessInvitation({signaling_->url(), response.body.value("authority").toString(),
                                                   response.body.value("token").toString()});
        if (!link.isEmpty()) emit accessInvitationReady(link);
        return;
    }
    if (handlePresenceResponse(type, response)) return;
    if (handleRecoveryResponse(response)) return;
    const auto& body = response.body;
    if (type == "signal.send") {
        serverSignalRequests_.remove(*response.requestId);
        return;
    }
    if (type == "room.leave") {
        serverOperation_.clear();
        emit signalingServerChanged();
        return;
    }
    if (!serverMesh_) {
        return;
    }
    if (type == "invite.create" && *response.requestId == serverInvitationRequestId_) {
        serverInvitationRequestId_.clear();
        serverOperation_.clear();
        const bool show = serverInvitationRequested_;
        serverInvitationRequested_ = false;
        if (!show) {
            emit signalingServerChanged();
            return;
        }
        if (body.value("roomId").toString() != serverRoomId_) {
            signaling_->stop();
            return;
        }
        const auto link = encodeServerInvitation({signaling_->url(), serverRoomId_, mesh_.meshId(),
                                                  body.value("token").toString()});
        if (link.isEmpty()) {
            emit errorOccurred("Не удалось сформировать ссылку приглашения.");
        } else {
            emit serverInvitationReady(link);
        }
    } else if (type == "room.join" && serverOperation_ == "joining" && pendingServerJoin_) {
        if (body.value("roomId").toString() != pendingServerJoin_->roomId) {
            signaling_->stop();
            return;
        }
        serverRoomId_ = pendingServerJoin_->roomId;
        serverPeerId_ = body.value("peerId").toString();
        pendingServerJoin_.reset(); // Do not keep the consumed bearer token.
        serverOperation_.clear();
        for (const auto& peer : body.value("peers").toArray()) {
            serverPeers_.insert(peer.toString());
        }
        // Bootstrap one link. Existing mesh announcements establish the others
        // with the same deterministic initiator rule as manual invitations.
        continueServerBootstrap();
        emit statusChanged("Приглашение принято. Устанавливаются P2P-соединения…");
    }
    broadcastSignalingState();
    emit signalingServerChanged();
}

void NetworkSession::handleServerEvent(const Envelope& event) {
    if (handlePresenceEvent(event)) return;
    if (!serverMesh_ || serverRoomId_.isEmpty() ||
        event.body.value("roomId").toString() != serverRoomId_) {
        return;
    }
    const auto peerId = event.body.value("peerId").toString();
    if (event.type == "peer.joined") {
        if (peerId != serverPeerId_ && serverPeers_.size() < policy_.maxPeers - 1) {
            serverPeers_.insert(peerId); // A newcomer bootstraps one P2P link.
        }
    } else if (event.type == "peer.left") {
        serverPeers_.remove(peerId);
        // Loss of signaling membership does not imply loss of the WebRTC path.
    } else if (event.type == "signal.received") {
        const auto sender = event.body.value("fromPeerId").toString();
        if (serverPeers_.contains(sender)) {
            handleServerPayload(sender, event.body.value("payload").toString());
        }
    }
}

void NetworkSession::continueServerBootstrap() {
    if (!serverMesh_ || mesh_.joined() || !signalingConnected() || serverRoomId_.isEmpty()) return;
    for (const auto& id : serverLinks_.keys()) {
        if (connections_->contains(id)) return;
    }
    auto peers = serverPeers_.values();
    std::sort(peers.begin(), peers.end());
    for (const auto& peer : peers) {
        if (serverBootstrapAttempts_.contains(peer)) continue;
        serverBootstrapAttempts_.insert(peer);
        startServerOffer(peer);
        return;
    }
}

void NetworkSession::startServerOffer(const QString& peerId) {
    if (!signalingConnected() || !serverPeers_.contains(peerId) ||
        serverLinks_.values().contains(peerId) || connections_->connections().size() >= policy_.maxPeers - 1) {
        return;
    }
    const auto id = createUuid();
    serverLinks_.insert(id, peerId);
    auto result = connections_->create(id, {}, ConnectionKind::ServerOffer);
    if (result) {
        result = connections_->startOffer(id);
    }
    if (!result) {
        connections_->discard(id);
        serverLinks_.remove(id);
        emit errorOccurred("Не удалось начать P2P-соединение через сервер.");
    }
}

void NetworkSession::emitServerSignaling(const QString& connectionId, const QString& sdp) {
    const auto info = connections_->info(connectionId);
    const auto target = serverLinks_.value(connectionId);
    if (!info || !serverMesh_ || serverRoomId_.isEmpty() || !signalingConnected() ||
        !serverPeers_.contains(target)) {
        connections_->discard(connectionId);
        emit errorOccurred("Сигналинг для нового соединения недоступен.");
        return;
    }
    const Envelope envelope{signaling_protocol::ProtocolVersion, isOffer(info->kind) ? QStringLiteral("webrtc.offer")
                                                 : QStringLiteral("webrtc.answer"), std::nullopt,
        {{"meshId", mesh_.meshId()}, {"connectionId", connectionId},
         {"identityId", app_.identity().peerId}, {"displayName", app_.identity().displayName},
         {"sdp", sdp}}};
    const auto encoded = EnvelopeCodec::encode(envelope);
    const auto* bytes = std::get_if<QByteArray>(&encoded);
    if (!bytes || bytes->size() > signaling_protocol::MaxSignalBytes) {
        connections_->discard(connectionId);
        emit errorOccurred("Описание соединения превышает лимит сигналинга.");
        return;
    }
    const auto payload = QString::fromLatin1(bytes->toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const auto result = signaling_->request(QStringLiteral("signal.send"),
            {{"roomId", serverRoomId_}, {"toPeerId", target}, {"payload", payload}});
    if (const auto* requestId = std::get_if<QString>(&result)) {
        serverSignalRequests_.insert(*requestId, connectionId);
    } else {
        connections_->discard(connectionId);
        emit errorOccurred("Не удалось отправить описание соединения.");
    }
}

void NetworkSession::handleServerPayload(const QString& sender, const QString& payload) {
    const auto bytes = QByteArray::fromBase64(payload.toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    const auto decoded = EnvelopeCodec::decode(bytes);
    const auto* envelope = std::get_if<Envelope>(&decoded);
    if (!envelope || envelope->requestId || envelope->body.size() != 5 ||
        (envelope->type != "webrtc.offer" && envelope->type != "webrtc.answer")) {
        emit errorOccurred("Получено некорректное описание соединения.");
        return;
    }
    const auto& body = envelope->body;
    for (const auto* field : {"meshId", "connectionId", "identityId", "displayName", "sdp"}) {
        if (!body.value(QLatin1String(field)).isString()) {
            return;
        }
    }
    const auto id = body.value("connectionId").toString();
    const PeerIdentity remote{body.value("identityId").toString(), body.value("displayName").toString()};
    const auto sdp = body.value("sdp").toString();
    if (body.value("meshId").toString() != mesh_.meshId() || !isCanonicalUuid(id) ||
        !remote.isValid() || remote.peerId == app_.identity().peerId ||
        remote.displayName.size() > limits::MaxDisplayNameLength || sdp.isEmpty()) {
        emit errorOccurred("Описание соединения не соответствует текущему mesh.");
        return;
    }
    for (const auto c : remote.displayName) {
        if (!c.isPrint()) {
            return;
        }
    }
    if (envelope->type == "webrtc.offer") {
        if (connections_->contains(id) || connections_->infoForPeer(remote.peerId) ||
            serverLinks_.values().contains(sender) ||
            connections_->connections().size() >= policy_.maxPeers - 1) {
            return;
        }
        serverLinks_.insert(id, sender);
        const auto created = connections_->create(id, remote, ConnectionKind::ServerAnswer);
        if (!created) {
            serverLinks_.remove(id);
            return;
        }
        mesh_.rememberPeer(remote);
        emit peerChanged(remote.peerId, remote.displayName, false);
        updateMesh();
        if (!connections_->acceptOffer(id, sdp)) {
            emit errorOccurred("Не удалось принять описание P2P-соединения.");
        }
    } else {
        const auto info = connections_->info(id);
        const auto existing = connections_->infoForPeer(remote.peerId);
        if (!info || info->kind != ConnectionKind::ServerOffer || info->answerApplied ||
            serverLinks_.value(id) != sender || (existing && existing->connectionId != id) ||
            !connections_->setExpectedRemote(id, remote)) {
            return;
        }
        mesh_.rememberPeer(remote);
        emit peerChanged(remote.peerId, remote.displayName, false);
        updateMesh();
        if (!connections_->acceptAnswer(id, sdp)) {
            emit errorOccurred("Не удалось применить ответ P2P-соединения.");
        }
    }
}

void NetworkSession::leaveServerRoom() {
    pendingContactTarget_.clear();
    onlineInvitationTargets_.clear();
    emit acquaintancesChanged();
    if (!serverMesh_ && serverOperation_.isEmpty()) {
        return;
    }
    const auto room = serverRoomId_;
    const bool canLeave = signalingConnected() && !room.isEmpty() && serverOperation_.isEmpty();
    serverMesh_ = false;
    serverRoomId_.clear();
    serverPeerId_.clear();
    serverPeers_.clear();
    serverBootstrapAttempts_.clear();
    serverLinks_.clear();
    serverSignalRequests_.clear();
    pendingServerJoin_.reset();
    serverJoinDeadline_.stop();
    serverOperation_.clear();
    serverInvitationRequested_ = false;
    serverInvitationRequestId_.clear();
    resetRoomRecovery();
    if (canLeave) {
        serverOperation_ = QStringLiteral("leave");
        const auto result = signaling_->request(QStringLiteral("room.leave"), {{"roomId", room}});
        if (std::holds_alternative<SignalingClient::RequestError>(result)) {
            signaling_->stop();
            serverOperation_.clear();
        }
    } else if (signaling_) {
        // Abort in-flight create/join so a late reply cannot leave an orphan room.
        signaling_->stop();
        if (!app_.config().signalingServerUrl.isEmpty()) {
            signaling_->connectTo(QUrl(app_.config().signalingServerUrl));
        }
    }
    emit signalingServerChanged();
}

} // namespace tmc

namespace tmc {
Result<void> NetworkSession::createAccessInvitation() {
    if (!signalingConnected()) return Result<void>::failure("Сначала получите доступ и подключитесь к серверу.");
    const auto result = signaling_->request("access.invite");
    if (std::holds_alternative<SignalingClient::RequestError>(result)) {
        return Result<void>::failure("Не удалось запросить приглашение доступа.");
    }
    return Result<void>::success();
}
Result<void> NetworkSession::importServerAccess(const QString& text) {
    if (!mesh_.meshId().isEmpty() || signalingBusy()) return Result<void>::failure("Выйдите из mesh перед изменением доступа к серверу.");
    initializeSignalingClient();
    const auto access = decodeAccessInvitation(text.trimmed());
    if (access) {
        const auto configured = app_.updateSignalingServer(access->server.toString(QUrl::FullyEncoded));
        if (!configured) return configured;
        signaling_->redeemAccess(access->server, access->authority, access->token);
    } else {
        const auto grant = decodeAccessGrant(text.trimmed());
        const QUrl url(app_.config().signalingServerUrl, QUrl::StrictMode);
        if (grant.isEmpty() || !SignalingClient::validServerUrl(url))
            return Result<void>::failure("Укажите сервер в настройках и вставьте действительное разрешение доступа.");
        const auto saved = app_.saveServerAccess(url.toString(QUrl::FullyEncoded), grant);
        if (!saved) return saved;
        signaling_->configureAccess(app_.signingKey(), app_.serverAccess());
        signaling_->connectTo(url);
    }
    emit signalingServerChanged();
    return Result<void>::success();
}
}
