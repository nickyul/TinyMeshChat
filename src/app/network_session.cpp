#include "tmc/app/network_session.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/session_packet_handlers.h"
#include "tmc/app/voice_session.h"
#include "tmc/core/logger.h"
#include "tmc/protocol/packet.h"
#include "tmc/protocol/packet_codec.h"
#include "tmc/protocol/packet_dispatcher.h"
#include "tmc/signaling/invitation_codec.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

namespace tmc {

namespace {

QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

NetworkSession::NetworkSession(ApplicationController& app, ConnectionPolicy policy, QObject* parent)
    : QObject(parent), app_(app), policy_(policy),
      connections_(std::make_unique<ConnectionManager>(app.config().stunServers, policy)),
      packetDispatcher_(std::make_unique<PacketDispatcher>()),
      packetHandlers_(std::make_unique<SessionPacketHandlers>(*this)), mesh_(policy),
      voice_(std::make_unique<VoiceSession>(app.config().audio)) {
    Q_ASSERT(policy_.isValid());
    qRegisterMetaType<ChatMessage>();
    qRegisterMetaType<ConnectionAttemptState>();
    qRegisterMetaType<MeshSessionState>();
    packetHandlers_->registerWith(*packetDispatcher_);

    connect(connections_.get(), &ConnectionManager::localDescriptionReady, this,
            &NetworkSession::emitSignaling);
    connect(connections_.get(), &ConnectionManager::statusChanged, this,
            &NetworkSession::statusChanged);
    connect(connections_.get(), &ConnectionManager::attemptChanged, this,
            [this](const QString& connectionId, ConnectionAttemptState state) {
                emit connectionAttemptChanged(connectionId, state);
                if (connectionId == manualInvitationConnectionId_) {
                    emit invitationStateChanged(true, toString(state));
                }
            });
    connect(connections_.get(), &ConnectionManager::attemptFailed, this,
            [this](const PeerIdentity& peer, bool meshManaged, bool localOffer, const QString&) {
                if (meshManaged && localOffer) {
                    mesh_.scheduleRetry(peer);
                }
                updateMesh();
            });
    connect(connections_.get(), &ConnectionManager::linkOpened, this,
            [this](const QString& connectionId, const PeerIdentity& remote) {
                if (connectionId == manualInvitationConnectionId_) {
                    manualInvitationConnectionId_.clear();
                    emit invitationStateChanged(false, "connected");
                }
                mesh_.connectionOpened(remote);
                Logger::instance().log(QtInfoMsg, "network",
                                       "Direct DataChannel opened for " + connectionId.left(8));
                emit peerChanged(remote.peerId, remote.displayName, true);
                emit statusChanged("Прямое P2P-соединение установлено. Relay не используется.");
                updateMesh();
                sendHello(connectionId);
                sendPeerList(connectionId);
                sendVoiceState(connectionId);
                broadcastPeerList(connectionId);
                ensureDynamicMesh();
            });
    connect(connections_.get(), &ConnectionManager::linkRemoved, this,
            [this](const QString& connectionId, const PeerIdentity& remote, bool wasOpen) {
                if (connectionId == manualInvitationConnectionId_) {
                    manualInvitationConnectionId_.clear();
                    emit invitationStateChanged(false, "finished");
                }
                const auto replacement = connections_->infoForPeer(remote.peerId);
                if (!remote.peerId.isEmpty() && (!replacement || !replacement->open)) {
                    emit peerChanged(remote.peerId, remote.displayName, false);
                    voice_->removePeer(remote.peerId);
                }
                if (wasOpen) {
                    emit statusChanged("Участник отключился от прямого P2P-канала.");
                }
                updateMesh();
            });
    connect(connections_.get(), &ConnectionManager::textReceived, this,
            &NetworkSession::handleIncoming);
    connect(connections_.get(), &ConnectionManager::voiceFrameReceived, this,
            [this](const QString& connectionId, quint32 sequence, const QByteArray& payload,
                   qint64 receivedAtNs) {
                const auto connection = connections_->info(connectionId);
                if (connection) {
                    voice_->receiveFrame(connection->remote.peerId, sequence, payload,
                                         receivedAtNs);
                }
            });

    connect(&mesh_, &MeshCoordinator::stateChanged, this, &NetworkSession::meshStateChanged);
    connect(&mesh_, &MeshCoordinator::statusChanged, this, &NetworkSession::statusChanged);
    connect(&mesh_, &MeshCoordinator::retryRequested, this, &NetworkSession::startMeshOffer);

    connect(voice_.get(), &VoiceSession::encodedFrameReady, this,
            [this](quint32 sequence, const QByteArray& payload, const VoiceFrameTiming& timing) {
                if (voice_->active() && !voice_->muted()) {
                    connections_->sendVoiceFrameToOpen(sequence, payload, timing);
                }
            });
    connect(voice_.get(), &VoiceSession::stateChanged, this, [this](bool active, bool muted) {
        broadcastVoiceState();
        emit callStateChanged(active, muted);
    });
    connect(voice_.get(), &VoiceSession::peerChanged, this, &NetworkSession::peerVoiceChanged);
    connect(voice_.get(), &VoiceSession::errorOccurred, this, &NetworkSession::errorOccurred);
    connect(voice_.get(), &VoiceSession::microphoneLevelChanged, this,
            &NetworkSession::microphoneLevelChanged);

    keepalive_ = new QTimer(this);
    keepalive_->setInterval(policy_.heartbeatIntervalSeconds * 1000);
    connect(keepalive_, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto inactive =
            connections_->inactiveConnectionIds(now, policy_.livenessTimeoutSeconds * 1000LL);
        for (const auto& connectionId : inactive) {
            const auto connection = connections_->info(connectionId);
            if (connection) {
                emit statusChanged("Соединение с участником потеряно: " +
                                   peerDisplayName(connection->remote.peerId));
            }
            connections_->discard(connectionId);
        }
        for (const auto& connectionId : connections_->openConnectionIds()) {
            sendPacket(connectionId,
                       basePacket(PacketType::Ping,
                                  HeartbeatPayload{uuid(), QDateTime::currentDateTimeUtc()}));
        }
    });
    keepalive_->start();

