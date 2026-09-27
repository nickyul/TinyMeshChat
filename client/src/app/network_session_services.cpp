#include "tmc/app/network_session.h"
#include "tmc/app/application_controller.h"
#include "tmc/app/personal_connections.h"
#include "tmc/sharing/screen_share_service.h"
#include "tmc/signaling_client/signaling_client.h"
#include "tmc/transfers/file_transfer_service.h"

namespace tmc {

void NetworkSession::configurePeerServices() {
    sharing_ = std::make_unique<ScreenShareService>(app_.identity().peerId,
        [this](const QString& peerId, const QByteArray& bytes) {
            const auto link = connections_->infoForPeer(peerId);
            return link && link->open && connections_->sendStream(link->connectionId, bytes);
        });
    connect(sharing_.get(), &ScreenShareService::errorOccurred, this, &NetworkSession::errorOccurred);
    connect(connections_.get(), &ConnectionManager::streamReceived, this,
            [this](const QString& id, const QByteArray& bytes) {
                const auto link = connections_->info(id);
                if (!link || !link->open) return;
                notePeerReachable(link->remote.peerId);
                sharing_->receive(link->remote.peerId, bytes);
            });
    personal_ = std::make_unique<PersonalConnections>(app_,
        [this](const QString& peer, const QString& id, const QString& kind, const QString& sdp) {
            if (!signalingConnected()) return false;
            return std::holds_alternative<QString>(signaling_->request("direct.send",
                {{"identityId", peer}, {"connectionId", id}, {"kind", kind}, {"sdp", sdp}}));
        });
    files_ = std::make_unique<FileTransferService>([this](const QString& peerId, const QByteArray& bytes) {
        const auto link = connections_->infoForPeer(peerId);
        return link && link->open && connections_->sendTransfer(link->connectionId, bytes);
    });
    connect(connections_.get(), &ConnectionManager::transferReceived, this,
            [this](const QString& id, const QByteArray& bytes) {
                const auto link = connections_->info(id);
                if (!link || !link->open) return;
                notePeerReachable(link->remote.peerId);
                files_->receive(link->remote.peerId, link->remote.displayName, bytes);
            });
}

QVariantList NetworkSession::fileRecipients() const {
    QVariantList result;
    for (const auto& peer : mesh_.peers()) {
        const auto link = connections_->infoForPeer(peer.peerId);
        if (peer.peerId != app_.identity().peerId && link && link->open)
            result.append(QVariantMap{{"peerId", peer.peerId}, {"displayName", peer.displayName}});
    }
    return result;
}

FileTransferService* NetworkSession::fileTransfers() const { return files_.get(); }
FileTransferService* NetworkSession::personalFileTransfers() const { return personal_->files(); }
ScreenShareService* NetworkSession::screenShare() const { return sharing_.get(); }

Result<void> NetworkSession::sendFile(const QString& path, const QString& peerId, const QString& groupId) {
    if (!peerId.isEmpty() && mesh_.peer(peerId).peerId.isEmpty()) {
        if (!signalingConnected()) return Result<void>::failure("Подключитесь к серверу для передачи знакомому.");
        return personal_->sendFile(peerId, path);
    }
    if (!mesh_.joined()) return Result<void>::failure("Сначала подключитесь к беседе.");
    int recipients = 0;
    for (const auto& peer : mesh_.peers()) {
        if (peer.peerId == app_.identity().peerId || (!peerId.isEmpty() && peer.peerId != peerId)) continue;
        const auto link = connections_->infoForPeer(peer.peerId);
        if (!link || !link->open) continue;
        const auto result = files_->offer(peer.peerId, peer.displayName, path, groupId);
        if (!result) return result;
        ++recipients;
    }
    return recipients > 0 ? Result<void>::success() : Result<void>::failure("Нет подключённых получателей.");
}


} // namespace tmc
