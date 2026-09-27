#pragma once

#include "tmc/core/result.h"
#include <QObject>
#include <QVariantList>
#include <QByteArray>
#include <functional>
#include <memory>

namespace tmc {

class FileTransferService final : public QObject {
    Q_OBJECT
public:
    using Sender = std::function<bool(const QString&, const QByteArray&)>;
    explicit FileTransferService(Sender sender, QObject* parent = nullptr);
    ~FileTransferService() override;
    Result<void> offer(const QString& peerId, const QString& peerName, const QString& path);
    Result<void> accept(const QString& transferId, const QString& path);
    void cancel(const QString& transferId);
    void dismiss(const QString& transferId);
    void receive(const QString& peerId, const QString& peerName, const QByteArray& bytes);
    void setPeerConnected(const QString& peerId, bool connected);
    void removePeer(const QString& peerId);
    void clear();
    QVariantList transfers() const;
    bool contains(const QString& transferId) const;
    bool hasActive(const QString& peerId) const;
signals:
    void changed();
    void incomingOffered();
private:
    struct State;
    std::unique_ptr<State> state_;
    void pump();
};

} // namespace tmc
