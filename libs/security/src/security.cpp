#include "tmc/security/security.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>

#include <sodium.h>

namespace tmc::security {

namespace {

bool initialized() {
    static const bool ready = sodium_init() >= 0;
    return ready;
}

QByteArray grantMessage(const QString& authority, const QString& subject) {
    return transcript("tmc.access.v1", {authority, subject, "member"});
}

} // namespace

bool validKey(const QString& hex) {
    if (hex.size() != 64) {
        return false;
    }

    for (const auto c : hex) {
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) {
            return false;
        }
    }

    return hex != QString(64, '0');
}

QString identityId(const QString& key) {
    if (!validKey(key)) {
        return {};
    }

    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArray::fromHex(key.toLatin1()), QCryptographicHash::Sha256)
            .toHex());
}

QByteArray transcript(const QByteArray& domain, const QStringList& fields) {
    QByteArray bytes = domain + '\0';
    for (const auto& field : fields) {
        const auto encoded = field.toUtf8();
        bytes += QByteArray::number(encoded.size()) + ':' + encoded;
    }

    return bytes;
}

QString base64(const QByteArray& bytes) {
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray unbase64(const QString& text, int expectedSize) {
    const auto bytes = QByteArray::fromBase64(
        text.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (base64(bytes) != text || (expectedSize && bytes.size() != expectedSize)) {
        return {};
    }

    return bytes;
}

QString randomToken() {
    if (!initialized()) {
        return {};
    }

    QByteArray bytes(32, Qt::Uninitialized);
    randombytes_buf(bytes.data(), size_t(bytes.size()));
    return base64(bytes);
}

bool verify(const QString& key, const QByteArray& message, const QString& signature) {
    if (!initialized() || !validKey(key)) {
        return false;
    }

    const auto pk = QByteArray::fromHex(key.toLatin1());
    const auto sig = unbase64(signature, crypto_sign_BYTES);
    return sig.size() == crypto_sign_BYTES &&
           crypto_sign_verify_detached(
               reinterpret_cast<const unsigned char*>(sig.constData()),
               reinterpret_cast<const unsigned char*>(message.constData()), message.size(),
               reinterpret_cast<const unsigned char*>(pk.constData())) == 0;
}

SigningKey::~SigningKey() {
    sodium_memzero(secret_.data(), secret_.size());
}

std::shared_ptr<SigningKey> SigningKey::load(const QString& path) {
    if (!initialized()) {
        return {};
    }

    const auto object = readJson(path, 4096);
    auto seed = unbase64(object.value("seed").toString(), crypto_sign_SEEDBYTES);
    if (object.value("v").toInt(-1) != 1 || seed.size() != crypto_sign_SEEDBYTES) {
        return {};
    }

    auto key = std::shared_ptr<SigningKey>(new SigningKey);
    crypto_sign_seed_keypair(key->public_.data(), key->secret_.data(),
                            reinterpret_cast<const unsigned char*>(seed.constData()));
    sodium_memzero(seed.data(), size_t(seed.size()));
    if (object.value("publicKey").toString() != key->publicKey()) {
        return {};
    }

    return key;
}

std::shared_ptr<SigningKey> SigningKey::create(const QString& path) {
    if (!initialized() || path.isEmpty()) {
        return {};
    }

    QLockFile creationLock(path + ".lock");
    if (!creationLock.tryLock(0) || QFile::exists(path)) {
        return {};
    }

    auto key = std::shared_ptr<SigningKey>(new SigningKey);
    crypto_sign_keypair(key->public_.data(), key->secret_.data());

    QByteArray seed(crypto_sign_SEEDBYTES, Qt::Uninitialized);
    crypto_sign_ed25519_sk_to_seed(reinterpret_cast<unsigned char*>(seed.data()), key->secret_.data());
    const bool saved =
        writePrivateJson(path, {{"v", 1}, {"publicKey", key->publicKey()}, {"seed", base64(seed)}});
    sodium_memzero(seed.data(), size_t(seed.size()));

    return saved ? key : nullptr;
}

QString SigningKey::publicKey() const {
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(public_.data()), public_.size()).toHex());
}

QString SigningKey::sign(const QByteArray& message) const {
    QByteArray signature(crypto_sign_BYTES, Qt::Uninitialized);
    if (crypto_sign_detached(reinterpret_cast<unsigned char*>(signature.data()), nullptr,
                             reinterpret_cast<const unsigned char*>(message.constData()),
                             message.size(), secret_.data()) != 0) {
        return {};
    }

    return base64(signature);
}

QJsonObject issueGrant(const SigningKey& authority, const QString& subject) {
    if (!validKey(subject)) {
        return {};
    }

    const auto root = authority.publicKey();
    return {{"v", 1},
            {"authority", root},
            {"subject", subject},
            {"role", "member"},
            {"signature", authority.sign(grantMessage(root, subject))}};
}

bool verifyGrant(const QJsonObject& grant, const QString& authority, const QString& subject) {
    return grant.size() == 5 && grant.value("v").toInt(-1) == 1 &&
           grant.value("authority").toString() == authority &&
           grant.value("subject").toString() == subject &&
           grant.value("role").toString() == "member" && validKey(subject) &&
           verify(authority, grantMessage(authority, subject), grant.value("signature").toString());
}

QJsonObject readJson(const QString& path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) {
        return {};
    }

    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writePrivateJson(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        return false;
    }

    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}

} // namespace tmc::security
