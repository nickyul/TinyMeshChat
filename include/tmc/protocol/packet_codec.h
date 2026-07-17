#pragma once

#include "tmc/core/result.h"
#include "tmc/protocol/packet.h"

#include <QByteArray>
#include <QSet>

namespace tmc {

class PacketCodec {
public:
    static constexpr qsizetype MaxBytes = 64 * 1024, MaxTextChars = 4096;
    static QByteArray encode(const Packet&);
    static Result<Packet> decode(const QByteArray&, const QString& expectedMesh = {},
                                 const QSet<QString>& allowedSenders = {});
    static const QSet<QString>& knownTypes();
};

} // namespace tmc
