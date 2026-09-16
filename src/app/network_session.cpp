#include "tmc/app/network_session.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/negotiation_policy.h"
#include "tmc/app/voice_session.h"
#include "tmc/core/logger.h"
#include "tmc/core/uuid.h"
#include "tmc/network/audio_transport_worker.h"
#include "tmc/protocol/packet.h"
#include "tmc/protocol/packet_codec.h"
#include "tmc/signaling/invitation_codec.h"

#include <QDateTime>
#include <QNetworkInformation>
#include <QRandomGenerator>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace tmc {

namespace {

qint64 monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

NetworkSession::NetworkSession(ApplicationController& app, ConnectionPolicy policy, QObject* parent)
    : QObject(parent), app_(app), policy_(policy),
      connections_(std::make_unique<ConnectionManager>(app.config().stunServers, policy)),
      mesh_(policy), voice_(std::make_unique<VoiceSession>(app.config().audio)),
      rendezvous_(std::make_unique<RendezvousService>(
          app.identity(), app.config().rendezvous, app.config().stunServers,
          app.dataDirectory() + "/contacts.json")) {
    Q_ASSERT(policy_.isValid());
    qRegisterMetaType<ChatMessage>();
    qRegisterMetaType<ConnectionKind>();
    qRegisterMetaType<ConnectionAttemptState>();
    qRegisterMetaType<MeshSessionState>();
    qRegisterMetaType<ContactPresence>();

    const auto audioTransport = connections_->audioTransport();
    audioTransport->setIncomingSink(voice_->incomingAudioSink());
    voice_->setOutgoingAudioSink(audioTransport);

    connectConnectionSignals();
    connectMeshSignals();
    connectVoiceSignals();
    connectRendezvousSignals();
    configureKeepalive();
    connectApplicationSignals();
    const auto started = rendezvous_->start();
    if (!started) {
        Logger::instance().log(QtWarningMsg, "rendezvous",
                               "Automatic rendezvous unavailable: " + started.error());
    }
}

void NetworkSession::connectConnectionSignals() {
    connect(connections_.get(), &ConnectionManager::localDescriptionReady, this,
            &NetworkSession::emitSignaling);
    connect(connections_.get(), &ConnectionManager::audioDescriptionReady, this,
            [this](const QString& connectionId, const QString& type, const QString& sdp) {
                const auto connection = connections_->info(connectionId);
                if (!connection || !connection->open) {
                    return;
                }
                auto negotiation = audioNegotiations_.value(connectionId);
                const bool offer = type == "offer";
                if (offer) {
                    negotiation = qMax<quint64>(1, negotiation + 1);
                    audioNegotiations_.insert(connectionId, negotiation);
                }
                auto packet =
                    basePacket(offer ? PacketType::SessionOffer : PacketType::SessionAnswer,
                               SessionSignalingPayload{connectionId, negotiation, sdp});
                packet.targetId = connection->remote.peerId;
                sendPacket(connectionId, packet);
            });
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
            [this](const PeerIdentity& peer, ConnectionKind kind, const QString&) {
                if (isMeshManaged(kind) && isOffer(kind)) {
                    mesh_.scheduleRetry(peer);
                }
            });
    connect(connections_.get(), &ConnectionManager::transportOpened, this,
            [this](const QString& connectionId, const PeerIdentity&) { sendHello(connectionId); });
    connect(connections_.get(), &ConnectionManager::linkOpened, this,
            &NetworkSession::handleLinkOpened);
    connect(connections_.get(), &ConnectionManager::linkRemoved, this,
            &NetworkSession::handleLinkRemoved);
    connect(connections_.get(), &ConnectionManager::controlTextReceived, this,
            [this](const QString& connectionId, const QString& text) {
                handleIncoming(connectionId, text, PacketChannel::Control);
            });
    connect(connections_.get(), &ConnectionManager::chatTextReceived, this,
            [this](const QString& connectionId, const QString& text) {
                handleIncoming(connectionId, text, PacketChannel::Chat);
            });
}

void NetworkSession::handleLinkOpened(const QString& connectionId, const PeerIdentity& remote) {
    const auto rendezvousRequestId = rendezvousConnections_.take(connectionId);
    if (!rendezvousRequestId.isEmpty()) {
        rendezvousNegotiations_.remove(rendezvousRequestId);
    }
    if (connectionId == manualInvitationConnectionId_) {
        manualInvitationConnectionId_.clear();
        emit invitationStateChanged(false, "connected");
    }

    mesh_.connectionOpened(remote);
    router_.observeDirect(remote.peerId, connectionId);
    Logger::instance().log(QtInfoMsg, "network",
                           "Direct DataChannel opened for " + connectionId.left(8));

    emit peerChanged(remote.peerId, remote.displayName, true);
    emit statusChanged("Прямое соединение установлено.");
    updateMesh();

    sendPeerList(connectionId);
    sendVoiceState(connectionId);
    broadcastPeerList(connectionId);
    announceLocalPeer();
    ensureDynamicMesh();
    sendPing(connectionId);
    sendRendezvousMetadata(connectionId, false);
    rendezvous_->setContactConnected(remote.peerId, true);

    if (shouldInitiateNegotiation(app_.identity().peerId, remote.peerId)) {
        const auto audio = connections_->startAudioOffer(connectionId);
        if (!audio) {
            emit errorOccurred("Не удалось начать согласование audio Track: " + audio.error());
        }
    }
}