    connect(&app_, &ApplicationController::displayNameChanged, this, [this] {
        if (mesh_.meshId().isEmpty()) {
            return;
        }
        mesh_.rememberPeer(app_.identity());
        for (const auto& connectionId : connections_->openConnectionIds()) {
            sendHello(connectionId);
        }
        broadcastPeerList();
    });
    connect(&app_, &ApplicationController::stunServersChanged, connections_.get(),
            &ConnectionManager::setStunServers);
}

NetworkSession::~NetworkSession() = default;

MeshSessionState NetworkSession::meshState() const {
    return mesh_.state();
}

Result<void> NetworkSession::createMesh() {
    if (!connections_->connections().isEmpty() || !mesh_.meshId().isEmpty()) {
        return Result<void>::failure("Сначала покиньте текущую mesh-сессию.");
    }
    clearSessionData();
    mesh_.create(app_.identity(), uuid());
    emit statusChanged("Mesh создан. Теперь можно пригласить участника.");
    updateMesh();
    return Result<void>::success();
}

void NetworkSession::leaveMesh() {
    if (mesh_.meshId().isEmpty() && connections_->connections().isEmpty()) {
        return;
    }
    voice_->leave();
    const auto connections = connections_->connections();
    for (const auto& connection : connections) {
        connections_->discard(connection.connectionId);
    }
    mesh_.leave();
    clearSessionData();
    emit statusChanged("Вы вышли из mesh.");
    updateMesh();
}

Result<void> NetworkSession::createInvitation() {
    if (mesh_.meshId().isEmpty() || !mesh_.established()) {
        return Result<void>::failure("Сначала создайте mesh или завершите подключение.");
    }
    if (knownPeerCount() >= policy_.maxPeers) {
        return Result<void>::failure("Достигнут лимит участников mesh.");
    }
    if (!manualInvitationConnectionId_.isEmpty() &&
        connections_->contains(manualInvitationConnectionId_)) {
        return Result<void>::failure(
            "Исходящее приглашение уже создаётся или ожидает answer. Отмените его перед повтором.");
    }

    const auto connectionId = uuid();
    auto created = connections_->create(connectionId, {}, true, false);
    if (!created) {
        return created;
    }
    manualInvitationConnectionId_ = connectionId;
    emit invitationStateChanged(true, "gathering");
    emit statusChanged("Создаётся приглашение для нового участника…");
    const auto started = connections_->startOffer(connectionId);
    if (!started) {
        manualInvitationConnectionId_.clear();
        emit invitationStateChanged(false, "failed");
    }
    return started;
}

void NetworkSession::cancelInvitation() {
    if (manualInvitationConnectionId_.isEmpty()) {
        return;
    }
    const auto connectionId = manualInvitationConnectionId_;
    manualInvitationConnectionId_.clear();
    connections_->discard(connectionId);
    emit invitationStateChanged(false, "cancelled");
    emit statusChanged("Создание приглашения отменено.");
}

Result<void> NetworkSession::recreateInvitation() {
    cancelInvitation();
    return createInvitation();
}

Result<void> NetworkSession::importSignalingText(const QString& text) {
    const auto normalized = text.trimmed();
    auto decoded = normalized.startsWith('{') ? InvitationCodec::decode(normalized.toUtf8())
                                              : InvitationCodec::decodeText(normalized);
    if (!decoded) {
        return Result<void>::failure(decoded.error());
    }
    return importSignalingDocument(InvitationCodec::encode(decoded.value()));
}

Result<Invitation> NetworkSession::decodeSignaling(const QByteArray& document) const {
    return InvitationCodec::decode(document);
}

Result<void> NetworkSession::importSignalingDocument(const QByteArray& document) {
    auto decoded = decodeSignaling(document);
    if (!decoded) {
        return Result<void>::failure(decoded.error());
    }
    const auto invitation = decoded.value();

    if (invitation.kind == Invitation::Kind::Offer) {
        if (!mesh_.meshId().isEmpty() && mesh_.meshId() != invitation.meshId) {
            return Result<void>::failure(
                "Приглашение относится к другому mesh. Сначала выйдите из текущего.");
        }
        if (connections_->contains(invitation.connectionId)) {
            return Result<void>::failure(
                "Повторно получен offer для уже известного соединения. Убедитесь, что друг "
                "отправил строку из окна «Answer готов», а не исходное приглашение.");
        }
        const auto existing = connections_->infoForPeer(invitation.fromPeer.peerId);
        if (existing && existing->open) {
            return Result<void>::failure("Этот участник уже подключён к mesh.");
        }
        connections_->discardStale(invitation.fromPeer.peerId);

        const bool joiningMesh = mesh_.meshId().isEmpty();
        if (joiningMesh) {
            clearSessionData();
            mesh_.beginJoin(app_.identity(), invitation.fromPeer, invitation.meshId);
        } else {
            mesh_.rememberPeer(invitation.fromPeer);
        }

        auto created =
            connections_->create(invitation.connectionId, invitation.fromPeer, false, false);
        if (!created) {
            if (joiningMesh) {
                mesh_.leave();
            }
            return created;
        }
        emit peerChanged(invitation.fromPeer.peerId, invitation.fromPeer.displayName, false);
        emit statusChanged("Offer импортирован. Создаётся answer…");
        const auto accepted = connections_->acceptOffer(invitation.connectionId, invitation.sdp);
        if (!accepted && joiningMesh) {
            mesh_.leave();
            clearSessionData();
        }
        return accepted;
    }

    if (mesh_.meshId() != invitation.meshId) {
        return Result<void>::failure("Answer относится к другому mesh.");
    }
    const auto connection = connections_->info(invitation.connectionId);
    if (!connection) {
        return Result<void>::failure("Не найдено исходное приглашение для этого answer.");
    }
    if (connection->answerApplied) {
        return Result<void>::failure("Этот answer уже импортирован.");
    }

    connections_->setRemote(invitation.connectionId, invitation.fromPeer);
    mesh_.rememberPeer(invitation.fromPeer);
    emit peerChanged(invitation.fromPeer.peerId, invitation.fromPeer.displayName, false);
    emit statusChanged("Answer импортирован. Устанавливается прямое P2P-соединение…");
    return connections_->acceptAnswer(invitation.connectionId, invitation.sdp);
}

void NetworkSession::emitSignaling(const QString& connectionId, const QString& type,
                                   const QString& sdp) {
    Q_UNUSED(type)
    const auto connection = connections_->info(connectionId);
    if (!connection) {
        return;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    Invitation invitation;
    invitation.kind = connection->localOffer ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    invitation.meshId = mesh_.meshId();
    invitation.connectionId = connectionId;
    invitation.sdp = sdp;
    invitation.nonce = uuid();
    invitation.fromPeer = app_.identity();
    invitation.createdAt = now;
    invitation.expiresAt = now.addSecs(policy_.manualSignalingTimeoutSeconds);

    if (connection->meshManaged) {
        const auto packetType =
            connection->localOffer ? PacketType::MeshOffer : PacketType::MeshAnswer;
        broadcastService(packetType,
                         {connection->localOffer ? "offer" : "answer", uuid(), 0, connectionId,
                          app_.identity(), connection->remote.peerId, sdp});
        emit statusChanged(connection->localOffer
                               ? "Mesh offer отправлен через доступные P2P-каналы."
                               : "Mesh answer отправлен через доступные P2P-каналы.");
        return;
    }

    const auto document = InvitationCodec::encode(invitation);
    const auto text = InvitationCodec::encodeText(invitation);
    const auto kind = invitation.kind == Invitation::Kind::Offer ? "offer" : "answer";
    const auto extension = invitation.kind == Invitation::Kind::Offer ? ".tmcinvite" : ".tmcanswer";
    emit signalingReady(kind, text, document, "tiny-mesh-" + connectionId.left(8) + extension);
    emit statusChanged(invitation.kind == Invitation::Kind::Offer
                           ? "Приглашение готово. Ожидание answer."
                           : "Answer готов. Отправьте его пригласившему участнику.");
}

Packet NetworkSession::basePacket(PacketType type, PacketPayload payload) const {
    return {type,
            uuid(),
            mesh_.meshId(),
            app_.identity().peerId,
            QDateTime::currentDateTimeUtc(),
            std::move(payload)};
}

void NetworkSession::sendPacket(const QString& connectionId, const Packet& packet) {
    const auto bytes = PacketCodec::encode(packet);
    if (!connections_->sendText(connectionId, QString::fromUtf8(bytes))) {
        emit errorOccurred("Не удалось отправить пакет участнику.");
    }
}

void NetworkSession::sendHello(const QString& connectionId) {
    sendPacket(connectionId,
               basePacket(PacketType::PeerHello,
                          HelloPayload{app_.identity().displayName, app_.identity().deviceId}));
}

void NetworkSession::handleIncoming(const QString& connectionId, const QString& text) {
    QSet<QString> senders;
    const auto connection = connections_->info(connectionId);
    if (connection && !connection->remote.peerId.isEmpty()) {
        senders.insert(connection->remote.peerId);
    }
    auto decoded = PacketCodec::decode(text.toUtf8(), mesh_.meshId(), senders);
    if (!decoded) {
        Logger::instance().log(QtWarningMsg, "protocol", decoded.error());
        emit errorOccurred("Получен некорректный сетевой пакет: " + decoded.error());
        return;
    }
    if (!packetDispatcher_->dispatch({connectionId}, decoded.value())) {
        emit errorOccurred("Для типа пакета не зарегистрирован обработчик: " +
                           toString(decoded.value().type));
    }
}

void NetworkSession::sendPeerList(const QString& connectionId) {
    sendPacket(connectionId, basePacket(PacketType::PeerList, PeerListPayload{mesh_.peerList()}));
}

void NetworkSession::sendVoiceState(const QString& connectionId) {
    sendPacket(connectionId, basePacket(PacketType::VoiceState,
                                        VoiceStatePayload{voice_->active(), voice_->muted()}));
}

void NetworkSession::broadcastVoiceState() {
    for (const auto& connectionId : connections_->openConnectionIds()) {
        sendVoiceState(connectionId);
    }
}

void NetworkSession::broadcastPeerList(const QString& excludedConnection) {
    for (const auto& connectionId : connections_->openConnectionIds()) {
        if (connectionId != excludedConnection) {
            sendPeerList(connectionId);
        }
    }
}

void NetworkSession::ensureDynamicMesh() {
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == app_.identity().peerId ||
            !mesh_.shouldInitiateLink(app_.identity().peerId, peer.peerId) ||
            !mesh_.canAttemptLink(peer.peerId)) {
            continue;
        }
        const auto existing = connections_->infoForPeer(peer.peerId);
        if (existing && (existing->open || !existing->everOpened)) {
            continue;
        }
        if (existing) {
            connections_->discard(existing->connectionId);
        }
        startMeshOffer(peer);
    }
}

