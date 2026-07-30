#include "tmc/core/uuid.h"

#include <QUuid>

namespace tmc {

QString createUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool isCanonicalUuid(const QString& value) {
    const auto uuid = QUuid::fromString(value);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value;
}

} // namespace tmc
