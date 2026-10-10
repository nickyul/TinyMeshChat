#include "tmc/sharing/screen_streams_model.h"

#include <QSet>

#include <algorithm>

namespace tmc {

ScreenStreamsModel::ScreenStreamsModel(QObject* parent) : QAbstractListModel(parent) {
    const QList<QByteArray> names{
        "streamId", "peerId", "title",  "local", "hasAudio",        "selected",     "audioSelected",
        "state",    "error",  "height", "fps",   "requestedHeight", "requestedFps", "qualityText"};
    int role = Qt::UserRole;
    for (const auto& name : names) {
        roles_[++role] = name;
    }
}

int ScreenStreamsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant ScreenStreamsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    return rows_[index.row()].value(QString::fromUtf8(roles_.value(role)));
}

QHash<int, QByteArray> ScreenStreamsModel::roleNames() const {
    return roles_;
}

void ScreenStreamsModel::sync(const QVariantList& streams) {
    QSet<QString> ids;
    for (const auto& stream : streams) {
        ids.insert(stream.toMap().value("streamId").toString());
    }
    for (int i = rows_.size() - 1; i >= 0; --i) {
        if (!ids.contains(rows_[i].value("streamId").toString())) {
            beginRemoveRows({}, i, i);
            rows_.removeAt(i);
            endRemoveRows();
        }
    }
    for (const auto& value : streams) {
        const auto row = value.toMap();
        auto found = std::find_if(rows_.begin(), rows_.end(), [&](const auto& r) {
            return r.value("streamId") == row.value("streamId");
        });
        if (found == rows_.end()) {
            const int i = rows_.size();
            beginInsertRows({}, i, i);
            rows_.append(row);
            endInsertRows();
        } else if (*found != row) {
            const int i = std::distance(rows_.begin(), found);
            *found = row;
            emit dataChanged(index(i), index(i));
        }
    }
}

} // namespace tmc
