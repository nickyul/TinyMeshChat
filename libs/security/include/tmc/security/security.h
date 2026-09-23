#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <array>
#include <memory>

namespace tmc::security {

bool validKey(const QString& hex);
QString identityId(const QString& publicKey);
QByteArray transcript(const QByteArray& domain, const QStringList& fields);
QString randomToken();
QString base64(const QByteArray& bytes);
QByteArray unbase64(const QString& text, int expectedSize = 0);
bool verify(const QString& publicKey, const QByteArray& message, const QString& signature);

class SigningKey final {
public:
    ~SigningKey();
    SigningKey(const SigningKey&) = delete;
    SigningKey& operator=(const SigningKey&) = delete;

    static std::shared_ptr<SigningKey> load(const QString& path);
    static std::shared_ptr<SigningKey> create(const QString& path);
    QString publicKey() const;
    QString sign(const QByteArray& message) const;

private:
    SigningKey() = default;
    std::array<unsigned char, 64> secret_{};
    std::array<unsigned char, 32> public_{};
};

QJsonObject issueGrant(const SigningKey& authority, const QString& subject);
bool verifyGrant(const QJsonObject& grant, const QString& authority, const QString& subject);
bool writePrivateJson(const QString& path, const QJsonObject& object);
QJsonObject readJson(const QString& path, qint64 limit = 256 * 1024);

} // namespace tmc::security