void NetworkSession::startMeshOffer(const PeerIdentity& peer) {
    if (peer.peerId.isEmpty() || !mesh_.canAttemptLink(peer.peerId)) {
        return;
    }
    const auto existing = connections_->infoForPeer(peer.peerId);
    if (existing && (existing->open || !existing->everOpened)) {
        return;
    }
    if (existing) {
        connections_->discard(existing->connectionId);
    }

    const auto connectionId = uuid();
    auto created = connections_->create(connectionId, peer, true, true);
    if (!created) {
        mesh_.scheduleRetry(peer);
        return;
    }
    emit statusChanged("Создаётся прямой канал с " + peer.displayName + "…");
    const auto started = connections_->startOffer(connectionId);
    if (!started) {
        mesh_.scheduleRetry(peer);
    }
}

void NetworkSession::broadcastService(PacketType type, MeshSignalingPayload payload,
                                      const QString& excludedConnection) {
    mesh_.rememberRoute(payload.routeId);
    for (const auto& connectionId : connections_->openConnectionIds()) {
        if (connectionId != excludedConnection) {
            sendPacket(connectionId, basePacket(type, payload));
        }
    }
}

Result<void> NetworkSession::sendMessage(const QString& text) {
    if (mesh_.meshId().isEmpty() || !mesh_.established()) {
        return Result<void>::failure("Сначала войдите в mesh.");
    }
    QSet<QString> targets;
    for (const auto& connection : connections_->connections()) {
        if (connection.open && !connection.remote.peerId.isEmpty()) {
            targets.insert(connection.remote.peerId);
        }
    }
    auto outgoing = messaging_.createMessage(text, mesh_.meshId(), app_.identity().peerId, targets);
    if (!outgoing) {
        return Result<void>::failure(outgoing.error());
    }
    emit messageReceived(outgoing.value().message, true);
    emit deliveryChanged(outgoing.value().message.messageId, 0,
                         outgoing.value().expectedDeliveries);
    for (const auto& connectionId : connections_->openConnectionIds()) {
        sendPacket(connectionId, outgoing.value().packet);
    }
    return Result<void>::success();
}

