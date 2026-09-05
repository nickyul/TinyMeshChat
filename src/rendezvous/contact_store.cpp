#include "tmc/rendezvous/contact_store.h"

#include "tmc/core/logger.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <utility>

namespace tmc {

namespace {

constexpr int ContactFileVersion = 1;
constexpr qsizetype RendezvousSecretBytes = 32;

QJsonObject endpointToJson(const RendezvousEndpoint& endpoint) {
    return {{"address", endpoint.address}, {"port", endpoint.port}};
}

Result<RendezvousEndpoint> endpointFromJson(const QJsonValue& value, const QString& name) {
    if (!value.isObject()) {
        return Result<RendezvousEndpoint>::failure(name + " must be an object");
    }
    const auto object = value.toObject();
    if (!object.value("address").isString() || !object.value("port").isDouble()) {
        return Result<RendezvousEndpoint>::failure(name + " is invalid");
    }
    const auto port = object.value("port").toInt(-1);
    RendezvousEndpoint endpoint{object.value("address").toString(),
                                static_cast<quint16>(qMax(port, 0))};
    if (port < 0 || port > 65535 || !endpoint.isValid()) {
        return Result<RendezvousEndpoint>::failure(name + " is invalid");
    }
    return Result<RendezvousEndpoint>::success(std::move(endpoint));
}

} // namespace

bool RendezvousEndpoint::isValid() const {
    if (address.isEmpty() && port == 0) {
        return true;
    }
    QHostAddress parsed;
    return port != 0 && parsed.setAddress(address) &&
           parsed.protocol() == QAbstractSocket::IPv4Protocol;
}

bool ContactRecord::isValid() const {
    return identity.isValid() && rendezvousSecret.size() == RendezvousSecretBytes &&
           publicEndpoint.isValid() && localEndpoint.isValid() &&
           (!lastSeen.isValid() || lastSeen.timeSpec() == Qt::UTC);
}

ContactStore::ContactStore(QString path) : path_(std::move(path)) {
}

Result<QList<ContactRecord>> ContactStore::load() const {
    if (!QFile::exists(path_)) {
        Logger::instance().log(QtInfoMsg, "rendezvous", "No stored contacts yet");
        return Result<QList<ContactRecord>>::success({});
    }
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<QList<ContactRecord>>::failure("Cannot open contacts: " +
                                                     file.errorString());
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return Result<QList<ContactRecord>>::failure("Invalid contacts JSON: " +
                                                     error.errorString());
    }
    const auto root = document.object();
    if (root.value("version").toInt(-1) != ContactFileVersion ||
        !root.value("contacts").isArray()) {
        return Result<QList<ContactRecord>>::failure("Unsupported contacts file");
    }

    QList<ContactRecord> contacts;
    for (const auto& value : root.value("contacts").toArray()) {
        if (!value.isObject()) {
            return Result<QList<ContactRecord>>::failure("A contact entry is invalid");
        }
        const auto object = value.toObject();
        const auto publicEndpoint = endpointFromJson(object.value("public_endpoint"),
                                                     "contact.public_endpoint");
        const auto localEndpoint = endpointFromJson(object.value("local_endpoint"),
                                                    "contact.local_endpoint");
        if (!publicEndpoint || !localEndpoint) {
            return Result<QList<ContactRecord>>::failure(
                !publicEndpoint ? publicEndpoint.error() : localEndpoint.error());
        }
        ContactRecord contact;
        contact.identity = {object.value("peer_id").toString(),
                            object.value("display_name").toString()};
        contact.rendezvousSecret = QByteArray::fromBase64(
            object.value("rendezvous_secret").toString().toLatin1(),
            QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
        contact.publicEndpoint = publicEndpoint.value();
        contact.localEndpoint = localEndpoint.value();
        contact.lastSeen = QDateTime::fromString(object.value("last_seen").toString(),
                                                Qt::ISODateWithMs);
        contact.lastMeshId = object.value("last_mesh_id").toString();
        contact.mappingMethod = object.value("mapping_method").toString();
        if (!contact.isValid()) {
            return Result<QList<ContactRecord>>::failure("A contact entry is invalid");
        }
        if (std::any_of(contacts.cbegin(), contacts.cend(), [&contact](const auto& existing) {
                return existing.identity.peerId == contact.identity.peerId;
            })) {
            return Result<QList<ContactRecord>>::failure("Duplicate peer ID in contacts file");
        }
        contacts.append(std::move(contact));
    }
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           QString("Loaded %1 contact(s)").arg(contacts.size()));
    return Result<QList<ContactRecord>>::success(std::move(contacts));
}

Result<void> ContactStore::save(const QList<ContactRecord>& contacts) const {
    QJsonArray values;
    for (const auto& contact : contacts) {
        if (!contact.isValid()) {
            return Result<void>::failure("Cannot save an invalid contact");
        }
        values.append(QJsonObject{
            {"peer_id", contact.identity.peerId},
            {"display_name", contact.identity.displayName},
            {"rendezvous_secret",
             QString::fromLatin1(contact.rendezvousSecret.toBase64(
                 QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
            {"public_endpoint", endpointToJson(contact.publicEndpoint)},
            {"local_endpoint", endpointToJson(contact.localEndpoint)},
            {"last_seen", contact.lastSeen.isValid()
                              ? contact.lastSeen.toUTC().toString(Qt::ISODateWithMs)
                              : QString()},
            {"last_mesh_id", contact.lastMeshId},
            {"mapping_method", contact.mappingMethod}});
    }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        return Result<void>::failure("Cannot write contacts: " + file.errorString());
    }
    const auto contents = QJsonDocument(
                              QJsonObject{{"version", ContactFileVersion},
                                          {"contacts", values}})
                              .toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size() || !file.commit()) {
        return Result<void>::failure("Cannot save contacts: " + file.errorString());
    }
    QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    Logger::instance().log(QtInfoMsg, "rendezvous",
                           QString("Saved %1 contact(s)").arg(contacts.size()));
    return Result<void>::success();
}

} // namespace tmc
