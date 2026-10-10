#pragma once

#include <QAbstractListModel>
#include <QVariantMap>

namespace tmc {

class ScreenStreamsModel final : public QAbstractListModel {
    Q_OBJECT

public:
    explicit ScreenStreamsModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void sync(const QVariantList& streams);

private:
    QList<QVariantMap> rows_;
    QHash<int, QByteArray> roles_;
};

} // namespace tmc
