#include "tmc/app/application_controller.h"

#include "tmc/core/logger.h"
#include "tmc/core/uuid.h"
#include "tmc/identity/identity_manager.h"
#include "tmc/signaling_protocol/message_codec.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

namespace tmc {
namespace {

bool validAcquaintance(const PeerIdentity& peer) {
    if (!peer.isValid() || peer.displayName.size() > 128) return false;
    for (const auto c : peer.displayName) if (!c.isPrint()) return false;
    return true;
}

bool validLegacyAcquaintance(const PeerIdentity& peer) {
    if (!isCanonicalUuid(peer.peerId) || peer.displayName.trimmed().isEmpty() || peer.displayName.size() > 128) return false;
    for (const auto c : peer.displayName) if (!c.isPrint()) return false;
    return true;
}

Result<void> ensureUserConfig(const QString& path) {
    if (!QFile::exists(path)) {
        QFile source(QStringLiteral(":/default-config.json"));
        if (!source.open(QIODevice::ReadOnly)) {
            return Result<void>::failure(
                "Cannot read the default configuration: " + source.errorString());
        }

        const auto contents = source.readAll();

        QSaveFile target(path);
        if (!target.open(QIODevice::WriteOnly) ||
            target.write(contents) != contents.size() ||
            !target.commit()) {
            return Result<void>::failure(
                "Cannot create the user configuration: " + target.errorString());
        }
    }

    // Repair installations where config.json was previously copied
    // from the read-only Qt resource.
    QFile probe(path);
    if (probe.open(QIODevice::ReadWrite)) {
        return Result<void>::success();
    }

    const auto permissions = QFile::permissions(path);
    if (!QFile::setPermissions(
            path,
            permissions | QFileDevice::WriteOwner | QFileDevice::WriteUser)) {
        return Result<void>::failure(
            "Cannot make the user configuration writable: " + path);
    }

    QFile retry(path);
    if (!retry.open(QIODevice::ReadWrite)) {
        return Result<void>::failure(
            "User configuration is not writable: " + retry.errorString());
    }

    return Result<void>::success();
}

} // namespace

ApplicationController::ApplicationController(QObject* p) : QObject(p) {
}

const PeerIdentity& ApplicationController::identity() const {
    return identity_;
}

const AppConfig& ApplicationController::config() const {
    return config_;
}

const ConnectionPolicy& ApplicationController::connectionPolicy() const {
    return connectionPolicy_;
}

QString ApplicationController::dataDirectory() const {
    return dataDir_;
}

Result<InitializationState> ApplicationController::initialize() {
    dataDir_ = qEnvironmentVariable("TMC_DATA_DIR");
    if (dataDir_.isEmpty()) {
        dataDir_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (!QDir().mkpath(dataDir_)) {
        return Result<InitializationState>::failure("Cannot create application data directory: " +
                                                    dataDir_);
    }
    Logger::instance().setFilePath(dataDir_ + "/debug.log");
    const auto configPath = dataDir_ + "/config.json";
    const auto configReady = ensureUserConfig(configPath);
    if (!configReady) {
        return Result<InitializationState>::failure(configReady.error());
    }

    auto config = AppConfig::load(configPath);
    if (!config) {
        return Result<InitializationState>::failure(config.error());
    }
    config_ = config.value();
    identityPath_ = dataDir_ + "/identity.json";
    if (!QFile::exists(identityPath_)) {
        return Result<InitializationState>::success(InitializationState::DisplayNameRequired);
    }

    auto identity = IdentityManager::load(identityPath_);
    if (!identity) {
        return Result<InitializationState>::failure(identity.error());
    }
    identity_ = identity.value();
    signingKey_ = security::SigningKey::load(dataDir_ + "/identity-key.json");
    if (!signingKey_) return Result<InitializationState>::failure("Cannot load identity key");
    QFile accessFile(dataDir_ + "/server-access.json");
    if (accessFile.exists()) {
        if (!accessFile.open(QIODevice::ReadOnly) || accessFile.size() > 256 * 1024) {
            return Result<InitializationState>::failure("Cannot read server access file");
        }
        const auto document = QJsonDocument::fromJson(accessFile.readAll());
        if (!document.isObject()) {
            return Result<InitializationState>::failure("Invalid server access file");
        }
        serverAccess_ = document.object();
        for (auto it = serverAccess_.constBegin(); it != serverAccess_.constEnd(); ++it) {
            const auto grant = it.value().toObject();
            if (!security::verifyGrant(grant, grant.value("authority").toString(), signingKey_->publicKey())) {
                return Result<InitializationState>::failure("Stored server access does not match the identity key");
            }
        }
    }
    QFile contacts(dataDir_ + "/acquaintances.json");
    if (contacts.exists()) {
        if (!contacts.open(QIODevice::ReadOnly) || contacts.size() > 256 * 1024)
            return Result<InitializationState>::failure("Cannot read acquaintances");
        const auto doc = QJsonDocument::fromJson(contacts.readAll());
        const auto object = doc.object();
        if (!doc.isObject() || (object.value("v").toInt(-1) != 1 && object.value("v").toInt(-1) != 2) ||
            !object.value("peers").isArray() || (!isCanonicalUuid(object.value("identityId").toString()) && !security::validKey(object.value("identityId").toString())))
            return Result<InitializationState>::failure("Invalid acquaintances file");
        if ((object.value("identityId").toString() == identity_.peerId || object.value("identityId").toString() == identity_.legacyPeerId)) {
            const auto peers = object.value("peers").toArray();
            if (peers.size() > signaling_protocol::MaxAcquaintances)
                return Result<InitializationState>::failure("Too many acquaintances");
            QSet<QString> seen;
            for (const auto& value : peers) {
                const auto entry = value.toObject();
                PeerIdentity peer{entry.value("id").toString(), entry.value("name").toString()};
                if ((!validAcquaintance(peer) && !validLegacyAcquaintance(peer)) ||
                    peer.peerId == identity_.peerId || seen.contains(peer.peerId))
                    return Result<InitializationState>::failure("Invalid acquaintance");
                seen.insert(peer.peerId);
                acquaintances_.append(peer);
            }
        }
    }
    Logger::instance().log(QtInfoMsg, "app",
                           "Application initialized for peer " + identity_.peerId.left(8));
    return Result<InitializationState>::success(InitializationState::Ready);
}

Result<void> ApplicationController::createIdentity(const QString& displayName) {
    if (identityPath_.isEmpty()) {
        return Result<void>::failure("Application is not initialized");
    }
    auto identity = IdentityManager::create(identityPath_, displayName);
    if (!identity) {
        return Result<void>::failure(identity.error());
    }
    identity_ = identity.value();
    signingKey_ = security::SigningKey::load(dataDir_ + "/identity-key.json");
    if (!signingKey_) return Result<void>::failure("Cannot load identity key");
    Logger::instance().log(QtInfoMsg, "app",
                           "Identity created for peer " + identity_.peerId.left(8));
    return Result<void>::success();
}

Result<void> ApplicationController::updateDisplayName(const QString& displayName) {
    if (identityPath_.isEmpty() || !identity_.isValid()) {
        return Result<void>::failure("Identity is not initialized");
    }
    auto identity = IdentityManager::updateDisplayName(identityPath_, identity_, displayName);
    if (!identity) {
        return Result<void>::failure(identity.error());
    }
    if (identity_.displayName == identity.value().displayName) {
        return Result<void>::success();
    }
    identity_ = identity.value();
    emit displayNameChanged(identity_.displayName);
    Logger::instance().log(QtInfoMsg, "identity", "Display name updated");
    return Result<void>::success();
}

Result<void> ApplicationController::updateSignalingServer(const QString& url) {
    auto updated = config_;
    updated.signalingServerUrl = url.trimmed();
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved) {
        return saved;
    }
    config_ = updated;
    return Result<void>::success();
}

const QList<PeerIdentity>& ApplicationController::acquaintances() const { return acquaintances_; }

Result<void> ApplicationController::rememberAcquaintance(const PeerIdentity& peer) {
    if (!validAcquaintance(peer) || peer.peerId == identity_.peerId)
        return Result<void>::failure("Некорректные данные знакомого.");
    auto updated = acquaintances_;
    bool found = false;
    for (auto& item : updated) {
        if (item.peerId != peer.peerId) continue;
        if (item.displayName == peer.displayName) return Result<void>::success();
        item.displayName = peer.displayName;
        found = true;
        break;
    }
    if (!found) {
        if (updated.size() >= signaling_protocol::MaxAcquaintances)
            return Result<void>::failure("Достигнут лимит списка знакомых (256).");
        updated.append(peer);
    }
    QJsonArray peers;
    for (const auto& item : updated) peers.append(QJsonObject{{"id", item.peerId}, {"name", item.displayName}});
    const auto bytes = QJsonDocument(QJsonObject{{"v", 2}, {"identityId", identity_.peerId},
                                                {"peers", peers}}).toJson(QJsonDocument::Indented);
    QSaveFile file(dataDir_ + "/acquaintances.json");
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return Result<void>::failure("Не удалось сохранить список знакомых.");
    acquaintances_ = std::move(updated);
    emit acquaintancesChanged();
    return Result<void>::success();
}

Result<void> ApplicationController::updateStunServers(const QStringList& servers) {
    AppConfig updated = config_;
    updated.stunServers.clear();
    for (const auto& server : servers) {
        const auto normalized = server.trimmed();
        if (!normalized.isEmpty() && !updated.stunServers.contains(normalized)) {
            updated.stunServers.append(normalized);
        }
    }
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved) {
        return saved;
    }
    config_ = updated;
    emit stunServersChanged(config_.stunServers);
    Logger::instance().log(QtInfoMsg, "config", "STUN server list updated");
    return Result<void>::success();
}