Result<void> NetworkSession::startCall() {
    if (voice_->active()) {
        return Result<void>::success();
    }
    if (mesh_.meshId().isEmpty() || !mesh_.established()) {
        return Result<void>::failure("Сначала войдите в mesh.");
    }
    if (connectedPeerCount() == 0) {
        return Result<void>::failure("Для звонка нужен хотя бы один подключённый участник.");
    }
    const auto started = voice_->start();
    if (!started) {
        return started;
    }
    emit statusChanged("Вы присоединились к голосовому звонку.");
    return Result<void>::success();
}

void NetworkSession::leaveCall() {
    if (!voice_->active()) {
        return;
    }
    voice_->leave();
    emit statusChanged("Вы вышли из голосового звонка.");
}

void NetworkSession::setMuted(bool muted) {
    if (!voice_->active() || voice_->muted() == muted) {
        return;
    }
    voice_->setMuted(muted);
    emit statusChanged(muted ? "Микрофон выключен." : "Микрофон включён.");
}

void NetworkSession::setDeafened(bool deafened) {
    voice_->setDeafened(deafened);
    emit audioStateChanged();
}

void NetworkSession::setMicrophoneTest(bool enabled) {
    voice_->setMicrophoneTest(enabled);
    emit audioStateChanged();
}

