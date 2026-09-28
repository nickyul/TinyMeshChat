#include "tmc/signaling_protocol/envelope_codec.h"
#include "proto/signaling.pb.h"

#include <QJsonArray>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>

#include <limits>
#include <cmath>

namespace tmc::signaling_protocol {
namespace {
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;

CodecError error(CodecErrorCode code, const char* message) {
    return {code, QString::fromLatin1(message)};
}

bool isLowerOrDigit(QChar c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// The application owns Qt values; the wire schema owns field names and types.
// No JSON text is serialized into the protobuf envelope.
bool writeObject(const QJsonObject& object, Message& message) {
    const auto* descriptor = message.GetDescriptor();
    const auto* reflection = message.GetReflection();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto* field = descriptor->FindFieldByName(it.key().toStdString());
        if (!field) return false;
        const auto value = it.value();
        if (field->is_repeated()) {
            if (!value.isArray()) return false;
            for (const auto& item : value.toArray()) {
                if (field->cpp_type() == FieldDescriptor::CPPTYPE_STRING) {
                    if (!item.isString()) return false;
                    reflection->AddString(&message, field, item.toString().toStdString());
                } else if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
                    if (!item.isObject() || !writeObject(item.toObject(), *reflection->AddMessage(&message, field))) return false;
                } else return false;
            }
            continue;
        }
        switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING:
            if (!value.isString()) return false;
            reflection->SetString(&message, field, value.toString().toStdString());
            break;
        case FieldDescriptor::CPPTYPE_BOOL:
            if (!value.isBool()) return false;
            reflection->SetBool(&message, field, value.toBool());
            break;
        case FieldDescriptor::CPPTYPE_UINT32: {
            const auto number = value.toDouble(-1);
            if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > std::numeric_limits<uint32_t>::max() ||
                number != static_cast<uint32_t>(number)) return false;
            reflection->SetUInt32(&message, field, static_cast<uint32_t>(number));
            break;
        }
        case FieldDescriptor::CPPTYPE_MESSAGE:
            if (!value.isObject() || !writeObject(value.toObject(), *reflection->MutableMessage(&message, field))) return false;
            break;
        default: return false;
        }
    }
    return true;
}

std::optional<QJsonObject> readObject(const Message& message) {
    const auto* descriptor = message.GetDescriptor();
    const auto* reflection = message.GetReflection();
    if (reflection->GetUnknownFields(message).field_count() != 0) return std::nullopt;
    QJsonObject object;
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto* field = descriptor->field(i);
        const auto fieldName = field->name();
        const auto name = QString::fromUtf8(fieldName.data(), static_cast<qsizetype>(fieldName.size()));
        if (field->is_repeated()) {
            QJsonArray array;
            for (int j = 0; j < reflection->FieldSize(message, field); ++j) {
                if (field->cpp_type() == FieldDescriptor::CPPTYPE_STRING)
                    array.append(QString::fromStdString(reflection->GetRepeatedString(message, field, j)));
                else if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
                    const auto child = readObject(reflection->GetRepeatedMessage(message, field, j));
                    if (!child) return std::nullopt;
                    array.append(*child);
                } else return std::nullopt;
            }
            // Empty TURN credentials are represented by an empty object.
            if (!array.isEmpty() || descriptor->name() != "Turn") object.insert(name, array);
            continue;
        }
        if (!reflection->HasField(message, field)) continue;
        switch (field->cpp_type()) {
        case FieldDescriptor::CPPTYPE_STRING:
            object.insert(name, QString::fromStdString(reflection->GetString(message, field)));
            break;
        case FieldDescriptor::CPPTYPE_BOOL:
            object.insert(name, reflection->GetBool(message, field));
            break;
        case FieldDescriptor::CPPTYPE_UINT32:
            object.insert(name, static_cast<double>(reflection->GetUInt32(message, field)));
            break;
        case FieldDescriptor::CPPTYPE_MESSAGE: {
            const auto child = readObject(reflection->GetMessage(message, field));
            if (!child) return std::nullopt;
            object.insert(name, *child);
            break;
        }
        default: return std::nullopt;
        }
    }
    return object;
}
} // namespace

std::optional<CodecError> EnvelopeCodec::validateHeader(const Envelope& envelope) {
    if (envelope.version != ProtocolVersion) {
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

EnvelopeCodec::EncodeResult EnvelopeCodec::encode(const Envelope& envelope) {
    if (const auto failure = validateHeader(envelope)) return *failure;
    wire::Envelope message;
    message.set_version(envelope.version);
    if (envelope.requestId) message.set_request_id(envelope.requestId->toStdString());
    const auto name = QString(envelope.type).replace('.', '_').toStdString();
    const auto* field = message.GetDescriptor()->FindFieldByName(name);
    if (!field || !field->containing_oneof()) return error(CodecErrorCode::UnknownType, "Unknown message type");
    if (!writeObject(envelope.body, *message.GetReflection()->MutableMessage(&message, field)))
        return error(CodecErrorCode::InvalidBody, "Invalid protobuf message body");
    const auto size = message.ByteSizeLong();
    if (size > MaxMessageBytes) return error(CodecErrorCode::MessageTooLarge, "Message exceeds size limit");
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (!message.SerializeToArray(bytes.data(), static_cast<int>(bytes.size())))
        return error(CodecErrorCode::InvalidEnvelope, "Cannot serialize protobuf message");
    return bytes;
}

EnvelopeCodec::DecodeResult EnvelopeCodec::decode(const QByteArray& bytes) {
    if (bytes.size() > MaxMessageBytes) return error(CodecErrorCode::MessageTooLarge, "Message exceeds size limit");
    wire::Envelope message;
    if (!message.ParseFromArray(bytes.constData(), static_cast<int>(bytes.size())))
        return error(CodecErrorCode::InvalidEnvelope, "Invalid protobuf message");
    if (!message.has_version() || message.version() != ProtocolVersion)
        return error(CodecErrorCode::UnsupportedVersion, "Unsupported protocol version");
    const auto* reflection = message.GetReflection();
    if (reflection->GetUnknownFields(message).field_count() != 0)
        return error(CodecErrorCode::UnknownField, "Unknown envelope field");
    const auto* field = reflection->GetOneofFieldDescriptor(message, message.GetDescriptor()->FindOneofByName("body"));
    if (!field) return error(CodecErrorCode::InvalidEnvelope, "Missing message body");
    Envelope envelope;
    const auto fieldName = field->name();
    envelope.type = QString::fromUtf8(fieldName.data(), static_cast<qsizetype>(fieldName.size())).replace('_', '.');
    if (message.has_request_id()) envelope.requestId = QString::fromStdString(message.request_id());
    const auto body = readObject(reflection->GetMessage(message, field));
    if (!body) return error(CodecErrorCode::InvalidBody, "Invalid message body");
    envelope.body = *body;
    if (const auto failure = validateHeader(envelope)) return *failure;
    return envelope;
}
} // namespace tmc::signaling_protocol
