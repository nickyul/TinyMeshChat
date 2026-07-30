#include "tmc/identity/identity_manager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace tmc {

namespace {

constexpr qsizetype MaxDisplayNameLength = 128;

Result<QString> normalizeDisplayName(const QString& name) {
    const auto normalized = name.trimmed();
    if (normalized.isEmpty()) {
        return Result<QString>::failure("Display name is required");
    }
    if (normalized.size() > MaxDisplayNameLength) {
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
    const QJsonObject object{{"peer_id", identity.peerId},
                             {"display_name", identity.displayName}};
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(object).toJson()) < 0 ||
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
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<PeerIdentity>::failure("Stored identity is invalid");
    }

    const auto object = document.object();
    PeerIdentity identity{object.value("peer_id").toString(),
                          object.value("display_name").toString()};
    const auto displayName = normalizeDisplayName(identity.displayName);
    if (!identity.isValid() || !displayName) {
        return Result<PeerIdentity>::failure("Stored identity is invalid");
    }
    identity.displayName = displayName.value();
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

    PeerIdentity identity{QUuid::createUuid().toString(QUuid::WithoutBraces), normalized.value()};
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