void NetworkSession::handleLinkRemoved(const QString& connectionId, const PeerIdentity& remote,
                                       bool wasOpen) {
    router_.forgetConnection(connectionId);
    pendingPings_.remove(connectionId);
    audioNegotiations_.remove(connectionId);
    const auto rendezvousRequestId = rendezvousConnections_.take(connectionId);
    if (!rendezvousRequestId.isEmpty()) {
        rendezvousNegotiations_.remove(rendezvousRequestId);
    }

    if (connectionId == manualInvitationConnectionId_) {
        manualInvitationConnectionId_.clear();
        emit invitationStateChanged(false, "finished");
    }

    const auto replacement = connections_->infoForPeer(remote.peerId);
    if (!remote.peerId.isEmpty() && (!replacement || !replacement->open)) {
        emit peerRttChanged(remote.peerId, -1);
        emit peerChanged(remote.peerId, remote.displayName, false);
        voice_->removePeer(remote.peerId);
        rendezvous_->setContactConnected(remote.peerId, false);
    }

    if (wasOpen) {
        emit statusChanged("Участник отключился от прямого P2P-канала.");
    }
    updateMesh();
}

void NetworkSession::connectMeshSignals() {
    connect(&mesh_, &MeshCoordinator::stateChanged, this, &NetworkSession::meshStateChanged);
    connect(&mesh_, &MeshCoordinator::statusChanged, this, &NetworkSession::statusChanged);
    connect(&mesh_, &MeshCoordinator::retryRequested, this, &NetworkSession::startMeshOffer);
}

void NetworkSession::connectVoiceSignals() {
    connect(voice_.get(), &VoiceSession::stateChanged, this, [this](bool active, bool muted) {
        if (!active || muted) {
            pttPressed_ = false;
        }
        updateAudioTransportGates();
        broadcastVoiceState();
        emit callStateChanged(active, muted);
    });
    connect(voice_.get(), &VoiceSession::peerChanged, this, &NetworkSession::peerVoiceChanged);
    connect(voice_.get(), &VoiceSession::networkStatsChanged, this,
            [this](const QString& peerId, double loss, int jitter, int buffer) {
                emit peerAudioStatsChanged(peerId, loss, jitter, buffer);
                const auto connection = connections_->infoForPeer(peerId);
                if (!connection || !connection->open) {
                    return;
                }
                auto packet =
                    basePacket(PacketType::VoiceQuality, VoiceQualityPayload{loss, jitter, buffer});
                packet.targetId = peerId;
                sendPacket(connection->connectionId, packet);
            });
    connect(voice_.get(), &VoiceSession::errorOccurred, this, &NetworkSession::errorOccurred);
    connect(voice_.get(), &VoiceSession::talkingStateChanged, this,
            &NetworkSession::localTalkingChanged);
    connect(voice_.get(), &VoiceSession::peerTalkingStateChanged, this,
            &NetworkSession::peerTalkingChanged);
}

void NetworkSession::connectRendezvousSignals() {
    connect(rendezvous_.get(), &RendezvousService::persistentPortsChanged, this,
            [this](const QList<quint16>& ports, quint16 boundPort) {
                const auto saved = app_.updateRendezvousPorts(ports, boundPort);
                if (!saved) {
                    Logger::instance().log(QtWarningMsg, "rendezvous", saved.error());
                }
            });
    connect(rendezvous_.get(), &RendezvousService::externalEndpointChanged, this,
            [this](const QString& address, quint16 port, const QString& method) {
                const auto saved = app_.updateRendezvousExternalEndpoint(address, port, method);
                if (!saved) {
                    Logger::instance().log(QtWarningMsg, "rendezvous", saved.error());
                }
                for (const auto& connectionId : connections_->openConnectionIds()) {
                    sendRendezvousMetadata(connectionId, true);
                }
            });
    connect(rendezvous_.get(), &RendezvousService::contactChanged, this,
            &NetworkSession::contactChanged);
    connect(rendezvous_.get(), &RendezvousService::connectionRequestReceived, this,
            [this](const QString& peerId, const QString& displayName,
                   const QString& requestId, const QString& requestedMesh) {
                const auto currentMesh = mesh_.meshId();
                if (!currentMesh.isEmpty() && currentMesh != requestedMesh) {
                    rendezvous_->respondToConnection(peerId, requestId, "busy", currentMesh);
                    Logger::instance().log(QtInfoMsg, "rendezvous",
                                           "Connection request answered busy: another mesh active");
                    return;
                }
                rendezvousNegotiations_.insert(
                    requestId,
                    {{peerId, displayName}, requestId, requestedMesh, false});
                emit contactConnectionRequest(peerId, displayName, requestId, requestedMesh);
            });
    connect(rendezvous_.get(), &RendezvousService::connectionResponseReceived, this,
            &NetworkSession::handleRendezvousResponse);
    connect(rendezvous_.get(), &RendezvousService::connectionRequestExpired, this,
            [this](const QString&, const QString& requestId) {
                rendezvousNegotiations_.remove(requestId);
                emit contactConnectionRequestExpired(requestId);
            });
    connect(rendezvous_.get(), &RendezvousService::signalingReceived, this,
            &NetworkSession::handleRendezvousSignaling);
}

