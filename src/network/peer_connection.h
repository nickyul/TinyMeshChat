#pragma once
#include "core/app_config.h"
#include "network/connection_state.h"
#include <QObject>
#include <memory>
namespace rtc {
class PeerConnection;
class DataChannel;
} // namespace rtc
namespace tmc {
class PeerConnection final : public QObject {
    Q_OBJECT
  public:
    explicit PeerConnection(const AppConfig&, QObject* parent = nullptr);
    ~PeerConnection() override;
    void createOffer();
    void acceptOffer(const QString&);
    void acceptAnswer(const QString&);
    bool sendText(const QString&);
  signals:
    void localDescriptionReady(QString type, QString sdp);
    void stateChanged(tmc::ConnectionState);
    void channelOpened();
    void channelClosed();
    void textReceived(QString);
    void errorOccurred(QString);

  private:
    struct State;
    std::shared_ptr<State> state_;
    void configureChannel(const std::shared_ptr<rtc::DataChannel>&);
};
} // namespace tmc
