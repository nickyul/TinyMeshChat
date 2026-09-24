#pragma once

#include "tmc/core/result.h"

#include <QByteArray>
#include <QString>

namespace tmc {

enum class SdpDescriptionType {
    Offer,
    Answer,
};

class SdpDescriptionCodec {
public:
    static Result<QByteArray> pack(const QString& sdp);
    static Result<QString> unpack(const QByteArray& payload, SdpDescriptionType type);
};

} // namespace tmc
