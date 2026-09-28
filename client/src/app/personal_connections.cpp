#include "tmc/app/personal_connections.h"
#include "tmc/app/application_controller.h"
#include "tmc/app/negotiation_policy.h"
#include "tmc/core/uuid.h"
#include "tmc/protocol/packet_codec.h"

namespace tmc {
PersonalConnections::PersonalConnections(ApplicationController& app, SignalSender sender, QObject* parent)
    : QObject(parent), app_(app), sender_(std::move(sender)),
      connections_(app.config().stunServers, app.connectionPolicy()),
      files_([this](const QString& peerId, const QByteArray& bytes) {
          const auto link = connections_.infoForPeer(peerId);
          return link && link->open && connections_.sendTransfer(link->connectionId, bytes);
      }) {
    clock_.start();
    connect(&app_, &ApplicationController::stunServersChanged, &connections_, &ConnectionManager::setStunServers);
    connect(&connections_, &ConnectionManager::localDescriptionReady, this,
            [this](const QString& id, const QString& sdp) {
                const auto link = connections_.info(id);
                if (link && !sender_(link->remote.peerId, id, isOffer(link->kind) ? "offer" : "answer", sdp))
                    connections_.discard(id);
            });
    connect(&connections_, &ConnectionManager::transportOpened, this,
            [this](const QString& id, const PeerIdentity&) {
                nonces_.insert(id, security::randomToken());
                send(id, PacketType::PeerHello, HelloPayload{app_.identity().displayName, 0,
                    app_.signingKey()->publicKey(), nonces_.value(id)});
                sendProof(id);
            });
    connect(&connections_, &ConnectionManager::controlTextReceived, this, &PersonalConnections::receiveControl);
    connect(&connections_, &ConnectionManager::linkOpened, this,
            [this](const QString&, const PeerIdentity& peer) {
                retryAt_.remove(peer.peerId);
                files_.setPeerConnected(peer.peerId, true);
            });
    connect(&connections_, &ConnectionManager::linkRemoved, this,
            [this](const QString& id, const PeerIdentity& peer, bool) {
                nonces_.remove(id);
                hellos_.remove(id);
                pingNonces_.remove(id);
                idleAt_.remove(id);
                retryAt_.insert(peer.peerId, clock_.elapsed() + 3000);
                files_.setPeerConnected(peer.peerId, false);
            });
    connect(&connections_, &ConnectionManager::transferReceived, this,
            [this](const QString& id, const QByteArray& bytes) {
                const auto link = connections_.info(id);
                if (link && link->open) files_.receive(link->remote.peerId, link->remote.displayName, bytes);
            });
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &PersonalConnections::maintain);
    timer_.start();
}

PeerIdentity PersonalConnections::knownPeer(const QString& id) const {
    for (const auto& peer : app_.acquaintances()) if (peer.peerId == id) return peer;
    return {};
}

Result<void> PersonalConnections::sendFile(const QString& peerId, const QString& path) {
    const auto peer = knownPeer(peerId);
    if (!peer.isValid()) return Result<void>::failure("Получатель отсутствует в списке знакомых.");
    const auto offered = files_.offer(peer.peerId, peer.displayName, path);
    if (offered) begin(peer);
    return offered;
}

void PersonalConnections::begin(const PeerIdentity& peer) {
    if (!peer.isValid() || connections_.infoForPeer(peer.peerId) ||
        connections_.connections().size() >= app_.connectionPolicy().maxPeers - 1) return;
    const auto id = createUuid();
    const auto created = connections_.create(id, peer, ConnectionKind::MeshOffer, 1);
    if (created) {
        idleAt_.insert(id, clock_.elapsed());
        const auto started = connections_.startOffer(id);
        if (!started) connections_.discard(id);
    }
    retryAt_.insert(peer.peerId, clock_.elapsed() + 5000);
}

void PersonalConnections::receiveSignal(const QString& peerId, const QString& id,
                                         const QString& kind, const QString& sdp) {
    const auto peer = knownPeer(peerId);
    if (!peer.isValid() || !isCanonicalUuid(id) || sdp.isEmpty()) return;
    const auto existing = connections_.infoForPeer(peerId);
    if (kind == "offer") {
        if (existing) {
            if (existing->open || existing->connectionId == id ||
                shouldInitiateNegotiation(app_.identity().peerId, peerId)) return;
            connections_.discard(existing->connectionId);
        }
        if (connections_.contains(id) || connections_.connections().size() >= app_.connectionPolicy().maxPeers - 1) return;
        const auto created = connections_.create(id, peer, ConnectionKind::MeshAnswer, 1);
        if (!created) return;
        idleAt_.insert(id, clock_.elapsed());
        const auto accepted = connections_.acceptOffer(id, sdp);
        if (!accepted) connections_.discard(id);
    } else if (kind == "answer" && existing && existing->connectionId == id &&
               isOffer(existing->kind) && !existing->answerApplied) {
        const auto accepted = connections_.acceptAnswer(id, sdp);
        if (!accepted) connections_.discard(id);
    }
}

