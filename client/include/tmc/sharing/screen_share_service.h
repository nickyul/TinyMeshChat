#pragma once
#include "tmc/core/result.h"
#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QVariantList>
#include <functional>
#include <memory>

namespace tmc {
class ScreenShareService final : public QObject {
    Q_OBJECT
public:
    using Sender = std::function<bool(const QString&, const QByteArray&)>;
    ScreenShareService(QString localIdentity, Sender sender, QObject* parent = nullptr);
    ~ScreenShareService() override;
    QVariantList sources();
    Result<void> start(int sourceIndex, bool systemAudio);
    void stop();
    void clear();
    void setPeerConnected(const QString& peerId, bool connected);
    void receive(const QString& peerId, const QByteArray& bytes);
    bool sharing() const;
    bool viewing() const;
    QString owner() const;
    QString title() const;
    bool systemAudioSupported() const;
    void setAudioMuted(bool muted);
    void setAudioVolume(int percent);
signals:
    void changed();
    void frameReady(QImage image);
    void errorOccurred(QString message);
private:
    struct State;
    std::unique_ptr<State> state_;
    bool prepareCapture();
    void announce();
    void maintain();
    void captureFrame();
    void encodeAudio(const QByteArray& samples);
    void clearRemote(bool retire = true);
};
} // namespace tmc
