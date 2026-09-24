#pragma once

#include "tmc/core/result.h"
#include "tmc/protocol/packet.h"

#include <QByteArray>

namespace tmc {

class PacketCodec {
public:
    static constexpr qsizetype MaxBytes = 64 * 1024;

    static Result<QByteArray> encode(const Packet&);
    static Result<Packet> decode(const QByteArray&, const QString& expectedMesh = {});
};

} // namespace tmc
