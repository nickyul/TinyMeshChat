#include "tmc/signaling_protocol/turn_credentials.h"

#include <QJsonArray>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace tmc::signaling_protocol {

bool validTurnUrl(const QString& text) {
    if (text.size() > 2048 || text.contains("://") ||
        (!text.startsWith("turn:") && !text.startsWith("turns:"))) {
        return false;
    }
    for (const auto c : text) {
        if (c.isSpace() || !c.isPrint()) {
            return false;
        }
    }
    const auto separator = text.indexOf(':');
    const QUrl url(text.left(separator + 1) + "//" + text.mid(separator + 1), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        !url.path().isEmpty() || url.hasFragment() || url.port() == 0) {
        return false;
    }
    const auto query = QUrlQuery(url).queryItems();
    if (query.isEmpty()) {
        return !url.hasQuery();
    }
    return query.size() == 1 && query.first().first == "transport" &&
           (query.first().second == "tcp" ||
            (url.scheme() == "turn" && query.first().second == "udp"));
}

QJsonObject encodeTurnCredentials(const TurnCredentials& credentials) {
    if (credentials.urls.isEmpty()) {
        return {};
    }
    return {{"urls", QJsonArray::fromStringList(credentials.urls)},
            {"username", credentials.username}, {"password", credentials.password},
            {"expiresInSeconds", credentials.expiresInSeconds}};
}

std::optional<TurnCredentials> decodeTurnCredentials(const QJsonObject& object) {
    if (object.isEmpty()) {
        return TurnCredentials{};
    }
    if (object.size() != 4 || !object.value("urls").isArray() ||
        !object.value("username").isString() || !object.value("password").isString() ||
        !object.value("expiresInSeconds").isDouble()) {
        return std::nullopt;
    }
    TurnCredentials credentials;
    credentials.username = object.value("username").toString();
    credentials.password = object.value("password").toString();
    const auto lifetime = object.value("expiresInSeconds").toDouble();
    if (lifetime < MinTurnLifetimeSeconds || lifetime > MaxTurnLifetimeSeconds ||
        lifetime != static_cast<int>(lifetime) || credentials.username.isEmpty() ||
        credentials.username.size() > 128 || credentials.password.isEmpty() ||
        credentials.password.size() > 256) {
        return std::nullopt;
    }
    for (const auto& value : {credentials.username, credentials.password}) {
        for (const auto c : value) {
            if (!c.isPrint() || c.isSpace()) {
                return std::nullopt;
            }
        }
    }
    credentials.expiresInSeconds = static_cast<int>(lifetime);
    const auto urls = object.value("urls").toArray();
    if (urls.isEmpty() || urls.size() > MaxTurnUrls) {
        return std::nullopt;
    }
    QSet<QString> seen;
    for (const auto& item : urls) {
        if (!item.isString() || !validTurnUrl(item.toString()) || seen.contains(item.toString())) {
            return std::nullopt;
        }
        seen.insert(item.toString());
        credentials.urls.append(item.toString());
    }
    return credentials;
}

} // namespace tmc::signaling_protocol
