#include "network/peer_connection.h"
#include <QPointer>
#include <rtc/rtc.hpp>
using namespace tmc;
struct PeerConnection::State {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
};
static ConnectionState mapState(rtc::PeerConnection::State s) {
    switch (s) {
    case rtc::PeerConnection::State::Connected:
        return ConnectionState::Connected;
    case rtc::PeerConnection::State::Connecting:
        return ConnectionState::Connecting;
    case rtc::PeerConnection::State::Failed:
        return ConnectionState::Failed;
    default:
        return ConnectionState::Disconnected;
    }
}
PeerConnection::PeerConnection(const AppConfig& c, QObject* p)
    : QObject(p), state_(std::make_shared<State>()) {
    rtc::Configuration cfg;
    for (const auto& s : c.stunServers)
        cfg.iceServers.emplace_back(s.toStdString());
    cfg.disableAutoNegotiation = true;
    state_->pc = std::make_shared<rtc::PeerConnection>(cfg);
    QPointer<PeerConnection> self(this);
    std::weak_ptr<State> weak = state_;
    state_->pc->onStateChange([self](auto s) {
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self, s] {
                    if (self)
                        emit self->stateChanged(mapState(s));
                },
                Qt::QueuedConnection);
    });
    state_->pc->onIceStateChange([self](auto state) {
        if (!self || state != rtc::PeerConnection::IceState::Failed)
            return;
        QMetaObject::invokeMethod(
            self,
            [self] {
                if (self)
                    emit self->errorOccurred(
                        "ICE failed: direct P2P connection could not be established.");
            },
            Qt::QueuedConnection);
    });
    state_->pc->onGatheringStateChange([self, weak](auto s) {
        if (!self || s != rtc::PeerConnection::GatheringState::Complete)
            return;
        auto state = weak.lock();
        if (!state)
            return;
        auto local = state->pc->localDescription();
        if (!local)
            return;
        QString type = QString::fromStdString(local->typeString()),
                sdp = QString::fromStdString(std::string(*local));
        QMetaObject::invokeMethod(
            self,
            [self, type, sdp] {
                if (self) {
                    emit self->localDescriptionReady(type, sdp);
                    emit self->stateChanged(type == "offer" ? ConnectionState::WaitingForAnswer
                                                            : ConnectionState::Connecting);
                }
            },
            Qt::QueuedConnection);
    });
    state_->pc->onDataChannel([self](auto dc) {
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self, dc] {
                    if (self)
                        self->configureChannel(dc);
                },
                Qt::QueuedConnection);
    });
}
PeerConnection::~PeerConnection() {
    auto s = std::move(state_);
    if (s) {
        if (s->dc)
            s->dc->close();
        if (s->pc)
            s->pc->close();
    }
}
void PeerConnection::configureChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    state_->dc = dc;
    QPointer<PeerConnection> self(this);
    dc->onOpen([self] {
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self] {
                    if (self)
                        emit self->channelOpened();
                },
                Qt::QueuedConnection);
    });
    dc->onClosed([self] {
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self] {
                    if (self)
                        emit self->channelClosed();
                },
                Qt::QueuedConnection);
    });
    dc->onError([self](const std::string& error) {
        if (!self)
            return;
        const auto message = QString::fromStdString(error);
        QMetaObject::invokeMethod(
            self,
            [self, message] {
                if (self)
                    emit self->errorOccurred(message);
            },
            Qt::QueuedConnection);
    });
    dc->onMessage([self](std::variant<rtc::binary, rtc::string> m) {
        if (!self || !std::holds_alternative<rtc::string>(m))
            return;
        auto text = QString::fromUtf8(std::get<rtc::string>(m));
        QMetaObject::invokeMethod(
            self,
            [self, text] {
                if (self)
                    emit self->textReceived(text);
            },
            Qt::QueuedConnection);
    });
}
void PeerConnection::createOffer() {
    emit stateChanged(ConnectionState::Gathering);
    configureChannel(state_->pc->createDataChannel("tiny-mesh-chat"));
    state_->pc->setLocalDescription(rtc::Description::Type::Offer);
}
void PeerConnection::acceptOffer(const QString& s) {
    emit stateChanged(ConnectionState::Gathering);
    state_->pc->setRemoteDescription(rtc::Description(s.toStdString(), "offer"));
    state_->pc->setLocalDescription(rtc::Description::Type::Answer);
}
void PeerConnection::acceptAnswer(const QString& s) {
    state_->pc->setRemoteDescription(rtc::Description(s.toStdString(), "answer"));
    emit stateChanged(ConnectionState::Connecting);
}
bool PeerConnection::sendText(const QString& s) {
    return state_->dc && state_->dc->isOpen() && state_->dc->send(s.toUtf8().toStdString());
}
