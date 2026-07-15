#include "core/app_config.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

using namespace tmc;
Result<AppConfig> AppConfig::load(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Result<AppConfig>::failure("Cannot open configuration: " + f.errorString());
    QJsonParseError e;
    const auto d = QJsonDocument::fromJson(f.readAll(), &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        return Result<AppConfig>::failure("Invalid configuration JSON: " + e.errorString());
    AppConfig c;
    const auto root = d.object();
    const auto ice = root.value("ice").toObject();
    if (!ice.value("stun_servers").isArray())
        return Result<AppConfig>::failure("ice.stun_servers must be an array");
    c.stunServers.clear();
    for (const auto& v : ice.value("stun_servers").toArray())
        if (v.isString() && v.toString().startsWith("stun:"))
            c.stunServers << v.toString();
    if (c.stunServers.isEmpty())
        return Result<AppConfig>::failure("At least one STUN URI is required");
    c.turnEnabled = ice.value("turn_enabled").toBool(false);
    if (c.turnEnabled)
        return Result<AppConfig>::failure("TURN is disabled in this MVP");
    c.iceGatheringTimeoutSeconds = ice.value("ice_gathering_timeout_seconds").toInt(20);
    c.connectionTimeoutSeconds = ice.value("connection_timeout_seconds").toInt(30);
    c.keepaliveSeconds = root.value("keepalive_seconds").toInt(15);
    c.maxRoomPeers = root.value("max_room_peers").toInt(8);
    if (c.iceGatheringTimeoutSeconds < 1 || c.connectionTimeoutSeconds < 1 ||
        c.keepaliveSeconds < 1 || c.maxRoomPeers < 2 || c.maxRoomPeers > 16)
        return Result<AppConfig>::failure("Configuration contains an invalid timeout");
    return Result<AppConfig>::success(c);
}

Result<void> AppConfig::save(const QString& path) const {
    if (stunServers.isEmpty())
        return Result<void>::failure("At least one STUN URI is required");
    QJsonArray servers;
    for (const auto& server : stunServers) {
        const auto normalized = server.trimmed();
        if (!normalized.startsWith("stun:") || normalized.size() > 512)
            return Result<void>::failure("Every ICE server must be a valid stun: URI");
        servers.append(normalized);
    }
    const QJsonObject root{
        {"ice", QJsonObject{{"stun_servers", servers},
                            {"turn_enabled", false},
                            {"ice_gathering_timeout_seconds", iceGatheringTimeoutSeconds},
                            {"connection_timeout_seconds", connectionTimeoutSeconds}}},
        {"keepalive_seconds", keepaliveSeconds},
        {"max_room_peers", maxRoomPeers}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return Result<void>::failure("Cannot write configuration: " + file.errorString());
    const auto contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit())
        return Result<void>::failure("Cannot save configuration: " + file.errorString());
    return Result<void>::success();
}
