#include "tmc/identity/identity_manager.h"

#include "tmc/core/limits.h"
#include "tmc/core/uuid.h"
#include "tmc/security/security.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace tmc {

namespace {

Result<QString> normalizeDisplayName(const QString& name) {
    const auto normalized = name.trimmed();
    if (normalized.isEmpty()) {
        return Result<QString>::failure("Display name is required");
    }
    if (normalized.size() > limits::MaxDisplayNameLength) {
        return Result<QString>::failure("Display name must not exceed 128 characters");
    }
    for (const auto ch : normalized) {
        if (!ch.isPrint()) {
            return Result<QString>::failure("Display name contains a control character");
        }
    }
    return Result<QString>::success(normalized);
}

Result<void> saveIdentity(const QString& path, const PeerIdentity& identity) {
    const QJsonObject object{{"v", 1}, {"peer_id", identity.peerId},
                             {"display_name", identity.displayName}, {"legacy_peer_id", identity.legacyPeerId}};
    QSaveFile out(path);
    const auto bytes = QJsonDocument(object).toJson();
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() ||
        !out.commit()) {
        return Result<void>::failure("Cannot persist identity: " + out.errorString());
    }
    return Result<void>::success();
}

} // namespace

Result<PeerIdentity> IdentityManager::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<PeerIdentity>::failure("Cannot load identity: " + file.errorString());
    }

    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<PeerIdentity>::failure("Stored identity is invalid");
    }

    const auto object = document.object();
    PeerIdentity identity{object.value("peer_id").toString(),
                          object.value("display_name").toString()};
    const auto displayName = normalizeDisplayName(identity.displayName);
    if (!displayName || (!identity.isValid() && !isCanonicalUuid(identity.peerId))) {
        return Result<PeerIdentity>::failure("Stored identity is invalid");
    }
    identity.displayName = displayName.value();
    const auto keyPath = QFileInfo(path).absolutePath() + "/identity-key.json";
    if (isCanonicalUuid(identity.peerId)) {
        // Preserve the old profile before adopting a key-derived identity.
        if (!QFile::exists(path + ".legacy") && !QFile::copy(path, path + ".legacy"))
            return Result<PeerIdentity>::failure("Cannot back up legacy identity");
        auto key = QFile::exists(keyPath) ? security::SigningKey::load(keyPath) : security::SigningKey::create(keyPath);
        if (!key) return Result<PeerIdentity>::failure("Cannot initialize identity key");
        identity.legacyPeerId = identity.peerId;
        identity.peerId = security::identityId(key->publicKey());
        const auto saved = saveIdentity(path, identity);
        if (!saved) return Result<PeerIdentity>::failure(saved.error());
    } else {
        if (object.value("v").toInt(-1) != 1) {
            return Result<PeerIdentity>::failure("Unsupported identity version");
        }
        const auto key = security::SigningKey::load(keyPath);
        if (!key || security::identityId(key->publicKey()) != identity.peerId)
            return Result<PeerIdentity>::failure("Identity key is missing or does not match; restore its backup");
        identity.legacyPeerId = object.value("legacy_peer_id").toString();
    }
    return Result<PeerIdentity>::success(identity);
}

Result<PeerIdentity> IdentityManager::create(const QString& path, const QString& displayName) {
    if (QFile::exists(path)) {
        return Result<PeerIdentity>::failure("Identity already exists");
    }
    const auto normalized = normalizeDisplayName(displayName);
    if (!normalized) {
        return Result<PeerIdentity>::failure(normalized.error());
    }

    const auto keyPath = QFileInfo(path).absolutePath() + "/identity-key.json";
    auto key = QFile::exists(keyPath) ? security::SigningKey::load(keyPath) : security::SigningKey::create(keyPath);
    if (!key) return Result<PeerIdentity>::failure("Cannot initialize identity key");
    PeerIdentity identity{security::identityId(key->publicKey()), normalized.value()};
    const auto saved = saveIdentity(path, identity);
    if (!saved) {
        return Result<PeerIdentity>::failure(saved.error());
    }
    return Result<PeerIdentity>::success(identity);
}

Result<PeerIdentity> IdentityManager::updateDisplayName(const QString& path,
                                                        const PeerIdentity& identity,
                                                        const QString& displayName) {
    if (!identity.isValid()) {
        return Result<PeerIdentity>::failure("Identity is not initialized");
    }
    const auto normalized = normalizeDisplayName(displayName);
    if (!normalized) {
        return Result<PeerIdentity>::failure(normalized.error());
    }
    if (identity.displayName == normalized.value()) {
        return Result<PeerIdentity>::success(identity);
    }

    auto updated = identity;
    updated.displayName = normalized.value();
    const auto saved = saveIdentity(path, updated);
    if (!saved) {
        return Result<PeerIdentity>::failure(saved.error());
    }
    return Result<PeerIdentity>::success(updated);
}

} // namespace tmc