void PersonalConnections::setTurnServers(const QList<RelayServer>& servers, int seconds) {
    connections_.setTurnServers(servers, seconds);
}

void PersonalConnections::send(const QString& id, PacketType type, PacketPayload payload) {
    const Packet packet{type, createUuid(), id, app_.identity().peerId,
        QDateTime::currentDateTimeUtc(), std::move(payload)};
    const auto encoded = PacketCodec::encode(packet);
    if (encoded) connections_.sendControl(id, QString::fromUtf8(encoded.value()));
}

void PersonalConnections::sendProof(const QString& id) {
    const auto hello = hellos_.constFind(id);
    const auto fingerprints = connections_.fingerprints(id);
    if (hello == hellos_.cend() || !nonces_.contains(id) || fingerprints.first.isEmpty() || fingerprints.second.isEmpty()) return;
    const auto signature = app_.signingKey()->sign(security::transcript("tmc.personal-auth.v1", {
        id, app_.identity().peerId, security::identityId(hello->publicKey), nonces_.value(id), hello->nonce,
        fingerprints.first, fingerprints.second, app_.identity().displayName}));
    send(id, PacketType::PeerProof, PeerProofPayload{signature});
}

void PersonalConnections::receiveControl(const QString& id, const QString& text) {
    const auto link = connections_.info(id);
    const auto decoded = PacketCodec::decode(text.toUtf8(), id);
    if (!link || !decoded || decoded.value().senderId != link->remote.peerId) { connections_.discard(id); return; }
    const auto& packet = decoded.value();
    if (packet.type == PacketType::PeerHello) {
        const auto& hello = std::get<HelloPayload>(packet.payload);
        const auto previous = hellos_.constFind(id);
        if (security::identityId(hello.publicKey) != link->remote.peerId ||
            (previous != hellos_.cend() && (previous->nonce != hello.nonce || previous->publicKey != hello.publicKey))) {
            connections_.discard(id);
            return;
        }
        hellos_.insert(id, hello);
        sendProof(id);
    } else if (packet.type == PacketType::PeerProof) {
        const auto hello = hellos_.constFind(id);
        const auto fingerprints = connections_.fingerprints(id);
        if (hello == hellos_.cend() || !nonces_.contains(id) || fingerprints.first.isEmpty() || fingerprints.second.isEmpty()) return;
        const auto transcript = security::transcript("tmc.personal-auth.v1", {
            id, link->remote.peerId, app_.identity().peerId, hello->nonce, nonces_.value(id),
            fingerprints.second, fingerprints.first, hello->displayName});
        if (!security::verify(hello->publicKey, transcript, std::get<PeerProofPayload>(packet.payload).signature)) {
            connections_.discard(id);
            return;
        }
        connections_.markHelloReceived(id, {link->remote.peerId, hello->displayName});
    } else if (link->open && packet.type == PacketType::Ping) {
        send(id, PacketType::Pong, packet.payload);
    } else if (link->open && packet.type == PacketType::Pong) {
        if (pingNonces_.value(id) == std::get<HeartbeatPayload>(packet.payload).nonce) pingNonces_.remove(id);
    } else connections_.discard(id);
}

void PersonalConnections::maintain() {
    const auto now = clock_.elapsed();
    for (const auto& peer : app_.acquaintances()) {
        if (files_.hasActive(peer.peerId) && !connections_.infoForPeer(peer.peerId) && now >= retryAt_.value(peer.peerId)) begin(peer);
    }
    for (const auto& link : connections_.connections()) {
        if (files_.hasActive(link.remote.peerId)) idleAt_.insert(link.connectionId, now);
        if (now - idleAt_.value(link.connectionId, now) >= 60000) { connections_.discard(link.connectionId); continue; }
    }
    for (const auto& id : connections_.inactiveConnectionIds(QDateTime::currentMSecsSinceEpoch(), 30000))
        connections_.markTimedOut(id, "Связь для передачи файлов потеряна.");
    if ((now / 1000) % 10 != 0) return;
    for (const auto& id : connections_.openConnectionIds()) {
        // A nonce remains pending until echoed by the authenticated remote endpoint.
        if (!pingNonces_.contains(id)) pingNonces_.insert(id, security::randomToken());
        send(id, PacketType::Ping, HeartbeatPayload{pingNonces_.value(id), QDateTime::currentDateTimeUtc()});
    }
}
} // namespace tmc
