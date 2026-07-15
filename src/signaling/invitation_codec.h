#pragma once
#include "core/result.h"
#include "signaling/invitation.h"
#include <QByteArray>
namespace tmc {
class InvitationCodec {
  public:
    static constexpr qsizetype MaxBytes = 256 * 1024;
    static QByteArray encode(const Invitation&);
    static QString encodeText(const Invitation&);
    static Result<Invitation> decode(const QByteArray&,
                                     const QDateTime& now = QDateTime::currentDateTimeUtc());
    static Result<Invitation> decodeText(const QString&,
                                         const QDateTime& now = QDateTime::currentDateTimeUtc());
};
} // namespace tmc