Result<void> ApplicationController::updateAudioPreferences(const AudioPreferences& preferences) {
    if (!preferences.isValid()) {
        return Result<void>::failure("Некорректные настройки аудио.");
    }
    AppConfig updated = config_;
    updated.audio = preferences;
    const auto saved = updated.save(dataDir_ + "/config.json");
    if (!saved) {
        return saved;
    }
    config_ = updated;
    emit audioPreferencesChanged(config_.audio);
    Logger::instance().log(QtInfoMsg, "config", "Audio preferences updated");
    return Result<void>::success();
}

} // namespace tmc

namespace tmc {
std::shared_ptr<security::SigningKey> ApplicationController::signingKey() const { return signingKey_; }
QJsonObject ApplicationController::serverAccess() const { return serverAccess_; }
Result<void> ApplicationController::saveServerAccess(const QString& url, const QJsonObject& grant) {
    if (!signingKey_ || !security::verifyGrant(grant, grant.value("authority").toString(), signingKey_->publicKey()))
        return Result<void>::failure("Разрешение не соответствует вашей идентичности.");
    auto updated = serverAccess_;
    updated.insert(url, grant);
    if (QJsonDocument(updated).toJson(QJsonDocument::Indented).size() > 256 * 1024)
        return Result<void>::failure("Достигнут лимит локального хранилища разрешений.");
    if (!security::writePrivateJson(dataDir_ + "/server-access.json", updated))
        return Result<void>::failure("Не удалось сохранить разрешение сервера.");
    serverAccess_ = updated;
    return Result<void>::success();
}
} // namespace tmc