void NetworkSession::setPeerVolume(const QString& peerId, int percent) {
    voice_->setPeerVolume(peerId, percent);
}

Result<void> NetworkSession::applyAudioPreferences(const AudioPreferences& preferences) {
    const auto result = voice_->applyPreferences(preferences);
    if (result) {
        emit audioStateChanged();
    }
    return result;
}

QPair<QStringList, QStringList> NetworkSession::refreshAudioDevices() {
    return voice_->refreshDevices();
}

bool NetworkSession::callActive() const {
    return voice_->active();
}

bool NetworkSession::muted() const {
    return voice_->muted();
}

bool NetworkSession::deafened() const {
    return voice_->deafened();
}

bool NetworkSession::microphoneTest() const {
    return voice_->microphoneTest();
}

bool NetworkSession::invitationPending() const {
    return !manualInvitationConnectionId_.isEmpty() &&
           connections_->contains(manualInvitationConnectionId_);
}

QString NetworkSession::invitationState() const {
    const auto connection = connections_->info(manualInvitationConnectionId_);
    return connection ? toString(connection->attemptState) : QString{};
}

Result<QPair<int, int>> NetworkSession::deliveryCounts(const QString& messageId) const {
    return Result<QPair<int, int>>::success(messaging_.deliveryCounts(messageId));
}

