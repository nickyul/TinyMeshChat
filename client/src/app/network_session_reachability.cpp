#include "tmc/app/network_session.h"
#include "tmc/app/application_controller.h"
#include "tmc/app/voice_session.h"
#include "tmc/transfers/file_transfer_service.h"
#include <QUrl>

namespace tmc {

void NetworkSession::notePeerReachable(const QString& peerId) {
    if (peerId != app_.identity().peerId && !mesh_.peer(peerId).peerId.isEmpty())
        lastReachable_.insert(peerId, reachabilityClock_.elapsed());
}

void NetworkSession::removeMeshPeer(const QString& peerId) {
    if (peerId == app_.identity().peerId) return;
    if (!mesh_.forgetPeer(peerId)) return;
    files_->removePeer(peerId);
    const auto link = connections_->infoForPeer(peerId);
    if (link) connections_->discard(link->connectionId);
    router_.forgetPeer(peerId);
    pendingRouted_.remove(peerId);
    routeRequests_.remove(peerId);
    lastReachable_.remove(peerId);
    peerSignalingServers_.remove(peerId);
    voice_->removePeer(peerId);
    emit peerRemoved(peerId);
    updateMesh();
}

void NetworkSession::maintainPeerReachability() {
    if (!mesh_.joined()) return;
    const auto now = reachabilityClock_.elapsed();
    bool removed = false;
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == app_.identity().peerId) continue;
        if (!lastReachable_.contains(peer.peerId)) lastReachable_.insert(peer.peerId, now);
        const auto silentFor = now - lastReachable_.value(peer.peerId);
        const auto link = connections_->infoForPeer(peer.peerId);
        if (!link || !link->open || silentFor >= 10000) requestRoute(peer.peerId);
        if (silentFor < 30000) continue;
        // Capability survives a temporary server outage, but an explicitly
        // disabled server cannot provide this path.
        const auto server = peerSignalingServers_.value(peer.peerId);
        const bool serverRecovery = !server.isEmpty() &&
            QUrl(server) == QUrl(app_.config().signalingServerUrl);
        if (serverRecovery) continue;
        const auto name = peer.displayName;
        removeMeshPeer(peer.peerId);
        emit statusChanged("Участник «" + name + "» недоступен более 30 секунд и удалён из беседы.");
        removed = true;
    }
    if (removed && mesh_.peerCount() == 1) {
        leaveMesh();
        emit statusChanged("Связь с участниками потеряна. Для подключения примите новое приглашение.");
    } else {
        ensureDynamicMesh();
    }
}

} // namespace tmc
