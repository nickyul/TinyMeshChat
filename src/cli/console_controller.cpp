#include "cli/console_controller.h"
#include "app/application_controller.h"
#include "core/app_config.h"
#ifdef TMC_WITH_LIBDATACHANNEL
#include "network/peer_connection.h"
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#endif
#include <QTextStream>
using namespace tmc;
ConsoleController::ConsoleController(ApplicationController& a, QObject* p) : QObject(p), app_(a) {
}
int ConsoleController::run() {
    QTextStream in(stdin), out(stdout);
    out << "TinyMesh Chat console. /help for commands\n";
#ifdef TMC_WITH_LIBDATACHANNEL
    AppConfig cfg;
    peer_ = std::make_shared<PeerConnection>(cfg);
    connect(peer_.get(), &PeerConnection::stateChanged, this, [&out](auto s) {
        out << "state: " << toString(s) << "\n" << Qt::flush;
    });
    connect(peer_.get(), &PeerConnection::channelOpened, this, [&out] {
        out << "DataChannel open; direct P2P connected; relay not used\n" << Qt::flush;
    });
    connect(peer_.get(), &PeerConnection::textReceived, this, [&out](const QString& s) {
        out << "received: " << s << "\n" << Qt::flush;
    });
    auto gather = [this, &out](const QString& path, auto start) {
        QEventLoop loop;
        QString sdp, type;
        auto c = connect(peer_.get(), &PeerConnection::localDescriptionReady, &loop,
                         [&](QString t, QString s) {
                             type = t;
                             sdp = s;
                             loop.quit();
                         });
        QTimer::singleShot(25000, &loop, &QEventLoop::quit);
        start();
        loop.exec();
        disconnect(c);
        if (sdp.isEmpty()) {
            out << "ICE gathering timed out\n";
            return;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(sdp.toUtf8()) < 0)
            out << "Cannot write " << path << "\n";
        else
            out << type << " saved to " << path << "\n";
    };
#endif
    for (;;) {
        out << "> " << Qt::flush;
        auto line = in.readLine();
        if (line.isNull() || line == "/quit")
            break;
        if (line == "/help")
            out << "/identity /create-room /create-invite /import <path> /peers /status "
                   "/send <text> /p2p-offer <file> /p2p-answer <offer> <answer> "
                   "/p2p-import-answer <file> /p2p-send <text> /quit\n";
        else if (line == "/identity")
            out << app_.identity().displayName << " " << app_.identity().peerId << "\n";
        else if (line == "/status")
            out << "Dynamic P2P mesh; relay not used\n";
#ifdef TMC_WITH_LIBDATACHANNEL
        else if (line.startsWith("/p2p-offer ")) {
            auto path = line.sliced(11).trimmed();
            gather(path, [this] { peer_->createOffer(); });
        } else if (line.startsWith("/p2p-answer ")) {
            auto args = line.sliced(12).split(' ', Qt::SkipEmptyParts);
            if (args.size() != 2) {
                out << "usage: /p2p-answer <offer-file> <answer-file>\n";
                continue;
            }
            QFile f(args[0]);
            if (!f.open(QIODevice::ReadOnly)) {
                out << "Cannot read offer\n";
                continue;
            }
            auto sdp = QString::fromUtf8(f.readAll());
            gather(args[1], [this, sdp] { peer_->acceptOffer(sdp); });
        } else if (line.startsWith("/p2p-import-answer ")) {
            QFile f(line.sliced(19).trimmed());
            if (!f.open(QIODevice::ReadOnly))
                out << "Cannot read answer\n";
            else
                peer_->acceptAnswer(QString::fromUtf8(f.readAll()));
        } else if (line.startsWith("/p2p-send ")) {
            if (!peer_->sendText(line.sliced(10)))
                out << "DataChannel is not open\n";
        }
#else
        else if (line.startsWith("/p2p-"))
            out << "This build was compiled without libdatachannel\n";
#endif
        else
            out << "Command is unavailable until a room is active\n";
    }
    return 0;
}