void NetworkSession::configureKeepalive() {
    keepalive_ = new QTimer(this);
    keepalive_->setInterval(policy_.heartbeatIntervalSeconds * 1000);
    connect(keepalive_, &QTimer::timeout, this, &NetworkSession::handleKeepaliveTimeout);
    keepalive_->start();
}

void NetworkSession::handleKeepaliveTimeout() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto inactive =
        connections_->inactiveConnectionIds(now, policy_.livenessTimeoutSeconds * 1000LL);
    for (const auto& connectionId : inactive) {
        const auto connection = connections_->info(connectionId);
        const auto peerName =
            connection ? peerDisplayName(connection->remote.peerId) : connectionId;
        connections_->markTimedOut(connectionId, "Соединение с участником потеряно: " + peerName);
    }

    for (const auto& connectionId : connections_->openConnectionIds()) {
        sendPing(connectionId);
        const auto connection = connections_->info(connectionId);
        if (connection && (voice_->active() || connection->audioFramesAttempted > 0 ||
                           connection->audioFramesReceived > 0)) {
            Logger::instance().trace("voice_transport",
                                     QString("peer=%1 track=%2 attempted=%3 sent=%4 received=%5")
                                         .arg(connection->remote.peerId.left(8),
                                              connection->audioTrackOpen ? "open" : "closed")
                                         .arg(connection->audioFramesAttempted)
                                         .arg(connection->audioFramesSent)
                                         .arg(connection->audioFramesReceived));
        }
    }
}

void NetworkSession::connectApplicationSignals() {
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

    if (QNetworkInformation::loadDefaultBackend()) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this] {
                    mesh_.resetRetryBackoff();
                    ensureDynamicMesh();
                    rendezvous_->restartDiscovery();
                });
    }
}

NetworkSession::~NetworkSession() {
    rendezvous_->stop();
    const auto transport = connections_->audioTransport();
    transport->setReceiveEnabled(false);
    transport->setTransmitEnabled(false);
    transport->setIncomingSink({});
    voice_->setOutgoingAudioSink({});
    transport->stop();
    voice_.reset();
    connections_.reset();
}

QList<ContactPresence> NetworkSession::contacts() const {
    return rendezvous_->contacts();
}

Result<void> NetworkSession::requestContactConnection(const QString& peerId) {
    const auto contact = rendezvous_->contact(peerId);
    if (!contact) {
        return Result<void>::failure("Контакт не найден.");
    }
    if (connections_->infoForPeer(peerId)) {
        return Result<void>::failure("Прямое соединение с контактом уже существует.");
    }
    auto requestedMesh = mesh_.meshId();
    if (requestedMesh.isEmpty()) {
        requestedMesh = createUuid();
    }
    const auto requestId = rendezvous_->requestConnection(peerId, requestedMesh);
    if (requestId.isEmpty()) {
        return Result<void>::failure("Контакт сейчас недоступен для подключения.");
    }
    rendezvousNegotiations_.insert(
        requestId, {contact->identity, requestId, requestedMesh, true});
    QTimer::singleShot(65000, this, [this, requestId] {
        if (rendezvousNegotiations_.remove(requestId)) {
            Logger::instance().log(QtInfoMsg, "rendezvous",
                                   "Expired rendezvous negotiation state " +
                                       requestId.left(8));
        }
    });
    emit statusChanged("Запрос подключения отправлен контакту " +
                       contact->identity.displayName + ".");
    return Result<void>::success();
}

Result<void> NetworkSession::acceptContactConnection(const QString& requestId) {
    auto negotiation = rendezvousNegotiations_.find(requestId);
    if (negotiation == rendezvousNegotiations_.end() || negotiation->initiatedLocally) {
        return Result<void>::failure("Запрос подключения больше не актуален.");
    }
    if (!mesh_.meshId().isEmpty() && mesh_.meshId() != negotiation->meshId) {
        rendezvous_->respondToConnection(negotiation->peer.peerId, requestId, "busy",
                                         mesh_.meshId());
        rendezvousNegotiations_.erase(negotiation);
        return Result<void>::failure("Вы уже находитесь в другой mesh.");
    }
    if (mesh_.meshId().isEmpty()) {
        clearSessionData();
        mesh_.beginJoin(app_.identity(), negotiation->meshId);
    }
    negotiation->accepted = true;
    rendezvous_->respondToConnection(negotiation->peer.peerId, requestId, "accepted",
                                     negotiation->meshId);
    if (shouldInitiateNegotiation(app_.identity().peerId, negotiation->peer.peerId)) {
        startRendezvousOffer(requestId);
    }
    emit statusChanged("Запрос принят. Создаётся свежее P2P-соединение…");
    return Result<void>::success();
}

void NetworkSession::declineContactConnection(const QString& requestId) {
    const auto negotiation = rendezvousNegotiations_.take(requestId);
    if (negotiation.peer.peerId.isEmpty() || negotiation.initiatedLocally) {
        return;
    }
    rendezvous_->respondToConnection(negotiation.peer.peerId, requestId, "declined",
                                     negotiation.meshId);
}

