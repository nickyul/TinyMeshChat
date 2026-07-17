#pragma once

#include "tmc/core/result.h"

#include <QByteArray>
#include <QObject>

#include <memory>

namespace tmc {

class AudioEngine final : public QObject {
    Q_OBJECT

public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    Result<void> start();
    void stop();
    void setMuted(bool muted);

    bool isRunning() const;

    void receiveFrame(const QString& peerId, quint32 sequence, const QByteArray& opusPayload);
    void removePeer(const QString& peerId);

signals:
    void encodedFrameReady(quint32 sequence, QByteArray opusPayload);
    void errorOccurred(QString message);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace tmc