int NetworkSession::connectedPeerCount() const {
    return connections_->connectedPeerCount();
}

int NetworkSession::knownPeerCount() const {
    return mesh_.peerCount();
}

QString NetworkSession::diagnostics() const {
    QStringList lines{"Состояние mesh: " + toString(mesh_.state()),
                      QString("Прямых каналов: %1/%2")
                          .arg(connectedPeerCount())
                          .arg(qMax(0, knownPeerCount() - 1)),
                      "TURN/relay: отключён"};
    const auto connections = connections_->connections();
    if (connections.isEmpty()) {
        lines.append("Соединения: отсутствуют");
    } else {
        lines.append("Соединения:");
    }
    for (const auto& connection : connections) {
        const auto peer = connection.remote.displayName.isEmpty() ? "не определён"
                                                                  : connection.remote.displayName;
        const auto elapsed =
            qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - connection.createdAtMs) / 1000;
        lines.append(QString("  %1 · transport: %2 · ICE: %3 · attempt: %4 · %5 s · %6")
                         .arg(peer, toString(connection.transportState), connection.iceState,
                              toString(connection.attemptState))
                         .arg(elapsed)
                         .arg(connection.connectionId.left(8)));
        lines.append(QString("    candidates: host=%1 srflx=%2 relay=%3 · selected: %4 · "
                             "voice drops=%5")
                         .arg(connection.hostCandidates)
                         .arg(connection.serverReflexiveCandidates)
                         .arg(connection.relayCandidates)
                         .arg(connection.selectedCandidatePair.isEmpty()
                                  ? "не выбрана"
                                  : connection.selectedCandidatePair)
                         .arg(connection.droppedVoiceFrames));
    }
    const auto recent = connections_->recentAttempts();
    if (!recent.isEmpty()) {
        lines.append("Последние завершённые попытки:");
        for (const auto& connection : recent) {
            lines.append(QString("  %1 · %2 · ICE: %3 · host=%4 srflx=%5 relay=%6%7")
                             .arg(connection.connectionId.left(8),
                                  toString(connection.attemptState), connection.iceState)
                             .arg(connection.hostCandidates)
                             .arg(connection.serverReflexiveCandidates)
                             .arg(connection.relayCandidates)
                             .arg(connection.lastError.isEmpty() ? QString{}
                                                                 : " · " + connection.lastError));
        }
    }
    return lines.join('\n');
}

QString NetworkSession::peerDisplayName(const QString& peerId) const {
    const auto peer = mesh_.peer(peerId);
    return peer.displayName.isEmpty() ? peerId.left(8) : peer.displayName;
}

void NetworkSession::updateMesh() {
    emit meshChanged(connectedPeerCount(), qMax(0, knownPeerCount() - 1));
}

void NetworkSession::clearSessionData() {
    messaging_.clear();
    voice_->clear();
}

} // namespace tmc