void NetworkSession::handleRendezvousResponse(const QString& peerId, const QString& requestId,
                                              const QString& response,
                                              const QString& acceptedMesh) {
    auto negotiation = rendezvousNegotiations_.find(requestId);
    if (negotiation == rendezvousNegotiations_.end() || negotiation->peer.peerId != peerId) {
        return;
    }
    if (response != "accepted") {
        emit statusChanged(response == "busy" ? "Контакт находится в другой mesh."
                                               : "Контакт отклонил запрос подключения.");
        rendezvousNegotiations_.erase(negotiation);
        return;
    }
    negotiation->meshId = acceptedMesh;
    negotiation->accepted = true;
    if (!mesh_.meshId().isEmpty() && mesh_.meshId() != acceptedMesh) {
        emit statusChanged("Запрос принят, но активная mesh уже изменилась. Подключение отменено.");
        rendezvousNegotiations_.erase(negotiation);
        return;
    }
    if (mesh_.meshId().isEmpty()) {
        clearSessionData();
        mesh_.create(app_.identity(), acceptedMesh);
    }
    const auto deferredOffer = deferredRendezvousOffers_.take(peerId);
    if (!deferredOffer.isEmpty()) {
        QTimer::singleShot(0, this, [this, peerId, deferredOffer] {
            handleRendezvousSignaling(peerId, deferredOffer);
        });
    }
    if (shouldInitiateNegotiation(app_.identity().peerId, peerId)) {
        startRendezvousOffer(requestId);
    }
}

void NetworkSession::startRendezvousOffer(const QString& requestId) {
    const auto negotiation = rendezvousNegotiations_.value(requestId);
    if (negotiation.peer.peerId.isEmpty() || !negotiation.accepted) {
        return;
    }
    const auto connectionId = createUuid();
    const auto created = connections_->create(connectionId, negotiation.peer,
                                              ConnectionKind::RendezvousOffer);
    if (!created) {
        emit errorOccurred(created.error());
        return;
    }
    rendezvousConnections_.insert(connectionId, requestId);
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           "Creating fresh WebRTC offer for " +
                               negotiation.peer.displayName);
    const auto started = connections_->startOffer(connectionId);
    if (!started) {
        rendezvousConnections_.remove(connectionId);
        emit errorOccurred(started.error());
    }
}

void NetworkSession::handleRendezvousSignaling(const QString& peerId,
                                               const QByteArray& document) {
    const auto decoded = InvitationCodec::decode(document);
    if (!decoded) {
        Logger::instance().log(QtWarningMsg, "rendezvous",
                               "Invalid rendezvous signaling document: " + decoded.error());
        return;
    }
    const auto invitation = decoded.value();
    if (invitation.meshId != mesh_.meshId()) {
        Logger::instance().log(QtWarningMsg, "rendezvous",
                               "Rendezvous signaling belongs to another mesh");
        return;
    }
    const auto contact = rendezvous_->contact(peerId);
    if (!contact) {
        return;
    }
    if (invitation.kind == Invitation::Kind::Offer) {
        if (connections_->contains(invitation.connectionId)) {
            return;
        }
        auto requestId = QString{};
        for (auto it = rendezvousNegotiations_.cbegin(); it != rendezvousNegotiations_.cend();
             ++it) {
            if (it->accepted && it->peer.peerId == peerId &&
                it->meshId == invitation.meshId) {
                requestId = it.key();
                break;
            }
        }
        if (requestId.isEmpty()) {
            const auto pendingRequest = std::find_if(
                rendezvousNegotiations_.cbegin(), rendezvousNegotiations_.cend(),
                [&peerId, &invitation](const RendezvousNegotiation& negotiation) {
                    return negotiation.initiatedLocally && !negotiation.accepted &&
                           negotiation.peer.peerId == peerId &&
                           negotiation.meshId == invitation.meshId;
                });
            if (pendingRequest != rendezvousNegotiations_.cend() &&
                deferredRendezvousOffers_.size() < 8) {
                deferredRendezvousOffers_.insert(peerId, document);
                QTimer::singleShot(10000, this, [this, peerId, document] {
                    if (deferredRendezvousOffers_.value(peerId) == document) {
                        deferredRendezvousOffers_.remove(peerId);
                    }
                });
                Logger::instance().log(
                    QtInfoMsg, "rendezvous",
                    "Fresh offer arrived before acceptance response and was deferred");
            } else {
                Logger::instance().log(
                    QtWarningMsg, "rendezvous",
                    "Ignored signaling without an accepted connection request");
            }
            return;
        }
        const auto created = connections_->create(invitation.connectionId, contact->identity,
                                                  ConnectionKind::RendezvousAnswer);
        if (!created) {
            emit errorOccurred(created.error());
            return;
        }
        rendezvousConnections_.insert(invitation.connectionId, requestId);
        Logger::instance().log(QtInfoMsg, "rendezvous", "Fresh WebRTC offer applied");
        const auto accepted = connections_->acceptOffer(invitation.connectionId, invitation.sdp);
        if (!accepted) {
            emit errorOccurred(accepted.error());
        }
        return;
    }
    const auto connection = connections_->info(invitation.connectionId);
    if (!connection || connection->remote.peerId != peerId ||
        connection->kind != ConnectionKind::RendezvousOffer) {
        return;
    }
    Logger::instance().log(QtInfoMsg, "rendezvous", "Fresh WebRTC answer applied");
    const auto accepted = connections_->acceptAnswer(invitation.connectionId, invitation.sdp);
    if (!accepted) {
        emit errorOccurred(accepted.error());
    }
}

