#include "identity/identity_manager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
using namespace tmc;
Result<PeerIdentity> IdentityManager::loadOrCreate(const QString& path, const QString& name) {
    QFile f(path);
    PeerIdentity i;
    if (f.exists()) {
        if (!f.open(QIODevice::ReadOnly))
            return Result<PeerIdentity>::failure(f.errorString());
        auto o = QJsonDocument::fromJson(f.readAll()).object();
        i = {o["peer_id"].toString(), o["display_name"].toString(), o["device_id"].toString(),
             QDateTime::fromString(o["created_at"].toString(), Qt::ISODateWithMs)};
        if (!i.isValid())
            return Result<PeerIdentity>::failure("Stored identity is invalid");
        return Result<PeerIdentity>::success(i);
    }
    i = {QUuid::createUuid().toString(QUuid::WithoutBraces), name.trimmed(),
         QUuid::createUuid().toString(QUuid::WithoutBraces), QDateTime::currentDateTimeUtc()};
    if (!i.isValid())
        return Result<PeerIdentity>::failure("Display name is required");
    QJsonObject o{{"peer_id", i.peerId},
                  {"display_name", i.displayName},
                  {"device_id", i.deviceId},
                  {"created_at", i.createdAt.toString(Qt::ISODateWithMs)}};
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(o).toJson()) < 0 ||
        !out.commit())
        return Result<PeerIdentity>::failure("Cannot persist identity");
    return Result<PeerIdentity>::success(i);
}
