#pragma once

#include <QString>

namespace tmc {

QString createUuid();
bool isCanonicalUuid(const QString& value);

} // namespace tmc