MeshSessionState NetworkSession::meshState() const {
    return mesh_.state();
}

Result<void> NetworkSession::createMesh() {
    if (!connections_->connections().isEmpty() || !mesh_.meshId().isEmpty()) {
        return Result<void>::failure("Сначала покиньте текущую mesh-сессию.");
    }
    clearSessionData();
    mesh_.create(app_.identity(), createUuid());
    emit statusChanged("Mesh создан. Теперь можно пригласить участника.");
    updateMesh();
    return Result<void>::success();
}

void NetworkSession::leaveMesh() {
    if (mesh_.meshId().isEmpty() && connections_->connections().isEmpty()) {
        return;
    }
    pttPressed_ = false;
    connections_->audioTransport()->setReceiveEnabled(false);
    connections_->audioTransport()->setTransmitEnabled(false);
    voice_->leave();
    if (!mesh_.meshId().isEmpty()) {
        auto leave = basePacket(PacketType::PeerLeave, PeerLeavePayload{"left"});
        leave.ttl = policy_.maxPeers;
        router_.rememberPacket(leave.packetId);
        broadcastService(leave);
    }
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
    if (mesh_.meshId().isEmpty() || !mesh_.joined()) {
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

    const auto connectionId = createUuid();
    auto created = connections_->create(connectionId, {}, ConnectionKind::ManualOffer);
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

Result<void> NetworkSession::importSignalingDocument(const QByteArray& document) {
    auto decoded = InvitationCodec::decode(document);
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
        const bool joiningMesh = mesh_.meshId().isEmpty();
        if (joiningMesh) {
            clearSessionData();
            mesh_.beginJoin(app_.identity(), invitation.meshId);
        }

        auto created =
            connections_->create(invitation.connectionId, {}, ConnectionKind::ManualAnswer);
        if (!created) {
            if (joiningMesh) {
                mesh_.leave();
            }
            return created;
        }
        emit statusChanged("Offer импортирован. Создаётся answer…");
        const auto accepted = connections_->acceptOffer(invitation.connectionId, invitation.sdp);
        if (!accepted && joiningMesh) {
            mesh_.leave();
            clearSessionData();
        }
        return accepted;
    }

    if (!invitation.meshId.isEmpty() && mesh_.meshId() != invitation.meshId) {
        return Result<void>::failure("Answer относится к другому mesh.");
    }
    const auto connection = connections_->info(invitation.connectionId);
    if (!connection) {
        return Result<void>::failure("Не найдено исходное приглашение для этого answer.");
    }
    if (connection->answerApplied) {
        return Result<void>::failure("Этот answer уже импортирован.");
    }

    emit statusChanged("Answer импортирован. Устанавливается прямое P2P-соединение…");
    return connections_->acceptAnswer(invitation.connectionId, invitation.sdp);
}

void NetworkSession::emitSignaling(const QString& connectionId, const QString& sdp) {
    const auto connection = connections_->info(connectionId);
    if (!connection) {
        return;
    }
    Invitation invitation;
    invitation.kind =
        isOffer(connection->kind) ? Invitation::Kind::Offer : Invitation::Kind::Answer;
    invitation.meshId = mesh_.meshId();
    invitation.connectionId = connectionId;
    invitation.sdp = sdp;

    if (isRendezvous(connection->kind)) {
        const auto sent = rendezvous_->sendSignal(connection->remote.peerId,
                                                  InvitationCodec::encode(invitation));
        if (!sent) {
            emit errorOccurred("Не удалось передать fresh WebRTC signaling: " + sent.error());
            return;
        }
        Logger::instance().log(
            QtInfoMsg, "rendezvous",
            QString("Fresh WebRTC %1 queued for %2")
                .arg(isOffer(connection->kind) ? "offer" : "answer",
                     connection->remote.displayName));
        emit statusChanged(isOffer(connection->kind)
                               ? "Fresh offer отправлен через rendezvous."
                               : "Fresh answer отправлен через rendezvous.");
        return;
    }

    if (isMeshManaged(connection->kind)) {
        auto packet =
            basePacket(isOffer(connection->kind) ? PacketType::LinkOffer : PacketType::LinkAnswer,
                       LinkSignalingPayload{connectionId, connection->generation, sdp});
        packet.targetId = connection->remote.peerId;
        packet.ttl = policy_.maxPeers;
        router_.rememberPacket(packet.packetId);
        Logger::instance().log(QtInfoMsg, "mesh_signaling",
                               QString("origin type=%1 link=%2 generation=%3 target=%4 ttl=%5")
                                   .arg(toString(packet.type), connectionId.left(8))
                                   .arg(connection->generation)
                                   .arg(packet.targetId.left(8))
                                   .arg(packet.ttl));
        // Link negotiation is rare and the mesh is capped at six peers. Flooding it over
        // every established edge is more reliable than trusting one possibly stale route;
        // packet-id deduplication and TTL keep the traffic bounded.
        broadcastService(packet);
        emit statusChanged(isOffer(connection->kind)
                               ? "Mesh offer отправлен через доступные P2P-каналы."
                               : "Mesh answer отправлен через доступные P2P-каналы.");
        return;
    }

    const auto document = InvitationCodec::encode(invitation);
    const auto text = InvitationCodec::encodeText(invitation);
    if (!text) {
        emit errorOccurred("Не удалось создать код приглашения: " + text.error());
        return;
    }
    const auto kind = invitation.kind == Invitation::Kind::Offer ? "offer" : "answer";
    emit signalingReady(kind, text.value(), document);
    emit statusChanged(invitation.kind == Invitation::Kind::Offer
                           ? "Приглашение готово. Ожидание answer."
                           : "Answer готов. Отправьте его пригласившему участнику.");
}

Packet NetworkSession::basePacket(PacketType type, PacketPayload payload) const {
    return {type,
            createUuid(),
            mesh_.meshId(),
            app_.identity().peerId,
            QDateTime::currentDateTimeUtc(),
            std::move(payload)};
}

bool NetworkSession::sendPacket(const QString& connectionId, const Packet& packet) {
    const auto encoded = PacketCodec::encode(packet);
    if (!encoded) {
        Logger::instance().log(QtWarningMsg, "protocol",
                               QString("encode_failed type=%1 connection=%2 target=%3 error=%4")
                                   .arg(toString(packet.type), connectionId.left(8),
                                        packet.targetId.left(8), encoded.error()));
        emit errorOccurred("Не удалось подготовить сетевой пакет: " + encoded.error());
        return false;
    }
    const auto& bytes = encoded.value();
    const bool chatPacket =
        packet.type == PacketType::ChatMessage || packet.type == PacketType::ChatAck;
    const bool sent = chatPacket
                          ? connections_->sendChat(connectionId, QString::fromUtf8(bytes))
                          : connections_->sendControl(connectionId, QString::fromUtf8(bytes));
    if (!sent) {
        Logger::instance().log(
            QtWarningMsg, "protocol",
            QString("send_failed type=%1 connection=%2 target=%3 bytes=%4")
                .arg(toString(packet.type), connectionId.left(8), packet.targetId.left(8))
                .arg(bytes.size()));
        emit errorOccurred("Не удалось отправить пакет участнику.");
    }
    return sent;
}

void NetworkSession::sendHello(const QString& connectionId) {
    sendPacket(connectionId,
               basePacket(PacketType::PeerHello, HelloPayload{app_.identity().displayName}));
}

void NetworkSession::sendPing(const QString& connectionId) {
    const auto nonce = createUuid();
    pendingPings_.insert(connectionId, PendingPing{nonce, monotonicNs()});
    sendPacket(connectionId, basePacket(PacketType::Ping,
                                        HeartbeatPayload{nonce, QDateTime::currentDateTimeUtc()}));
}

void NetworkSession::receivePong(const QString& connectionId, const HeartbeatPayload& payload) {
    const auto pending = pendingPings_.find(connectionId);
    if (pending == pendingPings_.end() || pending->nonce != payload.nonce) {
        return;
    }
    const auto elapsedNs = monotonicNs() - pending->sentAtNs;
    pendingPings_.erase(pending);
    const auto sampleMs = static_cast<int>((elapsedNs + 500'000) / 1'000'000);
    const auto smoothedMs = connections_->recordRoundTripTime(connectionId, sampleMs);
    const auto connection = connections_->info(connectionId);
    if (smoothedMs >= 0 && connection && !connection->remote.peerId.isEmpty()) {
        emit peerRttChanged(connection->remote.peerId, smoothedMs);
    }
}

void NetworkSession::sendPeerList(const QString& connectionId) {
    sendPacket(connectionId,
               basePacket(PacketType::PeerSnapshot, PeerSnapshotPayload{mesh_.peers()}));
}

void NetworkSession::announceLocalPeer(const QString& excludedConnection) {
    auto packet = basePacket(PacketType::PeerAnnounce, PeerAnnouncePayload{app_.identity(), 0});
    packet.ttl = policy_.maxPeers;
    router_.rememberPacket(packet.packetId);
    broadcastService(packet, excludedConnection);
}

void NetworkSession::sendVoiceState(const QString& connectionId) {
    sendPacket(connectionId, basePacket(PacketType::VoiceState,
                                        VoiceStatePayload{voice_->active(), voice_->muted()}));
}

void NetworkSession::sendRendezvousMetadata(const QString& connectionId,
                                            bool acknowledgement) {
    const auto connection = connections_->info(connectionId);
    if (!connection || !connection->open || connection->remote.peerId.isEmpty()) {
        return;
    }
    auto secret = rendezvous_->secretForPeer(connection->remote.peerId);
    const bool localOwnsSecret = shouldInitiateNegotiation(app_.identity().peerId,
                                                           connection->remote.peerId);
    bool generatedSecret = false;
    if (secret.isEmpty() && localOwnsSecret) {
        secret.resize(32);
        for (qsizetype offset = 0; offset < secret.size(); offset += 4) {
            const auto random = QRandomGenerator::system()->generate();
            std::memcpy(secret.data() + offset, &random, sizeof(random));
        }
        ContactRecord contact;
        contact.identity = connection->remote;
        contact.rendezvousSecret = secret;
        contact.publicEndpoint = {};
        contact.localEndpoint = {};
        contact.lastSeen = QDateTime::currentDateTimeUtc();
        contact.lastMeshId = mesh_.meshId();
        const auto remembered = rendezvous_->rememberContact(std::move(contact), false);
        if (!remembered) {
            Logger::instance().log(QtWarningMsg, "rendezvous", remembered.error());
            return;
        }
        generatedSecret = true;
    }
    if (secret.isEmpty()) {
        return;
    }
    const auto local = rendezvous_->localEndpoint();
    const auto external = rendezvous_->publicEndpoint();
    const bool manualPairing = connection->kind == ConnectionKind::ManualOffer ||
                               connection->kind == ConnectionKind::ManualAnswer;
    const auto serializedSecret = !acknowledgement && localOwnsSecret &&
                                          (generatedSecret || manualPairing)
                                      ? QString::fromLatin1(secret.toBase64(
                                            QByteArray::Base64UrlEncoding |
                                            QByteArray::OmitTrailingEquals))
                                      : QString{};
    auto packet = basePacket(
        PacketType::RendezvousMetadata,
        RendezvousMetadataPayload{serializedSecret, external.address, external.port,
                                  local.address, local.port, rendezvous_->mappingMethod(),
                                  acknowledgement});
    packet.targetId = connection->remote.peerId;
    if (sendPacket(connectionId, packet)) {
        Logger::instance().log(
            QtInfoMsg, "rendezvous",
            QString("Rendezvous metadata %1 for %2")
                .arg(acknowledgement ? "acknowledged" : "sent",
                     connection->remote.displayName));
    }
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
            !shouldInitiateNegotiation(app_.identity().peerId, peer.peerId) ||
            !mesh_.canAttemptLink(peer.peerId)) {
            continue;
        }
        const auto existing = connections_->infoForPeer(peer.peerId);
        if (existing) {
            continue;
        }
        startMeshOffer(peer);
    }
}

void NetworkSession::startMeshOffer(const PeerIdentity& peer) {
    if (peer.peerId.isEmpty() || !mesh_.canAttemptLink(peer.peerId)) {
        return;
    }
    const auto existing = connections_->infoForPeer(peer.peerId);
    if (existing) {
        return;
    }

    const auto connectionId = createUuid();
    const auto generation = mesh_.nextLinkGeneration(peer.peerId);
    auto created = connections_->create(connectionId, peer, ConnectionKind::MeshOffer, generation);
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

void NetworkSession::broadcastService(const Packet& packet, const QString& excludedConnection) {
    for (const auto& connectionId : connections_->openConnectionIds()) {
        if (connectionId != excludedConnection) {
            sendPacket(connectionId, packet);
        }
    }
}

bool NetworkSession::sendRouted(Packet packet, const QString& excludedConnection) {
    if (packet.targetId.isEmpty()) {
        return false;
    }
    const auto direct = connections_->infoForPeer(packet.targetId);
    if (direct && direct->open && direct->connectionId != excludedConnection) {
        return sendPacket(direct->connectionId, packet);
    }
    const auto nextHop = router_.nextHop(packet.targetId, excludedConnection);
    if (nextHop && connections_->info(*nextHop) && connections_->info(*nextHop)->open) {
        return sendPacket(*nextHop, packet);
    }
    if (packet.senderId == app_.identity().peerId) {
        auto& queue = pendingRouted_[packet.targetId];
        if (packet.type == PacketType::LinkOffer || packet.type == PacketType::LinkAnswer) {
            queue.clear();
        }
        if (queue.size() < 16) {
            queue.append(std::move(packet));
        }
        requestRoute(queue.constFirst().targetId);
    }
    return false;
}

void NetworkSession::requestRoute(const QString& peerId) {
    if (peerId.isEmpty() || routeRequests_.contains(peerId)) {
        return;
    }
    const auto requestId = createUuid();
    routeRequests_.insert(peerId, requestId);
    auto request = basePacket(PacketType::RouteRequest, RoutePayload{requestId, 0});
    request.targetId = peerId;
    request.ttl = policy_.maxPeers;
    router_.rememberPacket(request.packetId);
    broadcastService(request);
    QTimer::singleShot(5000, this, [this, peerId, requestId] {
        if (routeRequests_.value(peerId) == requestId) {
            routeRequests_.remove(peerId);
        }
    });
}

void NetworkSession::flushRouted(const QString& peerId) {
    routeRequests_.remove(peerId);
    const auto packets = pendingRouted_.take(peerId);
    for (auto packet : packets) {
        sendRouted(std::move(packet));
    }
}

Result<void> NetworkSession::sendMessage(const QString& text) {
    if (mesh_.meshId().isEmpty() || !mesh_.joined()) {
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
    emit deliveryChanged(outgoing.value().message.messageId, 0, static_cast<int>(targets.size()));
    for (const auto& connectionId : connections_->openConnectionIds()) {
        sendPacket(connectionId, outgoing.value().packet);
    }
    return Result<void>::success();
}

Result<void> NetworkSession::startCall() {
    if (voice_->active()) {
        return Result<void>::success();
    }
    if (mesh_.meshId().isEmpty() || !mesh_.joined()) {
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
    pttPressed_ = false;
    connections_->audioTransport()->setReceiveEnabled(false);
    connections_->audioTransport()->setTransmitEnabled(false);
    voice_->leave();
    emit statusChanged("Вы вышли из голосового звонка.");
}

void NetworkSession::setMuted(bool muted) {
    if (!voice_->active() || voice_->muted() == muted) {
        return;
    }
    if (muted) {
        pttPressed_ = false;
        connections_->audioTransport()->setTransmitEnabled(false);
    }
    voice_->setMuted(muted);
    updateAudioTransportGates();
    emit statusChanged(muted ? "Микрофон выключен." : "Микрофон включён.");
}

void NetworkSession::setDeafened(bool deafened) {
    voice_->setDeafened(deafened);
    emit audioStateChanged();
}

void NetworkSession::setMicrophoneTest(bool enabled) {
    if (enabled) {
        pttPressed_ = false;
        connections_->audioTransport()->setTransmitEnabled(false);
    }
    voice_->setMicrophoneTest(enabled);
    updateAudioTransportGates();
    emit audioStateChanged();
}

void NetworkSession::setPttPressed(bool pressed) {
    pttPressed_ = pressed && voice_->active() && !voice_->muted() && !voice_->microphoneTest();
    if (!pttPressed_) {
        connections_->audioTransport()->setTransmitEnabled(false);
    }
    voice_->setPttPressed(pttPressed_);
    updateAudioTransportGates();
}

void NetworkSession::setPeerVolume(const QString& peerId, int percent) {
    voice_->setPeerVolume(peerId, percent);
}

double NetworkSession::microphoneLevel() const {
    return voice_ ? voice_->microphoneLevel() : 0.0;
}

Result<void> NetworkSession::applyAudioPreferences(const AudioPreferences& preferences) {
    pttPressed_ = false;
    connections_->audioTransport()->setTransmitEnabled(false);
    voice_->setPttPressed(false);
    const auto result = voice_->applyPreferences(preferences);
    updateAudioTransportGates();
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
    const auto localRendezvous = rendezvous_->localEndpoint();
    const auto publicRendezvous = rendezvous_->publicEndpoint();
    lines.prepend(QString("Rendezvous contacts: %1").arg(rendezvous_->contacts().size()));
    lines.prepend(QString("Rendezvous mapping: %1").arg(rendezvous_->mappingMethod()));
    lines.prepend(QString("Rendezvous public UDP: %1:%2")
                      .arg(publicRendezvous.address.isEmpty() ? "unknown"
                                                             : publicRendezvous.address)
                      .arg(publicRendezvous.port));
    lines.prepend(QString("Rendezvous local UDP: %1:%2")
                      .arg(localRendezvous.address.isEmpty() ? "unknown"
                                                            : localRendezvous.address)
                      .arg(localRendezvous.port));
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
        lines.append(QString("    candidates: host=%1 srflx=%2 relay=%3 · selected: %4")
                         .arg(connection.hostCandidates)
                         .arg(connection.serverReflexiveCandidates)
                         .arg(connection.relayCandidates)
                         .arg(connection.selectedCandidatePair.isEmpty()
                                  ? "не выбрана"
                                  : connection.selectedCandidatePair));
        lines.append(QString("    channels: control=%1 chat=%2 audio-rtp=%3 hello=%4 · "
                             "buffered: control=%5 chat=%6 · queued: control=%7 chat=%8")
                         .arg(connection.controlChannelOpen ? "open" : "closed",
                              connection.chatChannelOpen ? "open" : "closed",
                              connection.audioTrackOpen ? "open" : "closed",
                              connection.helloReceived ? "yes" : "no")
                         .arg(connection.controlBufferedBytes)
                         .arg(connection.chatBufferedBytes)
                         .arg(connection.queuedControlBytes)
                         .arg(connection.queuedChatBytes));
        lines.append(QString("    audio frames: attempted=%1 sent=%2 received=%3")
                         .arg(connection.audioFramesAttempted)
                         .arg(connection.audioFramesSent)
                         .arg(connection.audioFramesReceived));
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

void NetworkSession::updateAudioTransportGates() {
    const auto transport = connections_->audioTransport();
    const bool active = voice_->active();
    transport->setReceiveEnabled(active);

    const bool canTransmit = active && !voice_->muted() && !voice_->microphoneTest();
    const bool modeAllowsTransmit =
        voice_->preferences().inputMode == AudioInputMode::VoiceActivity || pttPressed_;
    transport->setTransmitEnabled(canTransmit && modeAllowsTransmit);
}

void NetworkSession::clearSessionData() {
    pttPressed_ = false;
    connections_->audioTransport()->setReceiveEnabled(false);
    connections_->audioTransport()->setTransmitEnabled(false);
    messaging_.clear();
    voice_->clear();
    router_.clear();
    pendingRouted_.clear();
    routeRequests_.clear();
    audioNegotiations_.clear();
    pendingPings_.clear();
}

} // namespace tmc
