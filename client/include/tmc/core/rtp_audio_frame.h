#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace tmc {

struct IncomingRtpAudioFrame {
    QString peerId;
    quint32 ssrc{0};
    quint16 sequenceNumber{0};
    quint32 rtpTimestamp{0};
    QByteArray opusPayload;
    qint64 receivedAtNs{0};
};

struct OutgoingOpusAudioFrame {
    quint32 rtpTimestamp{0};
    QByteArray opusPayload;
};

class IncomingRtpAudioSink {
public:
    virtual ~IncomingRtpAudioSink() = default;
    virtual void enqueue(IncomingRtpAudioFrame frame) = 0;
};

class OutgoingOpusAudioSink {
public:
    virtual ~OutgoingOpusAudioSink() = default;
    virtual void enqueue(OutgoingOpusAudioFrame frame) = 0;
};

} // namespace tmc
