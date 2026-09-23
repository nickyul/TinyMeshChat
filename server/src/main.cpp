#include "tmc/server/signaling_server.h"

#include "tmc/security/security.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTimer>
#include <QJsonDocument>
#include <QTextStream>

#include <optional>

namespace {

std::optional<QSslConfiguration> loadTlsConfiguration(const QString& certificatePath,
                                                     const QString& keyPath) {
    if (!QSslSocket::supportsSsl()) {
        qCritical("TLS is unavailable in this Qt installation");
        return std::nullopt;
    }

    QFile certificateFile(certificatePath);
    if (!certificateFile.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "Cannot read TLS certificate:" << certificateFile.errorString();
        return std::nullopt;
    }
    const auto certificates = QSslCertificate::fromDevice(&certificateFile, QSsl::Pem);
    if (certificates.isEmpty() || certificates.first().publicKey().isNull()) {
        qCritical("Invalid TLS certificate; expected a PEM certificate chain, server certificate first");
        return std::nullopt;
    }

    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "Cannot read TLS private key:" << keyFile.errorString();
        return std::nullopt;
    }
    const QSslKey key(&keyFile, certificates.first().publicKey().algorithm(),
                     QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull()) {
        qCritical("Invalid TLS private key; expected an unencrypted PEM key for the certificate");
        return std::nullopt;
    }

    auto configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificateChain(certificates);
    configuration.setPrivateKey(key);
    configuration.setProtocol(QSsl::TlsV1_2OrLater);
    // Clients authenticate with TinyMesh grants, not TLS client certificates.
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
    return configuration;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("TinyMeshSignalingServer"));
    QCoreApplication::setApplicationVersion(QStringLiteral(TMC_APP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Optional WebSocket signaling server for TinyMesh Chat"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption addressOption(
        {QStringLiteral("a"), QStringLiteral("listen-address")},
        QStringLiteral("IP address to listen on."),
        QStringLiteral("address"),
        QStringLiteral("127.0.0.1"));
    const QCommandLineOption portOption(
        {QStringLiteral("p"), QStringLiteral("port")},
        QStringLiteral("TCP port to listen on; use 0 to select a free port."),
        QStringLiteral("port"),
        QStringLiteral("8080"));
    const QCommandLineOption startupSmokeOption(
        QStringLiteral("startup-smoke"),
        QStringLiteral("Listen successfully and exit immediately."));
    parser.addOption(addressOption);
    parser.addOption(portOption);
    parser.addOption(startupSmokeOption);
    parser.addOption({"tls-cert", "Enable wss with this PEM certificate chain (server first).", "path"});
    parser.addOption({"tls-key", "Unencrypted PEM private key for --tls-cert.", "path"});
    parser.addOption({"authority-key", "Authority private key file (required to serve).", "path"});
    parser.addOption({"init-authority", "Create a new authority key file and exit.", "path"});
    parser.addOption({"grant-access", "Issue a permanent grant to this public key and exit.", "public-key"});
    parser.process(application);
    if (parser.isSet("init-authority")) {
        const auto key = tmc::security::SigningKey::create(parser.value("init-authority"));
        if (!key) { qCritical("Cannot create authority key; an existing key is never overwritten"); return 2; }
        QTextStream(stdout) << "Authority public key: " << key->publicKey() << Qt::endl;
        return 0;
    }
    const auto authority = tmc::security::SigningKey::load(parser.value("authority-key"));
    if (!authority) { qCritical("A valid --authority-key file is required"); return 2; }
    if (parser.isSet("grant-access")) {
        const auto grant = tmc::security::issueGrant(*authority, parser.value("grant-access"));
        if (grant.isEmpty()) { qCritical("Invalid public key"); return 2; }
        QTextStream(stdout) << "tmc-access1:" << tmc::security::base64(QJsonDocument(grant).toJson(QJsonDocument::Compact)) << Qt::endl;
        return 0;
    }

    QHostAddress address;
    if (!address.setAddress(parser.value(addressOption))) {
        qCritical().noquote()
            << QStringLiteral("Invalid listen address: %1")
                   .arg(parser.value(addressOption));
        return 2;
    }

    if (parser.isSet("tls-cert") != parser.isSet("tls-key")) {
        qCritical("--tls-cert and --tls-key must be provided together");
        return 2;
    }
    std::optional<QSslConfiguration> tls;
    if (parser.isSet("tls-cert")) {
        tls = loadTlsConfiguration(parser.value("tls-cert"), parser.value("tls-key"));
        if (!tls) {
            return 2;
        }
    }
    if (!tls && !address.isLoopback()) {
        qCritical("Without TLS, bind to loopback behind a TLS proxy; for direct wss use "
                  "--tls-cert and --tls-key");
        return 2;
    }
    bool portValid = false;
    const quint16 parsedPort = parser.value(portOption).toUShort(&portValid);
    if (!portValid) {
        qCritical().noquote()
            << QStringLiteral("Invalid port: %1").arg(parser.value(portOption));
        return 2;
    }

    tmc::server::SignalingServer server(authority, tls);
    if (!server.listen(address, parsedPort)) {
        qCritical().noquote()
            << QStringLiteral("Failed to listen on %1:%2: %3")
                   .arg(address.toString())
                   .arg(parsedPort)
                   .arg(server.errorString());
        return 1;
    }

    qInfo().noquote()
        << QStringLiteral("TinyMesh signaling server listening on %1 (%2)")
               .arg(server.serverAddress().toString() + ":" + QString::number(server.serverPort()))
               .arg(tls ? "wss" : "ws");

    QObject::connect(&application, &QCoreApplication::aboutToQuit,
                     &server, &tmc::server::SignalingServer::close);
    if (parser.isSet(startupSmokeOption)) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    return application.exec();
}
