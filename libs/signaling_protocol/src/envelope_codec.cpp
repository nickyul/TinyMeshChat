#include "tmc/signaling_protocol/envelope_codec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QVector>

namespace tmc::signaling_protocol {
namespace {

CodecError error(CodecErrorCode code, const char* message) {
    return {code, QString::fromLatin1(message)};
}

bool isLowerOrDigit(QChar character) {
    const auto c = character.unicode();
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

std::optional<CodecError> validateFields(const Envelope& envelope) {
    if (envelope.version != 1) {
        return error(CodecErrorCode::UnsupportedVersion, "Unsupported protocol version");
    }
    if (envelope.type.isEmpty() || envelope.type.size() > EnvelopeCodec::MaxTypeLength) {
        return error(CodecErrorCode::InvalidType, "Invalid message type");
    }
    for (const auto c : envelope.type) {
        if (!isLowerOrDigit(c) && c != '.' && c != '_') {
            return error(CodecErrorCode::InvalidType, "Invalid message type");
        }
    }
    if (envelope.requestId) {
        const auto& id = *envelope.requestId;
        if (id.isEmpty() || id.size() > EnvelopeCodec::MaxRequestIdLength) {
            return error(CodecErrorCode::InvalidRequestId, "Invalid request identifier");
        }
        for (const auto c : id) {
            if (!isLowerOrDigit(c) && !(c >= 'A' && c <= 'Z') && c != '-' && c != '_') {
                return error(CodecErrorCode::InvalidRequestId, "Invalid request identifier");
            }
        }
    }
    return std::nullopt;
}

bool isWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Run only after Qt has validated the complete JSON syntax. Scan the original
// bytes because QJsonObject no longer preserves duplicate keys. Objects inside
// arrays get their own key sets; escaped key names are decoded by Qt as well.
bool hasDuplicateKeys(const QByteArray& bytes) {
    QVector<QSet<QString>> objects;
    for (qsizetype pos = 0; pos < bytes.size();) {
        const auto c = bytes[pos];
        if (c == '{') {
            objects.append(QSet<QString>{});
            ++pos;
        } else if (c == '}') {
            objects.removeLast();
            ++pos;
        } else if (c == '"') {
            const auto start = pos++;
            while (pos < bytes.size()) {
                if (bytes[pos] == '\\') {
                    pos += 2;
                } else if (bytes[pos++] == '"') {
                    break;
                }
            }
            const auto end = pos;
            while (pos < bytes.size() && isWhitespace(bytes[pos])) {
                ++pos;
            }
            if (pos < bytes.size() && bytes[pos] == ':') {
                QByteArray wrapped("[");
                wrapped.append(bytes.mid(start, end - start));
                wrapped.append(']');
                const auto key = QJsonDocument::fromJson(wrapped).array().at(0).toString();
                auto& keys = objects.last();
                if (keys.contains(key)) {
                    return true;
                }
                keys.insert(key);
            }
        } else {
            ++pos;
        }
    }
    return false;
}

} // namespace

EnvelopeCodec::EncodeResult EnvelopeCodec::encode(const Envelope& envelope) {
    if (const auto failure = validateFields(envelope)) {
        return *failure;
    }
    QJsonObject object{{QStringLiteral("v"), envelope.version},
                       {QStringLiteral("type"), envelope.type},
                       {QStringLiteral("body"), envelope.body}};
    if (envelope.requestId) {
        object.insert(QStringLiteral("requestId"), *envelope.requestId);
    }
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxMessageBytes) {
        return error(CodecErrorCode::MessageTooLarge, "Message exceeds size limit");
    }
    return bytes;
}

std::optional<CodecError> EnvelopeCodec::validate(const Envelope& envelope) {
    const auto encoded = encode(envelope);
    if (const auto* failure = std::get_if<CodecError>(&encoded)) {
        return *failure;
    }
    return std::nullopt;
}

EnvelopeCodec::DecodeResult EnvelopeCodec::decode(const QByteArray& bytes) {
    if (bytes.size() > MaxMessageBytes) {
        return error(CodecErrorCode::MessageTooLarge, "Message exceeds size limit");
    }
    if (QString::fromUtf8(bytes).toUtf8() != bytes) {
        return error(CodecErrorCode::InvalidJson, "Invalid UTF-8 JSON document");
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return error(CodecErrorCode::InvalidJson, "Invalid JSON document");
    }
    if (!document.isObject()) {
        return error(CodecErrorCode::InvalidEnvelope, "Envelope must be an object");
    }
    if (hasDuplicateKeys(bytes)) {
        return error(CodecErrorCode::DuplicateKey, "Duplicate JSON object key");
    }
    const auto object = document.object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.key() != "v" && it.key() != "type" && it.key() != "requestId" &&
            it.key() != "body") {
            return error(CodecErrorCode::UnknownField, "Unknown envelope field");
        }
    }
    const auto version = object.value(QStringLiteral("v"));
    if (!version.isDouble() || !object.value(QStringLiteral("type")).isString() ||
        !object.value(QStringLiteral("body")).isObject()) {
        return error(CodecErrorCode::InvalidEnvelope, "Missing or invalid envelope field");
    }
    if (version.toDouble() != 1.0) {
        return error(CodecErrorCode::UnsupportedVersion, "Unsupported protocol version");
    }
    Envelope envelope;
    envelope.type = object.value(QStringLiteral("type")).toString();
    envelope.body = object.value(QStringLiteral("body")).toObject();
    if (object.contains(QStringLiteral("requestId"))) {
        const auto id = object.value(QStringLiteral("requestId"));
        if (!id.isString()) {
            return error(CodecErrorCode::InvalidRequestId, "Invalid request identifier");
        }
        envelope.requestId = id.toString();
    }
    if (const auto failure = validateFields(envelope)) {
        return *failure;
    }
    return envelope;
}

} // namespace tmc::signaling_protocol
