#pragma once

#include "tmc/signaling_protocol/envelope.h"

#include <QByteArray>

#include <variant>

namespace tmc::signaling_protocol {

enum class CodecErrorCode {
    MessageTooLarge,
    InvalidJson,
    DuplicateKey,
    InvalidEnvelope,
    UnknownField,
    UnsupportedVersion,
    InvalidType,
    InvalidRequestId,
    InvalidBody,
    UnknownType,
};

// Local diagnostics, not a server-to-client error message. Never contains input data.
struct CodecError {
    CodecErrorCode code;
    QString message;
};

class EnvelopeCodec {
public:
    static constexpr qsizetype MaxMessageBytes = 64 * 1024;
    static constexpr qsizetype MaxTypeLength = 64;
    static constexpr qsizetype MaxRequestIdLength = 64;

    using EncodeResult = std::variant<QByteArray, CodecError>;
    using DecodeResult = std::variant<Envelope, CodecError>;

    [[nodiscard]] static std::optional<CodecError> validate(const Envelope& envelope);
    [[nodiscard]] static EncodeResult encode(const Envelope& envelope);
    [[nodiscard]] static DecodeResult decode(const QByteArray& bytes);
};

} // namespace tmc::signaling_protocol
