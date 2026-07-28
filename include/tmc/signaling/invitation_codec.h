#pragma once

#include "tmc/core/result.h"
#include "tmc/signaling/invitation.h"

#include <QByteArray>

namespace tmc {

class InvitationCodec {
public:
    static constexpr qsizetype MaxBytes = 256 * 1024;
    static QByteArray encode(const Invitation&);
    static QString encodeText(const Invitation&);
    static QString encodeLink(const Invitation&);
    static Result<Invitation> decode(const QByteArray&);
    static Result<Invitation> decodeText(const QString&);
};

} // namespace tmc
