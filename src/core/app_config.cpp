#include "tmc/core/app_config.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace tmc {

Result<AppConfig> AppConfig::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return Result<AppConfig>::failure("Cannot open configuration: " + f.errorString());
    }
    QJsonParseError e;
    const auto d = QJsonDocument::fromJson(f.readAll(), &e);
    if (e.error != QJsonParseError::NoError || !d.isObject()) {
        return Result<AppConfig>::failure("Invalid configuration JSON: " + e.errorString());
    }
    AppConfig c;
    const auto root = d.object();
    auto serverValue = root.value("stun_servers");
    if (!serverValue.isArray()) {
        serverValue = root.value("ice").toObject().value("stun_servers");
    }
    if (!serverValue.isArray()) {
        return Result<AppConfig>::failure("stun_servers must be an array");
    }
    c.stunServers.clear();
    for (const auto& value : serverValue.toArray()) {
        if (!value.isString()) {
            continue;
        }
        const auto server = value.toString().trimmed();
        if (server.startsWith("stun:") && server.size() <= 512 && !c.stunServers.contains(server)) {
            c.stunServers.append(server);
        }
    }
    if (c.stunServers.isEmpty()) {
        return Result<AppConfig>::failure("At least one STUN URI is required");
    }
    return Result<AppConfig>::success(c);
}

Result<void> AppConfig::save(const QString& path) const {
    if (stunServers.isEmpty()) {
        return Result<void>::failure("At least one STUN URI is required");
    }
    QJsonArray servers;
    for (const auto& server : stunServers) {
        const auto normalized = server.trimmed();
        if (!normalized.startsWith("stun:") || normalized.size() > 512) {
            return Result<void>::failure("Every ICE server must be a valid stun: URI");
        }
        servers.append(normalized);
    }
    const QJsonObject root{{"stun_servers", servers}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return Result<void>::failure("Cannot write configuration: " + file.errorString());
    }
    const auto contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        return Result<void>::failure("Cannot save configuration: " + file.errorString());
    }
    return Result<void>::success();
}

} // namespace tmc
