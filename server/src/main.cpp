#include "signaling_server.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QHostAddress>
#include <QTimer>

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
    parser.process(application);

    QHostAddress address;
    if (!address.setAddress(parser.value(addressOption))) {
        qCritical().noquote()
            << QStringLiteral("Invalid listen address: %1")
                   .arg(parser.value(addressOption));
        return 2;
    }

    bool portValid = false;
    const quint16 parsedPort = parser.value(portOption).toUShort(&portValid);
    if (!portValid) {
        qCritical().noquote()
            << QStringLiteral("Invalid port: %1").arg(parser.value(portOption));
        return 2;
    }

    tmc::server::SignalingServer server;
    if (!server.listen(address, parsedPort)) {
        qCritical().noquote()
            << QStringLiteral("Failed to listen on %1:%2: %3")
                   .arg(address.toString())
                   .arg(parsedPort)
                   .arg(server.errorString());
        return 1;
    }

    qInfo().noquote()
        << QStringLiteral("TinyMesh signaling server listening on %1:%2")
               .arg(server.serverAddress().toString())
               .arg(server.serverPort());

    QObject::connect(&application, &QCoreApplication::aboutToQuit,
                     &server, &tmc::server::SignalingServer::close);
    if (parser.isSet(startupSmokeOption)) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    return application.exec();
}
